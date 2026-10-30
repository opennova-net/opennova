extends GutTest

# The player.mnu PLAYER_INFO screen's device half: PlayerInfoMenuCompanion gives the
# engine controller (PlayerInfoScreen over engine inmatch::PlayerInfoScreen) the tables
# it reads and mounts what it asks for -- the PLAYER_PREVIEW portrait over the shared
# MenuFrame, the weapon slot icons, the voice preview's sound -- riding a MenuDriver over
# a real MnuDocument. The screen's logic (the cascade, the side radios, the name rule,
# the voice override, the loadout's saved page and the ACCEPT's save) is pinned by the
# ctest player_info_screen (tests/menu/player_info_screen_test.cpp). The table is the
# minted synth_avatars.def fixture (tests/fixtures/minimal_avatars_gen.cpp), injected
# directly (root-less) like the avatar data tests. The join's character profile
# (CharacterJoinProfile) is pinned here too.
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
		assert_eq(store.apply_character_selection(CharacterSelection.from_player_info_snapshot({
			"team": side,
			"player_class": 5,
			"side_profiles": [BLUE_CHARACTER, RED_CHARACTER],
			"kit": blue_kit if side == 0 else red_kit,
		})), OK, "the side's medic page is seeded")
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


# Open the screen the way the menu shell hands it a document: the companion
# attached to the driver and the root, the driver's open document claimed by the
# screen's controller (MenuDriver.claim_screen runs its open).
func _open(companion: PlayerInfoMenuCompanion, driver: MenuDriver, root: ResourceRoot = null) -> void:
	driver.release_screen()
	companion.attach(driver, root)
	assert_true(driver.claim_screen(companion.screen()), "the PLAYER_INFO screen opens on the driver")


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


# A side whose packed word the avatar table does not resolve stays the record's in the
# loaded profile (its raw word and class byte, no tree indices), and the join carries the
# word as it is as CI and the class byte as CT, its avatar byte the resolve's miss, 1;
# with no table the same (D-PLAYERINFO-33; engine inmatch::join_character_profile carries
# the witness).
func test_join_profile_carries_an_unresolved_sides_record_word() -> void:
	var root := _staged_avatars_root()
	assert_not_null(root)
	if root == null:
		return
	var store := PlayerProfile.store()
	var stale_packed := 0x8000 | (63 << 9) | (15 << 5) | 30
	assert_eq(store.apply_character_selection(CharacterSelection.from_player_info_snapshot({
			"team": 1, "player_class": 7,
			"side_profiles": [{}, {"avatar_a": 30, "avatar_b": 15, "avatar_packed": stale_packed}]})),
			OK)
	var db := _load_db()
	assert_null(db.resolve_character_id(stale_packed, 1), "the table has no such character")
	var loaded := PlayerProfile.load_character_profile(root)
	var red: Dictionary = (loaded.get("side_profiles", [{}, {}]) as Array)[1]
	assert_false(red.is_empty(), "the unresolved side stays the record's")
	assert_eq(int(red.get("avatar_packed", -1)), stale_packed, "its raw word")
	assert_eq(int(red.get("player_class", -1)), 7, "its class byte")
	assert_eq(int(red.get("nationality", 0)), -1, "no tree indices")
	var profile := db.character_join_profile(loaded, true)
	assert_eq(profile.get_character_id(1), stale_packed, "CI1 is the record's word")
	assert_eq(profile.get_player_class(1), 7, "CTB is the record's class byte")
	assert_eq(profile.get_avatar(1), 1, "VCB is the resolve's miss")
	var no_table := AvatarDatabase.new().character_join_profile(loaded, true)
	assert_eq(no_table.get_character_id(1), stale_packed, "no table: CI1 is still the word")
	assert_eq(no_table.get_player_class(1), 7, "no table: CTB is still the class byte")


func test_mounts_3d_preview_when_widget_present() -> void:
	# The preview mounts as a frame child placed by widget_frame_rect, so this
	# seam needs a real MenuFrame attached (the geometry half of the driver).
	var frame := MenuFrame.new()
	frame.size = Vector2(800, 600)
	add_child_autofree(frame)
	var driver := MenuDriver.new()
	driver.attach(frame, null)
	assert_true(driver.open_document(MenuDriverFixture.doc_from_xml(self, _avatar_screen_xml(true)),
			null, null, "player.mnu"), "the preview document opens on the driver")
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	# No resource root, so the preview mounts but loads no .3di (graceful); we only
	# assert the surface is wired over the PLAYER_PREVIEW widget rect.
	_open(companion, driver)
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
			null, null, "player.mnu"), "the preview document opens on the driver")
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	_open(companion, driver)
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
			null, null, "player.mnu"), "the preview document opens on the driver")
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	_open(companion, driver)
	assert_not_null(frame.find_child("PlayerInfoAvatarPreview", true, false),
			"the preview is mounted while the companion owns the document")
	driver.release_screen()
	await get_tree().process_frame  # queue_free drains
	assert_null(frame.find_child("PlayerInfoAvatarPreview", true, false),
			"releasing the companion frees the frame-child preview mount")


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
	_open(companion, driver)
	# The ACCEPT saves the dialog into the record (the companion's own write), then a save point.
	driver.activate(driver.widget_id("ACCEPT"))
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
			null, null, "player.mnu"), "the loadout document opens on the driver")
	return driver


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
	_open(companion, _ammo_driver, root)
	assert_eq(_ammo_driver.item_count(_ammo_id("PRIMARY")), 1, "no weapon.def: only NONE")

	assert_eq(root.set_root_dir(full_dir), OK)  # the same root, another folder
	_open(companion, _ammo_driver, root)
	assert_eq(_ammo_driver.item_count(_ammo_id("PRIMARY")), 2,
		"the re-pointed root's weapon.def is read: NONE + WPN_PLAIN")

	var picked := ResourceRoot.new()  # CHANGE FOLDER hands over a new root
	assert_eq(picked.set_root_dir(empty_dir), OK)
	_open(companion, _ammo_driver, picked)
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
	_open(companion, _ammo_driver, picked)
	assert_eq(_ammo_driver.item_count(_ammo_id("PRIMARY")), 1, "the base game has no weapon.def")
	picked.clear()
	assert_eq(picked.mount_runtime(install, "jox01"), OK)
	_open(companion, _ammo_driver, picked)
	assert_eq(_ammo_driver.item_count(_ammo_id("PRIMARY")), 2,
		"the expansion's weapon.def is read after the switch")
	picked.clear()
	TestFs.remove_dir_recursive(empty_dir)
	TestFs.remove_dir_recursive(full_dir)
	TestFs.remove_dir_recursive(install)


func _make_ammo_companion(wdb: WeaponDatabase, with_frame := false) -> PlayerInfoMenuCompanion:
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_weapon_database(wdb)
	_ammo_driver = _make_loadout_driver(with_frame)
	_open(companion, _ammo_driver)
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


# --- The authored PLAYER_INFO screen and the loadout catalog ---------------------

# Our base game's player.mnu: the authored PLAYER_INFO widget tree the runtime opens.
const ASSETS_PLAYER_MNU := "res://../fixtures/game/player.mnu"


func _assets_player_doc() -> MnuDocument:
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(FileAccess.get_file_as_bytes(ASSETS_PLAYER_MNU)), OK,
		"fixture player.mnu loads")
	return doc


# ArmoryFixture's loadout catalog: authored rows under the names the cases below read
# (WPN_M4AUTO, the M4M203 family, the grenades, ...), with the shipped values they pin.
func _load_weapons() -> WeaponDatabase:
	var wdb := WeaponDatabase.new()
	var path := ArmoryFixture.stage_loadout_catalog("player_info_seam").path_join("weapon.def")
	assert_eq(wdb.load(path), OK, "the loadout catalog weapon.def loads")
	return wdb


func test_voice_preview_requests_selected_avatar_voice() -> void:
	var root := _staged_avatars_root()
	assert_not_null(root, "the minted table stages as the root's Avatars.def")
	if root == null:
		return

	var doc := _assets_player_doc()
	var driver := MenuDriver.new()
	assert_true(driver.open_document(doc, root, null, "player.mnu", "PLAYER_INFO"),
		"the authored player menu opens on the driver")

	var companion := PlayerInfoMenuCompanion.new()
	_open(companion, driver, root)
	watch_signals(driver)
	assert_true(driver.has_widget("TESTPLAYERVOICE"),
		"the authored PLAYER_INFO screen carries its voice-preview button")
	driver.activate(driver.widget_id("TESTPLAYERVOICE"))

	# The table's initially selected N00/D00 head (SYN_HEAD_BOONIE) carries voice 1.
	# Retail formats that avatar-derived fallback as VOICE_1 and plays it from the
	# dedicated menu.lwf bank. [orig: PlayerInfo_PreviewVoice @ 0x55ff70]
	assert_signal_emitted_with_parameters(driver, "sound_requested", ["menu.lwf", "VOICE_1"])


func test_real_player_mnu_loadout_populates() -> void:
	var driver := MenuDriverFixture.driver_over(self, _assets_player_doc(), "player.mnu", "PLAYER_INFO")

	assert_true(driver.has_widget("PRIMARY"), "the real player.mnu authors a PRIMARY combobox")
	var pclass := driver.widget_id("PLAYERCLASS")
	assert_gte(pclass, 0, "the real player.mnu authors a PLAYERCLASS combobox")
	assert_gt(driver.item_count(pclass), 0, "PLAYERCLASS carries its static class items")

	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	companion.set_weapon_database(_load_weapons())  # injected (root-less unit), as if weapon.def had loaded
	_open(companion, driver)

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
