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
	var type_items := ('<ITEMS><ITEM value="0">FMJ</ITEM><ITEM value="1">AP</ITEM>'
			+ '<ITEM value="2">SP</ITEM></ITEMS>')
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
	body += MenuDriverFixture.wnd("combobox", "PLAYERCLASS", y + 24, "<ITEMS>%s</ITEMS>" % class_items)
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
# its own value [orig: sub_560030 @0x56004c; PlayerInfo_PopulatePlayerVoiceCombo
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
