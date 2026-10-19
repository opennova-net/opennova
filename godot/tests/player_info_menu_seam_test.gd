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
# no gametext table and the resolved-names test starts from a known state. The
# process's profile (PlayerProfile.store(), which the screen reads and writes) starts
# each test as a fresh record (_reset_profile) and is left as the test leaves it.
func before_each() -> void:
	Strings.clear()
	_reset_profile()


func after_all() -> void:
	Strings.clear()


# Each side's first character in the minted table (N00/D00 and N04/D00, combo id 1).
const BLUE_CHARACTER := {"avatar_a": 0, "avatar_b": 0, "avatar_packed": 0x0200}
const RED_CHARACTER := {"avatar_a": 4, "avatar_b": 0, "avatar_packed": 0x8204}


# The process's profile as a fresh record, both sides on the medic class with
# each side's first character, the voice pair on DEFAULT_VOICE and the two medic
# pages the given kits: the screen reads its class and restores its loadout from
# the record (engine menu::player_info_restore_kit), so every test opens on the
# medic with empty pages unless it seeds them.
func _reset_profile(blue_kit: Array = [], red_kit: Array = []) -> void:
	var store := PlayerProfile.store()
	store.load_bytes(PackedByteArray(), false, PackedByteArray(), false, "")
	for side in 2:
		assert_eq(store.apply_character_selection({
			"team": side,
			"player_class": 5,
			"side_profiles": [BLUE_CHARACTER, RED_CHARACTER],
			"kit": blue_kit if side == 0 else red_kit,
		}), OK, "the side's medic page is seeded")
	store.set_voice(0, 0)
	store.set_voice(1, 0)


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
	return MenuDriverFixture.screen_xml("PLAYER_INFO", _avatar_body(include_preview))


func _avatar_body(include_preview := false) -> String:
	var body := ""
	var y := 10
	for n in ["NATIONALITY", "DIVISION", "COMBO_LIST", "PLAYERVOICE"]:
		body += MenuDriverFixture.wnd("combobox", n, y)
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
	return body


func _make_avatar_driver(include_preview := false) -> MenuDriver:
	return MenuDriverFixture.driver_over(self, MenuDriverFixture.doc_from_xml(self, _avatar_screen_xml(include_preview)), "player.mnu")


# The same screen over a real MenuFrame: the runtime's read-backs go through the
# frame's bytes (MenuFrame.get_widget_text), as the live screen's do.
func _make_framed_avatar_driver() -> MenuDriver:
	var frame := MenuFrame.new()
	add_child_autofree(frame)
	frame.size = Vector2(800, 600)
	var driver := MenuDriver.new()
	driver.attach(frame, null)
	assert_true(driver.open_document(MenuDriverFixture.doc_from_xml(self, _avatar_screen_xml()),
			null, null, null, "player.mnu"), "the document opens on the frame")
	return driver


func _character_ids(profile: CharacterJoinProfile) -> Array[int]:
	return [profile.get_character_id(0), profile.get_character_id(1)]


func _player_classes(profile: CharacterJoinProfile) -> Array[int]:
	return [profile.get_player_class(0), profile.get_player_class(1)]


func test_join_auth_profile_uses_retail_avatar_packing_and_defaults() -> void:
	var db := _load_db()
	var profile := db.character_join_profile({}, true)

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
	assert_false("team_request" in profile,
			"the profile carries no side request: every join sends TR=-1 (D-NET-590)")


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
	var profile := db.character_join_profile(selected, true)
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
	var profile := db.character_join_profile(selected, true)
	assert_eq(_character_ids(profile), [0x0400, 0x8407],
			"assignment to either team receives that side's persisted character")
	assert_eq(_player_classes(profile), [5, 9],
			"an untouched loaded profile retains its two retail class bytes")


# A saved class byte outside 5..9: a session's start clamps it (a 4 goes up as
# 5, a 200 as 9), single player's takes no class from the record and the
# projection keeps the default 8 (engine inmatch::join_character_profile
# carries the witness).
func test_join_auth_profile_clamps_a_class_outside_five_to_nine_in_a_session() -> void:
	var db := _load_db()
	var selected := {
		"team": 0,
		"side_profiles": [{"team": 0, "player_class": 4}, {"team": 1, "player_class": 200}],
	}
	assert_eq(_player_classes(db.character_join_profile(selected, true)), [5, 9],
			"in a session the class bytes clamp into 5..9")
	assert_eq(_player_classes(db.character_join_profile(selected, false)), [8, 8],
			"out of a session a byte outside 5..9 keeps the default")


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
	# The edit shows the current record's name, which the populate sets.
	assert_true(PlayerProfile.store().rename("Persistent"))
	companion.set_persisted_profile({
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


# Every populate sets PLAYERNAME through the edit's own text set, whose change
# notice runs the name rule on the record's name once: a refused last character
# is cut from the edit, whose own set reaches no handler (the dispatch detaches
# the edit's callback chain while it walks it), so the record keeps its name
# [orig: PlayerInfo_PopulateAllControls @0x5608d5 -> CEditWnd_SetText @0x661900
# -> PlayerInfo_HandlePlayerNameChange @0x55fbb0, the cut @0x55fc35..0x55fc6d;
# CWnd_EmitEventToNamedHandlerAndCallbacks @0x646990..0x6469a2, @0x646a09]. The
# ACCEPT's rule (white space only) keeps such a name, so a record can hold one.
func test_populate_cuts_one_refused_last_character_from_the_edit() -> void:
	var store := PlayerProfile.store()
	store.commit_name("Bob!?")
	assert_eq(store.get_player_name(), "Bob!?", "the ACCEPT's leg keeps a refused last character")
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	var edit := driver.widget_id("PLAYERNAME")
	assert_eq(driver.get_widget_text(edit), "Bob!", "the build's set cuts one refused character")
	assert_eq(store.get_player_name(), "Bob!?", "and the record keeps its name")
	# A side radio saves the edit's text through the ACCEPT's leg, then
	# populates: the repopulate's set runs the rule too.
	driver.set_widget_text(edit, "Ann#")
	var red_radio := driver.widget_id("SIDE_RED")
	driver.set_widget_checked(red_radio, true)
	driver.set_widget_checked(driver.widget_id("SIDE_BLUE"), false)
	driver.widget_activated.emit(red_radio, "SIDE_RED")
	assert_eq(driver.get_widget_text(edit), "Ann", "the side switch's populate cuts it")
	assert_eq(store.get_player_name(), "Ann#", "and the record keeps the saved name")


# The populate's set and a refused key's cut are both the edit's text set,
# which cuts the text's first {hot} marker: a record named "A{hot}B{hot}C"
# leaves "AB{hot}C" in the edit and names the record so, and a refused key's
# cut then cuts the second marker; a refused last character at the populate is
# cut and set again, which cuts the next marker too.
# [orig: PlayerInfo_PopulateAllControls @0x5608d5 -> CEditWnd_SetText @0x661900
#  -> CButtonWnd_SetLabel @0x661912 (strstr @0x657451); the cut's set
#  @0x55fc6d]
func test_the_name_sets_cut_one_hot_marker_each() -> void:
	var store := PlayerProfile.store()
	store.commit_name("A{hot}B{hot}C")
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	var edit := driver.widget_id("PLAYERNAME")
	assert_eq(driver.get_widget_text(edit), "AB{hot}C", "the populate's set cuts the first marker")
	assert_eq(store.get_player_name(), "AB{hot}C", "and its notice names the record with the label")
	# A refused key: the edit's change carries it, the rule cuts it and sets the
	# cut, which cuts the next marker; the record keeps its name.
	driver.widget_value_changed.emit("PLAYERNAME", "edit", 0, "AB{hot}C!")
	assert_eq(driver.get_widget_text(edit), "ABC", "the cut's set cuts the second marker")
	assert_eq(store.get_player_name(), "AB{hot}C", "the refused key renames nothing")
	# The populate's refused last character: the set cuts the first marker, the
	# rule cuts the character, the cut's set the next marker.
	store.commit_name("A{hot}B{hot}!")
	var reopened := PlayerInfoMenuCompanion.new()
	reopened.set_database(_load_db())
	var again := _make_avatar_driver()
	reopened.on_menu_built(again, "player.mnu", "PLAYER_INFO", null)
	assert_eq(again.get_widget_text(again.widget_id("PLAYERNAME")), "AB")
	assert_eq(store.get_player_name(), "A{hot}B{hot}!", "the record keeps the saved name")


# Over a real frame the populate's notice carries the label the set stored, not
# the frame's read-back of it (which decodes the frame's bytes as cp1252,
# D-MNU-49): a record named "José" refuses its last character and leaves "Jos"
# in the edit with the record kept, and a "Zoë Smith" is kept and names the
# record as it is, not double-encoded.
# [orig: CEditWnd_SetText @0x661900 - the label +732 @0x66195c into the
#  0x7000001 notice @0x661986; PlayerInfo_HandlePlayerNameChange @0x55fbb0]
func test_the_populate_rule_reads_the_set_label_over_a_frame() -> void:
	var store := PlayerProfile.store()
	store.commit_name("José")
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	var driver := _make_framed_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	assert_eq(driver.get_widget_text(driver.widget_id("PLAYERNAME")), "Jos",
			"the refused last character comes off the edit")
	assert_eq(store.get_player_name(), "José", "and the record keeps its name")
	store.commit_name("Zoë Smith")
	var reopened := PlayerInfoMenuCompanion.new()
	reopened.set_database(_load_db())
	var again := _make_framed_avatar_driver()
	reopened.on_menu_built(again, "player.mnu", "PLAYER_INFO", null)
	assert_eq(store.get_player_name(), "Zoë Smith", "a kept name renames the record as it is")


# The record keeps its name as the game's code-page bytes (D-NET-661): "Jörg"
# is J, 0xF6, r, g in player.sav, not the UTF-8 C3 B6, and reads back as the
# same text; a character cp1252 has no byte for is stored as '?'. The ACCEPT's
# 15-character cut then counts characters, one byte each.
# [orig: the ACCEPT's copy into the 16-byte field @0x55efc8; the record's name
#  +4, its readers the session copy @0x551695 and CNapiNPConnection_SendClientJoin's
#  NA @0x620162]
func test_the_record_keeps_the_name_as_code_page_bytes() -> void:
	var store := PlayerProfile.store()
	store.commit_name("Jörg")
	var bytes := store.player_sav_bytes()
	var record := 16
	var stored := PackedByteArray()
	for i in 5:
		stored.append(bytes[record + 4 + i])
	assert_eq(
		stored,
		PackedByteArray([0x4A, 0xF6, 0x72, 0x67, 0]),
		"the name is its cp1252 bytes, NUL-ended"
	)
	assert_eq(store.get_player_name(), "Jörg", "and reads back as the same text")
	store.commit_name("Ł1")
	assert_eq(store.get_player_name(), "?1", "a character cp1252 has no byte for is '?'")
	store.commit_name("ééééééééééééééééé")
	assert_eq(
		store.get_player_name(), "ééééééééééééééé", "the ACCEPT keeps 15 characters, one byte each"
	)


# The rule's kept text, a name or none, clears the record's no-name bit at the
# populate [orig: PlayerInfo_HandlePlayerNameChange @0x55fc9b], so opening the
# screen on a nameless record clears the bit without an ACCEPT.
func test_populate_clears_the_no_name_bit() -> void:
	var store := PlayerProfile.store()
	# A record flagged nameless that still carries a name: the bit at +52 of
	# record 0 (after the 16-byte header), the name at +4.
	var bytes := store.player_sav_bytes()
	var record := 16
	var stored := "Ghost".to_ascii_buffer()
	for i in 16:
		bytes[record + 4 + i] = stored[i] if i < stored.size() else 0
	bytes.encode_u32(record + 52, bytes.decode_u32(record + 52) | 1)
	store.load_bytes(bytes, true, store.weapon_sav_bytes(), true, "")
	assert_true(store.is_nameless(), "the record loads flagged nameless")
	assert_eq(store.get_player_name(), "Ghost")
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	assert_eq(driver.get_widget_text(driver.widget_id("PLAYERNAME")), "Ghost")
	assert_false(store.is_nameless(), "the build's set clears the no-name bit")
	# No text is kept too: the ACCEPT's leg empties a name that starts with white
	# space and flags the record, and the next populate clears the flag again.
	store.commit_name(" Ghost")
	assert_true(store.is_nameless())
	var reopened := PlayerInfoMenuCompanion.new()
	reopened.set_database(_load_db())
	var again := _make_avatar_driver()
	reopened.on_menu_built(again, "player.mnu", "PLAYER_INFO", null)
	assert_eq(again.get_widget_text(again.widget_id("PLAYERNAME")), "")
	assert_false(store.is_nameless(), "an empty name is kept and clears the bit")


## ADR 0046 S13 A8: an ACCEPT writes the profile record in memory and the next save point saves
## it where retail keeps it, player.sav and weapon.sav in the directory the game was started in
## [orig: PlayerProfile_SaveToFiles @ 0x54be00; the weapon.sav path built relative to it
## @0x54f68c-0x54f6b7] (LaunchFlags.working_dir, the run directory under the editor's Play),
## never under the mounted resource root, which for the editor's Play is a build no game may
## write: the root's tree is the same before and after, and the profile reads back.
func test_accept_saves_beside_the_game_never_in_the_root() -> void:
	var root := _staged_avatars_root()
	assert_not_null(root, "the minted avatar table stages")
	if root == null:
		return
	var run_dir := OS.get_cache_dir().path_join("opennova_player_info_run_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(run_dir), OK)
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", run_dir]))
	var before := _tree_digest(String(root.get_root_dir()))
	PlayerProfile.load_for(root)
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	watch_signals(companion)
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	# The ACCEPT saves the dialog into the record (the companion's own write), then a save point.
	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	assert_signal_emitted(companion, "avatar_chosen")
	var profile: Dictionary = get_signal_parameters(companion, "avatar_chosen")[0]
	assert_false(FileAccess.file_exists(run_dir.path_join("weapon.sav")),
			"the ACCEPT writes the record in memory, as the original's dialog does")
	assert_eq(PlayerProfile.save(), OK)
	assert_true(FileAccess.file_exists(run_dir.path_join("weapon.sav")), "the profile is saved beside the game")
	assert_true(FileAccess.file_exists(run_dir.path_join("player.sav")), "player.sav beside it")
	assert_eq(_tree_digest(String(root.get_root_dir())), before, "the resource root is not written")
	PlayerProfile.load_for(root)
	var loaded := PlayerProfile.load_character_profile(root)
	var saved_sides: Array = profile.get("side_profiles", [])
	var loaded_sides: Array = loaded.get("side_profiles", [])
	assert_eq(loaded_sides.size(), 2)
	if saved_sides.size() == 2 and loaded_sides.size() == 2:
		assert_eq(int((loaded_sides[0] as Dictionary).get("avatar_packed", -1)),
				int((saved_sides[0] as Dictionary).get("avatar_packed", -2)), "it reads back from there")
	LaunchFlags.clear_args_override()
	TestFs.remove_dir_recursive(run_dir)


# What a directory tree holds, as one text: each file's path under `dir`, its size and the
# SHA-256 of its bytes, sorted (a tree compared before and after a write that must not reach it).
func _tree_digest(dir: String) -> String:
	var lines := PackedStringArray()
	var pending := PackedStringArray([""])
	while not pending.is_empty():
		var relative: String = pending[pending.size() - 1]
		pending.remove_at(pending.size() - 1)
		var path := dir if relative.is_empty() else dir.path_join(relative)
		for sub in DirAccess.get_directories_at(path):
			pending.append(sub if relative.is_empty() else relative.path_join(sub))
		for file in DirAccess.get_files_at(path):
			var bytes := FileAccess.get_file_as_bytes(path.path_join(file))
			var hashing := HashingContext.new()
			hashing.start(HashingContext.HASH_SHA256)
			if not bytes.is_empty():
				hashing.update(bytes)
			lines.append("%s %d %s" % [file if relative.is_empty() else relative.path_join(file),
					bytes.size(), hashing.finish().hex_encode()])
	lines.sort()
	return "\n".join(lines)


# A synthetic PLAYER_INFO loadout screen: the three weapon combos, PLAYERCLASS
# carrying the CHARTYPE values 5..9 as authored `value=` items (as the .mnu's
# static items do), the ammo/type/grenade combos, STATIC_TOTAL_WEIGHT, and the
# bare *_ICON windows.
func _loadout_screen_xml() -> String:
	return MenuDriverFixture.screen_xml("PLAYER_INFO", _loadout_body())


func _loadout_body(top := 10) -> String:
	# A combo's rows are its LIST_BOX's, as the shipped menus author them.
	var type_items := ('<LIST_BOX><ITEMS><ITEM value="0">FMJ</ITEM><ITEM value="1">AP</ITEM>'
			+ '<ITEM value="2">SP</ITEM></ITEMS></LIST_BOX>')
	var body := ""
	var y := top
	for n in ["PRIMARY", "SECONDARY", "ACCESSORY"]:
		body += MenuDriverFixture.wnd("combobox", n, y)
		body += MenuDriverFixture.wnd("combobox", n + "_AMMO1", y + 24)
		body += MenuDriverFixture.wnd("combobox", n + "_AMMO2", y + 48)
		body += MenuDriverFixture.wnd("window", n + "_ICON", y + 72)
		y += 100
	for n in ["PRIMARY", "SECONDARY"]:
		# player.mnu authors the TYPE statics (FMJ/AP/SP, values 0/1/2); the
		# companion selects/locks them but never refills.
		body += MenuDriverFixture.wnd("combobox", n + "_AMMO1_TYPE", y, type_items)
		y += 24
	for n in ["GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"]:
		body += MenuDriverFixture.wnd("combobox", n, y)
		y += 24
	body += MenuDriverFixture.wnd("static", "STATIC_TOTAL_WEIGHT", y)
	var class_items := ""
	for v in range(5, 10):  # Medic..Engineer = values 5..9
		class_items += '<ITEM value="%d">class %d</ITEM>' % [v, v]
	body += MenuDriverFixture.wnd("combobox", "PLAYERCLASS", y + 24, "<LIST_BOX><ITEMS>%s</ITEMS></LIST_BOX>" % class_items)
	return body


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


func test_snapshot_carries_class_without_a_weapon_database() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	_ammo_driver = _make_loadout_driver()
	companion.on_menu_built(_ammo_driver, "player.mnu", "PLAYER_INFO", null)

	var profile := companion.snapshot()
	assert_eq(int(profile.get("player_class", 0)), 5,
		"class selection does not depend on weapon.def loading")
	assert_false(profile.has("primary"),
		"no table at all (no root) remains distinct from an explicit all-NONE kit")


const PLAIN_WEAPON_DEF := """
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
"""


# A missing weapon.def is a loaded, empty table: each list shows only NONE, and
# the ACCEPT's kit page names the catalog's seeded "None" row for every NONE
# category and empty grenade slot, so the profile takes it [orig: WeaponDef_LoadAll
# @0x54dd10 seeds "None" (@0x54dd45) and File_ParseASCIIFile @0x53d823 returns
# silently; PlayerInfo_PopulateWeaponSlotLists @0x560571..0x5605ab;
# PlayerInfo_SerializeWeaponLoadout names g_WeaponDefTable + 192 * row @0x55e6f0 /
# @0x55e7ee] (D-MNU-27).
func test_missing_weapon_def_accepts_a_none_kit() -> void:
	var empty_dir := TestFs.cache_dir(self, "player_info_missing_weapon_def")
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(empty_dir), OK)
	var companion := PlayerInfoMenuCompanion.new()
	_ammo_driver = _make_loadout_driver()
	companion.on_menu_built(_ammo_driver, "player.mnu", "PLAYER_INFO", root)
	for slot in ["PRIMARY", "SECONDARY", "ACCESSORY"]:
		assert_eq(_ammo_driver.item_count(_ammo_id(slot)), 1, "no weapon.def: %s lists only NONE" % slot)
	var profile := companion.snapshot()
	var names: Array[String] = []
	for entry: Dictionary in profile.get("kit", []):
		names.append(String(entry.get("name", "")))
	assert_eq(names, ["WPN_KNIFE", "WPN_MEDPACK", "None", "None", "None", "None", "None", "None"],
		"the medic's page: knife, medpack, then None for each NONE slot and empty grenade")
	# The ACCEPT's write (PlayerProfile.accept_player_info's), on a fresh record.
	# This screen has no avatar controls, so one side's character stands in.
	var profiles := PlayerProfiles.new()
	profiles.load_bytes(PackedByteArray(), false, "NOTAPROFILE00000".to_ascii_buffer(), true, "")
	profile["side_profiles"] = [{"avatar_a": 0, "avatar_b": 0, "avatar_packed": 0x0200}, {}]
	assert_eq(profiles.apply_character_selection(profile), OK, "the None kit is taken")
	TestFs.remove_dir_recursive(empty_dir)


# The companion lives the whole session, so its table follows the mount: the
# same root re-pointed at another folder, a Mods switch (the same root and
# folder remounted under another expansion) and a new root (CHANGE FOLDER)
# each read weapon.def again (D-MNU-27).
func test_loadout_table_follows_the_mount() -> void:
	var empty_dir := TestFs.cache_dir(self, "player_info_missing_weapon_def")
	var full_dir := TestFs.cache_dir(self, "player_info_weapon_def")
	TestFs.write_text(self, full_dir.path_join("weapon.def"), PLAIN_WEAPON_DEF)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(empty_dir), OK)
	var companion := PlayerInfoMenuCompanion.new()
	_ammo_driver = _make_loadout_driver()
	companion.on_menu_built(_ammo_driver, "player.mnu", "PLAYER_INFO", root)
	assert_eq(_ammo_driver.item_count(_ammo_id("PRIMARY")), 1, "no weapon.def: only NONE")

	assert_eq(root.set_root_dir(full_dir), OK)  # the same root, another folder
	companion.on_menu_built(_ammo_driver, "player.mnu", "PLAYER_INFO", root)
	assert_eq(_ammo_driver.item_count(_ammo_id("PRIMARY")), 2,
		"the re-pointed root's weapon.def is read: NONE + WPN_PLAIN")

	var picked := ResourceRoot.new()  # CHANGE FOLDER hands over a new root
	assert_eq(picked.set_root_dir(empty_dir), OK)
	companion.on_menu_built(_ammo_driver, "player.mnu", "PLAYER_INFO", picked)
	assert_eq(_ammo_driver.item_count(_ammo_id("PRIMARY")), 1,
		"the new root's missing weapon.def lists only NONE again")

	# A Mods switch: a packed install whose weapon.def only the expansion holds,
	# its root cleared and mounted again in place [orig: Game_ReloadExpansionAndMods
	# @0x552710 reloads the catalog, @0x55282b..0x55285b].
	var install := TestFs.cache_dir(self, "player_info_weapon_def_install")
	assert_eq(DirAccess.make_dir_recursive_absolute(install.path_join("expansion/jox01")), OK)
	WorldFixture.write_pff(self, install.path_join("resource.pff"),
			[{"name": "readme.txt", "bytes": "base".to_utf8_buffer()}])
	WorldFixture.write_pff(self, install.path_join("expansion/jox01/jox01.pff"),
			[{"name": "weapon.def", "bytes": TestFs.crlf(PLAIN_WEAPON_DEF).to_utf8_buffer()}])
	assert_eq(picked.mount_runtime(install), OK)
	companion.on_menu_built(_ammo_driver, "player.mnu", "PLAYER_INFO", picked)
	assert_eq(_ammo_driver.item_count(_ammo_id("PRIMARY")), 1, "the base game has no weapon.def")
	picked.clear()
	assert_eq(picked.mount_runtime(install, "jox01"), OK)
	companion.on_menu_built(_ammo_driver, "player.mnu", "PLAYER_INFO", picked)
	assert_eq(_ammo_driver.item_count(_ammo_id("PRIMARY")), 2,
		"the expansion's weapon.def is read after the switch")
	picked.clear()
	TestFs.remove_dir_recursive(empty_dir)
	TestFs.remove_dir_recursive(full_dir)
	TestFs.remove_dir_recursive(install)


# End-to-end against the REAL player.mnu (authored widget tree), so the loadout fills
# through the same control names/document the runtime opens, not just a stand-in. Runs
# in GUT so the autoloads (Strings) and the GDExtension are loaded.
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


# The picked row's VALUE is the override, written into the profile record at the
# pick, and a populate whose rows no longer offer that value writes DEFAULT_VOICE
# there before selecting by value (retail's "if (!found) profile[team + 1532] = 0").
func test_voice_override_persists_and_resets_when_the_avatar_drops_it() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	var voices := driver.widget_id("PLAYERVOICE")

	assert_eq(companion.selected_voice(), 0, "a fresh profile starts on DEFAULT_VOICE")
	driver.select_row(voices, 3)  # row 0 is DEFAULT_VOICE, so row 3 is "Voice 3"
	assert_eq(companion.selected_voice(), 3, "the pick stores the row VALUE, not the row")
	assert_eq(PlayerProfile.store().get_voice(0), 3,
		"the pick writes the profile's byte at once, before any ACCEPT")
	assert_false(companion.snapshot().has("voice"), "the ACCEPT snapshot carries no voice")

	# Voice 3 is a male row; moving to the female head drops it from the list.
	driver.select_row(driver.widget_id("DIVISION"), 3)
	driver.select_row(driver.widget_id("COMBO_LIST"), 1)
	assert_eq(companion.selected_voice(), 0,
		"an override the new avatar cannot offer resets to DEFAULT_VOICE")
	assert_eq(PlayerProfile.store().get_voice(0), 0, "the reset is written into the profile")
	assert_eq(driver.selected_row(voices), 0, "and the list selects that value's row")


# Each side keeps its own byte: the pick on the shown side writes that side's
# (+1532 blue, +1533 red) and leaves the other's, and a side shown again selects
# its own value [orig: PlayerInfo_HandlePlayerVoiceSelect @0x56004c; PlayerInfo_PopulatePlayerVoiceCombo
# @0x55de3c].
func test_voice_pick_writes_the_shown_sides_profile_byte() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	var voices := driver.widget_id("PLAYERVOICE")
	var blue_radio := driver.widget_id("SIDE_BLUE")
	var red_radio := driver.widget_id("SIDE_RED")

	driver.set_widget_checked(red_radio, true)
	driver.set_widget_checked(blue_radio, false)
	driver.widget_activated.emit(red_radio, "SIDE_RED")
	assert_eq(companion.team(), 1, "the red side is shown")
	assert_eq(driver.item_text(voices, 1), "Voice 7", "red's first head is female")
	driver.select_row(voices, 1)
	assert_eq(PlayerProfile.store().get_voice(1), 7, "red's pick lands in +1533")
	assert_eq(PlayerProfile.store().get_voice(0), 0, "blue's +1532 is untouched")

	driver.set_widget_checked(blue_radio, true)
	driver.set_widget_checked(red_radio, false)
	driver.widget_activated.emit(blue_radio, "SIDE_BLUE")
	assert_eq(companion.selected_voice(), 0, "blue shows its own byte")
	driver.select_row(voices, 2)  # "Voice 2", a male row
	assert_eq(PlayerProfile.store().get_voice(0), 2, "blue's pick lands in +1532")
	assert_eq(PlayerProfile.store().get_voice(1), 7, "red's +1533 stands")

	driver.set_widget_checked(red_radio, true)
	driver.set_widget_checked(blue_radio, false)
	driver.widget_activated.emit(red_radio, "SIDE_RED")
	assert_eq(driver.item_text(voices, driver.selected_row(voices)), "Voice 7",
		"red's list selects red's own value again")


# Press a side radio the way a click does: the driver checks it (and clears its
# group) before the activation reaches the companion.
func _press_side(driver: MenuDriver, team: int) -> void:
	var pressed := driver.widget_id("SIDE_RED" if team == 1 else "SIDE_BLUE")
	driver.set_widget_checked(pressed, true)
	driver.set_widget_checked(driver.widget_id("SIDE_BLUE" if team == 1 else "SIDE_RED"), false)
	driver.widget_activated.emit(pressed, "SIDE_RED" if team == 1 else "SIDE_BLUE")


# A build and a side switch fill the voice list once, against the side's restored
# selection, so a female head's voice survives them even though the side's first
# combo is male: the cascade's earlier legs, on the first rows, write nothing
# (engine profile::set_voice_override, retail's PopulateAllControls).
func test_restored_female_voice_survives_side_switches_and_rebuilds() -> void:
	var db := _load_db()
	assert_eq(db.get_combo(0, 0, 0).get_head().sex, 0, "blue's first head is male")
	assert_eq(db.get_combo(0, 3, 1).get_head().sex, 1, "N00/D03 combo 1 is a female head")
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(db)
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	var voices := driver.widget_id("PLAYERVOICE")
	driver.select_row(driver.widget_id("DIVISION"), 3)
	driver.select_row(driver.widget_id("COMBO_LIST"), 1)
	assert_eq(driver.item_text(voices, 1), "Voice 7", "the female list's first id")
	driver.select_row(voices, 1)
	assert_eq(PlayerProfile.store().get_voice(0), 7, "the pick lands in +1532")

	_press_side(driver, 1)
	_press_side(driver, 0)
	assert_eq(PlayerProfile.store().get_voice(0), 7, "a side round trip keeps blue's female voice")
	assert_eq(driver.item_text(voices, driver.selected_row(voices)), "Voice 7",
		"and the restored list selects it")

	var rebuilt := _make_avatar_driver()
	companion.on_menu_built(rebuilt, "player.mnu", "PLAYER_INFO", null)
	var rebuilt_voices := rebuilt.widget_id("PLAYERVOICE")
	assert_eq(rebuilt.selected_row(rebuilt.widget_id("COMBO_LIST")), 1, "the build restores blue's combo")
	assert_eq(PlayerProfile.store().get_voice(0), 7, "a fresh build keeps it too")
	assert_eq(rebuilt.item_text(rebuilt_voices, rebuilt.selected_row(rebuilt_voices)), "Voice 7")


# The mirror on red: red's first head is female, so a male voice on red's second
# combo survives the side switch and a fresh build only when the fills against
# the first rows write nothing.
func test_restored_male_voice_survives_on_red() -> void:
	var db := _load_db()
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(db)
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	var voices := driver.widget_id("PLAYERVOICE")
	_press_side(driver, 1)
	var red_first: int = companion.nationality_rows()[0]
	assert_eq(db.get_combo(red_first, 0, 0).get_head().sex, 1, "red's first head is female")
	assert_eq(db.get_combo(red_first, 0, 1).get_head().sex, 0, "red's second combo is a male head")
	driver.select_row(driver.widget_id("COMBO_LIST"), 1)
	assert_eq(driver.item_text(voices, 2), "Voice 2", "the male list's second id")
	driver.select_row(voices, 2)
	assert_eq(PlayerProfile.store().get_voice(1), 2, "the pick lands in +1533")

	_press_side(driver, 0)
	_press_side(driver, 1)
	assert_eq(PlayerProfile.store().get_voice(1), 2, "a side round trip keeps red's male voice")
	assert_eq(driver.item_text(voices, driver.selected_row(voices)), "Voice 2")

	var rebuilt := _make_avatar_driver()
	companion.on_menu_built(rebuilt, "player.mnu", "PLAYER_INFO", null)
	_press_side(rebuilt, 1)
	var rebuilt_voices := rebuilt.widget_id("PLAYERVOICE")
	assert_eq(rebuilt.selected_row(rebuilt.widget_id("COMBO_LIST")), 1, "red's combo is restored")
	assert_eq(PlayerProfile.store().get_voice(1), 2, "a fresh build and a switch to red keep it")
	assert_eq(rebuilt.item_text(rebuilt_voices, rebuilt.selected_row(rebuilt_voices)), "Voice 2")


# A persisted override the avatar still offers survives the build and selects its
# own row BY VALUE. Id 10 proves the distinction: it is the LAST male row
# (index 7) because the disabled id-9 row never enters the list.
func test_persisted_voice_selects_its_row_by_value() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	PlayerProfile.store().set_voice(0, 10)
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
	PlayerProfile.store().set_voice(0, 6)
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


func _make_ammo_companion(wdb: WeaponDatabase, with_frame := false) -> PlayerInfoMenuCompanion:
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_weapon_database(wdb)
	_ammo_driver = _make_loadout_driver(with_frame)
	companion.on_menu_built(_ammo_driver, "player.mnu", "PLAYER_INFO", null)
	return companion

func _ammo_id(name: String) -> int:
	return _ammo_driver.widget_id(name)

func _select_weapon(wdb: WeaponDatabase, control: String, slot: int,
		class_mask: int, weapon_name: String) -> WeaponDef:
	var combo := _ammo_id(control)
	for w: WeaponDef in wdb.get_slot_weapons(slot, class_mask, 2):  # blue
		if w.name.nocasecmp_to(weapon_name) == 0:
			var row := _weapon_row(wdb, combo, w)
			if row > 0:
				_ammo_driver.select_row(combo, row)  # emits -> the witnessed refill
				return w
	assert_true(false, "%s offers %s" % [control, weapon_name])
	return null


# The row showing a weapon: the list sorts by label after NONE (D-PLAYERINFO-21).
func _weapon_row(wdb: WeaponDatabase, combo: int, w: WeaponDef) -> int:
	var label := wdb.weapon_label(w.index, Strings.get_table(Strings.TABLE_GAMETEXT))
	for row in range(1, _ammo_driver.item_count(combo)):
		if _ammo_driver.item_text(combo, row) == label:
			return row
	return -1


# The expected *_AMMO2 sub-weapon, computed from the public table walk the companion
# mirrors [orig: the stricmp walk in PlayerInfo_PopulateAmmoComboBoxes @ 0x55def0].

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
	f.store_string(TestFs.crlf(def_text))
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


# --- The saved kit page (D-PLAYERINFO-19) --------------------------------------

# The screen's populate restores the shown side's class page from the current
# record into the lists and the picks, and the dialog's save (the ACCEPT, a side
# radio, a class pick) writes the page it restored back: an ACCEPT with no re-picks
# keeps the saved kit (engine menu::player_info_restore_kit;
# docs/playerinfo/avatars-re.md "Saved-kit consumption").

# Weapon.def rows 0..7: two primaries, two secondaries, an accessory and three
# grenades, each for both sides and the medic and rifleman classes.
const KIT_WEAPON_DEF := """
weapon "WPN_P1"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	primary
	round_type	"AMMO_P1"
	clipsize	30
	maxclips	5
	clipweight	1
	weaponweight	5
end

weapon "WPN_P2"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	primary
	round_type	"AMMO_P2"
	clipsize	20
	maxclips	4
	clipweight	1
	weaponweight	6
end

weapon "WPN_S1"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	secondary
	round_type	"AMMO_S1"
	clipsize	7
	maxclips	3
	clipweight	1
	weaponweight	2
end

weapon "WPN_S2"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	secondary
	round_type	"AMMO_S2"
	clipsize	8
	maxclips	3
	clipweight	1
	weaponweight	2
end

weapon "WPN_A1"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	accessory
	round_type	"AMMO_A1"
	clipsize	1
	maxclips	2
	clipweight	1
	weaponweight	3
end

weapon "WPN_G1"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	grenade
	round_type	"G1"
	clipsize	1
	maxclips	2
	clipweight	1
	weaponweight	1
end

weapon "WPN_G2"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	grenade
	round_type	"G2"
	clipsize	1
	maxclips	2
	clipweight	1
	weaponweight	1
end

weapon "WPN_G3"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	grenade
	round_type	"G3"
	clipsize	1
	maxclips	2
	clipweight	1
	weaponweight	1
end
"""

const HEADER_BYTES := 16
const SIDE_BYTES := 0x8006
const FIRST_PAGE_OFFSET := 6
const PAGE_BYTES := 2048

func _kit_entry(name: String, primary := -1, secondary := -1, flags := -1) -> Dictionary:
	return {"name": name, "ammo_primary": primary, "ammo_secondary": secondary, "flags": flags}


# The blue medic's page as the screen serializes it: knife, medpack, the three
# categories with their counts and type bytes, the three grenade slots.
func _blue_kit() -> Array:
	return [_kit_entry("WPN_KNIFE"), _kit_entry("WPN_MEDPACK"),
		_kit_entry("WPN_P1", 3, -1, 1), _kit_entry("WPN_S1", 2, -1, 2),
		_kit_entry("WPN_A1", 1), _kit_entry("WPN_G1", 0), _kit_entry("WPN_G2", 1),
		_kit_entry("WPN_G3")]


func _red_kit() -> Array:
	return [_kit_entry("WPN_KNIFE2"), _kit_entry("WPN_MEDPACK"),
		_kit_entry("WPN_P2", 2, -1, 0), _kit_entry("WPN_S2", 1, -1, 0),
		_kit_entry("None"), _kit_entry("WPN_G1"), _kit_entry("WPN_G2"), _kit_entry("WPN_G3")]


func _kit_weapons() -> WeaponDatabase:
	var path := TestFs.cache_dir(self, "player_info_kit_def").path_join("weapon.def")
	TestFs.write_text(self, path, KIT_WEAPON_DEF)
	var wdb := WeaponDatabase.new()
	assert_eq(wdb.load(path), OK, "the kit weapon.def loads")
	return wdb


# The strings of one side's class page in the current record, up to its empty one.
func _page_tokens(side: int, player_class: int) -> PackedStringArray:
	var bytes := PlayerProfile.store().weapon_sav_bytes()
	var at := HEADER_BYTES + SIDE_BYTES * side + FIRST_PAGE_OFFSET + PAGE_BYTES * (player_class - 5)
	var tokens := PackedStringArray()
	var start := at
	for i in range(at, at + PAGE_BYTES):
		if bytes[i] != 0:
			continue
		if i == start:
			break
		tokens.append(bytes.slice(start, i).get_string_from_ascii())
		start = i + 1
	return tokens


func _tokens_of(kit: Array) -> PackedStringArray:
	var tokens := PackedStringArray()
	for entry: Dictionary in kit:
		for key in ["name", "ammo_primary", "ammo_secondary", "flags"]:
			tokens.append(str(entry[key]))
	return tokens


# The full PLAYER_INFO screen (the character controls, the side radios, the name,
# ACCEPT and the loadout) over the minted avatar table and the kit weapon.def,
# opened as the shell opens it, on the profile the record gives.
func _open_kit_screen(root: ResourceRoot, wdb: WeaponDatabase) -> PlayerInfoMenuCompanion:
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_weapon_database(wdb)
	companion.set_persisted_profile(PlayerProfile.load_character_profile(root))
	var xml := MenuDriverFixture.screen_xml("PLAYER_INFO", _avatar_body() + _loadout_body(260))
	_ammo_driver = MenuDriverFixture.driver_over(self, MenuDriverFixture.doc_from_xml(self, xml), "player.mnu")
	companion.on_menu_built(_ammo_driver, "player.mnu", "PLAYER_INFO", root)
	return companion


func _selected_text(control: String) -> String:
	var id := _ammo_id(control)
	var row := _ammo_driver.selected_row(id)
	return _ammo_driver.item_text(id, row) if row >= 0 else ""


# Opening PLAYER and pressing ACCEPT with no re-picks writes back the saved page:
# the lists, the clip counts, the ammo-type bytes and the grenade counts all come
# from it, and the ACCEPT's save is the companion's own.
func test_accept_without_repicks_keeps_the_saved_kit() -> void:
	var root := _staged_avatars_root()
	assert_not_null(root)
	if root == null:
		return
	var wdb := _kit_weapons()
	_reset_profile(_blue_kit(), _red_kit())
	var saved := _page_tokens(0, 5)
	assert_eq(saved, _tokens_of(_blue_kit()), "the blue medic page is seeded")
	var companion := _open_kit_screen(root, wdb)
	watch_signals(companion)

	assert_eq(_selected_text("PRIMARY"), "WPN_P1", "the page's primary is selected")
	assert_eq(_selected_text("SECONDARY"), "WPN_S1")
	assert_eq(_selected_text("ACCESSORY"), "WPN_A1")
	assert_eq(_ammo_driver.selected_row(_ammo_id("PRIMARY_AMMO1")), 2, "three clips: the third row")
	assert_eq(_ammo_driver.selected_row(_ammo_id("SECONDARY_AMMO1")), 1, "two clips")
	assert_eq(_ammo_driver.selected_row(_ammo_id("ACCESSORY_AMMO1")), 0, "one clip")
	assert_eq(companion.selected_ammo_type("PRIMARY"), 1, "the page's PRIMARY type byte")
	assert_eq(companion.selected_ammo_type("SECONDARY"), 2, "the page's SECONDARY type byte")
	assert_eq(_ammo_driver.selected_row(_ammo_id("GRENADE_AMMO1")), 0, "a saved zero grenade count")
	assert_eq(_ammo_driver.selected_row(_ammo_id("GRENADE_AMMO2")), 1)
	assert_eq(_ammo_driver.selected_row(_ammo_id("GRENADE_AMMO3")), 2, "an untouched count: the full row")

	_ammo_driver.set_widget_text(_ammo_id("PLAYERNAME"), "Ghost")
	_ammo_driver.widget_activated.emit(_ammo_id("ACCEPT"), "ACCEPT")
	assert_signal_emitted(companion, "avatar_chosen")
	var profile: Dictionary = get_signal_parameters(companion, "avatar_chosen")[0]
	assert_eq(_tokens_of(profile["kit"]), saved, "the ACCEPT's page is the saved one")
	assert_eq(PlayerProfile.store().get_player_name(), "Ghost", "the ACCEPT saved the dialog itself")
	assert_eq(_page_tokens(0, 5), saved, "and the record's page is unchanged")


# A side radio saves the dialog first, the page of the side it leaves included,
# then restores the clicked side's page; a click on the shown side saves too.
func test_side_switch_saves_the_side_it_leaves() -> void:
	var root := _staged_avatars_root()
	assert_not_null(root)
	if root == null:
		return
	var wdb := _kit_weapons()
	_reset_profile(_blue_kit(), _red_kit())
	var companion := _open_kit_screen(root, wdb)
	_ammo_driver.select_row(_ammo_id("SECONDARY"), 2)  # WPN_S2
	_ammo_driver.select_row(_ammo_id("SECONDARY_AMMO1"), 0)  # one clip
	assert_eq(_selected_text("SECONDARY"), "WPN_S2")

	_press_side(_ammo_driver, 0)  # the shown side
	assert_eq(_page_tokens(0, 5).slice(12, 16), PackedStringArray(["WPN_S2", "1", "-1", "2"]),
		"a click on the shown side saves its page")
	assert_eq(_selected_text("SECONDARY"), "WPN_S2", "and restores it")

	_press_side(_ammo_driver, 1)
	assert_eq(companion.team(), 1)
	assert_eq(_selected_text("PRIMARY"), "WPN_P2", "red shows its own page")
	assert_eq(_selected_text("SECONDARY"), "WPN_S2")
	assert_eq(_selected_text("ACCESSORY"), "None")
	assert_eq(_ammo_driver.selected_row(_ammo_id("PRIMARY_AMMO1")), 1, "red's two clips")
	_ammo_driver.select_row(_ammo_id("PRIMARY"), 1)  # WPN_P1 on red

	_press_side(_ammo_driver, 0)
	assert_eq(_page_tokens(1, 5).slice(8, 12), PackedStringArray(["WPN_P1", "-1", "-1", "0"]),
		"red's edit is saved when blue is clicked")
	assert_eq(_selected_text("SECONDARY"), "WPN_S2", "blue's saved edit is restored")
	assert_eq(_ammo_driver.selected_row(_ammo_id("SECONDARY_AMMO1")), 0)
	assert_eq(_selected_text("PRIMARY"), "WPN_P1")


# A record whose class byte picks no page (outside 5..9, which retail's populate
# turns into a page pointer outside the side's pages): nothing is restored, and the
# dialog's save still writes the class the screen shows, the character and the kit,
# the kit into that class's page (D-PLAYERINFO-26).
func test_a_class_byte_with_no_page_still_saves() -> void:
	var root := _staged_avatars_root()
	assert_not_null(root)
	if root == null:
		return
	var wdb := _kit_weapons()
	var store := PlayerProfile.store()
	var bytes := store.weapon_sav_bytes()
	bytes[HEADER_BYTES] = 3
	bytes[HEADER_BYTES + SIDE_BYTES] = 3
	store.load_bytes(store.player_sav_bytes(), true, bytes, true, "")
	assert_eq(store.character_summary().blue.player_class, 3, "the record's class byte picks no page")
	var _companion := _open_kit_screen(root, wdb)
	assert_eq(_selected_text("PLAYERCLASS"), "class 5", "PLAYERCLASS keeps its authored row")
	assert_eq(_selected_text("PRIMARY"), "None", "no page is restored")
	_ammo_driver.select_row(_ammo_id("PRIMARY"), 1)  # WPN_P1

	_ammo_driver.widget_activated.emit(_ammo_id("ACCEPT"), "ACCEPT")
	var summary := store.character_summary()
	assert_eq([summary.blue.player_class, summary.red.player_class], [5, 5],
		"the class the screen shows is written to both sides")
	assert_eq(_page_tokens(0, 5).slice(0, 12), PackedStringArray(["WPN_KNIFE", "-1", "-1", "-1",
		"WPN_MEDPACK", "-1", "-1", "-1", "WPN_P1", "-1", "-1", "0"]),
		"and the kit lands in that class's page")


# A class pick saves the dialog under the class it leaves (that page's medpack
# entry follows the class the pick wrote, so it drops), then restores the picked
# class's page; picking the class back restores the edit.
func test_class_pick_saves_the_class_it_leaves() -> void:
	var root := _staged_avatars_root()
	assert_not_null(root)
	if root == null:
		return
	var wdb := _kit_weapons()
	_reset_profile(_blue_kit(), _red_kit())
	var _companion := _open_kit_screen(root, wdb)
	_ammo_driver.select_row(_ammo_id("PRIMARY"), 2)  # WPN_P2
	assert_eq(_selected_text("PRIMARY"), "WPN_P2")

	_ammo_driver.select_row(_ammo_id("PLAYERCLASS"), 3)  # class 8
	assert_eq(_page_tokens(0, 5).slice(0, 8), PackedStringArray(["WPN_KNIFE", "-1", "-1", "-1",
		"WPN_P2", "-1", "-1", "1"]),
		"the medic page holds the edit, without the medpack the new class byte drops")
	assert_eq(_selected_text("PRIMARY"), "None",
		"the rifleman's default page names no weapon this table carries")

	_ammo_driver.select_row(_ammo_id("PLAYERCLASS"), 0)  # back to class 5
	assert_eq(_selected_text("PRIMARY"), "WPN_P2", "the medic page's edit is restored")
	assert_eq(_selected_text("SECONDARY"), "WPN_S1")


# --- The slot lists' sort (D-PLAYERINFO-21) -----------------------------------

# Three primaries whose table order is neither their byte order nor their
# case-insensitive one, and two grenades out of label order.
const SORT_WEAPON_DEF := """
weapon "WPN_ZULU"
	loadout_selectable	1
	teamfilter	blue
	charfilter	medic
	weapon_class	primary
	round_type	"AMMO_Z"
	clipsize	30
	maxclips	5
	clipweight	1
	weaponweight	5
end

weapon "wpn_alpha"
	loadout_selectable	1
	teamfilter	blue
	charfilter	medic
	weapon_class	primary
	round_type	"AMMO_A"
	clipsize	30
	maxclips	5
	clipweight	1
	weaponweight	5
end

weapon "WPN_Mike"
	loadout_selectable	1
	teamfilter	blue
	charfilter	medic
	weapon_class	primary
	round_type	"AMMO_M"
	clipsize	30
	maxclips	5
	clipweight	1
	weaponweight	5
end

weapon "WPN_GZ"
	loadout_selectable	1
	teamfilter	blue
	charfilter	medic
	weapon_class	grenade
	round_type	"GZ"
	clipsize	1
	maxclips	2
	clipweight	1
	weaponweight	1
end

weapon "WPN_GA"
	loadout_selectable	1
	teamfilter	blue
	charfilter	medic
	weapon_class	grenade
	round_type	"GA"
	clipsize	1
	maxclips	2
	clipweight	1
	weaponweight	1
end
"""


# The populate sorts each slot list's rows by their text, case-insensitively and
# ascending, before it inserts NONE at row 0, as the WEAPON screen's populate
# does; each row keeps its weapon, so the kit restore still selects the page's
# weapon by value and a pick names the weapon its row shows. The grenade fill
# sorts nothing: its slots take the table's order.
# [orig: PlayerInfo_PopulateWeaponSlotLists @0x560430 -- ListWidget_SortRows(list,
#  0, 1) @0x56054c / @0x560557 / @0x560566 over cmp @0x6448a0 (stricmp,
#  ascending), the NONE inserts @0x56058f / @0x56059d / @0x5605ab;
#  PlayerInfo_PopulateAmmoComboBoxes @0x55def0 calls no sort]
func test_slot_lists_sort_by_their_labels() -> void:
	var root := _staged_avatars_root()
	assert_not_null(root)
	if root == null:
		return
	var path := TestFs.cache_dir(self, "player_info_sort_def").path_join("weapon.def")
	TestFs.write_text(self, path, SORT_WEAPON_DEF)
	var wdb := WeaponDatabase.new()
	assert_eq(wdb.load(path), OK, "the sort weapon.def loads")
	_reset_profile([_kit_entry("WPN_KNIFE"), _kit_entry("WPN_MEDPACK"),
		_kit_entry("WPN_Mike", 2, -1, 0), _kit_entry("None"), _kit_entry("None"),
		_kit_entry("WPN_GZ", 0), _kit_entry("WPN_GA", 1), _kit_entry("None")])
	var companion := _open_kit_screen(root, wdb)

	assert_eq(_items("PRIMARY"), ["None", "wpn_alpha", "WPN_Mike", "WPN_ZULU"],
		"NONE, then the rows by their text, case-insensitively")
	assert_eq(_selected_text("PRIMARY"), "WPN_Mike", "the restore selects the page's weapon by value")
	assert_eq(_ammo_driver.selected_row(_ammo_id("PRIMARY")), 2, "its sorted row")
	assert_eq(_ammo_driver.selected_row(_ammo_id("PRIMARY_AMMO1")), 1, "with the page's two clips")
	assert_eq(_ammo_driver.item_text(_ammo_id("GRENADE_AMMO1"), 0), "0 - GZ",
		"the first grenade slot is the table's first grenade")
	assert_eq(_ammo_driver.item_text(_ammo_id("GRENADE_AMMO2"), 0), "0 - GA")

	_ammo_driver.select_row(_ammo_id("PRIMARY"), 3)
	assert_eq(String(companion.snapshot().get("primary", "")), "WPN_ZULU",
		"a pick names the weapon its row shows")
	_ammo_driver.select_row(_ammo_id("PRIMARY"), 1)
	assert_eq(String(companion.snapshot().get("primary", "")), "wpn_alpha")


func _items(name: String) -> Array:
	return Array(_ammo_driver.get_widget_items(_ammo_id(name)))


# --- The dialog's write of the side not shown (D-PLAYERINFO-20) ----------------

# The dialog's save writes the class to both sides and the character of the shown
# side alone [orig: PlayerInfo_SaveFromDialog @0x55EE3F..0x55EE6D; the shown side's
# stores @0x55EEAB / @0x55EEEE / @0x55EF31, each indexed by g_PlayerInfoTeamIndex]:
# a red character the table no longer resolves stays as saved through a blue
# ACCEPT, though the screen holds red's first combo for it.
func test_accept_leaves_the_side_not_shown() -> void:
	var root := _staged_avatars_root()
	assert_not_null(root)
	if root == null:
		return
	var store := PlayerProfile.store()
	var stale_packed := 0x8000 | (63 << 9) | (15 << 5) | 30
	assert_eq(store.apply_character_selection({"team": 1, "player_class": 5,
			"side_profiles": [{}, {"avatar_a": 30, "avatar_b": 15, "avatar_packed": stale_packed}]}), OK)
	assert_null(_load_db().resolve_character_id(stale_packed, 1), "the table has no such character")
	# Blue's nationality and division bytes and red's class byte unlike what the
	# ACCEPT writes, so each write below shows.
	var bytes := store.weapon_sav_bytes()
	bytes[HEADER_BYTES + 1] = 7
	bytes[HEADER_BYTES + 2] = 3
	bytes[HEADER_BYTES + SIDE_BYTES] = 9
	store.load_bytes(store.player_sav_bytes(), true, bytes, true, "")
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_persisted_profile(PlayerProfile.load_character_profile(root))
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", root)
	var sides: Array = companion.snapshot()["side_profiles"]
	var blue: Dictionary = sides[0]
	var shown := [int(blue.get("avatar_a", -1)), int(blue.get("avatar_b", -1)),
			int(blue.get("avatar_packed", -1))]
	assert_ne(shown, [7, 3, 0x0200], "the screen's blue character differs from the record's bytes")
	assert_eq(int((sides[1] as Dictionary).get("avatar_packed", -1)), 0x8204,
			"the screen holds red's first combo for it")

	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	var summary := store.character_summary()
	assert_eq([summary.red.avatar_a, summary.red.avatar_b, summary.red.avatar_packed],
			[30, 15, stale_packed], "the ACCEPT on blue leaves red's bytes")
	assert_eq([summary.blue.avatar_a, summary.blue.avatar_b, summary.blue.avatar_packed], shown,
			"and writes blue's")
	assert_eq([summary.blue.player_class, summary.red.player_class], [5, 5],
			"the class goes to both sides, red's 9 included")


# --- The authored PLAYER_INFO screen and the shipped loadout fields -------------

# Our base game's player.mnu: the authored PLAYER_INFO widget tree the runtime opens.
const ASSETS_PLAYER_MNU := "res://../assets/player.mnu"


func _assets_player_doc() -> MnuDocument:
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(FileAccess.get_file_as_bytes(ASSETS_PLAYER_MNU)), OK,
		"assets/player.mnu loads")
	return doc


# ArmoryFixture's loadout catalog: the shipped weapon.def's rows and loadout fields
# the cases below read (WPN_M4AUTO, the M4M203 family, the grenades, ...).
func _load_weapons() -> WeaponDatabase:
	var wdb := WeaponDatabase.new()
	var path := ArmoryFixture.stage_loadout_catalog("player_info_seam").path_join("weapon.def")
	assert_eq(wdb.load(path), OK, "the loadout catalog weapon.def loads")
	return wdb


func _weight_text() -> String:
	return _ammo_driver.get_widget_text(_ammo_id("STATIC_TOTAL_WEIGHT"))


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


func test_voice_preview_requests_selected_avatar_voice() -> void:
	var root := _staged_avatars_root()
	assert_not_null(root, "the minted table stages as the root's Avatars.def")
	if root == null:
		return

	var doc := _assets_player_doc()
	var driver := MenuDriver.new()
	assert_true(driver.open_document(doc, root, null, null, "player.mnu", "PLAYER_INFO"),
		"the authored player menu opens on the driver")

	var companion := PlayerInfoMenuCompanion.new()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", root)
	watch_signals(driver)
	assert_true(driver.has_widget("TESTPLAYERVOICE"),
		"the authored PLAYER_INFO screen carries its voice-preview button")
	driver.widget_activated.emit(driver.widget_id("TESTPLAYERVOICE"), "TESTPLAYERVOICE")

	# The table's initially selected N00/D00 head (SYN_HEAD_BOONIE) carries voice 1.
	# Retail formats that avatar-derived fallback as VOICE_1 and plays it from the
	# dedicated menu.lwf bank. [orig: PlayerInfo_PreviewVoice @ 0x55ff70]
	assert_signal_emitted_with_parameters(driver, "sound_requested", ["menu.lwf", "VOICE_1"])


func test_populates_loadout_slots_filtered_by_class_and_team() -> void:
	var wdb := _load_weapons()
	var _companion := _make_ammo_companion(wdb)

	var primary := _ammo_id("PRIMARY")
	# Medic (class mask 1), blue (team mask 2): the DB's filtered set plus a leading NONE row.
	var expected := wdb.get_slot_weapons(WeaponDatabase.SLOT_PRIMARY, 1, 2)
	assert_gt(expected.size(), 0, "the fixture has medic/blue primary weapons")
	assert_eq(_ammo_driver.item_count(primary), expected.size() + 1, "PRIMARY = NONE + the filtered weapons")
	assert_eq(_ammo_driver.item_text(primary, 0), "None", "NONE leads the slot (fallback with no string table)")


func test_loadout_class_filter_includes_and_excludes() -> void:
	var _companion := _make_ammo_companion(_load_weapons())

	# Medic (value 5): WPN_M4AUTO (charfilter medic|rifleman|engineer, blue) is a primary -> present.
	assert_true(_items("PRIMARY").has("WPN_M4AUTO"), "M4 shows for Medic")

	# Sniper (value 6): M4's charfilter excludes sniper -> absent after re-fill.
	# select_row emits the "combo" value change — the class-mask refilter relay.
	_ammo_driver.select_row(_ammo_id("PLAYERCLASS"), 1)
	assert_false(_items("PRIMARY").has("WPN_M4AUTO"), "M4 is hidden for Sniper")


func test_snapshot_carries_the_selected_loadout_weapon_ids() -> void:
	var wdb := _load_weapons()
	var companion := _make_ammo_companion(wdb)

	var primary_defs := wdb.get_slot_weapons(
			WeaponDatabase.SLOT_PRIMARY, 1, 2)
	var selected_primary := ""
	var selected_row := -1
	for weapon: WeaponDef in primary_defs:
		if weapon.name.nocasecmp_to("WPN_M4AUTO") == 0:
			selected_primary = weapon.name
			selected_row = _weapon_row(wdb, _ammo_id("PRIMARY"), weapon)
			break
	assert_false(selected_primary == null, "the fixture offers M4AUTO for medic/blue")
	_ammo_driver.select_row(_ammo_id("PRIMARY"), selected_row, false)  # silent reselect

	var profile := companion.snapshot()
	assert_eq(String(profile.get("primary", "")), selected_primary,
		"PLAYER_INFO ACCEPT preserves the selected primary's weapon.def id")
	assert_eq(int(profile.get("player_class", 0)), 5,
		"the selected class travels with the spawn loadout")


func test_real_player_mnu_loadout_populates() -> void:
	var driver := MenuDriverFixture.driver_over(self, _assets_player_doc(), "player.mnu", "PLAYER_INFO")

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
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	# The M203 carbines are rifleman-filtered; switch PLAYERCLASS to Rifleman
	# (value 8, row 3) so the slot list offers them.
	_ammo_driver.select_row(_ammo_id("PLAYERCLASS"), 3)
	var w := _select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 8, "WPN_M4M203AUTO")
	# The witnessed walk skips same-round WPN_M4M203 and lands on WPN_M4M203HE
	# (AMMO_M203_40MM_NADE) within loadout_subclasses = 2: the shipped weapon.def's
	# three M4M203 rows, in its order (ArmoryFixture.LOADOUT_ROWS).
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


func test_grenade_zero_pick_stays_zero_in_the_weight() -> void:
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


# The shipped weapon.def authors WPN_M4M203AUTO with loadout_subclasses 2 (the
# catalog row ArmoryFixture.LOADOUT_ROWS carries).
func test_weapon_dict_carries_loadout_subclasses() -> void:
	var wdb := _load_weapons()
	var idx := wdb.find_weapon("WPN_M4M203AUTO")
	assert_gt(idx, 0)
	assert_eq(wdb.get_weapon(idx).loadout_subclasses, 2,
		"loadout_subclasses (+36) rides the transport dict")


func test_kit_page_carries_knife_medpack_slots_and_three_grenades() -> void:
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

	# Slot index 2 = PRIMARY. Nothing picked yet, so the NONE row's value (0)
	# serializes the catalog's row 0, the "None" WeaponDef_LoadAll seeds ahead
	# of the file's rows [orig: PlayerInfo_SerializeWeaponLoadout @0x55e6f0;
	# WeaponDef_LoadAll @0x54dd45], never the file's first weapon.
	assert_eq(String((kit[2] as Dictionary).get("name", "")), "None",
		"a NONE slot serializes the catalog's None row")
	assert_ne(wdb.get_weapon(0).name, "None", "the file's first row is a weapon")

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
	var companion := _make_ammo_companion(_load_weapons())
	_ammo_driver.select_row(_ammo_id("PLAYERCLASS"), 3)  # values 5..9 -> row 3 = class 8

	var kit: Array = companion.snapshot().get("kit", [])
	assert_eq(kit.size(), 7, "no medpack for a rifleman")
	assert_eq(String((kit[0] as Dictionary).get("name", "")), "WPN_KNIFE",
		"the blue side keeps WPN_KNIFE")
	for entry in kit:
		assert_ne(String((entry as Dictionary).get("name", "")), "WPN_MEDPACK")
