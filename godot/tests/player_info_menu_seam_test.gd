extends GutTest

# The player.mnu PLAYER_INFO screen seam: the companion (player_info_menu_companion.gd)
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
	var selected := {
		"team": 0,
		"nationality": 0,
		"division": 0,
		"combo": 1,
		"player_class": 6,
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


func test_owns_menu_detects_player_info() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var menu := _make_menu()
	assert_true(companion.owns_menu(menu), "a menu carrying NATIONALITY + COMBO_LIST is the PLAYER_INFO screen")
	var plain := Node.new()
	add_child_autofree(plain)
	assert_false(companion.owns_menu(plain), "a plain menu is left to the shell / other companions")


func test_populates_avatar_lists_and_combo_label() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var db := _load_db()
	companion._db = db  # inject directly (no resource root in the unit)
	var menu := _make_menu()
	companion.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)

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
	NovaStrings.register_table("gameui", t)

	var menu := _make_menu()
	companion.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)

	assert_eq(_combo(menu, "NATIONALITY").get_item_text(0), "United States",
		"nationality resolves via the gametext Avatars section")
	var expected_combo := "Boonie Hat - %s" % ("Boonie Hat" if body_key == head_key else "Camo BDU")
	assert_eq(_combo(menu, "COMBO_LIST").get_item_text(0), expected_combo,
		"combo label resolves head/body display names via Avatars")


func test_division_change_refills_combos() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var db := _load_db()
	companion._db = db
	var menu := _make_menu()
	companion.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)
	var div := _combo(menu, "DIVISION")
	var combos := _combo(menu, "COMBO_LIST")

	# Selecting US division 1 refills the combo list with that division's combos (cascade).
	var div1_count := db.get_combo_count(0, 1)
	div.item_selected.emit(1, div.get_item_text(1))
	assert_eq(combos.get_item_count(), div1_count, "a division change refills COMBO_LIST")


func test_team_filter_partitions_nationalities_by_alignment() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var db := _load_db()
	companion._db = db
	var menu := _make_menu()
	companion.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)

	# Team 0 (blue/SIDE_BLUE default): every shown nationality is good-aligned.
	var good_rows: Array = companion._nat_db_index.duplicate()
	assert_gt(good_rows.size(), 0, "at least one good nationality")
	for i in good_rows:
		assert_eq(int(db.get_nationality(i).get("alignment", -1)), NovaAvatarDatabase.ALIGN_GOOD,
			"team 0 shows only good-aligned nationalities")

	# Switching to SIDE_RED (team 1) re-filters to evil-aligned nationalities.
	menu.find_child("SIDE_RED", true, false).emit_signal("pressed")
	for i in companion._nat_db_index:
		assert_eq(int(db.get_nationality(i).get("alignment", -1)), NovaAvatarDatabase.ALIGN_EVIL,
			"team 1 shows only evil-aligned nationalities")

	# The two teams partition every nationality (good->blue, evil->red; D-PLAYERINFO-5).
	assert_eq(good_rows.size() + companion._nat_db_index.size(), db.get_nationality_count(),
		"good + evil = all nationalities")


func test_initial_team_follows_checked_side_radio() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion._db = _load_db()
	var menu := _make_menu()
	(menu.find_child("SIDE_RED", true, false) as BaseButton).button_pressed = true
	companion.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)
	assert_eq(companion._team, 1, "the initial team follows the checked SIDE_RED radio")


func test_degrades_without_avatar_db() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var menu := _make_menu()
	# No resource root and no injected db: the screen wires up but the combos stay empty.
	companion.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)
	assert_eq(_combo(menu, "NATIONALITY").get_item_count(), 0, "no Avatars.def -> empty combos, no crash")


func test_mounts_3d_preview_when_widget_present() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion._db = _load_db()
	var menu := _make_menu()
	var preview_rect := Control.new()
	preview_rect.name = "PLAYER_PREVIEW"
	menu.add_child(preview_rect)
	# No resource root, so the preview mounts but loads no .3di (graceful); we only
	# assert the surface is wired into PLAYER_PREVIEW.
	companion.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)
	assert_not_null(preview_rect.find_child("PlayerInfoAvatarPreview", true, false),
		"the 3D character preview is mounted into PLAYER_PREVIEW")


func test_snapshot_reports_current_selection() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion._db = _load_db()
	var menu := _make_menu()
	companion.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)
	(menu.find_child("PLAYERNAME", true, false) as LineEdit).text = "Ghost"
	var snap := companion.snapshot()
	assert_eq(String(snap.get("name", "")), "Ghost", "snapshot carries the player name")
	assert_eq(int(snap.get("team", -1)), 0, "snapshot carries the team")
	assert_eq(int(snap.get("nationality", -1)), 0, "snapshot carries the selected nationality index")


func test_accept_emits_avatar_chosen() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion._db = _load_db()
	watch_signals(companion)
	var menu := _make_menu()
	companion.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)
	(menu.find_child("PLAYERNAME", true, false) as LineEdit).text = "Sandman"
	menu.find_child("ACCEPT", true, false).emit_signal("pressed")
	assert_signal_emitted(companion, "avatar_chosen", "OK commits the chosen avatar")
	var profile: Dictionary = get_signal_parameters(companion, "avatar_chosen")[0]
	assert_eq(String(profile.get("name", "")), "Sandman", "the committed profile carries the name")
	assert_eq(int(profile.get("nationality", -1)), 0, "the committed profile carries the selection")


func test_voice_preview_requests_selected_avatar_voice() -> void:
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/avatars")), OK,
		"the avatar fixture directory mounts as a retail resource root")

	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(
			FileAccess.get_file_as_bytes("res://../fixtures/mnu/jo_player.mnu")), OK,
		"the retail player menu fixture loads")
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_edit_mode(false)
	menu.set_resource_root(root)
	menu.menu = doc

	var companion := PlayerInfoMenuCompanion.new()
	companion.on_menu_built(menu, "player.mnu", "PLAYER_INFO", root)
	watch_signals(menu)
	var preview := menu.find_child("TESTPLAYERVOICE", true, false) as BaseButton
	assert_not_null(preview, "the retail PLAYER_INFO screen builds its voice-preview button")
	preview.pressed.emit()

	# The fixture's initially selected US/SEAL head carries voice 1. Retail formats
	# that avatar-derived fallback as VOICE_1 and plays it from the dedicated menu.lwf
	# bank. [orig: PlayerInfo_PreviewVoice @ 0x55ff70]
	assert_signal_emitted_with_parameters(menu, "sound_requested", ["menu.lwf", "VOICE_1"])


# --- Loadout (PRIMARY/SECONDARY/ACCESSORY) ------------------------------------

const WEAPON_FIXTURE := "res://../fixtures/def/weapon.def"


func _load_weapons() -> NovaWeaponDatabase:
	var wdb := NovaWeaponDatabase.new()
	var path := ProjectSettings.globalize_path(WEAPON_FIXTURE)
	assert_eq(wdb.load(path), OK, "weapon.def fixture loads")
	return wdb


# A stand-in PLAYER_INFO menu with the loadout controls: the three weapon combos,
# PLAYERCLASS carrying the CHARTYPE values 5..9 (as the .mnu's static items do), the
# ammo/type/grenade combos, the STATIC_TOTAL_WEIGHT label shape the builder makes
# (Control + child "Label"), and the bare *_ICON windows.
func _make_loadout_menu() -> Node:
	var menu := Node.new()
	menu.name = "Menu"
	add_child_autofree(menu)
	for n in ["PRIMARY", "SECONDARY", "ACCESSORY"]:
		var c := NovaMnuCombo.new()
		c.name = n
		menu.add_child(c)
		for suffix in ["_AMMO1", "_AMMO2"]:
			var ammo := NovaMnuCombo.new()
			ammo.name = n + suffix
			menu.add_child(ammo)
		var icon := Control.new()
		icon.name = n + "_ICON"
		menu.add_child(icon)
	for n in ["PRIMARY", "SECONDARY"]:
		# The .mnu authors the TYPE statics (FMJ/AP/SP, values 0/1/2); the companion
		# selects/locks them but never refills.
		var t := NovaMnuCombo.new()
		t.name = n + "_AMMO1_TYPE"
		t.add_item("FMJ", "0")
		t.add_item("AP", "1")
		t.add_item("SP", "2")
		t.select_silent(0)
		menu.add_child(t)
	for n in ["GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"]:
		var g := NovaMnuCombo.new()
		g.name = n
		menu.add_child(g)
	var weight := Control.new()
	weight.name = "STATIC_TOTAL_WEIGHT"
	var weight_label := Label.new()
	weight_label.name = "Label"
	weight.add_child(weight_label)
	menu.add_child(weight)
	var cls := NovaMnuCombo.new()
	cls.name = "PLAYERCLASS"
	for v in range(5, 10):  # Medic..Engineer = values 5..9
		cls.add_item("class %d" % v, str(v))
	cls.select_silent(0)  # Medic (value 5) -> class mask 1
	menu.add_child(cls)
	return menu


var _ammo_menu: Node = null  # the stand-in menu behind the current _make_ammo_companion


# A wired companion over the stand-in loadout menu (medic/blue), for the ammo cases —
# built through the public seams only (set_weapon_database + on_menu_built).
func _make_ammo_companion(wdb: NovaWeaponDatabase = null) -> PlayerInfoMenuCompanion:
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_weapon_database(wdb if wdb != null else _load_weapons())
	_ammo_menu = _make_loadout_menu()
	companion.on_menu_built(_ammo_menu, "player.mnu", "PLAYER_INFO", null)
	return companion


func _ammo_control(name: String) -> NovaMnuCombo:
	return _ammo_menu.find_child(name, true, false) as NovaMnuCombo


# Select the named weapon in a parent slot combo and fire the selection handler the
# way a user pick would. The row model is public: row = position in the filtered
# slot list + 1 (row 0 is NONE) [orig: populate_weapon_slot_lists @ 0x560430].
func _select_weapon(wdb: NovaWeaponDatabase, control: String, slot: int,
		class_mask: int, weapon_name: String) -> Dictionary:
	var combo := _ammo_control(control)
	var defs: Array = wdb.get_slot_weapons(slot, class_mask, 2)  # blue
	for i in defs.size():
		var w := defs[i] as Dictionary
		if String(w.get("name", "")).nocasecmp_to(weapon_name) == 0:
			combo.select(i + 1)  # select() emits item_selected -> the witnessed refill
			return w
	assert_true(false, "%s offers %s" % [control, weapon_name])
	return {}


# The expected *_AMMO2 sub-weapon, computed from the public table walk the companion
# mirrors [orig: the stricmp walk in populate_ammo_combo_boxes @ 0x55def0].
func _expected_sub(wdb: NovaWeaponDatabase, parent: Dictionary) -> Dictionary:
	var parent_round := String(parent.get("round_type", ""))
	for k in range(1, int(parent.get("loadout_subclasses", 0)) + 1):
		var cand := wdb.get_weapon(int(parent.get("index", -1)) + k)
		if cand.is_empty():
			return {}
		if String(cand.get("round_type", "")).nocasecmp_to(parent_round) != 0:
			return cand
	return {}


func _combo_texts(c: NovaMnuCombo) -> Array:
	var out: Array = []
	for i in c.get_item_count():
		out.append(c.get_item_text(i))
	return out


func test_populates_loadout_slots_filtered_by_class_and_team() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var wdb := _load_weapons()
	companion._weapons = wdb
	companion._menu = _make_loadout_menu()
	companion._team = 0  # blue -> team mask 2
	companion._populate_loadout()

	var primary := companion._menu.find_child("PRIMARY", true, false) as NovaMnuCombo
	# Medic (class mask 1), blue (team mask 2): the DB's filtered set plus a leading NONE row.
	var expected := wdb.get_slot_weapons(NovaWeaponDatabase.SLOT_PRIMARY, 1, 2)
	assert_gt(expected.size(), 0, "the fixture has medic/blue primary weapons")
	assert_eq(primary.get_item_count(), expected.size() + 1, "PRIMARY = NONE + the filtered weapons")
	assert_eq(primary.get_item_text(0), "None", "NONE leads the slot (fallback with no string table)")


func test_loadout_class_filter_includes_and_excludes() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	companion._weapons = _load_weapons()
	companion._menu = _make_loadout_menu()
	companion._team = 0
	var primary := companion._menu.find_child("PRIMARY", true, false) as NovaMnuCombo

	# Medic (value 5): WPN_M4AUTO (charfilter medic|rifleman|engineer, blue) is a primary -> present.
	companion._populate_loadout()
	assert_true(_combo_texts(primary).has("WPN_M4AUTO"), "M4 shows for Medic")

	# Sniper (value 6): M4's charfilter excludes sniper -> absent after re-fill.
	(companion._menu.find_child("PLAYERCLASS", true, false) as NovaMnuCombo).select_silent(1)
	companion._populate_loadout()
	assert_false(_combo_texts(primary).has("WPN_M4AUTO"), "M4 is hidden for Sniper")


func test_snapshot_carries_the_selected_loadout_weapon_ids() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var wdb := _load_weapons()
	companion.set_weapon_database(wdb)
	var menu := _make_loadout_menu()
	companion.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)

	var primary_defs := wdb.get_slot_weapons(
			NovaWeaponDatabase.SLOT_PRIMARY, 1, 2)
	var selected_primary := ""
	var selected_row := -1
	for i in primary_defs.size():
		var weapon: Dictionary = primary_defs[i]
		if String(weapon.get("name", "")).nocasecmp_to("WPN_M4AUTO") == 0:
			selected_primary = String(weapon.get("name", ""))
			selected_row = i + 1 # row 0 is NONE
			break
	assert_false(selected_primary.is_empty(), "the fixture offers M4AUTO for medic/blue")
	var primary := menu.find_child("PRIMARY", true, false) as NovaMnuCombo
	primary.select_silent(selected_row)

	var profile := companion.snapshot()
	assert_eq(String(profile.get("primary", "")), selected_primary,
		"PLAYER_INFO ACCEPT preserves the selected primary's weapon.def id")
	assert_eq(int(profile.get("player_class", 0)), 5,
		"the selected class travels with the spawn loadout")


func test_snapshot_carries_class_without_a_weapon_database() -> void:
	var companion := PlayerInfoMenuCompanion.new()
	var menu := _make_loadout_menu()
	companion.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)

	var profile := companion.snapshot()
	assert_eq(int(profile.get("player_class", 0)), 5,
		"class selection does not depend on weapon.def loading")
	assert_false(profile.has("primary"),
		"missing weapon.def remains distinct from an explicit all-NONE kit")


# End-to-end against the REAL player.mnu (built controls + nesting), so the loadout fills
# through the same control names/tree the runtime uses, not just a stand-in. Runs in GUT so
# the autoloads (NovaStrings) and the GDExtension are loaded.
func test_real_player_mnu_loadout_populates() -> void:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes("res://../fixtures/mnu/jo_player.mnu"))
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_edit_mode(false)
	menu.menu = doc

	var primary := menu.find_child("PRIMARY", true, false) as NovaMnuCombo
	var pclass := menu.find_child("PLAYERCLASS", true, false) as NovaMnuCombo
	assert_not_null(primary, "the real player.mnu builds a PRIMARY combobox")
	assert_not_null(pclass, "the real player.mnu builds a PLAYERCLASS combobox")
	assert_gt(pclass.get_item_count(), 0, "PLAYERCLASS carries its static class items")

	var companion := PlayerInfoMenuCompanion.new()
	companion._db = _load_db()
	companion._weapons = _load_weapons()  # injected (root-less unit), as if weapon.def had loaded
	companion.on_menu_built(menu, "player.mnu", "PLAYER_INFO", null)

	assert_gt(primary.get_item_count(), 1,
		"PRIMARY populates (NONE + weapons) through the real menu's control tree")
	# The ammo/weight tail fills through the same real control tree: the default
	# NONE selection hides PRIMARY_AMMO1, and the weight label renders the
	# witnessed "%s %.1f %s (%s)" shape (fallback strings, no tables registered).
	var primary_ammo := menu.find_child("PRIMARY_AMMO1", true, false) as NovaMnuCombo
	assert_not_null(primary_ammo, "the real player.mnu builds PRIMARY_AMMO1")
	assert_false(primary_ammo.visible, "NONE selected -> the ammo combo hides")
	var weight := menu.find_child("STATIC_TOTAL_WEIGHT", true, false)
	assert_not_null(weight, "the real player.mnu builds STATIC_TOTAL_WEIGHT")
	var weight_label := weight.find_child("Label", false, false) as Label
	assert_not_null(weight_label, "the built static carries its Label child")
	# The built PLAYERCLASS carries a default selection, so default grenades may
	# already weigh in — pin the witnessed "%s %.1f %s (%s)" shape, not the sum.
	assert_true(weight_label.text.begins_with("Total Weight "),
		"the weight readout renders through the real static's label")
	assert_true(weight_label.text.contains(" lbs ("),
		"the readout carries the witnessed format tail")


# --- Ammo combos + weight + icons (D-PLAYERINFO-11) -----------------------------

func test_primary_ammo_rows_follow_selected_weapon() -> void:
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	var w := _select_weapon(wdb, "PRIMARY", NovaWeaponDatabase.SLOT_PRIMARY, 1, "WPN_M4AUTO")
	var ammo := _ammo_control("PRIMARY_AMMO1")
	var maxclips := int(w.get("maxclips", 0))
	assert_true(ammo.visible, "a clip-carrying weapon shows its ammo combo")
	assert_eq(ammo.get_item_count(), maxclips,
		"rows 1..maxclips [orig: populate_ammo_combo_boxes @ 0x55def0]")
	assert_eq(ammo.get_item_text(0),
		"%d - %s" % [int(w.get("clipsize", 0)), String(w.get("round_type", ""))],
		"row labels are the witnessed \"%d - %s\" rounds + round type")
	assert_eq(ammo.get_selected(), maxclips - 1,
		"the untouched default selects the full (maxclips) row")


func test_none_selection_hides_ammo_and_clears_icon() -> void:
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	_select_weapon(wdb, "PRIMARY", NovaWeaponDatabase.SLOT_PRIMARY, 1, "WPN_M4AUTO")
	var primary := _ammo_control("PRIMARY")
	primary.select(0)  # back to NONE
	assert_false(_ammo_control("PRIMARY_AMMO1").visible, "NONE hides the ammo combo")
	var icon := _ammo_menu.find_child("PRIMARY_ICON", true, false)
	var rect := icon.get_node_or_null("LoadoutIcon") as TextureRect
	assert_not_null(rect, "the companion mounts an icon rect into the bare window")
	assert_null(rect.texture, "NONE clears the icon texture")


func test_m203_subweapon_fills_ammo2_from_the_differing_round_entry() -> void:
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	# The M203 carbines are rifleman-filtered; switch PLAYERCLASS to Rifleman
	# (value 8, row 3) so the slot list offers them.
	_ammo_control("PLAYERCLASS").select(3)
	var w := _select_weapon(wdb, "PRIMARY", NovaWeaponDatabase.SLOT_PRIMARY, 8, "WPN_M4M203AUTO")
	# The witnessed walk skips same-round WPN_M4M203 and lands on WPN_M4M203HE
	# (AMMO_M203_40MM_NADE) within loadout_subclasses = 2.
	var sub := _expected_sub(wdb, w)
	assert_eq(String(sub.get("name", "")), "WPN_M4M203HE",
		"the sub walk lands on the first DIFFERING round_type entry")
	var ammo2 := _ammo_control("PRIMARY_AMMO2")
	assert_true(ammo2.visible, "a live sub-weapon shows *_AMMO2")
	assert_eq(ammo2.get_item_count(), int(sub.get("maxclips", 0)),
		"*_AMMO2 rows come from the SUB-weapon's maxclips")
	assert_eq(ammo2.get_item_text(0),
		"%d - %s" % [int(sub.get("clipsize", 0)), String(sub.get("round_type", ""))],
		"*_AMMO2 labels use the sub-weapon's clipsize + round type")


func test_grenade_combos_fill_in_table_order_with_zero_row() -> void:
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	var expected: Array = wdb.get_slot_weapons(
			NovaWeaponDatabase.SLOT_GRENADE, 1, 2)  # medic/blue
	assert_gt(expected.size(), 0, "the fixture carries medic/blue grenades")
	for i in mini(expected.size(), 3):
		var combo := _ammo_control("GRENADE_AMMO%d" % (i + 1))
		var w := expected[i] as Dictionary
		assert_true(combo.visible, "an owned grenade control shows")
		assert_eq(combo.get_item_count(), int(w.get("maxclips", 0)) + 1,
			"grenade rows are 0..maxclips INCLUDING the zero row [orig: @ 0x55def0]")
		assert_eq(combo.get_item_text(0), "0 - %s" % String(w.get("round_type", "")),
			"row 0 is the zero-rounds row")
		assert_eq(combo.get_selected(), int(w.get("maxclips", 0)),
			"the untouched default selects the full row")
	for i in range(expected.size(), 3):
		assert_false(_ammo_control("GRENADE_AMMO%d" % (i + 1)).visible,
			"unowned grenade controls hide")


func test_weight_label_renders_witnessed_format_and_band() -> void:
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	var w := _select_weapon(wdb, "PRIMARY", NovaWeaponDatabase.SLOT_PRIMARY, 1, "WPN_M4AUTO")
	# Expected parent term [orig: calculate_loadout_weight @ 0x55f1f0]:
	# weight + maxclips*clip_weight (untouched default), plus the sub-weapon and
	# full-grenade clip-only terms the fill selects by default.
	var expected := float(w.get("weight", 0.0)) \
			+ int(w.get("maxclips", 0)) * float(w.get("clip_weight", 0.0))
	var sub := _expected_sub(wdb, w)
	if not sub.is_empty() and int(sub.get("clipsize", 0)) > 0:
		expected += int(sub.get("maxclips", 0)) * float(sub.get("clip_weight", 0.0))
	for g in wdb.get_slot_weapons(NovaWeaponDatabase.SLOT_GRENADE, 1, 2).slice(0, 3):
		expected += int(g.get("maxclips", 0)) * float(g.get("clip_weight", 0.0))
	var label := _ammo_menu.find_child("STATIC_TOTAL_WEIGHT", true, false) \
			.find_child("Label", false, false) as Label
	var band := "Light"
	if expected >= 66.6:
		band = "Heavy"
	elif expected >= 33.3:
		band = "Normal"
	assert_eq(label.text, "Total Weight %.1f lbs (%s)" % [expected, band],
		"the witnessed \"%s %.1f %s (%s)\" readout over the native weight math")
	assert_eq(wdb.encumbrance_class(0.0), NovaWeaponDatabase.ENCUMBRANCE_LIGHT)
	assert_eq(wdb.encumbrance_class(33.3), NovaWeaponDatabase.ENCUMBRANCE_NORMAL)
	assert_eq(wdb.encumbrance_class(66.6), NovaWeaponDatabase.ENCUMBRANCE_HEAVY)


func test_ammo_selection_recomputes_weight_and_snapshot() -> void:
	var wdb := _load_weapons()
	var companion := _make_ammo_companion(wdb)
	var w := _select_weapon(wdb, "PRIMARY", NovaWeaponDatabase.SLOT_PRIMARY, 1, "WPN_M4AUTO")
	assert_eq(companion.selected_clips("PRIMARY"), -1,
		"untouched ammo reports the -1 default [orig: the '-1' kit filler]")
	var label := _ammo_menu.find_child("STATIC_TOTAL_WEIGHT", true, false) \
			.find_child("Label", false, false) as Label
	var before := label.text
	var ammo := _ammo_control("PRIMARY_AMMO1")
	ammo.select(0)  # one clip
	assert_eq(companion.selected_clips("PRIMARY"), 1,
		"the pick records row+1 [orig: @ 0x55f730]")
	assert_ne(label.text, before, "an ammo pick recomputes the weight readout")
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
	var wdb := NovaWeaponDatabase.new()
	assert_eq(wdb.load(path), OK)
	var companion := _make_ammo_companion(wdb)
	var type_combo := _ammo_control("PRIMARY_AMMO1_TYPE")

	_select_weapon(wdb, "PRIMARY", NovaWeaponDatabase.SLOT_PRIMARY, 1, "WPN_PLAIN")
	assert_true(type_combo.visible, "a clip weapon shows the TYPE combo")
	assert_false(type_combo.disabled, "a normal weapon leaves the TYPE combo live")

	_select_weapon(wdb, "PRIMARY", NovaWeaponDatabase.SLOT_PRIMARY, 1, "WPN_LOCKED")
	assert_true(type_combo.disabled,
		"flags2 NOAMMOTYPES locks the TYPE combo [orig: @ 0x55def0 +188 & 0x40]")
	assert_eq(type_combo.get_selected(), 0, "the lock resets the type to 0 (FMJ)")
	assert_eq(companion.selected_ammo_type("PRIMARY"), 0, "the stored byte resets too")
	DirAccess.remove_absolute(path)


func test_grenade_zero_pick_stays_zero_in_the_weight() -> void:
	# The witnessed asymmetry [orig: calculate_loadout_weight @ 0x55f1f0]:
	# grenades default -1 -> maxclips, but a PICKED 0 stays 0 (the zero row) —
	# unlike the parents' <=0 -> maxclips rule.
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	var g := wdb.get_slot_weapons(NovaWeaponDatabase.SLOT_GRENADE, 1, 2)[0] as Dictionary
	assert_gt(float(g.get("clip_weight", 0.0)) * int(g.get("maxclips", 0)), 0.0,
		"the first grenade def carries weighable clips")
	var label := _ammo_menu.find_child("STATIC_TOTAL_WEIGHT", true, false) \
			.find_child("Label", false, false) as Label
	var default_text := label.text  # -1 default = full grenades weighed in
	_ammo_control("GRENADE_AMMO1").select(0)  # the zero row
	assert_ne(label.text, default_text,
		"picking the zero row removes that grenade's clip term")
	var zero_total := float(label.text.get_slice(" ", 2))
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
