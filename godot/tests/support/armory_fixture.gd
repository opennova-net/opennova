class_name ArmoryFixture
extends RefCounted

## Small authored armory catalog: different teams/classes, a hidden subclass,
## an accessory, and three grenade rows. It exercises filtering and transport
## without depending on the shipped weapon catalog, labels, or menu layout.
static var _directory := ""


static func weapon_row(name: String, category: int, rank: int, slot: String,
		clip: int, team := "", selectable := 1, subclasses := 0) -> String:
	var text := DefFixture.weapon_header(name, category, rank, clip, "FIXTURE_AMMO") + """ weapon_class %s
 loadout_selectable %d
 loadout_subclasses %d
 charfilter rifleman
 charfilter engineer
	weaponweight 4
 clipweight 1.5
 round_type FIXTURE_ROUND
end
""" % [slot, selectable, subclasses]
	var filters := " teamfilter blue\n teamfilter red\n" if team.is_empty() else " teamfilter %s\n" % team
	if name == "WPN_M4AUTO":
		filters += " loadout_menu_textid WEAP_SHORT_M4\n loadout_menu_icon M_4.tga\n"
	return text.replace("\nend\n", "\n" + filters + "end\n")


static func weapon_text() -> String:
	return "ammoclass_max_carry FIXTURE_AMMO 1000\n" \
		+ weapon_row("WPN_M4AUTO", 3, 0, "primary", 30, "blue") \
		+ weapon_row("WPN_AK47AUTO", 3, 2, "primary", 30, "red", 1, 1) \
		+ weapon_row("WPN_AK47", 3, 3, "accessory", 30, "red", 0) \
		+ weapon_row("WPN_M9Beretta", 2, 0, "secondary", 15) \
		+ weapon_row("WPN_SATCHEL_CHARGE", 7, 0, "accessory", 1) \
		+ weapon_row("WPN_GRENADEFB", 5, 0, "grenade", 1) \
		+ weapon_row("WPN_GRENADEHE", 5, 1, "grenade", 1) \
		+ weapon_row("WPN_GRENADESM", 5, 2, "grenade", 1)


static func menu_text() -> String:
	var body := MenuDriverFixture.wnd("spinlist", "PLAYER_CLASS", 10)
	var y := 40
	for name in ["PRIMARY", "SECONDARY", "ACCESSORY", "PRIMARY_AMMO1",
			"SECONDARY_AMMO1", "ACCESSORY_AMMO1", "GRENADE_AMMO1",
			"GRENADE_AMMO2", "GRENADE_AMMO3"]:
		body += MenuDriverFixture.wnd("combobox", name, y)
		y += 24
	for name in ["PRIMARY_ICON", "SECONDARY_ICON", "ACCESSORY_ICON"]:
		body += MenuDriverFixture.wnd("window", name, y)
		y += 24
	body += MenuDriverFixture.wnd("button", "ACCEPT", y)
	body += MenuDriverFixture.wnd("button", "CANCEL", y + 24,
			'<STRING type="id">WD_NOHOT_CANCEL</STRING>')
	body += MenuDriverFixture.wnd("static", "STATIC_TOTAL_WEIGHT", y + 48)
	return MenuDriverFixture.screen_xml("WEAPON",
			"<TEXT_RSRC>menutxt.BIN</TEXT_RSRC>" + body).replace(
			"<NAME>WEAPON</NAME>", "<NAME>WEAPON</NAME><MUSICVAR>14</MUSICVAR>")


static func directory() -> String:
	if not _directory.is_empty():
		return _directory
	_directory = ProjectSettings.globalize_path("res://../.godot-test-fixtures/armory")
	var made := DirAccess.make_dir_recursive_absolute(_directory)
	assert(made == OK, "the armory fixture directory is creatable")
	WorldFixture.write_file(_directory.path_join("weapon.def"), weapon_text())
	WorldFixture.write_file(_directory.path_join("weapon.mnu"), menu_text())
	var game := RtxtStringFile.new()
	game.add_section("WepDes")
	game.add_entry("WEAP_SHORT_M4", "Authored rifle label", 0, Vector2i())
	game.add_entry("FIXTURE_ROUND", "Authored round label", 0, Vector2i())
	var menu := RtxtStringFile.new()
	menu.add_section("Menu")
	menu.add_entry("WD_NOHOT_CANCEL", "Cancel fixture", 0, Vector2i())
	for pair in [["gametext.bin", game], ["menutxt.BIN", menu]]:
		var file := FileAccess.open(_directory.path_join(pair[0]), FileAccess.WRITE)
		assert(file != null)
		file.store_buffer((pair[1] as RtxtStringFile).to_byte_array())
		file.close()
	return _directory


## The loadout screens' slice of the shipped weapon.def: the rows the armory,
## PLAYER_INFO, spawn-kit and profile-page pins name, in the shipped file's order
## (the M4 sub-weapon walk and the grenade fill read row order), each carrying only
## the fields those screens read, valued as the shipped weapon.def authors them
## (WPN_M4AUTO: weaponweight 5.5, maxclips 10, clipweight 1.5, clipsize 30,
## loadout_subclasses 1; WPN_M4M203AUTO: loadout_subclasses 2 ahead of the
## same-round WPN_M4M203 and WPN_M4M203HE's AMMO_M203_40MM_NADE; WPN_SATCHEL_CHARGE:
## maxclips 2, clipweight 17.6; the grenades: maxclips 3, clipweight 1.1). Fields a
## shipped row leaves out stay out (the sub-weapon rows are not loadout_selectable).
const LOADOUT_ROWS := [
	{"weapon": "WPN_KNIFE", "category": 1, "rank": 0, "teamfilter": ["blue"],
		"charfilter": ["medic", "sniper", "gunner", "rifleman", "engineer"],
		"round_type": "AMMO_KNIFE01", "clipsize": -1, "startrounds": -1},
	{"weapon": "WPN_KNIFE2", "category": 1, "rank": 1, "teamfilter": ["red"],
		"charfilter": ["medic", "sniper", "gunner", "rifleman", "engineer"],
		"round_type": "AMMO_KNIFE01", "clipsize": -1, "startrounds": -1},
	{"weapon": "WPN_colt45", "category": 2, "rank": 0, "teamfilter": ["red", "blue"],
		"charfilter": ["medic", "sniper", "rifleman", "engineer"],
		"loadout_selectable": 1, "loadout_subclasses": 0, "maxclips": 5, "clipweight": 1,
		"weaponweight": 1.5, "loadout_menu_icon": "M_colt.tga",
		"loadout_menu_textid": "WEAP_SHORT_COLT45", "weapon_class": "secondary",
		"round_type": "AMMO_COLT_45", "clipsize": 7, "startrounds": 35,
		"ammoclass": "CLASS_45cal"},
	{"weapon": "WPN_M9Beretta", "category": 2, "rank": 1, "teamfilter": ["blue"],
		"charfilter": ["medic", "sniper", "rifleman", "engineer"],
		"loadout_selectable": 1, "loadout_subclasses": 0, "maxclips": 3, "clipweight": 1,
		"weaponweight": 2.5, "loadout_menu_icon": "M_bereta.tga",
		"loadout_menu_textid": "WEAP_SHORT_M9BERETTA", "weapon_class": "secondary",
		"round_type": "AMMO_BERETTA_9MM", "clipsize": 15, "startrounds": 45,
		"ammoclass": "CLASS_9mm"},
	{"weapon": "WPN_357", "category": 2, "rank": 3, "teamfilter": ["red"],
		"charfilter": ["medic", "sniper", "gunner", "rifleman", "engineer"],
		"loadout_selectable": 1, "loadout_subclasses": 0, "maxclips": 6, "clipweight": 0.35,
		"weaponweight": 2.5, "loadout_menu_icon": "M_357m.tga",
		"loadout_menu_textid": "WEAP_SHORT_357", "weapon_class": "secondary",
		"round_type": "AMMO_BERETTA_357", "clipsize": 6, "startrounds": 36,
		"ammoclass": "CLASS_357"},
	{"weapon": "WPN_M4AUTO", "category": 3, "rank": 0, "teamfilter": ["blue"],
		"charfilter": ["rifleman", "medic", "engineer"],
		"loadout_selectable": 1, "loadout_subclasses": 1, "maxclips": 10, "clipweight": 1.5,
		"weaponweight": 5.5, "loadout_menu_icon": "M_4.tga",
		"loadout_menu_textid": "WEAP_SHORT_M4", "weapon_class": "primary",
		"round_type": "AMMO_CAR15_556MM", "clipsize": 30, "startrounds": 300,
		"ammoclass": "CLASS_556MM"},
	{"weapon": "WPN_M4", "category": 3, "rank": 1,
		"charfilter": ["rifleman", "medic", "engineer"],
		"round_type": "AMMO_CAR15_556MM", "clipsize": 30, "startrounds": 300,
		"ammoclass": "CLASS_556MM", "weaponweight": 5.5},
	{"weapon": "WPN_M4M203AUTO", "category": 3, "rank": 2, "teamfilter": ["blue"],
		"charfilter": ["rifleman"],
		"loadout_selectable": 1, "loadout_subclasses": 2, "maxclips": 10, "clipweight": 1.5,
		"weaponweight": 8.5, "loadout_menu_icon": "M_4He.tga",
		"loadout_menu_textid": "WEAP_SHORT_M4M203", "weapon_class": "primary",
		"round_type": "AMMO_CAR15_556MM", "clipsize": 30, "startrounds": 300,
		"ammoclass": "CLASS_556MM"},
	{"weapon": "WPN_M4M203", "category": 3, "rank": 3, "charfilter": ["rifleman"],
		"round_type": "AMMO_CAR15_556MM", "clipsize": 30, "startrounds": 300,
		"ammoclass": "CLASS_556MM", "weaponweight": 8.5},
	{"weapon": "WPN_M4M203HE", "category": 3, "rank": 4, "charfilter": ["rifleman"],
		"maxclips": 6, "clipweight": 1.5, "weaponweight": 8.5,
		"round_type": "AMMO_M203_40MM_NADE", "clipsize": 1, "startrounds": 6,
		"ammoclass": "CLASS_40MMNADE"},
	{"weapon": "WPN_DRAGUNOV", "category": 3, "rank": 40, "teamfilter": ["red"],
		"charfilter": ["sniper"],
		"loadout_selectable": 1, "loadout_subclasses": 0, "maxclips": 5, "clipweight": 1,
		"weaponweight": 9.4, "loadout_menu_icon": "M_SVD.tga",
		"loadout_menu_textid": "WEAP_SHORT_DRAGUNOV", "weapon_class": "primary",
		"round_type": "AMMO_DRAGUNOV_mm", "clipsize": 10, "startrounds": 50,
		"ammoclass": "CLASS_DRAGUNOV"},
	{"weapon": "WPN_GRENADEFB", "category": 4, "rank": 0, "teamfilter": ["red", "blue"],
		"charfilter": ["medic", "gunner", "rifleman", "engineer"],
		"loadout_selectable": 1, "loadout_subclasses": 0, "maxclips": 3, "clipweight": 1.1,
		"weaponweight": 0, "loadout_menu_icon": "M_flash.tga",
		"loadout_menu_textid": "WEAP_SHORT_GRENADE_FLASH", "weapon_class": "grenade",
		"round_type": "GRENADEFB", "clipsize": 1, "startrounds": 3,
		"ammoclass": "CLASS_GRENADEFB"},
	{"weapon": "WPN_GRENADEHE", "category": 5, "rank": 0, "teamfilter": ["red", "blue"],
		"charfilter": ["medic", "sniper", "gunner", "rifleman", "engineer"],
		"loadout_selectable": 1, "loadout_subclasses": 0, "maxclips": 3, "clipweight": 1.1,
		"weaponweight": 0, "loadout_menu_icon": "M_frag.tga",
		"loadout_menu_textid": "WEAP_SHORT_GRENADE_FRAG", "weapon_class": "grenade",
		"round_type": "GRENADEHE", "clipsize": 1, "startrounds": 3,
		"ammoclass": "CLASS_GRENADEHE"},
	{"weapon": "WPN_GRENADESM", "category": 6, "rank": 0, "teamfilter": ["red", "blue"],
		"charfilter": ["medic", "pilot", "rifleman", "engineer"],
		"loadout_selectable": 1, "loadout_subclasses": 0, "maxclips": 3, "clipweight": 1.1,
		"weaponweight": 0, "loadout_menu_icon": "M_smoke.tga",
		"loadout_menu_textid": "WEAP_SHORT_GRENADE_SMOKE", "weapon_class": "grenade",
		"round_type": "GRENADESM", "clipsize": 1, "startrounds": 3,
		"ammoclass": "CLASS_GRENADESM"},
	{"weapon": "WPN_MEDPACK", "category": 9, "rank": 0, "teamfilter": ["red", "blue"],
		"charfilter": ["medic"], "round_type": "medpack", "clipsize": -1, "startrounds": -1},
	{"weapon": "WPN_SATCHEL_CHARGE", "category": 7, "rank": 0, "teamfilter": ["red", "blue"],
		"charfilter": ["engineer", "rifleman"],
		"loadout_selectable": 1, "loadout_subclasses": 1, "maxclips": 2, "clipweight": 17.6,
		"weaponweight": 0, "loadout_menu_icon": "M_satchl.tga",
		"loadout_menu_textid": "WEAP_SHORT_SATCHEL", "weapon_class": "accessory",
		"round_type": "satchel", "clipsize": 1, "startrounds": 2,
		"ammoclass": "CLASS_SATCHEL"},
	{"weapon": "WPN_SATCHEL_DETONATOR", "category": 8, "rank": 0,
		"teamfilter": ["red", "blue"], "charfilter": ["rifleman", "engineer"],
		"round_type": "AMMO_DETONATOR", "clipsize": -1, "startrounds": -1},
]


## LOADOUT_ROWS as weapon.def text: the ammo classes' carry caps, then one block per
## row with its fields in the order authored (a list field repeats its keyword).
static func loadout_catalog_text() -> String:
	var classes := PackedStringArray()
	var blocks := ""
	for row: Dictionary in LOADOUT_ROWS:
		blocks += 'weapon "%s"\n' % row["weapon"]
		for key: String in row:
			if key == "weapon":
				continue
			var value = row[key]
			if value is Array:
				for item in value:
					blocks += " %s %s\n" % [key, item]
			elif key == "round_type":
				blocks += ' round_type "%s"\n' % value
			elif key == "ammoclass":
				blocks += " ammoclass %s 1\n" % value
				if not classes.has(value):
					classes.append(value)
			else:
				blocks += " %s %s\n" % [key, value]
		blocks += "end\n"
	var header := ""
	for ammo_class in classes:
		header += "ammoclass_max_carry %s 1000\n" % ammo_class
	return header + blocks


## A flat def root (DefFixture.stage_catalog) whose weapon.def is the loadout
## catalog; returns its directory.
static func stage_loadout_catalog(name: String) -> String:
	return DefFixture.stage_catalog(name, loadout_catalog_text())


## Copy each of the authored files named in `targets` into `res_dir`.
static func stage(test: GutTest, res_dir: String, targets: PackedStringArray) -> void:
	var dir := ProjectSettings.globalize_path(res_dir)
	test.assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	for name in targets:
		test.assert_eq(DirAccess.copy_absolute(
				directory().path_join(name), dir.path_join(name)), OK)
