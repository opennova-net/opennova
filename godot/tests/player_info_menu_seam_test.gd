extends GutTest

# The player.mnu PLAYER_INFO screen seam: the companion (player_info_menu_companion.gd)
# drives the JO character screen by control NAME -- fills NATIONALITY / DIVISION /
# COMBO_LIST / PLAYERVOICE from Avatars.def, runs the nationality->division->combo
# cascade, and re-filters by the SIDE_BLUE/SIDE_RED team -- riding a MenuDriver over
# a real MnuDocument (the compiled-menu surface; the Control tree is gone). This pins
# the wiring against the witnessed original (docs/playerinfo/avatars-re.md,
# D-PLAYERINFO-5/7); the live in-engine render is the manual smoke. The table is the minted
# synth_avatars.def fixture (tests/fixtures/minimal_avatars_gen.cpp), injected directly
# (root-less) like the avatar data tests.

const AVATARS_FIXTURE := "res://../fixtures/avatars/synth_avatars.def"


# Strings is a shared autoload; keep the registry clean so the raw-key tests below see
# no gametext table and the resolved-names test starts from a known state.
func before_each() -> void:
	Strings.clear()


func after_all() -> void:
	Strings.clear()


func _load_db() -> AvatarDatabase:
	var db := AvatarDatabase.new()
	var path := ProjectSettings.globalize_path(AVATARS_FIXTURE)
	assert_eq(db.load(path), OK, "the minted avatar table loads")
	return db


# A resource root whose Avatars.def is the minted table: the companion reads the
# table under the name the engine binds [orig: CAvatarDefs_Init @ 0x57b180], so the
# fixture is staged under that name in the cache dir (ResourceRoot.set_root_dir
# rejects user://). Null when the fixture cannot be staged.
func _staged_avatars_root() -> ResourceRoot:
	var bytes := FileAccess.get_file_as_bytes(AVATARS_FIXTURE)
	if bytes.is_empty():
		return null
	var dir := OS.get_cache_dir().path_join("opennova_player_info_seam_test")
	DirAccess.make_dir_recursive_absolute(dir)
	var f := FileAccess.open(dir.path_join("Avatars.def"), FileAccess.WRITE)
	if f == null:
		return null
	f.store_buffer(bytes)
	f.close()
	var root := ResourceRoot.new()
	return root if root.set_root_dir(dir) == OK else null


# --- Driver harness (the compiled-menu seam) ----------------------------------

# A synthetic PLAYER_INFO screen: the cascade combos, the team radios (SIDE_BLUE
# ships CHECKED, both share the authored GROUP), the name edit and ACCEPT --
# exactly the control names player.mnu authors.
func _avatar_screen_xml(include_preview := false) -> String:
	var body := ""
	var y := 10
	for n in ["NATIONALITY", "DIVISION", "COMBO_LIST", "PLAYERVOICE"]:
		body += MenuDriverFixture.wnd("combo", n, y)
		y += 24
	body += MenuDriverFixture.wnd("radio", "SIDE_BLUE", y, "<GROUP>1</GROUP>", " CHECKED")
	body += MenuDriverFixture.wnd("radio", "SIDE_RED", y + 24, "<GROUP>1</GROUP>")
	body += MenuDriverFixture.wnd("edit", "PLAYERNAME", y + 48)
	body += MenuDriverFixture.wnd("button", "ACCEPT", y + 72)
	body += MenuDriverFixture.wnd("button", "TESTPLAYERVOICE", y + 96)
	if include_preview:
		body += ('<WINDOW type="window" name="PLAYER_PREVIEW">'
				+ '<POSITION><LEFT>500</LEFT><TOP>100</TOP><RIGHT>700</RIGHT>'
				+ '<BOTTOM>400</BOTTOM></POSITION></WINDOW>')
	return MenuDriverFixture.screen_xml("PLAYER_INFO", body)


func _make_avatar_driver(include_preview := false) -> MenuDriver:
	return MenuDriverFixture.driver_over(self, MenuDriverFixture.doc_from_xml(self, _avatar_screen_xml(include_preview)), "player.mnu")


func _character_ids(profile: CharacterJoinProfile) -> Array[int]:
	return [profile.get_character_id(0), profile.get_character_id(1)]


func _player_classes(profile: CharacterJoinProfile) -> Array[int]:
	return [profile.get_player_class(0), profile.get_player_class(1)]


func test_join_auth_profile_uses_retail_avatar_packing_and_defaults() -> void:
	var db := _load_db()
	var profile := db.character_join_profile()

	# The first combo of each alignment in table order (N00 D00 combo 1; N04 D00
	# combo 1). The shipped table yields 0x8207 there (its first evil entry is N07).
	assert_eq(_character_ids(profile), [0x0200, 0x8204],
			"fresh profile selects the first good/evil avatar-table entries")
	assert_eq(_player_classes(profile), [8, 8],
			"fresh retail profile is rifleman on both sides")
	# SYN_HEAD_BOONIE (N00 D00 combo 1) carries voice 1; N04 D00 combo 1 wears
	# SYN_HEAD_11, voice 3 (tests/fixtures/minimal_avatars_gen.cpp).
	assert_eq([profile.get_avatar(0), profile.get_avatar(1)], [1, 3],
			"zero voice overrides resolve through each selected combo's head voice")
	assert_eq(profile.team_request, -1,
			"fresh profile asks the companion to assign a side")


func test_join_auth_profile_packs_the_selected_character_for_its_side() -> void:
	var db := _load_db()
	# The ACCEPT snapshot shape: the chosen side's tree indices in its
	# side_profiles slot, class 6 stamped on both sides (retail's two-block
	# class loop), the other side untouched (empty = retail default).
	var selected := {
		"team": 0,
		"player_class": 6,
		"side_profiles": [
			{"team": 0, "nationality": 0, "division": 0, "combo": 1,
					"player_class": 6},
			{"player_class": 6},
		],
	}
	var profile := db.character_join_profile(selected)
	var combo := db.get_combo(0, 0, 1)

	assert_eq(profile.get_character_id(0), 0x0400,
			"nat 0 / div 0 / combo id 2 packs into bits 0..14")
	assert_eq(profile.get_character_id(1), 0x8204,
			"choosing side A does not erase side B's profile selection")
	assert_eq(profile.get_avatar(0), combo.get_head().voice,
			"the selected combo supplies its retail avatar byte")
	assert_eq(_player_classes(profile), [6, 6],
			"retail commits the chosen class to both side blocks")


func test_join_auth_profile_carries_both_persisted_side_characters() -> void:
	var db := _load_db()
	var blue := db.resolve_character_id(0x0400, 0)
	var red := db.resolve_character_id(0x8407, 1)
	assert_not_null(blue)
	assert_not_null(red)
	var selected := {
		"team": 0,
		"side_profiles": [
			{
				"team": 0,
				"nationality": blue.nationality_index,
				"division": blue.division_index,
				"combo": blue.combo_index,
				"player_class": 5,
			},
			{
				"team": 1,
				"nationality": red.nationality_index,
				"division": red.division_index,
				"combo": red.combo_index,
				"player_class": 9,
			},
		],
	}
	var profile := db.character_join_profile(selected)
	assert_eq(_character_ids(profile), [0x0400, 0x8407],
			"assignment to either team receives that side's persisted character")
	assert_eq(_player_classes(profile), [5, 9],
			"an untouched loaded profile retains its two retail class bytes")


func test_owns_menu_detects_player_info() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	assert_true(companion.owns_menu(_make_avatar_driver()),
			"a menu carrying NATIONALITY + COMBO_LIST is the PLAYER_INFO screen")
	var plain := MenuDriverFixture.driver_over(self, MenuDriverFixture.doc_from_xml(self, MenuDriverFixture.screen_xml("PLAIN",
			MenuDriverFixture.wnd("button", "OK", 10))), "plain.mnu")
	assert_false(companion.owns_menu(plain),
			"a plain menu is left to the shell / other companions")


func test_populates_avatar_lists_and_combo_label() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var db := _load_db()
	companion.set_database(db)  # inject directly (no resource root in the unit)
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)

	assert_gt(driver.item_count(driver.widget_id("NATIONALITY")), 0, "nationalities populate")
	# The initial team-0 cascade selects the first good nationality (N00, index 0) and
	# its first division (D00): division + combo lists fill from the avatar tree.
	assert_eq(driver.item_count(driver.widget_id("DIVISION")), 4, "N00 has 4 divisions")
	var combos := driver.widget_id("COMBO_LIST")
	assert_eq(driver.item_count(combos), 4, "division D00 has 4 combos")

	# Each combo row is "<head display> - <body display>". With no gametext table registered
	# (before_each cleared Strings) the names fall back to their raw keys.
	var c0 := db.get_combo(0, 0, 0)
	var expected := "%s - %s" % [c0.get_head().display_name, c0.get_body().display_name]
	assert_eq(driver.item_text(combos, 0), expected, "combo row is the last - first character label")


# With the gametext table's "Avatars" section registered (as the shell does from Game.bin at
# boot), the companion resolves nationality + combo display keys to friendly names instead of the
# raw AV_* keys [orig: GameText_GetStringWithFallback @ 0x51eb90, "Avatars" section].
func test_resolves_friendly_names_from_gametext_avatars_section() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var db := _load_db()
	companion.set_database(db)

	# Map the exact keys this test asserts on to friendly text in a synthetic Avatars table.
	var t := RtxtStringFile.new()
	t.add_section("Avatars")
	var nat0 := db.get_nationality(0).name_key
	t.add_entry(nat0, "United States", 0, Vector2i())
	var c0 := db.get_combo(0, 0, 0)
	var head_key := c0.get_head().display_name
	var body_key := c0.get_body().display_name
	t.add_entry(head_key, "Boonie Hat", 0, Vector2i())
	if body_key != head_key:
		t.add_entry(body_key, "Camo BDU", 0, Vector2i())
	Strings.register_table("gameui", t)

	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)

	assert_eq(driver.item_text(driver.widget_id("NATIONALITY"), 0), "United States",
		"nationality resolves via the gametext Avatars section")
	var expected_combo := "Boonie Hat - %s" % ("Boonie Hat" if body_key == head_key else "Camo BDU")
	assert_eq(driver.item_text(driver.widget_id("COMBO_LIST"), 0), expected_combo,
		"combo label resolves head/body display names via Avatars")


func test_division_change_refills_combos() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var db := _load_db()
	companion.set_database(db)
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)

	# Selecting US division 1 refills the combo list with that division's combos
	# (cascade). select_row emits the "combo" value change — the user-pick relay.
	var div1_count := db.get_combo_count(0, 1)
	driver.select_row(driver.widget_id("DIVISION"), 1)
	assert_eq(driver.item_count(driver.widget_id("COMBO_LIST")), div1_count,
		"a division change refills COMBO_LIST")


func test_team_filter_partitions_nationalities_by_alignment() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var db := _load_db()
	companion.set_database(db)
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)

	# Team 0 (blue/SIDE_BLUE default): every shown nationality is good-aligned.
	var good_rows: Array[int] = companion.nationality_rows()
	assert_gt(good_rows.size(), 0, "at least one good nationality")
	for i in good_rows:
		assert_eq(db.get_nationality(i).alignment, AvatarDatabase.ALIGN_GOOD,
			"team 0 shows only good-aligned nationalities")

	# Switching to SIDE_RED (team 1) re-filters to evil-aligned nationalities.
	# Mirror what a real click does: the driver flips checked with group
	# exclusivity through its own click path, then emits the activation by NAME.
	var side_red := driver.widget_id("SIDE_RED")
	driver.set_widget_checked(side_red, true)
	driver.set_widget_checked(driver.widget_id("SIDE_BLUE"), false)
	driver.widget_activated.emit(side_red, "SIDE_RED")
	for i in companion.nationality_rows():
		assert_eq(db.get_nationality(i).alignment, AvatarDatabase.ALIGN_EVIL,
			"team 1 shows only evil-aligned nationalities")

	# The two teams partition every nationality (good->blue, evil->red; D-PLAYERINFO-5).
	assert_eq(good_rows.size() + companion.nationality_rows().size(), db.get_nationality_count(),
		"good + evil = all nationalities")


func test_initial_team_follows_checked_side_radio() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	var driver := _make_avatar_driver()
	driver.set_widget_checked(driver.widget_id("SIDE_RED"), true)
	driver.set_widget_checked(driver.widget_id("SIDE_BLUE"), false)
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	assert_eq(companion.team(), 1, "the initial team follows the checked SIDE_RED radio")


func test_degrades_without_avatar_db() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var driver := _make_avatar_driver()
	# No resource root and no injected db: the screen wires up but the combos stay empty.
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	assert_eq(driver.item_count(driver.widget_id("NATIONALITY")), 0,
			"no Avatars.def -> empty combos, no crash")


func test_mounts_3d_preview_when_widget_present() -> void:
	# The preview mounts as a frame child placed by widget_frame_rect, so this
	# seam needs a real MenuFrame attached (the geometry half of the driver).
	var frame := MenuFrame.new()
	frame.size = Vector2(800, 600)
	add_child_autofree(frame)
	var driver := MenuDriver.new()
	driver.attach(frame, null)
	assert_true(driver.open_document(MenuDriverFixture.doc_from_xml(self, _avatar_screen_xml(true)),
			null, null, null, "player.mnu"), "the preview document opens on the driver")
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	# No resource root, so the preview mounts but loads no .3di (graceful); we only
	# assert the surface is wired over the PLAYER_PREVIEW widget rect.
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	var preview := frame.find_child("PlayerInfoAvatarPreview", true, false) as Control
	assert_not_null(preview, "the 3D character preview is mounted under the frame")
	if preview != null:
		assert_true(preview.visible and preview.size.x > 0.0,
			"the mount is placed over the PLAYER_PREVIEW widget_frame_rect")
		# The three character dropdowns author their LIST_BOX rows over this very
		# rect: the mount and every Control under it must stay mouse-transparent
		# after _ready(), or real clicks on the rows die in the mount instead of
		# reaching the frame pump.
		assert_eq(preview.mouse_filter, Control.MOUSE_FILTER_IGNORE,
			"the menu portrait never intercepts the frame's mouse")
		for child in preview.find_children("*", "Control", true, false):
			assert_eq((child as Control).mouse_filter, Control.MOUSE_FILTER_IGNORE,
				"%s under the portrait is mouse-transparent too" % child.name)


func test_preview_render_activity_follows_visibility() -> void:
	# A hidden portrait must not keep paying for a 3D pass or its animation
	# (the reflection-viewport rule): UPDATE_ALWAYS and _process follow
	# is_visible_in_tree(), covering both a hidden widget rect and the whole
	# shell hiding for a mission.
	var frame := MenuFrame.new()
	frame.size = Vector2(800, 600)
	add_child_autofree(frame)
	var driver := MenuDriver.new()
	driver.attach(frame, null)
	assert_true(driver.open_document(MenuDriverFixture.doc_from_xml(self, _avatar_screen_xml(true)),
			null, null, null, "player.mnu"), "the preview document opens on the driver")
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	var preview := frame.find_child("PlayerInfoAvatarPreview", true, false) as AvatarPreview
	assert_not_null(preview, "the preview is mounted")
	if preview == null:
		return
	assert_eq(preview.preview_viewport().render_target_update_mode,
			SubViewport.UPDATE_ALWAYS, "a visible portrait renders every frame")
	assert_true(preview.is_processing())
	frame.hide()
	assert_eq(preview.preview_viewport().render_target_update_mode,
			SubViewport.UPDATE_DISABLED,
			"hiding the frame stops the portrait's render and animation")
	assert_false(preview.is_processing())
	frame.show()
	assert_eq(preview.preview_viewport().render_target_update_mode,
			SubViewport.UPDATE_ALWAYS, "showing the frame restores rendering")
	assert_true(preview.is_processing())


func test_release_frees_the_preview_mount() -> void:
	# The mount survives document swaps by construction (it is a child of the
	# persistent MenuFrame); the shell's release call is its ONLY teardown when
	# another document takes the driver. Without it the portrait keeps
	# rendering, and a stale reposition can park it over the next document's
	# widgets (the options-menu leak).
	var frame := MenuFrame.new()
	frame.size = Vector2(800, 600)
	add_child_autofree(frame)
	var driver := MenuDriver.new()
	driver.attach(frame, null)
	assert_true(driver.open_document(MenuDriverFixture.doc_from_xml(self, _avatar_screen_xml(true)),
			null, null, null, "player.mnu"), "the preview document opens on the driver")
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	assert_not_null(frame.find_child("PlayerInfoAvatarPreview", true, false),
			"the preview is mounted while the companion owns the document")
	companion.on_menu_released()
	await get_tree().process_frame  # queue_free drains
	assert_null(frame.find_child("PlayerInfoAvatarPreview", true, false),
			"releasing the companion frees the frame-child preview mount")


func test_snapshot_reports_current_selection() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	driver.set_widget_text(driver.widget_id("PLAYERNAME"), "Ghost")
	var snap := companion.snapshot()
	assert_eq(String(snap.get("name", "")), "Ghost", "snapshot carries the player name")
	assert_eq(int(snap.get("team", -1)), 0, "snapshot carries the team")
	assert_eq(int(snap.get("nationality", -1)), 0, "snapshot carries the selected nationality index")
	var sides: Array = snap.get("side_profiles", [])
	assert_eq(sides.size(), 2, "snapshot carries both retail side records")
	assert_eq(int((sides[0] as Dictionary).get("avatar_packed", -1)), 0x0200,
			"the active side stores the exact packed Avatars.def identity")
	assert_eq(int((sides[1] as Dictionary).get("avatar_packed", -1)), 0x8204,
			"the opposite side is initialized to the resolved default (its first combo)")


func test_persisted_side_profiles_restore_each_team_cascade() -> void:
	var db := _load_db()
	var blue := db.resolve_character_id(0x0400, 0)
	var red := db.resolve_character_id(0x8407, 1)
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_persisted_profile({
		"name": "Persistent",
		"team": 0,
		"side_profiles": [
			{
				"team": 0,
				"nationality": blue.nationality_index,
				"division": blue.division_index,
				"combo": blue.combo_index,
				"player_class": 8,
				"avatar_a": 0, "avatar_b": 0, "avatar_packed": 0x0400,
			},
			{
				"team": 1,
				"nationality": red.nationality_index,
				"division": red.division_index,
				"combo": red.combo_index,
				"player_class": 8,
				"avatar_a": 7, "avatar_b": 0, "avatar_packed": 0x8407,
			},
		],
	})
	var driver := _make_avatar_driver()
	var root := _staged_avatars_root()
	assert_not_null(root, "the minted table stages as the root's Avatars.def")
	if root == null:
		return
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", root)
	assert_eq(driver.selected_row(driver.widget_id("COMBO_LIST")),
			blue.combo_index)
	assert_eq(driver.get_widget_text(driver.widget_id("PLAYERNAME")), "Persistent")
	var red_radio := driver.widget_id("SIDE_RED")
	driver.set_widget_checked(red_radio, true)
	driver.set_widget_checked(driver.widget_id("SIDE_BLUE"), false)
	driver.widget_activated.emit(red_radio, "SIDE_RED")
	var red_snapshot := companion.snapshot()
	assert_eq(int(red_snapshot.get("nationality", -1)),
			red.nationality_index)
	assert_eq(int(red_snapshot.get("division", -1)),
			red.division_index)
	assert_eq(driver.selected_row(driver.widget_id("COMBO_LIST")),
			red.combo_index,
			"switching side restores that side's persisted combo")
	var saved_sides: Array = companion.snapshot().get("side_profiles", [])
	assert_eq(int((saved_sides[0] as Dictionary).get("avatar_packed", -1)), 0x0400)
	assert_eq(int((saved_sides[1] as Dictionary).get("avatar_packed", -1)), 0x8407)


func test_accept_emits_avatar_chosen() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	watch_signals(companion)
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	driver.set_widget_text(driver.widget_id("PLAYERNAME"), "Sandman")
	# The activation seam: the driver emits widget_activated(id, NAME) on the
	# click/hotkey edge and companions route by NAME.
	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	assert_signal_emitted(companion, "avatar_chosen", "OK commits the chosen avatar")
	var profile: Dictionary = get_signal_parameters(companion, "avatar_chosen")[0]
	assert_eq(String(profile.get("name", "")), "Sandman", "the committed profile carries the name")
	assert_eq(int(profile.get("nationality", -1)), 0, "the committed profile carries the selection")


func test_voice_preview_requests_selected_avatar_voice() -> void:
	var root := _staged_avatars_root()
	assert_not_null(root, "the minted table stages as the root's Avatars.def")
	if root == null:
		return

	var player_mnu := RetailData.fixture("mnu/jo_player.mnu")
	if player_mnu.is_empty():
		pending(RetailData.fixture_pending_text("mnu/jo_player.mnu"))
		return
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(FileAccess.get_file_as_bytes(player_mnu)), OK,
		"the retail player menu fixture loads")
	var driver := MenuDriver.new()
	assert_true(driver.open_document(doc, root, null, null, "player.mnu", "PLAYER_INFO"),
		"the retail player menu opens on the driver")

	var companion := PlayerInfoMenuCompanion.new()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", root)
	watch_signals(driver)
	assert_true(driver.has_widget("TESTPLAYERVOICE"),
		"the retail PLAYER_INFO screen authors its voice-preview button")
	driver.widget_activated.emit(driver.widget_id("TESTPLAYERVOICE"), "TESTPLAYERVOICE")

	# The table's initially selected N00/D00 head (SYN_HEAD_BOONIE) carries voice 1.
	# Retail formats that avatar-derived fallback as VOICE_1 and plays it from the
	# dedicated menu.lwf bank. [orig: PlayerInfo_PreviewVoice @ 0x55ff70]
	assert_signal_emitted_with_parameters(driver, "sound_requested", ["menu.lwf", "VOICE_1"])


# --- Loadout (PRIMARY/SECONDARY/ACCESSORY) ------------------------------------

# The shipped weapon.def from the reference fixture set; the loadout legs pend
# without it.
func _load_weapons() -> WeaponDatabase:
	var wdb := WeaponDatabase.new()
	assert_eq(wdb.load(RetailData.fixture("def/weapon.def")), OK, "the shipped weapon.def loads")
	return wdb


# A synthetic PLAYER_INFO loadout screen: the three weapon combos, PLAYERCLASS
# carrying the CHARTYPE values 5..9 as authored `value=` items (as the .mnu's
# static items do), the ammo/type/grenade combos, STATIC_TOTAL_WEIGHT, and the
# bare *_ICON windows.
func _loadout_screen_xml() -> String:
	var type_items := ('<ITEMS><ITEM value="0">FMJ</ITEM><ITEM value="1">AP</ITEM>'
			+ '<ITEM value="2">SP</ITEM></ITEMS>')
	var body := ""
	var y := 10
	for n in ["PRIMARY", "SECONDARY", "ACCESSORY"]:
		body += MenuDriverFixture.wnd("combo", n, y)
		body += MenuDriverFixture.wnd("combo", n + "_AMMO1", y + 24)
		body += MenuDriverFixture.wnd("combo", n + "_AMMO2", y + 48)
		body += MenuDriverFixture.wnd("window", n + "_ICON", y + 72)
		y += 100
	for n in ["PRIMARY", "SECONDARY"]:
		# player.mnu authors the TYPE statics (FMJ/AP/SP, values 0/1/2); the
		# companion selects/locks them but never refills.
		body += MenuDriverFixture.wnd("combo", n + "_AMMO1_TYPE", y, type_items)
		y += 24
	for n in ["GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"]:
		body += MenuDriverFixture.wnd("combo", n, y)
		y += 24
	body += MenuDriverFixture.wnd("static", "STATIC_TOTAL_WEIGHT", y)
	var class_items := ""
	for v in range(5, 10):  # Medic..Engineer = values 5..9
		class_items += '<ITEM value="%d">class %d</ITEM>' % [v, v]
	body += MenuDriverFixture.wnd("combo", "PLAYERCLASS", y + 24, "<ITEMS>%s</ITEMS>" % class_items)
	return MenuDriverFixture.screen_xml("PLAYER_INFO", body)


var _ammo_driver: MenuDriver = null  # the driver behind the current _make_ammo_companion


func _make_loadout_driver(with_frame := false) -> MenuDriver:
	var driver := MenuDriver.new()
	if with_frame:
		# The icon mounts are frame children placed by widget_frame_rect.
		var frame := MenuFrame.new()
		frame.size = Vector2(800, 600)
		add_child_autofree(frame)
		driver.attach(frame, null)
	assert_true(driver.open_document(MenuDriverFixture.doc_from_xml(self, _loadout_screen_xml()),
			null, null, null, "player.mnu"), "the loadout document opens on the driver")
	return driver


# A wired companion over the loadout screen (medic/blue), for the ammo cases —
# built through the public seams only (set_weapon_database + on_menu_built).
func _make_ammo_companion(wdb: WeaponDatabase = null, with_frame := false) -> PlayerInfoMenuCompanion:
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_weapon_database(wdb if wdb != null else _load_weapons())
	_ammo_driver = _make_loadout_driver(with_frame)
	companion.on_menu_built(_ammo_driver, "player.mnu", "PLAYER_INFO", null)
	return companion


func _ammo_id(name: String) -> int:
	return _ammo_driver.widget_id(name)


func _weight_text() -> String:
	return _ammo_driver.get_widget_text(_ammo_id("STATIC_TOTAL_WEIGHT"))


# Select the named weapon in a parent slot combo and fire the selection relay the
# way a user pick would (select_row emits the "combo" value change). The row model
# is public: row = position in the filtered slot list + 1 (row 0 is NONE)
# [orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430].
func _select_weapon(wdb: WeaponDatabase, control: String, slot: int,
		class_mask: int, weapon_name: String) -> WeaponDef:
	var combo := _ammo_id(control)
	var defs := wdb.get_slot_weapons(slot, class_mask, 2)  # blue
	for i in defs.size():
		var w := defs[i] as WeaponDef
		if w.name.nocasecmp_to(weapon_name) == 0:
			_ammo_driver.select_row(combo, i + 1)  # emits -> the witnessed refill
			return w
	assert_true(false, "%s offers %s" % [control, weapon_name])
	return null


# The expected *_AMMO2 sub-weapon, computed from the public table walk the companion
# mirrors [orig: the stricmp walk in PlayerInfo_PopulateAmmoComboBoxes @ 0x55def0].
func _expected_sub(wdb: WeaponDatabase, parent: WeaponDef) -> WeaponDef:
	var parent_round := parent.round_type
	for k in range(1, parent.loadout_subclasses + 1):
		var cand := wdb.get_weapon(parent.index + k)
		if cand == null:
			return null
		if cand.round_type.nocasecmp_to(parent_round) != 0:
			return cand
	return null


func _items(name: String) -> Array:
	return Array(_ammo_driver.get_widget_items(_ammo_id(name)))


func test_populates_loadout_slots_filtered_by_class_and_team() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var _companion := _make_ammo_companion(wdb)

	var primary := _ammo_id("PRIMARY")
	# Medic (class mask 1), blue (team mask 2): the DB's filtered set plus a leading NONE row.
	var expected := wdb.get_slot_weapons(WeaponDatabase.SLOT_PRIMARY, 1, 2)
	assert_gt(expected.size(), 0, "the fixture has medic/blue primary weapons")
	assert_eq(_ammo_driver.item_count(primary), expected.size() + 1, "PRIMARY = NONE + the filtered weapons")
	assert_eq(_ammo_driver.item_text(primary, 0), "None", "NONE leads the slot (fallback with no string table)")


func test_loadout_class_filter_includes_and_excludes() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var _companion := _make_ammo_companion(_load_weapons())

	# Medic (value 5): WPN_M4AUTO (charfilter medic|rifleman|engineer, blue) is a primary -> present.
	assert_true(_items("PRIMARY").has("WPN_M4AUTO"), "M4 shows for Medic")

	# Sniper (value 6): M4's charfilter excludes sniper -> absent after re-fill.
	# select_row emits the "combo" value change — the class-mask refilter relay.
	_ammo_driver.select_row(_ammo_id("PLAYERCLASS"), 1)
	assert_false(_items("PRIMARY").has("WPN_M4AUTO"), "M4 is hidden for Sniper")


func test_snapshot_carries_the_selected_loadout_weapon_ids() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var companion := _make_ammo_companion(wdb)

	var primary_defs := wdb.get_slot_weapons(
			WeaponDatabase.SLOT_PRIMARY, 1, 2)
	var selected_primary := ""
	var selected_row := -1
	for i in primary_defs.size():
		var weapon: WeaponDef = primary_defs[i]
		if weapon.name.nocasecmp_to("WPN_M4AUTO") == 0:
			selected_primary = weapon.name
			selected_row = i + 1 # row 0 is NONE
			break
	assert_false(selected_primary == null, "the fixture offers M4AUTO for medic/blue")
	_ammo_driver.select_row(_ammo_id("PRIMARY"), selected_row, false)  # silent reselect

	var profile := companion.snapshot()
	assert_eq(String(profile.get("primary", "")), selected_primary,
		"PLAYER_INFO ACCEPT preserves the selected primary's weapon.def id")
	assert_eq(int(profile.get("player_class", 0)), 5,
		"the selected class travels with the spawn loadout")


func test_snapshot_carries_class_without_a_weapon_database() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	_ammo_driver = _make_loadout_driver()
	companion.on_menu_built(_ammo_driver, "player.mnu", "PLAYER_INFO", null)

	var profile := companion.snapshot()
	assert_eq(int(profile.get("player_class", 0)), 5,
		"class selection does not depend on weapon.def loading")
	assert_false(profile.has("primary"),
		"missing weapon.def remains distinct from an explicit all-NONE kit")


# End-to-end against the REAL player.mnu (authored widget tree), so the loadout fills
# through the same control names/document the runtime opens, not just a stand-in. Runs
# in GUT so the autoloads (Strings) and the GDExtension are loaded.
func test_real_player_mnu_loadout_populates() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var player_mnu := RetailData.fixture("mnu/jo_player.mnu")
	if player_mnu.is_empty():
		pending(RetailData.fixture_pending_text("mnu/jo_player.mnu"))
		return
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(FileAccess.get_file_as_bytes(player_mnu)), OK,
			"the shipped jo_player.mnu fixture loads")
	var driver := MenuDriverFixture.driver_over(self, doc, "player.mnu", "PLAYER_INFO")

	assert_true(driver.has_widget("PRIMARY"), "the real player.mnu authors a PRIMARY combobox")
	var pclass := driver.widget_id("PLAYERCLASS")
	assert_gte(pclass, 0, "the real player.mnu authors a PLAYERCLASS combobox")
	assert_gt(driver.item_count(pclass), 0, "PLAYERCLASS carries its static class items")

	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	companion.set_weapon_database(_load_weapons())  # injected (root-less unit), as if weapon.def had loaded
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)

	assert_gt(driver.item_count(driver.widget_id("PRIMARY")), 1,
		"PRIMARY populates (NONE + weapons) through the real menu's authored tree")
	# The ammo/weight tail fills through the same document: the default NONE
	# selection hides PRIMARY_AMMO1, and the weight readout renders the witnessed
	# "%s %.1f %s (%s)" shape (fallback strings, no tables registered).
	var primary_ammo := driver.widget_id("PRIMARY_AMMO1")
	assert_gte(primary_ammo, 0, "the real player.mnu authors PRIMARY_AMMO1")
	assert_false(driver.is_widget_shown(primary_ammo), "NONE selected -> the ammo combo hides")
	var weight := driver.widget_id("STATIC_TOTAL_WEIGHT")
	assert_gte(weight, 0, "the real player.mnu authors STATIC_TOTAL_WEIGHT")
	# The built PLAYERCLASS carries a default selection, so default grenades may
	# already weigh in — pin the witnessed "%s %.1f %s (%s)" shape, not the sum.
	assert_true(driver.get_widget_text(weight).begins_with("Total Weight "),
		"the weight readout renders through the compiled static")
	assert_true(driver.get_widget_text(weight).contains(" lbs ("),
		"the readout carries the witnessed format tail")


# --- Ammo combos + weight + icons (D-PLAYERINFO-11) -----------------------------

func test_primary_ammo_rows_follow_selected_weapon() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	var w := _select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 1, "WPN_M4AUTO")
	var ammo := _ammo_id("PRIMARY_AMMO1")
	var maxclips := w.maxclips
	assert_true(_ammo_driver.is_widget_shown(ammo), "a clip-carrying weapon shows its ammo combo")
	assert_eq(_ammo_driver.item_count(ammo), maxclips,
		"rows 1..maxclips [orig: PlayerInfo_PopulateAmmoComboBoxes @ 0x55def0]")
	assert_eq(_ammo_driver.item_text(ammo, 0),
		"%d - %s" % [w.clipsize, w.round_type],
		"row labels are the witnessed \"%d - %s\" rounds + round type")
	assert_eq(_ammo_driver.selected_row(ammo), maxclips - 1,
		"the untouched default selects the full (maxclips) row")


func test_none_selection_hides_ammo_and_clears_icon() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	# The icon mounts are frame children, so this case runs with a real MenuFrame.
	var _presenter := _make_ammo_companion(wdb, true)
	_select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 1, "WPN_M4AUTO")
	_ammo_driver.select_row(_ammo_id("PRIMARY"), 0)  # back to NONE
	assert_false(_ammo_driver.is_widget_shown(_ammo_id("PRIMARY_AMMO1")), "NONE hides the ammo combo")
	var rect := _ammo_driver.get_frame().find_child("PRIMARYLoadoutIcon", true, false) as TextureRect
	assert_not_null(rect, "the companion mounts an icon rect over PRIMARY_ICON")
	if rect != null:
		assert_null(rect.texture, "NONE clears the icon texture")


func test_m203_subweapon_fills_ammo2_from_the_differing_round_entry() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	# The M203 carbines are rifleman-filtered; switch PLAYERCLASS to Rifleman
	# (value 8, row 3) so the slot list offers them.
	_ammo_driver.select_row(_ammo_id("PLAYERCLASS"), 3)
	var w := _select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 8, "WPN_M4M203AUTO")
	# The witnessed walk skips same-round WPN_M4M203 and lands on WPN_M4M203HE
	# (AMMO_M203_40MM_NADE) within loadout_subclasses = 2.
	var sub := _expected_sub(wdb, w)
	assert_eq(sub.name, "WPN_M4M203HE",
		"the sub walk lands on the first DIFFERING round_type entry")
	var ammo2 := _ammo_id("PRIMARY_AMMO2")
	assert_true(_ammo_driver.is_widget_shown(ammo2), "a live sub-weapon shows *_AMMO2")
	assert_eq(_ammo_driver.item_count(ammo2), sub.maxclips,
		"*_AMMO2 rows come from the SUB-weapon's maxclips")
	assert_eq(_ammo_driver.item_text(ammo2, 0),
		"%d - %s" % [sub.clipsize, sub.round_type],
		"*_AMMO2 labels use the sub-weapon's clipsize + round type")


func test_grenade_combos_fill_in_table_order_with_zero_row() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	var expected: Array = wdb.get_slot_weapons(
			WeaponDatabase.SLOT_GRENADE, 1, 2)  # medic/blue
	assert_gt(expected.size(), 0, "the fixture carries medic/blue grenades")
	for i in mini(expected.size(), 3):
		var combo := _ammo_id("GRENADE_AMMO%d" % (i + 1))
		var w := expected[i] as WeaponDef
		assert_true(_ammo_driver.is_widget_shown(combo), "an owned grenade control shows")
		assert_eq(_ammo_driver.item_count(combo), w.maxclips + 1,
			"grenade rows are 0..maxclips INCLUDING the zero row [orig: @ 0x55def0]")
		assert_eq(_ammo_driver.item_text(combo, 0), "0 - %s" % w.round_type,
			"row 0 is the zero-rounds row")
		assert_eq(_ammo_driver.selected_row(combo), w.maxclips,
			"the untouched default selects the full row")
	for i in range(expected.size(), 3):
		assert_false(_ammo_driver.is_widget_shown(_ammo_id("GRENADE_AMMO%d" % (i + 1))),
			"unowned grenade controls hide")


func test_weight_label_renders_witnessed_format_and_band() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	var w := _select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 1, "WPN_M4AUTO")
	# Expected parent term [orig: PlayerInfo_CalculateLoadoutWeight @ 0x55f1f0]:
	# weight + maxclips*clip_weight (untouched default), plus the sub-weapon and
	# full-grenade clip-only terms the fill selects by default.
	var expected := w.weight \
			+ w.maxclips * w.clip_weight
	var sub := _expected_sub(wdb, w)
	if sub != null and sub.clipsize > 0:
		expected += sub.maxclips * sub.clip_weight
	for g in wdb.get_slot_weapons(WeaponDatabase.SLOT_GRENADE, 1, 2).slice(0, 3):
		expected += g.maxclips * g.clip_weight
	var band := "Light"
	if expected >= 66.6:
		band = "Heavy"
	elif expected >= 33.3:
		band = "Normal"
	assert_eq(_weight_text(), "Total Weight %.1f lbs (%s)" % [expected, band],
		"the witnessed \"%s %.1f %s (%s)\" readout over the native weight math")
	assert_eq(wdb.encumbrance_class(0.0), WeaponDatabase.ENCUMBRANCE_LIGHT)
	assert_eq(wdb.encumbrance_class(33.3), WeaponDatabase.ENCUMBRANCE_NORMAL)
	assert_eq(wdb.encumbrance_class(66.6), WeaponDatabase.ENCUMBRANCE_HEAVY)


func test_ammo_selection_recomputes_weight_and_snapshot() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var companion := _make_ammo_companion(wdb)
	var w := _select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 1, "WPN_M4AUTO")
	assert_eq(companion.selected_clips("PRIMARY"), -1,
		"untouched ammo reports the -1 default [orig: the '-1' kit filler]")
	var before := _weight_text()
	_ammo_driver.select_row(_ammo_id("PRIMARY_AMMO1"), 0)  # one clip
	assert_eq(companion.selected_clips("PRIMARY"), 1,
		"the pick records row+1 [orig: @ 0x55f730]")
	assert_ne(_weight_text(), before, "an ammo pick recomputes the weight readout")
	var profile := companion.snapshot()
	assert_eq(int(profile.get("primary_clips", -99)), 1,
		"snapshot carries the recorded clip pick")
	assert_eq(int(profile.get("secondary_clips", -99)), -1,
		"untouched slots stay at the -1 default")
	assert_eq(int(profile.get("primary_ammo_type", -99)), 0,
		"the type byte rides the snapshot [orig: the kit tuple flags field]")
	# Icon side of the same selection: the M4's authored icon texture name is
	# non-empty, but with no resource root the rect stays cleared (root-less unit).
	assert_false(w.icon.is_empty(), "the fixture authors an icon")


func test_type_combo_locks_for_noammotypes_weapons() -> void:
	var def_text := """
weapon "WPN_PLAIN"
	loadout_selectable	1
	teamfilter	blue
	charfilter	medic
	weapon_class	primary
	round_type	"AMMO_TEST"
	clipsize	10
	maxclips	4
	clipweight	1
	weaponweight	2
end

weapon "WPN_LOCKED"
	loadout_selectable	1
	teamfilter	blue
	charfilter	medic
	weapon_class	primary
	round_type	"AMMO_TEST"
	clipsize	10
	maxclips	4
	clipweight	1
	weaponweight	2
	flags	noammotypes
end
"""
	var path := OS.get_cache_dir().path_join(
			"opennova_playerinfo_type_lock_%d.def" % Time.get_ticks_usec())
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_string(def_text)
	f.close()
	var wdb := WeaponDatabase.new()
	assert_eq(wdb.load(path), OK)
	var companion := _make_ammo_companion(wdb)
	var type_combo := _ammo_id("PRIMARY_AMMO1_TYPE")

	_select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 1, "WPN_PLAIN")
	assert_true(_ammo_driver.is_widget_shown(type_combo), "a clip weapon shows the TYPE combo")
	assert_false(_ammo_driver.is_widget_disabled(type_combo), "a normal weapon leaves the TYPE combo live")

	_select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 1, "WPN_LOCKED")
	assert_true(_ammo_driver.is_widget_disabled(type_combo),
		"flags2 NOAMMOTYPES locks the TYPE combo [orig: @ 0x55def0 +188 & 0x40]")
	assert_eq(_ammo_driver.selected_row(type_combo), 0, "the lock resets the type to 0 (FMJ)")
	assert_eq(companion.selected_ammo_type("PRIMARY"), 0, "the stored byte resets too")
	DirAccess.remove_absolute(path)


func test_grenade_zero_pick_stays_zero_in_the_weight() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# The witnessed asymmetry [orig: PlayerInfo_CalculateLoadoutWeight @ 0x55f1f0]:
	# grenades default -1 -> maxclips, but a PICKED 0 stays 0 (the zero row) —
	# unlike the parents' <=0 -> maxclips rule.
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	var g: WeaponDef = wdb.get_slot_weapons(WeaponDatabase.SLOT_GRENADE, 1, 2)[0]
	assert_gt(g.clip_weight * g.maxclips, 0.0,
		"the first grenade def carries weighable clips")
	var default_text := _weight_text()  # -1 default = full grenades weighed in
	_ammo_driver.select_row(_ammo_id("GRENADE_AMMO1"), 0)  # the zero row
	assert_ne(_weight_text(), default_text,
		"picking the zero row removes that grenade's clip term")
	var zero_total := float(_weight_text().get_slice(" ", 2))
	var default_total := float(default_text.get_slice(" ", 2))
	assert_almost_eq(default_total - zero_total,
		g.maxclips * g.clip_weight, 0.06,
		"the delta is exactly the grenade's maxclips*clip_weight term")


func test_weapon_dict_carries_loadout_subclasses() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var idx := wdb.find_weapon("WPN_M4M203AUTO")
	assert_gt(idx, 0)
	assert_eq(wdb.get_weapon(idx).loadout_subclasses, 2,
		"loadout_subclasses (+36) rides the transport dict")


# --- PLAYERVOICE (the voice-definition table walk) -----------------------------

# The list is DEFAULT_VOICE plus every ENABLED voice-table row whose sex matches
# the selected head's sex byte, not the head's own voice id. The synth table's
# N00/D00 combos are all male heads, so the male set (ids 1..6 and 10 -- id 9 is
# the table's one disabled row) shows; N00/D03's second combo is a female head
# and swaps the list to the female set (7, 8, 11).
# docs/playerinfo/avatars-re.md "PLAYERVOICE list" (the table at 0x83C7A8).
func test_voice_list_carries_every_row_matching_the_avatar_sex() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var db := _load_db()
	companion.set_database(db)
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)

	var voices := driver.widget_id("PLAYERVOICE")
	assert_eq(db.get_combo(0, 0, 0).get_head().sex, 0, "N00/D00 combo 0 is a male head")
	assert_eq(driver.item_count(voices), 8,
		"DEFAULT_VOICE + the seven enabled male rows (1..6, 10)")
	assert_eq(driver.item_text(voices, 0), "Default", "DEFAULT_VOICE leads the list")
	assert_eq(driver.item_text(voices, 7), "Voice 10",
		"the disabled id-9 row is skipped, so 10 is the last male row")

	# The female head's division swaps the whole set.
	driver.select_row(driver.widget_id("DIVISION"), 3)
	driver.select_row(driver.widget_id("COMBO_LIST"), 1)
	assert_eq(db.get_combo(0, 3, 1).get_head().sex, 1, "N00/D03 combo 1 is a female head")
	assert_eq(driver.item_count(voices), 4, "DEFAULT_VOICE + the three female rows")
	assert_eq(driver.item_text(voices, 1), "Voice 7")
	assert_eq(driver.item_text(voices, 3), "Voice 11")


# The picked row's VALUE is the persisted override, and a populate whose rows no
# longer offer that value resets it to DEFAULT_VOICE before selecting by value
# (retail's "if (!found) profile[team + 1532] = 0").
func test_voice_override_persists_and_resets_when_the_avatar_drops_it() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	var voices := driver.widget_id("PLAYERVOICE")

	assert_eq(companion.selected_voice(), 0, "a fresh profile starts on DEFAULT_VOICE")
	driver.select_row(voices, 3)  # row 0 is DEFAULT_VOICE, so row 3 is "Voice 3"
	assert_eq(companion.selected_voice(), 3, "the pick stores the row VALUE, not the row")
	assert_eq(int(companion.snapshot().get("voice", -1)), 3,
		"ACCEPT persists the voice value")

	# Voice 3 is a male row; moving to the female head drops it from the list.
	driver.select_row(driver.widget_id("DIVISION"), 3)
	driver.select_row(driver.widget_id("COMBO_LIST"), 1)
	assert_eq(companion.selected_voice(), 0,
		"an override the new avatar cannot offer resets to DEFAULT_VOICE")
	assert_eq(driver.selected_row(voices), 0, "and the list selects that value's row")


# A persisted override the avatar still offers survives the build and selects its
# own row BY VALUE. Id 10 proves the distinction: it is the LAST male row
# (index 7) because the disabled id-9 row never enters the list.
func test_persisted_voice_selects_its_row_by_value() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	companion.set_persisted_profile({"voice": 10})
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)

	assert_eq(companion.selected_voice(), 10, "the persisted override survives the build")
	var voices := driver.widget_id("PLAYERVOICE")
	assert_eq(driver.item_text(voices, driver.selected_row(voices)), "Voice 10",
		"the list selects by value: id 10 is the LAST male row, not row 10")


# The preview prefers the persisted override; only a zero override falls back to
# the selected head's own voice byte (the synth head carries voice 1).
func test_voice_preview_prefers_the_persisted_override() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	companion.set_persisted_profile({"voice": 6})
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	watch_signals(driver)

	var button := driver.widget_id("TESTPLAYERVOICE")
	driver.widget_activated.emit(button, "TESTPLAYERVOICE")
	assert_signal_emitted_with_parameters(driver, "sound_requested",
		["menu.lwf", "VOICE_6"], 0)

	# Clearing the override hands the preview back to the avatar's own voice.
	driver.select_row(driver.widget_id("PLAYERVOICE"), 0)
	driver.widget_activated.emit(button, "TESTPLAYERVOICE")
	assert_signal_emitted_with_parameters(driver, "sound_requested",
		["menu.lwf", "VOICE_1"], 1)


# --- Kit page serialization (D-PLAYERINFO-9) ----------------------------------

# The saved kit page is the witnessed entry ORDER, not just the three loadout
# slots: the side's knife leads, the medic adds a medpack, then
# PRIMARY/SECONDARY/ACCESSORY, then ALWAYS three grenade entries.
# docs/playerinfo/avatars-re.md "Kit page serialization".
func test_kit_page_carries_knife_medpack_slots_and_three_grenades() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var companion := _make_ammo_companion(wdb)  # the screen opens on class 5 (medic)

	var kit: Array = companion.snapshot().get("kit", [])
	assert_eq(kit.size(), 8, "knife + medpack + three slots + three grenade slots")
	var knife: Dictionary = kit[0]
	assert_eq(String(knife.get("name", "")), "WPN_KNIFE",
		"the blue side's knife leads the page")
	for key in ["ammo_primary", "ammo_secondary", "flags"]:
		assert_eq(int(knife.get(key, 0)), -1,
			"the knife entry's three other values are the -1 filler")
	assert_eq(String((kit[1] as Dictionary).get("name", "")), "WPN_MEDPACK",
		"class 5 adds the medpack entry")

	# Slot index 2 = PRIMARY. Nothing picked yet, so retail's NONE row value (0)
	# serializes weapon-table entry 0.
	assert_eq(String((kit[2] as Dictionary).get("name", "")), wdb.get_weapon(0).name,
		"a NONE slot serializes weapon-table entry 0")

	var picked := _select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 1,
			"WPN_M4AUTO")
	_ammo_driver.select_row(_ammo_id("PRIMARY_AMMO1"), 0)  # clip pick stores row + 1
	kit = companion.snapshot().get("kit", [])
	var primary: Dictionary = kit[2]
	assert_eq(String(primary.get("name", "")), picked.name)
	assert_eq(int(primary.get("ammo_primary", 0)), 1,
		"the recorded clip pick rides the entry's second value")
	assert_eq(int(primary.get("flags", -99)), companion.selected_ammo_type("PRIMARY"),
		"PRIMARY's fourth value is its team ammo-type byte")
	assert_eq(int((kit[4] as Dictionary).get("flags", 0)), -1,
		"ACCESSORY's fourth value is always the filler")
	assert_eq(kit.size() - 5, 3, "exactly three grenade entries close the page")
	for i in 3:
		assert_false(String((kit[5 + i] as Dictionary).get("name", "")).is_empty(),
			"every grenade slot serializes a name, filled or not")


# Class 8 (rifleman) writes no medpack entry; the page then has seven entries.
func test_kit_page_medpack_is_class_five_only() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var companion := _make_ammo_companion(_load_weapons())
	_ammo_driver.select_row(_ammo_id("PLAYERCLASS"), 3)  # values 5..9 -> row 3 = class 8

	var kit: Array = companion.snapshot().get("kit", [])
	assert_eq(kit.size(), 7, "no medpack for a rifleman")
	assert_eq(String((kit[0] as Dictionary).get("name", "")), "WPN_KNIFE",
		"the blue side keeps WPN_KNIFE")
	for entry in kit:
		assert_ne(String((entry as Dictionary).get("name", "")), "WPN_MEDPACK")
