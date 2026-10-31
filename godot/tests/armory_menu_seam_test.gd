extends GutTest

# The WEAPON screen's device half over a framed MenuDriver: ArmoryMenuCompanion's slot icon
# mounts, which follow the engine controller's icons (ArmoryScreen over engine
# menu::ArmoryScreen). The screen's behavior (the on-show, the lists, the class pages, the
# weight, ACCEPT and CANCEL) is the engine's and pinned by the ctest armory_screen; the open
# over live play by armory_presenter_test. Also the loadout table loads PLAYER_INFO shares
# (LoadoutWeaponTable): a present-but-empty and a missing weapon.def are a loaded empty table.


func _load_weapons() -> WeaponDatabase:
	var wdb := WeaponDatabase.new()
	var path := ArmoryFixture.directory().path_join("weapon.def")
	assert_eq(wdb.load(path), OK, "the authored weapon.def loads")
	return wdb


# A framed synthetic WEAPON screen (ArmoryFixture's controls) the companion's mounts land on.
func _framed_driver() -> MenuDriver:
	var doc := MenuDriverFixture.doc_from_xml(self, ArmoryFixture.menu_text())
	var driver := MenuDriver.new()
	var frame := MenuFrame.new()
	frame.size = Vector2(800, 600)
	add_child_autofree(frame)
	driver.attach(frame, null)
	assert_true(driver.open_document(doc, null, null, "weapon.mnu", "WEAPON"),
			"the framed WEAPON document opens on the driver")
	return driver


# UI_UpdateWeaponWeightDisplay textures the blank *_ICON windows from the selected weapon.def
# row's loadout_menu_icon; NONE clears it (the engine controller's icons_changed): the
# companion mounts a TextureRect over PRIMARY_ICON and follows each refresh.
func test_selected_primary_mounts_and_clears_its_weapon_icon() -> void:
	var temp_dir := OS.get_temp_dir().path_join(
			"opennova_armory_icon_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(temp_dir), OK)
	TestFs.write_bytes(self, temp_dir.path_join("M_4.tga"), TestFs.tga_bytes(Vector2i(2, 2)))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(temp_dir), OK)

	var companion := ArmoryMenuCompanion.new()
	companion.screen.set_weapon_database(_load_weapons())
	var driver := _framed_driver()
	companion.mount(driver, root)
	assert_true(driver.claim_screen(companion.screen), "the screen claims the open document")
	var primary := driver.widget_id("PRIMARY")
	var m4_row := Array(driver.get_widget_items(primary)).find("Authored rifle label")
	if m4_row < 0:
		m4_row = Array(driver.get_widget_items(primary)).find("WPN_M4AUTO")
	assert_gt(m4_row, 0, "PRIMARY lists the M4")
	driver.select_row(primary, m4_row)

	var icon := driver.get_frame().find_child("PRIMARYArmoryIcon", true, false) as TextureRect
	assert_not_null(icon, "the armory mounts a texture over PRIMARY_ICON")
	if icon != null:
		assert_not_null(icon.texture, "the selected M4 displays its loadout_menu_icon texture")
		assert_eq(icon.position,
				driver.widget_frame_rect(driver.widget_id("PRIMARY_ICON")).position)
		driver.select_row(primary, 0)
		assert_null(icon.texture, "the NONE row clears the weapon preview texture")
	driver.release_screen()
	companion.on_menu_released()
	assert_null(driver.get_frame().find_child("PRIMARYArmoryIcon", true, false),
			"the release frees the mounts")

	DirAccess.remove_absolute(temp_dir.path_join("M_4.tga"))
	DirAccess.remove_absolute(temp_dir)


# The DEATH screen's icon mounts ride that screen's frame, which its presenter frees at each
# mission's teardown while the companion lives on: a mount freed under the companion is mounted
# afresh by the next refresh, its texture the selected weapon's (the controller's selection on
# that screen's driver).
func test_screen_icon_remounts_after_its_mount_is_freed() -> void:
	var temp_dir := OS.get_temp_dir().path_join(
			"opennova_armory_screen_icon_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(temp_dir), OK)
	TestFs.write_bytes(self, temp_dir.path_join("M_4.tga"), TestFs.tga_bytes(Vector2i(2, 2)))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(temp_dir), OK)

	var companion := ArmoryMenuCompanion.new()
	companion.screen.set_weapon_database(_load_weapons())
	var driver := _framed_driver()
	assert_true(driver.claim_screen(companion.screen), "the screen claims the open document")
	var primary := driver.widget_id("PRIMARY")
	var m4_row := Array(driver.get_widget_items(primary)).find("Authored rifle label")
	if m4_row < 0:
		m4_row = Array(driver.get_widget_items(primary)).find("WPN_M4AUTO")
	assert_gt(m4_row, 0, "PRIMARY lists the M4")
	driver.select_row(primary, m4_row)
	companion.refresh_screen_icons(driver, root)
	var frame := driver.get_frame()
	var icon := frame.find_child("PRIMARYDeathIcon", true, false) as TextureRect
	assert_not_null(icon, "the screen's refresh mounts a texture over PRIMARY_ICON")
	if icon != null:
		icon.free()
	companion.refresh_screen_icons(driver, root)
	var again := frame.find_child("PRIMARYDeathIcon", true, false) as TextureRect
	assert_not_null(again, "the refresh past the freed mount mounts a fresh one")
	if again != null:
		assert_not_null(again.texture, "the fresh mount shows the selected M4's icon")
	# The release frees and forgets the mounts; the next refresh mounts fresh ones again.
	companion.release_screen_icon_mounts()
	if again != null:
		assert_true(again.is_queued_for_deletion(), "the release frees the screen's mounts")
	driver.release_screen()

	DirAccess.remove_absolute(temp_dir.path_join("M_4.tga"))
	DirAccess.remove_absolute(temp_dir)


# A present weapon.def with no weapon rows is a LOADED, empty table, never "not loaded": the
# game builds its loadout catalog whatever the walk finds [orig: WeaponDef_LoadAll @0x54dd10
# seeds the "None" row and never reads the walk's result]. A comment-only file and a zero-byte
# one both load so.
func test_empty_weapon_def_is_a_loaded_empty_table() -> void:
	for body: PackedByteArray in [
			TestFs.crlf("// Weapon definitions\n\n").to_utf8_buffer(), PackedByteArray()]:
		var dir := TestFs.cache_dir(self, "armory_empty_weapon_def")
		TestFs.write_bytes(self, dir.path_join("weapon.def"), body)
		var root := ResourceRoot.new()
		assert_eq(root.set_root_dir(dir), OK)
		var weapons := LoadoutWeaponTable.load_weapon_database(root, "ArmoryMenuSeamTest",
				"the test fails")
		assert_not_null(weapons, "a %d-byte weapon.def loads" % body.size())
		if weapons != null:
			assert_true(weapons.is_loaded(), "an empty table is loaded")
			assert_eq(weapons.get_count(), 0, "with no weapon rows")
			assert_eq(weapons.get_last_error(), "", "and no error")
		TestFs.remove_dir_recursive(dir)


# A MISSING weapon.def is that same table, silently: the catalog is built and its "None" row
# seeded before the walk, a missing file ends the walk before any line with nothing logged,
# and no caller reads the result [orig: WeaponDef_LoadAll @0x54dd10; File_ParseASCIIFile
# @0x53d823; Game_InitSubsystems @0x4a70a6] (D-MNU-27).
func test_missing_weapon_def_is_a_loaded_empty_table() -> void:
	var dir := TestFs.cache_dir(self, "armory_missing_weapon_def")
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var weapons := LoadoutWeaponTable.load_weapon_database(root, "ArmoryMenuSeamTest",
			"the test fails")
	assert_not_null(weapons, "no weapon.def in the root still gives a table")
	if weapons != null:
		assert_true(weapons.is_loaded(), "the table is loaded")
		assert_eq(weapons.get_count(), 0, "with no weapon rows")
		assert_eq(weapons.get_last_error(), "", "and no error")
	TestFs.remove_dir_recursive(dir)
