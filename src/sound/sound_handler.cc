/*
 * Copyright (C) 2005-2019 by the Widelands Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include "sound/sound_handler.h"

#include <cerrno>
#include <memory>

#include <SDL.h>
#include <SDL_mixer.h>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/regex.hpp>
#ifdef _WIN32
#include <windows.h>
#endif

#include "base/i18n.h"
#include "base/log.h"
#include "helper.h"
#include "io/filesystem/layered_filesystem.h"
#include "profile/profile.h"

namespace {

constexpr int kDefaultMusicVolume = 64;
constexpr int kDefaultFxVolume = 128;
constexpr int kNumMixingChannels = 32;

/// How many milliseconds in the past to consider for
/// SoundHandler::play_or_not()
constexpr uint32_t kSlidingWindowSize = 20000;
}  // namespace

/** The global \ref SoundHandler object
 * The sound handler is a static object because otherwise it'd be quite
 * difficult to pass the --nosound command line option
 */
SoundHandler g_sound_handler;

/** This is just a basic constructor. The \ref SoundHandler must already exist
 * during command line parsing because --nosound needs to be known. At this
 * time, however, all other information is still unknown, so a real
 * initialization cannot take place.
 * \sa SoundHandler::init()
 */
SoundHandler::SoundHandler()
   : sound_options_{
{SoundType::kUI, SoundOptions(kDefaultFxVolume, "ui")},
{SoundType::kMessage, SoundOptions(kDefaultFxVolume, "message")},
{SoundType::kChat, SoundOptions(kDefaultFxVolume, "chat")},
{SoundType::kAmbient, SoundOptions(kDefaultFxVolume, "ambient")},
{SoundType::kMusic, SoundOptions(kDefaultMusicVolume, "music")}},
     fx_lock_(nullptr), backend_is_disabled_(false) {
}

/// Housekeeping: unset hooks. Audio data will be freed automagically by the
/// \ref Songset and \ref FXset destructors, but not the {song|fx}sets
/// themselves.
SoundHandler::~SoundHandler() {
}

/** The real initialization for SoundHandler.
 *
 * \see SoundHandler::SoundHandler()
 */
void SoundHandler::init() {
	read_config();

	// This RNG will still be somewhat predictable, but it's just to avoid
	// identical playback patterns
	rng_.seed(SDL_GetTicks());

	if (is_backend_disabled()) {
		return;
	}

// Windows Music has crickling inside if the buffer has another size
// than 4k, but other systems work fine with less, some crash
// with big buffers.
#ifdef _WIN32
	const uint16_t bufsize = 4096;
#else
	const uint16_t bufsize = 1024;
#endif

	SDL_version sdl_version;
	SDL_GetVersion(&sdl_version);
	log("**** SOUND REPORT ****\n");
	log("SDL version: %d.%d.%d\n", static_cast<unsigned int>(sdl_version.major),
	    static_cast<unsigned int>(sdl_version.minor), static_cast<unsigned int>(sdl_version.patch));

	/// SDL 2.0.6 will crash due to upstream bug:
	/// https://bugs.launchpad.net/ubuntu/+source/libsdl2/+bug/1722060
	if (sdl_version.major == 2 && sdl_version.minor == 0 && sdl_version.patch == 6) {
		log("Disabled sound due to a bug in SDL 2.0.6\n");
		disable_backend();
	}

	SDL_MIXER_VERSION(&sdl_version);
	log("SDL_mixer version: %d.%d.%d\n", static_cast<unsigned int>(sdl_version.major),
	    static_cast<unsigned int>(sdl_version.minor), static_cast<unsigned int>(sdl_version.patch));

	log("**** END SOUND REPORT ****\n");

	if (is_backend_disabled()) {
		return;
	}

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		initialization_error(SDL_GetError(), false);
		return;
	}

	if (Mix_OpenAudio(MIX_DEFAULT_FREQUENCY, MIX_DEFAULT_FORMAT, 2, bufsize) != 0) {
		initialization_error(Mix_GetError(), true);
		return;
	}

	constexpr int kMixInitFlags = MIX_INIT_OGG;
	int initted = Mix_Init(kMixInitFlags);
	if ((initted & kMixInitFlags) != kMixInitFlags) {
		initialization_error("No Ogg support in SDL_Mixer.", true);
		return;
	}

	if (Mix_AllocateChannels(kNumMixingChannels) != kNumMixingChannels) {
		initialization_error(Mix_GetError(), true);
		return;
	}

	Mix_HookMusicFinished(SoundHandler::music_finished_callback);
	Mix_ChannelFinished(SoundHandler::fx_finished_callback);
	Mix_VolumeMusic(sound_options_.at(SoundType::kMusic).volume);  //  can not do this before InitSubSystem

	if (fx_lock_ == nullptr) {
		fx_lock_ = SDL_CreateMutex();
	}
}

void SoundHandler::initialization_error(const char* const msg, bool quit_sdl) {
	log("WARNING: Failed to initialize sound system: %s\n", msg);
	disable_backend();
	if (quit_sdl) {
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	}
	return;
}

void SoundHandler::shutdown() {
	Mix_ChannelFinished(nullptr);
	Mix_HookMusicFinished(nullptr);

	int numtimesopened, frequency, channels;
	uint16_t format;
	numtimesopened = Mix_QuerySpec(&frequency, &format, &channels);
	log("SoundHandler closing times %i, freq %i, format %i, chan %i\n", numtimesopened, frequency,
	    format, channels);

	if (!numtimesopened)
		return;

	Mix_HaltChannel(-1);

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) == -1) {
		log("audio error %s\n", SDL_GetError());
	}

	log("SDL_AUDIODRIVER %s\n", SDL_GetCurrentAudioDriver());

	if (numtimesopened != 1) {
		log("PROBLEM: sound device opened multiple times, trying to close");
	}
	for (int i = 0; i < numtimesopened; ++i) {
		Mix_CloseAudio();
	}

	if (fx_lock_) {
		SDL_DestroyMutex(fx_lock_);
		fx_lock_ = nullptr;
	}

	songs_.clear();
	fxs_.clear();

	Mix_Quit();
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

/** Read the main config file, load background music and systemwide sound fx
 *
 */
void SoundHandler::read_config() {
	// TODO(GunChleoc): Compatibility code to avoid getting bug reports about unread sections. Remove after build 21.
	if (g_options.get_section("sound") == nullptr) {
		Section& global = g_options.pull_section("global");

		for (auto& option : sound_options_) {
			switch (option.first) {
			case SoundType::kMusic:
				option.second.volume = global.get_int("music_volume", option.second.volume);
				option.second.enabled = !global.get_bool("disable_music", !option.second.enabled);
				break;
			case SoundType::kChat:
				option.second.volume = global.get_int("fx_volume", option.second.volume);
				option.second.enabled = global.get_bool("sound_at_message", option.second.enabled);
				break;
			default:
				option.second.volume = global.get_int("fx_volume", option.second.volume);
				option.second.enabled = !global.get_bool("disable_fx", !option.second.enabled);
				break;
			}
		}
	}

	Section& sound = g_options.pull_section("sound");
	for (auto& option : sound_options_) {
		option.second.volume = sound.get_int(("volume_" + option.second.name).c_str(), option.second.volume);
		option.second.enabled = sound.get_bool(("enable_" + option.second.name).c_str(), option.second.enabled);
	}

	save_and_backup_config();

	if (is_backend_disabled()) {
		return;
	}

	register_songs("music", "intro");
	register_songs("music", "menu");
	register_songs("music", "ingame");

	register_fx(SoundType::kUI, "sound", "click", "click");
	register_fx(SoundType::kAmbient, "sound", "create_construction_site", "create_construction_site");
	register_fx(SoundType::kMessage, "sound", "message", "message");
	register_fx(SoundType::kMessage, "sound/military", "under_attack", "military/under_attack");
	register_fx(SoundType::kMessage, "sound/military", "site_occupied", "military/site_occupied");
	register_fx(SoundType::kChat, "sound", "lobby_chat", "lobby_chat");
	register_fx(SoundType::kChat, "sound", "lobby_freshmen", "lobby_freshmen");
}

void SoundHandler::save_and_backup_config() {
	Section& sound = g_options.pull_section("sound");
	for (auto& option : sound_options_) {
		const int volume = option.second.volume;
		const std::string& name = option.second.name;
		const bool enabled = option.second.enabled;

		backup_options_.insert(std::make_pair(option.first, SoundOptions(enabled, volume, name)));

		const std::string enable_name = "enable_" + name;
		sound.set_bool(enable_name.c_str(), enabled);

		const std::string volume_name = "volume_" + name;
		sound.set_int(volume_name.c_str(), volume);
	}
}

void SoundHandler::restore_config() {
	for (auto& option : backup_options_) {
		set_volume(option.first, option.second.volume);
		set_enable_sound(option.first, option.second.enabled);
	}
}

/**
 * Returns 'true' if the playing of sounds is disabled due to sound driver problems.
 */
bool SoundHandler::is_backend_disabled() const {
	return backend_is_disabled_;
}

void SoundHandler::disable_backend() {
	backend_is_disabled_ = true;
}

/** Register a sound effect. One sound effect can consist of several audio files
 * named EFFECT_XX.ogg, where XX is between 00 and 99.
 *
 * Subdirectories of and files under FILENAME_XX can be named anything you want.
 *
 * \param dir        The relative directory where the audio files reside in data/sound
 * \param filename   Name from which filenames will be formed
 *                   (BASENAME_XX.ogg);
 *                   also the name used with \ref play_fx
 */
void SoundHandler::register_fx(SoundType type, const std::string& dir,
                                     const std::string& basename,
                                     const std::string& fx_name) {
	if (is_backend_disabled() || fxs_[type].count(fx_name) > 0) {
		return;
	}
	fxs_[type].insert(std::make_pair(fx_name, std::unique_ptr<FXset>(new FXset(dir, basename))));
}

/** Find out whether to actually play a certain effect right now or rather not
 * (to avoid "sonic overload").
 */
// TODO(unknown): What is the selection algorithm? cf class documentation
bool SoundHandler::play_or_not(SoundType type, const std::string& fx_name,
                               uint8_t const priority) {
	assert(!backend_is_disabled_ && is_sound_enabled(type));

	if (fxs_[type].count(fx_name) == 0) {
		return false;
	}

	switch (type) {
	case SoundType::kAmbient:
		break;
	default:
		// We always play Ui, chat and system sounds
		return true;
	}

	// We always play important sounds
	if (priority == kFxPriorityAlwaysPlay) {
		return true;
	}

	// Do not run multiple instances of the same sound effect if the priority is too low
	bool too_many_playing = false;
	if (priority < kFxPriorityAllowMultiple) {
		// Access to active_fx_ is protected because it can
		// be accessed from callback
		if (fx_lock_) {
			SDL_LockMutex(fx_lock_);
		}

		// Find out if an fx called fx_name is already running
		for (const auto& fx_pair : active_fx_) {
			if (fx_pair.second == fx_name) {
				too_many_playing = true;
				break;
			}
		}
	}

	if (fx_lock_) {
		SDL_UnlockMutex(fx_lock_);
	}

	if (too_many_playing) {
		return false;
	}

	// TODO(unknown): long time since any play increases weighted_priority
	// TODO(unknown): high general frequency reduces weighted priority
	// TODO(unknown): deal with "coupled" effects like throw_net and retrieve_net

	uint32_t const ticks_since_last_play = fxs_[type][fx_name]->ticks_since_last_play();

	// Weighted total probability that this fx gets played; initially set according to priority
	//  float division! not integer
	float probability = (priority % kFxPriorityAllowMultiple) / static_cast<float>(kFxPriorityAllowMultiple);

	// Temporary to calculate single influences
	float evaluation = 1.0f;

	//  reward an fx for being silent
	if (ticks_since_last_play > kSlidingWindowSize) {
		evaluation = 1.0f;  //  arbitrary value; 0 -> no change, 1 -> probability = 1

		//  "decrease improbability"
		probability = 1.0f - ((1.0f - probability) * (1.0f - evaluation));
	} else {  // Penalize an fx for playing in short succession
		evaluation = static_cast<float>(ticks_since_last_play) / kSlidingWindowSize;
		probability *= evaluation;  //  decrease probability
	}

	// finally: the decision
	// float division! not integer
	return (rng_.rand() % kFxPriorityAlwaysPlay) / static_cast<float>(kFxPriorityAlwaysPlay) <= probability;
}

/** \overload
 * \param fx_name  The identifying name of the sound effect, see \ref load_fx .
 * \param stereo_position  position in widelands' game window, see
 *                         \ref stereo_position
 * \param priority         How important is it that this FX actually gets
 *                         played? (see \ref FXset::priority_)
 */
void SoundHandler::play_fx(SoundType type, const std::string& fx_name,
                           int32_t const stereo_pos,
                           uint8_t const priority, int distance) {
	if (backend_is_disabled_ || !is_sound_enabled(type)) {
		return;
	}

	assert(stereo_pos >= kStereoLeft);
	assert(stereo_pos <= kStereoRight);

	if (fxs_[type].count(fx_name) == 0) {
		log("SoundHandler: sound effect \"%s\" does not exist!\n", fx_name.c_str());
		return;
	}

	// See if the FX should be played
	if (!play_or_not(type, fx_name, priority)) {
		return;
	}

	//  retrieve the fx and play it if it's valid
	if (Mix_Chunk* const m = fxs_[type][fx_name]->get_fx(rng_.rand())) {
		const int32_t chan = Mix_PlayChannel(-1, m, 0);
		if (chan == -1) {
			log("SoundHandler: Mix_PlayChannel failed: %s\n", Mix_GetError());
		} else {
			Mix_SetPanning(chan, kStereoRight - stereo_pos, stereo_pos);
			Mix_SetDistance(chan, distance);
			Mix_Volume(chan, get_volume(type));

			// Access to active_fx_ is protected
			// because it can be accessed from callback
			if (fx_lock_)
				SDL_LockMutex(fx_lock_);
			active_fx_[chan] = fx_name;
			if (fx_lock_)
				SDL_UnlockMutex(fx_lock_);
		}
	} else
		log("SoundHandler: sound effect \"%s\" exists but contains no files!\n", fx_name.c_str());
}

/** Load a background song. One "song" can consist of several audio files named
 * FILE_XX.ogg, where XX is between 00 and 99.
 * \param dir        The directory where the audio files reside.
 * \param basename   Name from which filenames will be formed
 *                   (BASENAME_XX.ogg); also the name used with \ref play_fx .
 * This just registers the song, actual loading takes place when
 * \ref Songset::get_song() is called, i.e. when the song is about to be
 * played. The song will automatically be removed from memory when it has
 * finished playing.
 */
void SoundHandler::register_songs(const std::string& dir, const std::string& basename) {
	if (is_backend_disabled()) {
		return;
	}

	assert(g_fs);

	FilenameSet files = filter(g_fs->list_directory(dir), [&basename](const std::string& fn) {
		const std::string only_filename = FileSystem::fs_filename(fn.c_str());
		return boost::starts_with(only_filename, basename) && boost::ends_with(only_filename, ".ogg");
	});

	for (const std::string& filename : files) {
		assert(!g_fs->is_directory(filename));
		if (songs_.count(basename) == 0) {
			songs_.insert(std::make_pair(basename, std::unique_ptr<Songset>(new Songset())));
		}
		songs_[basename]->add_song(filename);
	}
}

/** Start playing a songset.
 * \param songset_name  The songset to play a song from.
 * \param fadein_ms     Song will fade from 0% to 100% during fadein_ms
 *                      milliseconds starting from now.
 * \note When calling start_music() while music is still fading out from
 * \ref stop_music()
 * or \ref change_music() this function will block until the fadeout is complete
 */
void SoundHandler::start_music(const std::string& songset_name, int fadein_ms) {
	if (backend_is_disabled_ || !is_sound_enabled(SoundType::kMusic)) {
		return;
	}

	if (Mix_PlayingMusic()) {
		change_music(songset_name, 0, fadein_ms);
	}

	if (songs_.count(songset_name) == 0) {
		log("SoundHandler: songset \"%s\" does not exist!\n", songset_name.c_str());
	} else {
		if (Mix_Music* const m = songs_[songset_name]->get_song(rng_.rand())) {
			Mix_FadeInMusic(m, 1, std::max(fadein_ms, kMinimumMusicFade));
			current_songset_ = songset_name;
		} else {
			log("SoundHandler: songset \"%s\" exists but contains no files!\n", songset_name.c_str());
		}
	}
	Mix_VolumeMusic(sound_options_.at(SoundType::kMusic).volume);
}

/** Stop playing a songset.
 * \param fadeout_ms Song will fade from 100% to 0% during fadeout_ms
 *                   milliseconds starting from now.
 */
void SoundHandler::stop_music(int fadeout_ms) {
	if (backend_is_disabled_) {
		return;
	}

	if (Mix_PlayingMusic()) {
		Mix_FadeOutMusic(std::max(fadeout_ms, kMinimumMusicFade));
	}
}

/** Play an other piece of music.
 * This is a member function provided for convenience. It is a wrapper around
 * \ref start_music and \ref stop_music.
 * \param fadeout_ms  Old song will fade from 100% to 0% during fadeout_ms
 *                    milliseconds starting from now.
 * \param fadein_ms   New song will fade from 0% to 100% during fadein_ms
 *                    milliseconds starting from now.
 * If songset_name is empty, another song from the currently active songset will
 * be selected
 */
void SoundHandler::change_music(const std::string& songset_name,
                                int const fadeout_ms,
                                int const fadein_ms) {
	if (is_backend_disabled()) {
		return;
	}

	if (!songset_name.empty()) {
		current_songset_ = songset_name;
	}

	if (Mix_PlayingMusic()) {
		stop_music(fadeout_ms);
	} else {
		start_music(current_songset_, fadein_ms);
	}
}

bool SoundHandler::is_sound_enabled(SoundType type) const {
	assert(sound_options_.count(type) == 1);
	return sound_options_.at(type).enabled;
}
int32_t SoundHandler::get_volume(SoundType type) const {
	assert(sound_options_.count(type) == 1);
	return sound_options_.at(type).volume;
}

// NOCOM usage? Document.
void SoundHandler::set_enable_sound(SoundType type, bool const enable) {
	assert(sound_options_.count(type) == 1);
	SoundOptions& sound_options = sound_options_.at(type);
	if (is_backend_disabled() || (sound_options.enabled == enable)) {
		return;
	}

	sound_options.enabled = enable;

	// Special treatment for music
	switch (type) {
	case SoundType::kMusic:
		if (enable) {
			start_music(current_songset_);
		} else {
			stop_music();
		}
		break;
	default:
		break;
	}
}

/**
 * Normal set_* function
 * Set the FX sound volume between 0 (muted) and \ref get_max_volume().
 * The new value is written back to the config file.
 *
 * \param volume The new music volume.
 */
void SoundHandler::set_volume(SoundType type, int32_t volume) {
	if (!is_backend_disabled()) {
		assert(sound_options_.count(type) == 1);
		SoundOptions& sound_options = sound_options_.at(type);
		sound_options.volume = volume;

		// Special treatment for music
		switch (type) {
		case SoundType::kMusic:
			Mix_VolumeMusic(volume);
			break;
		default:
			Mix_Volume(-1, volume);
			break;
		}
	}
}

/** Callback to notify \ref SoundHandler that a song has finished playing.
 * Usually, another song from the same songset will be started.
 * There is a special case for the intro screen's music: only one song will be
 * played. If the user has not clicked the mouse or pressed escape when the song
 * finishes, Widelands will automatically go on to the main menu.
 */
void SoundHandler::music_finished_callback() {
	// DO NOT CALL SDL_mixer FUNCTIONS OR SDL_LockAudio FROM HERE !!!

	SDL_Event event;
	if (g_sound_handler.current_songset_ == "intro") {
		// Special case for splashscreen: there, only one song is ever played
		event.type = SDL_KEYDOWN;
		event.key.state = SDL_PRESSED;
		event.key.keysym.sym = SDLK_ESCAPE;
	} else {
		// Else just play the next song - see general description for
		// further explanation
		event.type = SDL_USEREVENT;
		event.user.code = CHANGE_MUSIC;
	}
	SDL_PushEvent(&event);
}

/** Callback to notify \ref SoundHandler that a sound effect has finished
 * playing.
 */
void SoundHandler::fx_finished_callback(int32_t const channel) {
	// DO NOT CALL SDL_mixer FUNCTIONS OR SDL_LockAudio FROM HERE !!!

	assert(0 <= channel);
	g_sound_handler.handle_channel_finished(static_cast<uint32_t>(channel));
}

/** Remove a finished sound fx from the list of currently playing ones
 * This is part of \ref fx_finished_callback
 */
void SoundHandler::handle_channel_finished(uint32_t channel) {
	assert(!is_backend_disabled());

	// Needs locking because active_fx_ may be accessed
	// from this callback or from main thread
	if (fx_lock_)
		SDL_LockMutex(fx_lock_);
	active_fx_.erase(channel);
	if (fx_lock_)
		SDL_UnlockMutex(fx_lock_);
}
