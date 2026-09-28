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
		body += MenuDriverFixture.wnd("combo", name, y)
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


## Copy each of the authored files named in `targets` into `res_dir`.
static func stage(test: GutTest, res_dir: String, targets: PackedStringArray) -> void:
	var dir := ProjectSettings.globalize_path(res_dir)
	test.assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	for name in targets:
		test.assert_eq(DirAccess.copy_absolute(
				directory().path_join(name), dir.path_join(name)), OK)
