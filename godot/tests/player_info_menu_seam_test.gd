extends GutTest

# The player.mnu PLAYER_INFO screen seam: the companion (player_info_menu_companion.gd)
# drives the JO character screen by control NAME -- fills NATIONALITY / DIVISION /
# COMBO_LIST / PLAYERVOICE from Avatars.def, runs the nationality->division->combo
# cascade, and re-filters by the SIDE_BLUE/SIDE_RED team -- riding a MenuDriver over
# a real MnuDocument (the compiled-menu surface; the Control tree is gone). This pins
# the wiring against the witnessed original (docs/playerinfo/avatars-re.md,
# D-PLAYERINFO-5/7); the live in-engine render is the manual smoke. Avatars.def comes
# from the committed retail fixture, injected directly (root-less) like the avatar
# data tests.

const AVATARS_FIXTURE := "res://../fixtures/avatars/Avatars.def"


# Strings is a shared autoload; keep the registry clean so the raw-key tests below see
# no gametext table and the resolved-names test starts from a known state.
func before_each() -> void:
	Strings.clear()


func after_all() -> void:
	Strings.clear()


func _load_db() -> AvatarDatabase:
	var db := AvatarDatabase.new()
	var path := ProjectSettings.globalize_path(AVATARS_FIXTURE)
	assert_eq(db.load(path), OK, "Avatars.def fixture loads")
	return db


# --- Driver harness (the compiled-menu seam) ----------------------------------

func _doc_from_xml(xml: String) -> MnuDocument:
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(xml.to_utf8_buffer()), OK,
			"the synthetic .mnu XML parses")
	return doc


func _driver_over(doc: MnuDocument, menu_file: String, screen := "") -> MenuDriver:
	# Frameless on purpose: the driver's state store carries the companion seam
	# without a render surface (the documented headless-test contract).
	var driver := MenuDriver.new()
	assert_true(driver.open_document(doc, null, null, null, menu_file, screen),
			"the document opens on the driver")
	return driver


func _wnd(type: String, name: String, top: int, inner := "", attrs := "") -> String:
	return ('<WINDOW type="%s" name="%s"%s><POSITION><LEFT>10</LEFT><TOP>%d</TOP>'
			+ '<RIGHT>250</RIGHT><BOTTOM>%d</BOTTOM></POSITION>%s</WINDOW>') % [
			type, name, attrs, top, top + 20, inner]


func _screen_xml(screen_name: String, body: String) -> String:
	return ('<SCREEN><NAME>%s</NAME><WINDOW type="window" name="MAIN">'
			+ '<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT>'
			+ '<BOTTOM>600</BOTTOM></POSITION>%s</WINDOW></SCREEN>') % [
			screen_name, body]


# A synthetic PLAYER_INFO screen: the cascade combos, the team radios (SIDE_BLUE
# ships CHECKED, both share the authored GROUP), the name edit and ACCEPT --
# exactly the control names player.mnu authors.
func _avatar_screen_xml(include_preview := false) -> String:
	var body := ""
	var y := 10
	for n in ["NATIONALITY", "DIVISION", "COMBO_LIST", "PLAYERVOICE"]:
		body += _wnd("combo", n, y)
		y += 24
	body += _wnd("radio", "SIDE_BLUE", y, "<GROUP>1</GROUP>", " CHECKED")
	body += _wnd("radio", "SIDE_RED", y + 24, "<GROUP>1</GROUP>")
	body += _wnd("edit", "PLAYERNAME", y + 48)
	body += _wnd("button", "ACCEPT", y + 72)
	if include_preview:
		body += ('<WINDOW type="window" name="PLAYER_PREVIEW">'
				+ '<POSITION><LEFT>500</LEFT><TOP>100</TOP><RIGHT>700</RIGHT>'
				+ '<BOTTOM>400</BOTTOM></POSITION></WINDOW>')
	return _screen_xml("PLAYER_INFO", body)


func _make_avatar_driver(include_preview := false) -> MenuDriver:
	return _driver_over(_doc_from_xml(_avatar_screen_xml(include_preview)), "player.mnu")


func test_join_auth_profile_uses_retail_avatar_packing_and_defaults() -> void:
	var db := _load_db()
	var profile := NetSessionDrive.character_join_profile_from_database(db)
	var ids: Array = profile.get("character_ids", [])
	var classes: Array = profile.get("player_classes", [])
	var avatars: Array = profile.get("avatars", [])

	assert_eq(ids, [0x0200, 0x8207],
			"fresh profile selects the first good/evil Avatars.def entries")
	assert_eq(classes, [8, 8],
			"fresh retail profile is rifleman on both sides")
	assert_eq(avatars, [1, 10],
			"zero voice overrides resolve through each selected combo's head voice")
	assert_eq(int(profile.get("team_request", 0)), -1,
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
	var profile := NetSessionDrive.character_join_profile_from_database(db, selected)
	var ids: Array = profile.get("character_ids", [])
	var avatars: Array = profile.get("avatars", [])
	var combo: Dictionary = db.get_combo(0, 0, 1)
	var head: Dictionary = combo.get("head", {})

	assert_eq(int(ids[0]), 0x0400,
			"nat 0 / div 0 / combo id 2 packs into bits 0..14")
	assert_eq(int(ids[1]), 0x8207,
			"choosing side A does not erase side B's profile selection")
	assert_eq(int(avatars[0]), int(head.get("voice", -1)),
			"the selected combo supplies its retail avatar byte")
	assert_eq(profile.get("player_classes", []), [6, 6],
			"retail commits the chosen class to both side blocks")


func test_join_auth_profile_carries_both_persisted_side_characters() -> void:
	var db := _load_db()
	var blue: Dictionary = db.resolve_character_id(0x0400, 0)
	var red: Dictionary = db.resolve_character_id(0x8407, 1)
	assert_false(blue.is_empty())
	assert_false(red.is_empty())
	var selected := {
		"team": 0,
		"side_profiles": [
			{
				"team": 0,
				"nationality": int(blue.get("nationality_index", -1)),
				"division": int(blue.get("division_index", -1)),
				"combo": int(blue.get("combo_index", -1)),
				"player_class": 5,
			},
			{
				"team": 1,
				"nationality": int(red.get("nationality_index", -1)),
				"division": int(red.get("division_index", -1)),
				"combo": int(red.get("combo_index", -1)),
				"player_class": 9,
			},
		],
	}
	var profile := NetSessionDrive.character_join_profile_from_database(db, selected)
	assert_eq(profile.get("character_ids", []), [0x0400, 0x8407],
			"assignment to either team receives that side's persisted character")
	assert_eq(profile.get("player_classes", []), [5, 9],
			"an untouched loaded profile retains its two retail class bytes")


func test_owns_menu_detects_player_info() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	assert_true(companion.owns_menu(_make_avatar_driver()),
			"a menu carrying NATIONALITY + COMBO_LIST is the PLAYER_INFO screen")
	var plain := _driver_over(_doc_from_xml(_screen_xml("PLAIN",
			_wnd("button", "OK", 10))), "plain.mnu")
	assert_false(companion.owns_menu(plain),
			"a plain menu is left to the shell / other companions")


func test_populates_avatar_lists_and_combo_label() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var db := _load_db()
	companion._db = db  # inject directly (no resource root in the unit)
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)

	assert_gt(driver.item_count(driver.widget_id("NATIONALITY")), 0, "nationalities populate")
	# The initial team-0 cascade selects the first good nationality (US, index 0) and its
	# first division (SEAL): division + combo lists fill from the avatar tree.
	assert_eq(driver.item_count(driver.widget_id("DIVISION")), 9, "US has 9 divisions")
	var combos := driver.widget_id("COMBO_LIST")
	assert_eq(driver.item_count(combos), 4, "the SEAL division has 4 combos")

	# Each combo row is "<head display> - <body display>". With no gametext table registered
	# (before_each cleared Strings) the names fall back to their raw keys.
	var c0: Dictionary = db.get_combo(0, 0, 0)
	var head: Dictionary = c0.get("head", {})
	var body: Dictionary = c0.get("body", {})
	var expected := "%s - %s" % [String(head.get("display_name", "")), String(body.get("display_name", ""))]
	assert_eq(driver.item_text(combos, 0), expected, "combo row is the last - first character label")


# With the gametext table's "Avatars" section registered (as the shell does from Game.bin at
# boot), the companion resolves nationality + combo display keys to friendly names instead of the
# raw AV_* keys [orig: GameText_GetStringWithFallback @ 0x51eb90, "Avatars" section].
func test_resolves_friendly_names_from_gametext_avatars_section() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var db := _load_db()
	companion._db = db

	# Map the exact keys this test asserts on to friendly text in a synthetic Avatars table.
	var t := RtxtStringFile.new()
	t.add_section("Avatars")
	var nat0 := String(db.get_nationality(0).get("name_key", ""))
	t.add_entry(nat0, "United States", 0, Vector2i())
	var c0: Dictionary = db.get_combo(0, 0, 0)
	var head_key := String(c0.get("head", {}).get("display_name", ""))
	var body_key := String(c0.get("body", {}).get("display_name", ""))
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
	companion._db = db
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
	companion._db = db
	var driver := _make_avatar_driver()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)

	# Team 0 (blue/SIDE_BLUE default): every shown nationality is good-aligned.
	var good_rows: Array = companion._nat_db_index.duplicate()
	assert_gt(good_rows.size(), 0, "at least one good nationality")
	for i in good_rows:
		assert_eq(int(db.get_nationality(i).get("alignment", -1)), AvatarDatabase.ALIGN_GOOD,
			"team 0 shows only good-aligned nationalities")

	# Switching to SIDE_RED (team 1) re-filters to evil-aligned nationalities.
	# Mirror what a real click does: the driver flips checked with group
	# exclusivity through its own click path, then emits the activation by NAME.
	var side_red := driver.widget_id("SIDE_RED")
	driver.set_widget_checked(side_red, true)
	driver.set_widget_checked(driver.widget_id("SIDE_BLUE"), false)
	driver.widget_activated.emit(side_red, "SIDE_RED")
	for i in companion._nat_db_index:
		assert_eq(int(db.get_nationality(i).get("alignment", -1)), AvatarDatabase.ALIGN_EVIL,
			"team 1 shows only evil-aligned nationalities")

	# The two teams partition every nationality (good->blue, evil->red; D-PLAYERINFO-5).
	assert_eq(good_rows.size() + companion._nat_db_index.size(), db.get_nationality_count(),
		"good + evil = all nationalities")


func test_initial_team_follows_checked_side_radio() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion._db = _load_db()
	var driver := _make_avatar_driver()
	driver.set_widget_checked(driver.widget_id("SIDE_RED"), true)
	driver.set_widget_checked(driver.widget_id("SIDE_BLUE"), false)
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)
	assert_eq(companion._team, 1, "the initial team follows the checked SIDE_RED radio")


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
	assert_true(driver.open_document(_doc_from_xml(_avatar_screen_xml(true)),
			null, null, null, "player.mnu"), "the preview document opens on the driver")
	var companion := PlayerInfoMenuCompanion.new()
	companion._db = _load_db()
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


func test_snapshot_reports_current_selection() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion._db = _load_db()
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
	assert_eq(int((sides[1] as Dictionary).get("avatar_packed", -1)), 0x8207,
			"the opposite side is initialized to retail's resolved default")


func test_persisted_side_profiles_restore_each_team_cascade() -> void:
	var db := _load_db()
	var blue: Dictionary = db.resolve_character_id(0x0400, 0)
	var red: Dictionary = db.resolve_character_id(0x8407, 1)
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_persisted_profile({
		"name": "Persistent",
		"team": 0,
		"side_profiles": [
			{
				"team": 0,
				"nationality": int(blue.get("nationality_index", -1)),
				"division": int(blue.get("division_index", -1)),
				"combo": int(blue.get("combo_index", -1)),
				"player_class": 8,
				"avatar_a": 0, "avatar_b": 0, "avatar_packed": 0x0400,
			},
			{
				"team": 1,
				"nationality": int(red.get("nationality_index", -1)),
				"division": int(red.get("division_index", -1)),
				"combo": int(red.get("combo_index", -1)),
				"player_class": 8,
				"avatar_a": 7, "avatar_b": 0, "avatar_packed": 0x8407,
			},
		],
	})
	var driver := _make_avatar_driver()
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/avatars")), OK)
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", root)
	assert_eq(driver.selected_row(driver.widget_id("COMBO_LIST")),
			int(blue.get("combo_index", -1)))
	assert_eq(driver.get_widget_text(driver.widget_id("PLAYERNAME")), "Persistent")
	var red_radio := driver.widget_id("SIDE_RED")
	driver.set_widget_checked(red_radio, true)
	driver.set_widget_checked(driver.widget_id("SIDE_BLUE"), false)
	driver.widget_activated.emit(red_radio, "SIDE_RED")
	var red_snapshot := companion.snapshot()
	assert_eq(int(red_snapshot.get("nationality", -1)),
			int(red.get("nationality_index", -1)))
	assert_eq(int(red_snapshot.get("division", -1)),
			int(red.get("division_index", -1)))
	assert_eq(driver.selected_row(driver.widget_id("COMBO_LIST")),
			int(red.get("combo_index", -1)),
			"switching side restores that side's persisted combo")
	var saved_sides: Array = companion.snapshot().get("side_profiles", [])
	assert_eq(int((saved_sides[0] as Dictionary).get("avatar_packed", -1)), 0x0400)
	assert_eq(int((saved_sides[1] as Dictionary).get("avatar_packed", -1)), 0x8407)


func test_accept_emits_avatar_chosen() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion._db = _load_db()
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
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/avatars")), OK,
		"the avatar fixture directory mounts as a retail resource root")

	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(
			FileAccess.get_file_as_bytes("res://../fixtures/mnu/jo_player.mnu")), OK,
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

	# The fixture's initially selected US/SEAL head carries voice 1. Retail formats
	# that avatar-derived fallback as VOICE_1 and plays it from the dedicated menu.lwf
	# bank. [orig: PlayerInfo_PreviewVoice @ 0x55ff70]
	assert_signal_emitted_with_parameters(driver, "sound_requested", ["menu.lwf", "VOICE_1"])


# --- Loadout (PRIMARY/SECONDARY/ACCESSORY) ------------------------------------

const WEAPON_FIXTURE := "res://../fixtures/def/weapon.def"


func _load_weapons() -> WeaponDatabase:
	var wdb := WeaponDatabase.new()
	var path := ProjectSettings.globalize_path(WEAPON_FIXTURE)
	assert_eq(wdb.load(path), OK, "weapon.def fixture loads")
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
		body += _wnd("combo", n, y)
		body += _wnd("combo", n + "_AMMO1", y + 24)
		body += _wnd("combo", n + "_AMMO2", y + 48)
		body += _wnd("window", n + "_ICON", y + 72)
		y += 100
	for n in ["PRIMARY", "SECONDARY"]:
		# player.mnu authors the TYPE statics (FMJ/AP/SP, values 0/1/2); the
		# companion selects/locks them but never refills.
		body += _wnd("combo", n + "_AMMO1_TYPE", y, type_items)
		y += 24
	for n in ["GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"]:
		body += _wnd("combo", n, y)
		y += 24
	body += _wnd("static", "STATIC_TOTAL_WEIGHT", y)
	var class_items := ""
	for v in range(5, 10):  # Medic..Engineer = values 5..9
		class_items += '<ITEM value="%d">class %d</ITEM>' % [v, v]
	body += _wnd("combo", "PLAYERCLASS", y + 24, "<ITEMS>%s</ITEMS>" % class_items)
	return _screen_xml("PLAYER_INFO", body)


var _ammo_driver: MenuDriver = null  # the driver behind the current _make_ammo_companion


func _make_loadout_driver(with_frame := false) -> MenuDriver:
	var driver := MenuDriver.new()
	if with_frame:
		# The icon mounts are frame children placed by widget_frame_rect.
		var frame := MenuFrame.new()
		frame.size = Vector2(800, 600)
		add_child_autofree(frame)
		driver.attach(frame, null)
	assert_true(driver.open_document(_doc_from_xml(_loadout_screen_xml()),
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
# [orig: populate_weapon_slot_lists @ 0x560430].
func _select_weapon(wdb: WeaponDatabase, control: String, slot: int,
		class_mask: int, weapon_name: String) -> Dictionary:
	var combo := _ammo_id(control)
	var defs: Array = wdb.get_slot_weapons(slot, class_mask, 2)  # blue
	for i in defs.size():
		var w := defs[i] as Dictionary
		if String(w.get("name", "")).nocasecmp_to(weapon_name) == 0:
			_ammo_driver.select_row(combo, i + 1)  # emits -> the witnessed refill
			return w
	assert_true(false, "%s offers %s" % [control, weapon_name])
	return {}


# The expected *_AMMO2 sub-weapon, computed from the public table walk the companion
# mirrors [orig: the stricmp walk in populate_ammo_combo_boxes @ 0x55def0].
func _expected_sub(wdb: WeaponDatabase, parent: Dictionary) -> Dictionary:
	var parent_round := String(parent.get("round_type", ""))
	for k in range(1, int(parent.get("loadout_subclasses", 0)) + 1):
		var cand := wdb.get_weapon(int(parent.get("index", -1)) + k)
		if cand.is_empty():
			return {}
		if String(cand.get("round_type", "")).nocasecmp_to(parent_round) != 0:
			return cand
	return {}


func _items(name: String) -> Array:
	return Array(_ammo_driver.get_widget_items(_ammo_id(name)))


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
	for i in primary_defs.size():
		var weapon: Dictionary = primary_defs[i]
		if String(weapon.get("name", "")).nocasecmp_to("WPN_M4AUTO") == 0:
			selected_primary = String(weapon.get("name", ""))
			selected_row = i + 1 # row 0 is NONE
			break
	assert_false(selected_primary.is_empty(), "the fixture offers M4AUTO for medic/blue")
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
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(
			FileAccess.get_file_as_bytes("res://../fixtures/mnu/jo_player.mnu")), OK,
			"the shipped jo_player.mnu fixture loads")
	var driver := _driver_over(doc, "player.mnu", "PLAYER_INFO")

	assert_true(driver.has_widget("PRIMARY"), "the real player.mnu authors a PRIMARY combobox")
	var pclass := driver.widget_id("PLAYERCLASS")
	assert_gte(pclass, 0, "the real player.mnu authors a PLAYERCLASS combobox")
	assert_gt(driver.item_count(pclass), 0, "PLAYERCLASS carries its static class items")

	var companion := PlayerInfoMenuCompanion.new()
	companion._db = _load_db()
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
	var maxclips := int(w.get("maxclips", 0))
	assert_true(_ammo_driver.is_widget_shown(ammo), "a clip-carrying weapon shows its ammo combo")
	assert_eq(_ammo_driver.item_count(ammo), maxclips,
		"rows 1..maxclips [orig: populate_ammo_combo_boxes @ 0x55def0]")
	assert_eq(_ammo_driver.item_text(ammo, 0),
		"%d - %s" % [int(w.get("clipsize", 0)), String(w.get("round_type", ""))],
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
	# (AMMO_M203_40MM_NADE) within loadout_subclasses = 2.
	var sub := _expected_sub(wdb, w)
	assert_eq(String(sub.get("name", "")), "WPN_M4M203HE",
		"the sub walk lands on the first DIFFERING round_type entry")
	var ammo2 := _ammo_id("PRIMARY_AMMO2")
	assert_true(_ammo_driver.is_widget_shown(ammo2), "a live sub-weapon shows *_AMMO2")
	assert_eq(_ammo_driver.item_count(ammo2), int(sub.get("maxclips", 0)),
		"*_AMMO2 rows come from the SUB-weapon's maxclips")
	assert_eq(_ammo_driver.item_text(ammo2, 0),
		"%d - %s" % [int(sub.get("clipsize", 0)), String(sub.get("round_type", ""))],
		"*_AMMO2 labels use the sub-weapon's clipsize + round type")


func test_grenade_combos_fill_in_table_order_with_zero_row() -> void:
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	var expected: Array = wdb.get_slot_weapons(
			WeaponDatabase.SLOT_GRENADE, 1, 2)  # medic/blue
	assert_gt(expected.size(), 0, "the fixture carries medic/blue grenades")
	for i in mini(expected.size(), 3):
		var combo := _ammo_id("GRENADE_AMMO%d" % (i + 1))
		var w := expected[i] as Dictionary
		assert_true(_ammo_driver.is_widget_shown(combo), "an owned grenade control shows")
		assert_eq(_ammo_driver.item_count(combo), int(w.get("maxclips", 0)) + 1,
			"grenade rows are 0..maxclips INCLUDING the zero row [orig: @ 0x55def0]")
		assert_eq(_ammo_driver.item_text(combo, 0), "0 - %s" % String(w.get("round_type", "")),
			"row 0 is the zero-rounds row")
		assert_eq(_ammo_driver.selected_row(combo), int(w.get("maxclips", 0)),
			"the untouched default selects the full row")
	for i in range(expected.size(), 3):
		assert_false(_ammo_driver.is_widget_shown(_ammo_id("GRENADE_AMMO%d" % (i + 1))),
			"unowned grenade controls hide")


func test_weight_label_renders_witnessed_format_and_band() -> void:
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	var w := _select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 1, "WPN_M4AUTO")
	# Expected parent term [orig: calculate_loadout_weight @ 0x55f1f0]:
	# weight + maxclips*clip_weight (untouched default), plus the sub-weapon and
	# full-grenade clip-only terms the fill selects by default.
	var expected := float(w.get("weight", 0.0)) \
			+ int(w.get("maxclips", 0)) * float(w.get("clip_weight", 0.0))
	var sub := _expected_sub(wdb, w)
	if not sub.is_empty() and int(sub.get("clipsize", 0)) > 0:
		expected += int(sub.get("maxclips", 0)) * float(sub.get("clip_weight", 0.0))
	for g in wdb.get_slot_weapons(WeaponDatabase.SLOT_GRENADE, 1, 2).slice(0, 3):
		expected += int(g.get("maxclips", 0)) * float(g.get("clip_weight", 0.0))
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
	assert_false(String(w.get("icon", "")).is_empty(), "the fixture authors an icon")


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
	# The witnessed asymmetry [orig: calculate_loadout_weight @ 0x55f1f0]:
	# grenades default -1 -> maxclips, but a PICKED 0 stays 0 (the zero row) —
	# unlike the parents' <=0 -> maxclips rule.
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	var g := wdb.get_slot_weapons(WeaponDatabase.SLOT_GRENADE, 1, 2)[0] as Dictionary
	assert_gt(float(g.get("clip_weight", 0.0)) * int(g.get("maxclips", 0)), 0.0,
		"the first grenade def carries weighable clips")
	var default_text := _weight_text()  # -1 default = full grenades weighed in
	_ammo_driver.select_row(_ammo_id("GRENADE_AMMO1"), 0)  # the zero row
	assert_ne(_weight_text(), default_text,
		"picking the zero row removes that grenade's clip term")
	var zero_total := float(_weight_text().get_slice(" ", 2))
	var default_total := float(default_text.get_slice(" ", 2))
	assert_almost_eq(default_total - zero_total,
		int(g.get("maxclips", 0)) * float(g.get("clip_weight", 0.0)), 0.06,
		"the delta is exactly the grenade's maxclips*clip_weight term")


func test_weapon_dict_carries_loadout_subclasses() -> void:
	var wdb := _load_weapons()
	var idx := wdb.find_weapon("WPN_M4M203AUTO")
	assert_gt(idx, 0)
	assert_eq(int(wdb.get_weapon(idx).get("loadout_subclasses", -1)), 2,
		"loadout_subclasses (+36) rides the transport dict")
