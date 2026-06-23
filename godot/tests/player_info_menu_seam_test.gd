extends GutTest

# The player.mnu PLAYER_INFO screen seam: the companion (player_info_menu_host.gd)
# drives the JO character screen by control NAME -- fills NATIONALITY / DIVISION /
# COMBO_LIST / PLAYERVOICE from Avatars.def, runs the nationality->division->combo
# cascade, and re-filters by the SIDE_BLUE/SIDE_RED team. This pins the wiring against
# the witnessed original (docs/playerinfo/avatars-re.md, D-PLAYERINFO-5/7); the live
# in-engine render is the manual smoke. Avatars.def comes from the committed retail
# fixture, injected directly (root-less) like the avatar data tests.

const AVATARS_FIXTURE := "res://../fixtures/avatars/Avatars.def"


func _load_db() -> NovaAvatarDatabase:
	var db := NovaAvatarDatabase.new()
	var path := ProjectSettings.globalize_path(AVATARS_FIXTURE)
	assert_eq(db.load(path), OK, "Avatars.def fixture loads")
	return db


# A stand-in for the built PLAYER_INFO screen: a plain Node with the named NovaMnu*
# controls as children, exactly as the menu builder would name them from player.mnu.
func _make_menu() -> Node:
	var menu := Node.new()
	menu.name = "Menu"
	add_child_autofree(menu)
	for n in ["NATIONALITY", "DIVISION", "COMBO_LIST", "PLAYERVOICE"]:
		var c := NovaMnuCombo.new()
		c.name = n
		menu.add_child(c)
	for n in ["SIDE_BLUE", "SIDE_RED"]:
		var b := Button.new()
		b.toggle_mode = true
		b.name = n
		menu.add_child(b)
	var name_edit := NovaMnuEdit.new()
	name_edit.name = "PLAYERNAME"
	menu.add_child(name_edit)
	return menu


func _combo(menu: Node, name: String) -> NovaMnuCombo:
	return menu.find_child(name, true, false) as NovaMnuCombo


func test_owns_menu_detects_player_info() -> void:
	var host := PlayerInfoMenuHost.new()
	var menu := _make_menu()
	assert_true(host.owns_menu(menu), "a menu carrying NATIONALITY + COMBO_LIST is the PLAYER_INFO screen")
	var plain := Node.new()
	add_child_autofree(plain)
	assert_false(host.owns_menu(plain), "a plain menu is left to the shell / other companions")


func test_populates_avatar_lists_and_combo_label() -> void:
	var host := PlayerInfoMenuHost.new()
	var db := _load_db()
	host._db = db  # inject directly (no resource root in the unit)
	var menu := _make_menu()
	host.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)

	assert_gt(_combo(menu, "NATIONALITY").get_item_count(), 0, "nationalities populate")
	# The initial team-0 cascade selects the first good nationality (US, index 0) and its
	# first division (SEAL): division + combo lists fill from the avatar tree.
	assert_eq(_combo(menu, "DIVISION").get_item_count(), 9, "US has 9 divisions")
	var combos := _combo(menu, "COMBO_LIST")
	assert_eq(combos.get_item_count(), 4, "the SEAL division has 4 combos")

	# Each combo row is "<head display> - <body display>" (raw keys without an RTXT table).
	var c0: Dictionary = db.get_combo(0, 0, 0)
	var head: Dictionary = c0.get("head", {})
	var body: Dictionary = c0.get("body", {})
	var expected := "%s - %s" % [String(head.get("display_name", "")), String(body.get("display_name", ""))]
	assert_eq(combos.get_item_text(0), expected, "combo row is the last - first character label")


func test_division_change_refills_combos() -> void:
	var host := PlayerInfoMenuHost.new()
	var db := _load_db()
	host._db = db
	var menu := _make_menu()
	host.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)
	var div := _combo(menu, "DIVISION")
	var combos := _combo(menu, "COMBO_LIST")

	# Selecting US division 1 refills the combo list with that division's combos (cascade).
	var div1_count := db.get_combo_count(0, 1)
	div.item_selected.emit(1, div.get_item_text(1))
	assert_eq(combos.get_item_count(), div1_count, "a division change refills COMBO_LIST")


func test_team_filter_partitions_nationalities_by_alignment() -> void:
	var host := PlayerInfoMenuHost.new()
	var db := _load_db()
	host._db = db
	var menu := _make_menu()
	host.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)

	# Team 0 (blue/SIDE_BLUE default): every shown nationality is good-aligned.
	var good_rows: Array = host._nat_db_index.duplicate()
	assert_gt(good_rows.size(), 0, "at least one good nationality")
	for i in good_rows:
		assert_eq(int(db.get_nationality(i).get("alignment", -1)), NovaAvatarDatabase.ALIGN_GOOD,
			"team 0 shows only good-aligned nationalities")

	# Switching to SIDE_RED (team 1) re-filters to evil-aligned nationalities.
	menu.find_child("SIDE_RED", true, false).emit_signal("pressed")
	for i in host._nat_db_index:
		assert_eq(int(db.get_nationality(i).get("alignment", -1)), NovaAvatarDatabase.ALIGN_EVIL,
			"team 1 shows only evil-aligned nationalities")

	# The two teams partition every nationality (good->blue, evil->red; D-PLAYERINFO-5).
	assert_eq(good_rows.size() + host._nat_db_index.size(), db.get_nationality_count(),
		"good + evil = all nationalities")


func test_initial_team_follows_checked_side_radio() -> void:
	var host := PlayerInfoMenuHost.new()
	host._db = _load_db()
	var menu := _make_menu()
	(menu.find_child("SIDE_RED", true, false) as BaseButton).button_pressed = true
	host.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)
	assert_eq(host._team, 1, "the initial team follows the checked SIDE_RED radio")


func test_degrades_without_avatar_db() -> void:
	var host := PlayerInfoMenuHost.new()
	var menu := _make_menu()
	# No resource root and no injected db: the screen wires up but the combos stay empty.
	host.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)
	assert_eq(_combo(menu, "NATIONALITY").get_item_count(), 0, "no Avatars.def -> empty combos, no crash")


func test_mounts_3d_preview_when_widget_present() -> void:
	var host := PlayerInfoMenuHost.new()
	host._db = _load_db()
	var menu := _make_menu()
	var preview_rect := Control.new()
	preview_rect.name = "PLAYER_PREVIEW"
	menu.add_child(preview_rect)
	# No resource root, so the preview mounts but loads no .3di (graceful); we only
	# assert the surface is wired into PLAYER_PREVIEW.
	host.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)
	assert_not_null(preview_rect.find_child("PlayerInfoAvatarPreview", true, false),
		"the 3D character preview is mounted into PLAYER_PREVIEW")


func test_snapshot_reports_current_selection() -> void:
	var host := PlayerInfoMenuHost.new()
	host._db = _load_db()
	var menu := _make_menu()
	host.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)
	(menu.find_child("PLAYERNAME", true, false) as LineEdit).text = "Ghost"
	var snap := host.snapshot()
	assert_eq(String(snap.get("name", "")), "Ghost", "snapshot carries the player name")
	assert_eq(int(snap.get("team", -1)), 0, "snapshot carries the team")
	assert_eq(int(snap.get("nationality", -1)), 0, "snapshot carries the selected nationality index")
