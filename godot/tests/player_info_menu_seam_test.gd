extends GutTest

# The player.mnu PLAYER_INFO screen seam: the companion (player_info_menu_host.gd)
# drives the JO character screen by control NAME -- fills NATIONALITY / DIVISION /
# COMBO_LIST / PLAYERVOICE from Avatars.def, runs the nationality->division->combo
# cascade, and re-filters by the SIDE_BLUE/SIDE_RED team. This pins the wiring against
# the witnessed original (docs/playerinfo/avatars-re.md, D-PLAYERINFO-5/7); the live
# in-engine render is the manual smoke. Avatars.def comes from the committed retail
# fixture, injected directly (root-less) like the avatar data tests.

const AVATARS_FIXTURE := "res://../fixtures/avatars/Avatars.def"


# NovaStrings is a shared autoload; keep the registry clean so the raw-key tests below see
# no gametext table and the resolved-names test starts from a known state.
func before_each() -> void:
	NovaStrings.clear()


func after_all() -> void:
	NovaStrings.clear()


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
	var accept := Button.new()
	accept.name = "ACCEPT"
	menu.add_child(accept)
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

	# Each combo row is "<head display> - <body display>". With no gametext table registered
	# (before_each cleared NovaStrings) the names fall back to their raw keys.
	var c0: Dictionary = db.get_combo(0, 0, 0)
	var head: Dictionary = c0.get("head", {})
	var body: Dictionary = c0.get("body", {})
	var expected := "%s - %s" % [String(head.get("display_name", "")), String(body.get("display_name", ""))]
	assert_eq(combos.get_item_text(0), expected, "combo row is the last - first character label")


# With the gametext table's "Avatars" section registered (as the shell does from Game.bin at
# boot), the host resolves nationality + combo display keys to friendly names instead of the
# raw AV_* keys [orig: GameText_GetStringWithFallback @ 0x51eb90, "Avatars" section].
func test_resolves_friendly_names_from_gametext_avatars_section() -> void:
	var host := PlayerInfoMenuHost.new()
	var db := _load_db()
	host._db = db

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
	NovaStrings.register_table("gametext", t)

	var menu := _make_menu()
	host.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)

	assert_eq(_combo(menu, "NATIONALITY").get_item_text(0), "United States",
		"nationality resolves via the gametext Avatars section")
	var expected_combo := "Boonie Hat - %s" % ("Boonie Hat" if body_key == head_key else "Camo BDU")
	assert_eq(_combo(menu, "COMBO_LIST").get_item_text(0), expected_combo,
		"combo label resolves head/body display names via Avatars")


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


func test_accept_emits_avatar_chosen() -> void:
	var host := PlayerInfoMenuHost.new()
	host._db = _load_db()
	watch_signals(host)
	var menu := _make_menu()
	host.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)
	(menu.find_child("PLAYERNAME", true, false) as LineEdit).text = "Sandman"
	menu.find_child("ACCEPT", true, false).emit_signal("pressed")
	assert_signal_emitted(host, "avatar_chosen", "OK commits the chosen avatar")
	var profile: Dictionary = get_signal_parameters(host, "avatar_chosen")[0]
	assert_eq(String(profile.get("name", "")), "Sandman", "the committed profile carries the name")
	assert_eq(int(profile.get("nationality", -1)), 0, "the committed profile carries the selection")


# --- Loadout (PRIMARY/SECONDARY/ACCESSORY) ------------------------------------

const WEAPON_FIXTURE := "res://../fixtures/def/weapon.def"


func _load_weapons() -> NovaWeaponDatabase:
	var wdb := NovaWeaponDatabase.new()
	var path := ProjectSettings.globalize_path(WEAPON_FIXTURE)
	assert_eq(wdb.load(path), OK, "weapon.def fixture loads")
	return wdb


# A stand-in PLAYER_INFO menu with just the loadout controls: the three weapon combos plus
# PLAYERCLASS carrying the CHARTYPE values 5..9 (as the .mnu's static items do).
func _make_loadout_menu() -> Node:
	var menu := Node.new()
	menu.name = "Menu"
	add_child_autofree(menu)
	for n in ["PRIMARY", "SECONDARY", "ACCESSORY"]:
		var c := NovaMnuCombo.new()
		c.name = n
		menu.add_child(c)
	var cls := NovaMnuCombo.new()
	cls.name = "PLAYERCLASS"
	for v in range(5, 10):  # Medic..Engineer = values 5..9
		cls.add_item("class %d" % v, str(v))
	cls.select_silent(0)  # Medic (value 5) -> class mask 1
	menu.add_child(cls)
	return menu


func _combo_texts(c: NovaMnuCombo) -> Array:
	var out: Array = []
	for i in c.get_item_count():
		out.append(c.get_item_text(i))
	return out


func test_populates_loadout_slots_filtered_by_class_and_team() -> void:
	var host := PlayerInfoMenuHost.new()
	var wdb := _load_weapons()
	host._weapons = wdb
	host._menu = _make_loadout_menu()
	host._team = 0  # blue -> team mask 2
	host._populate_loadout()

	var primary := host._menu.find_child("PRIMARY", true, false) as NovaMnuCombo
	# Medic (class mask 1), blue (team mask 2): the DB's filtered set plus a leading NONE row.
	var expected := wdb.get_slot_weapons(NovaWeaponDatabase.SLOT_PRIMARY, 1, 2)
	assert_gt(expected.size(), 0, "the fixture has medic/blue primary weapons")
	assert_eq(primary.get_item_count(), expected.size() + 1, "PRIMARY = NONE + the filtered weapons")
	assert_eq(primary.get_item_text(0), "None", "NONE leads the slot (fallback with no string table)")


func test_loadout_class_filter_includes_and_excludes() -> void:
	var host := PlayerInfoMenuHost.new()
	host._weapons = _load_weapons()
	host._menu = _make_loadout_menu()
	host._team = 0
	var primary := host._menu.find_child("PRIMARY", true, false) as NovaMnuCombo

	# Medic (value 5): WPN_M4AUTO (charfilter medic|rifleman|engineer, blue) is a primary -> present.
	host._populate_loadout()
	assert_true(_combo_texts(primary).has("WPN_M4AUTO"), "M4 shows for Medic")

	# Sniper (value 6): M4's charfilter excludes sniper -> absent after re-fill.
	(host._menu.find_child("PLAYERCLASS", true, false) as NovaMnuCombo).select_silent(1)
	host._populate_loadout()
	assert_false(_combo_texts(primary).has("WPN_M4AUTO"), "M4 is hidden for Sniper")
