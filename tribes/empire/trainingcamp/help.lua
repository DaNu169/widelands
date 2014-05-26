-- The Imperial Training Camp

include "scripting/formatting.lua"
include "scripting/format_help.lua"

set_textdomain("tribe_empire")

return {
   func = function(building_description)
	-- need to get this again, so the building description will be of type "trainingsite"
	local building_description = wl.Game():get_building_description("empire", building_description.name)
	return

	--Lore Section
	building_help_lore_string("empire", building_description, _[[Text needed]], _[[Source needed]]) ..

	--General Section
	building_help_general_string("empire", building_description, "soldier",
		_"Trains soldiers in Attack up to level %1$s, and in Health up to level %2$s."
			:bformat(building_description.max_attack+1,building_description.max_hp+1)
			.. "<br>" .."Equips the soldiers with all necessary weapons and armor parts.") ..

	--Dependencies
	-- We would need to parse the production programs to automate the parameters here; so we do it manually
	-- TODO make pictures dependencies_training("empire", building_description, "fulltrained-evade", "untrained+evade") ..

	rt(h3(_"Attack Training:")) ..
	dependencies_training_food("empire", { {"fish", "meat"}, {"bread"}}) ..
	dependencies_training_weapons("empire", building_description, "and", 
		{"lance", "advanced_lance", "heavy_lance", "war_lance"}, "weaponsmithy") ..

	rt(h3(_"Health Training:")) ..
	dependencies_training_food("empire", { {"fish", "meat"}, {"bread"}}) ..
	dependencies_training_weapons("empire", building_description, "and", 
		{"helm", "armor", "chain_armor", "plate_armor"}, "armorsmithy") ..

	--Workers Section
	building_help_crew_string("empire", building_description) ..

	--Building Section
	building_help_building_section("empire", building_description) ..

	--Production Section
	building_help_production_section(_[[Calculation needed]])
   end
}
