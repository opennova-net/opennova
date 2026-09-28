class_name DefFixture
extends RefCounted

## Authored test tables for binding and presenter tests. These are deliberately
## small inputs, not copies of shipped definitions. Retail parser/catalog pins
## remain in the retail suite. Files exist only because ResourceRoot and the
## GDExtension loaders exercise the real file-loading boundary.
static var _directory := ""

const HUD := """VEHICLE_HUD
 sid test_vehicle
 interface fixture_panel.tga
VEHICLE_END
"""

const WEAPON_FIELDS := """
 scope_max_mag 3
 error 0 0 0 0
 hudclipgfx 11 12 fixture_clip.tga
 hudrndgfx 13 14 15 16 2 fixture_round.tga
 gfx1 M4_1st
 gfx3 M4_3RD
 animadm M4_1st
 pos 1 2 3 0 0 1
 tpos 4 5 6 0 0 0
"""

## The lines every authored weapon row opens with (ArmoryFixture's rows share
## them): the name, category and rank, the clip and carry counts, the ammo class.
static func weapon_header(name: String, category: int, rank: int, clip: int,
		ammo_class: String) -> String:
	return """weapon "%s"
 category %d
 rank %d
 clipsize %d
 maxclips 7
 startrounds 210
 ammoclass %s 1
""" % [name, category, rank, clip, ammo_class]


static func weapon_row(name: String, category: int, rank: int, clip: int,
		flags := "", ammo := "AM_556MM") -> String:
	var text := weapon_header(name, category, rank, clip, "CLASS_556MM") + """ round_type %s
 loadout_selectable 1
 charfilter rifleman
 charfilter engineer
 teamfilter blue
 teamfilter red
 targetyawrange 180
 targetpitchmin -80
 targetpitchmax 80
""" % [ammo]
	if not flags.is_empty():
		text += " flags %s\n" % flags
	if name == "WPN_M4AUTO":
		text += " loadout_subclasses 1\n"
	if category == 3:
		text += " weapon_class primary\n"
	elif category == 2:
		text += " weapon_class secondary\n"
	text += WEAPON_FIELDS
	if name == "WPN_EMPLCD50NA":
		text += " flags emplaced\n scope_max_mag 0\n"
	for action in ["IDLE", "EMPTYIDLE", "FIRE", "RECOIL", "RELOAD", "EMPTY",
			"SWITCHTO", "SWITCHFROM", "SWITCHRANK"]:
		var animation: String = "IDLE" if action == "EMPTYIDLE" else action
		var delay_start := 0
		var delay_end := "auto"
		if action == "FIRE":
			delay_end = "5"
		elif action == "RECOIL":
			delay_end = "0"
		elif action == "RELOAD":
			delay_start = 20
			delay_end = "1"
		elif action.begins_with("SWITCH"):
			delay_start = 1
			delay_end = "1"
		text += """ ACTION "%s"
 DELAYSTART %d
 DELAYEND %s
 ANIM anim_wpn_%s
 FUNCTION WPN_STD_%s
 END
""" % [action, delay_start, delay_end, animation.to_lower(), action]
	return text + "end\n"


static func weapon_text() -> String:
	return "ammoclass_max_carry CLASS_556MM 1000\n" \
		+ weapon_row("WPN_M4AUTO", 3, 0, 30, "auto") \
		+ weapon_row("WPN_M4", 3, 1, 30) \
		+ weapon_row("WPN_M9Beretta", 2, 0, 15) \
		+ weapon_row("WPN_SATCHEL_CHARGE", 7, 0, 3) \
		+ weapon_row("WPN_SATCHEL_DETONATOR", 8, 0, 1) \
		+ weapon_row("WPN_EMPLCD50NA", 9, 0, 100, "auto")


static func directory() -> String:
	if not _directory.is_empty():
		return _directory
	_directory = ProjectSettings.globalize_path("res://../.godot-test-fixtures/defs")
	var made := DirAccess.make_dir_recursive_absolute(_directory)
	assert(made == OK, "the def fixture directory is creatable")
	WorldFixture.write_file(_directory.path_join("weapon.def"), weapon_text())
	WorldFixture.write_file(_directory.path_join("hud_weapon.def"), weapon_text().replace(
			"error 0 0 0 0", "error 1 2 3 4\n sights fixture_sight.tga 7 8 9 10 blend"))
	WorldFixture.write_file(_directory.path_join("ammo.def"), ammo_text())
	WorldFixture.write_file(_directory.path_join("hudpos.def"), HUD)
	var copied := DirAccess.copy_absolute(
			ProjectSettings.globalize_path(ItemDbFixture.FIXTURE_ITEMS),
			_directory.path_join("items.def"))
	assert(copied == OK, "the committed items table copies beside the authored defs")
	return _directory


static func ammo_text() -> String:
	# The impact drain only publishes authored effect/sound rows. Distinct
	# sentinels let the real projectile produce an observable presentation event.
	return """ammo AT_NULL
end
ammo AM_556MM
 velocity 3000
 max_age 5
 drag 1
 min_damage 25
 max_damage 40
 penetration_impact 100
 effects_table
 player FixtureHit FixtureImpact 0
 obj FixtureHit FixtureImpact 0
 dirt FixtureHit FixtureImpact 0
 grass FixtureHit FixtureImpact 0
 snow FixtureHit FixtureImpact 0
 cement FixtureHit FixtureImpact 0
 sand FixtureHit FixtureImpact 0
 packeddirt FixtureHit FixtureImpact 0
 water FixtureHit FixtureImpact 0
 railroad FixtureHit FixtureImpact 0
 mud FixtureHit FixtureImpact 0
 ice FixtureHit FixtureImpact 0
 quicksand FixtureHit FixtureImpact 0
 stone FixtureHit FixtureImpact 0
 wood FixtureHit FixtureImpact 0
 metal FixtureHit FixtureImpact 0
 glass FixtureHit FixtureImpact 0
 cloth FixtureHit FixtureImpact 0
 foliage FixtureHit FixtureImpact 0
 hmetal FixtureHit FixtureImpact 0
 flesh FixtureHit FixtureImpact 0
 bodyarmor FixtureHit FixtureImpact 0
 end
end
"""
