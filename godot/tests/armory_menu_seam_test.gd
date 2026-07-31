extends GutTest

# The in-game armory seam: weapon.mnu's WEAPON screen driven by the ArmoryMenuCompanion
# companion (godot/engine/world/armory_menu_companion.gd). Pins the witnessed wiring [orig:
# WeaponDef_RegisterUICallbacks @0x567020 registers PLAYER_CLASS / PRIMARY / SECONDARY /
# ACCESSORY / *_AMMO / ACCEPT / CANCEL on the "WEAPON" screen; population
# populate_three_category_lists @0x566db0 (sorted rows, NONE at 0); class resolution
# Armory_ResolveSelectedClass @0x5642f0; weight update_weapon_weight_display @0x565640;
# ACCEPT WeaponLoadout_ApplyFromBuffer @0x565cd0]. The zone-gated OPEN path (the
# useitem key, action 177, on entity Flags 0x400000 @0x4e0b4d) lives in main_game +
# the collision resolver (ctest collision_test) — here the screen itself is the unit.

const WEAPON_FIXTURE := "res://../fixtures/def/weapon.def"


func before_each() -> void:
	NovaStrings.clear()


func after_all() -> void:
	NovaStrings.clear()


func _load_weapons() -> NovaWeaponDatabase:
	var wdb := NovaWeaponDatabase.new()
	var path := ProjectSettings.globalize_path(WEAPON_FIXTURE)
	assert_eq(wdb.load(path), OK, "weapon.def fixture loads")
	return wdb


func _load_weapons_with_weight(weapon_name: String, weight: float) -> NovaWeaponDatabase:
	# Exercise the public parser/database seam with an authored sentinel instead of
	# reaching into ArmoryMenuCompanion's private row cache. Restrict the substitution to
	# the named weapon's top-level block so identically named properties elsewhere
	# in the production-sized fixture remain untouched.
	var source := FileAccess.get_file_as_string(WEAPON_FIXTURE)
	var block_start := source.find('weapon "%s"' % weapon_name)
	assert_gte(block_start, 0, "%s exists in the weapon.def fixture" % weapon_name)
	var block_end := source.find("\nend", block_start)
	assert_gt(block_end, block_start, "%s has a complete weapon block" % weapon_name)
	var weight_start := source.find("\tweaponweight", block_start)
	assert_true(weight_start >= block_start and weight_start < block_end,
			"%s authors weaponweight in its top-level block" % weapon_name)
	var weight_end := source.find("\n", weight_start)
	assert_true(weight_end > weight_start and weight_end <= block_end,
			"%s weaponweight has a complete line" % weapon_name)
	if block_start < 0 or block_end <= block_start \
			or weight_start < block_start or weight_start >= block_end \
			or weight_end <= weight_start or weight_end > block_end:
		return NovaWeaponDatabase.new()
	source = (source.substr(0, weight_start)
			+ "\tweaponweight\t%.1f" % weight
			+ source.substr(weight_end))

	var temp_path := OS.get_temp_dir().path_join(
			"opennova_armory_grenade_weight_%d.def" % Time.get_ticks_usec())
	var output := FileAccess.open(temp_path, FileAccess.WRITE)
	assert_not_null(output, "temporary weapon.def variant opens for writing")
	if output == null:
		return NovaWeaponDatabase.new()
	output.store_string(source)
	output.close()
	var wdb := NovaWeaponDatabase.new()
	var err := wdb.load(temp_path)
	DirAccess.remove_absolute(temp_path)
	assert_eq(err, OK, "temporary weapon.def variant loads through NovaWeaponDatabase")
	return wdb


# A stand-in for the built WEAPON screen: the named NovaMnu* controls as the menu
# builder would create them from weapon.mnu.
func _make_menu() -> Node:
	var menu := Node.new()
	menu.name = "Menu"
	add_child_autofree(menu)
	var spin := NovaMnuSpinList.new()
	spin.name = "PLAYER_CLASS"
	menu.add_child(spin)
	for n in ["PRIMARY", "SECONDARY", "ACCESSORY",
			"PRIMARY_AMMO1", "SECONDARY_AMMO1", "ACCESSORY_AMMO1",
			"GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"]:
		var c := NovaMnuCombo.new()
		c.name = n
		menu.add_child(c)
	for n in ["ACCEPT", "CANCEL"]:
		var b := Button.new()
		b.name = n
		menu.add_child(b)
	var weight := Label.new()
	weight.name = "STATIC_TOTAL_WEIGHT"
	menu.add_child(weight)
	return menu


func _combo(menu: Node, name: String) -> NovaMnuCombo:
	return menu.find_child(name, true, false) as NovaMnuCombo


func _combo_texts(c: NovaMnuCombo) -> Array:
	var out: Array = []
	for i in c.get_item_count():
		out.append(c.get_item_text(i))
	return out


# The armory row label = LOADOUT_MENU_TEXTID resolved in the gametext table's WepDes
# section [orig: WeaponDef_ParseProperty @0x54d730 — GameText_GetString("WepDes",
# textid) -> g_loadoutWeaponTable entry+40, raw weapon name fallback; g_TextGameText
# loads gametext.bin @0x4a6cd0 — NOT Game.bin, the separate menu resource @0x552510].
func test_weapon_labels_resolve_from_gametext_wepdes() -> void:
	var t := RtxtStringFile.new()
	assert_eq(t.load_from_byte_array(
			FileAccess.get_file_as_bytes("res://../fixtures/rtxt/gametext.bin")), OK,
			"gametext.bin fixture loads")
	NovaStrings.register_table("gametext", t)
	var expected := t.get_string_in_section("WepDes", "WEAP_SHORT_M4")
	assert_true(not expected.is_empty(), "the fixture carries WepDes/WEAP_SHORT_M4")
	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	var menu := _make_menu()
	companion.on_menu_built(menu, "weapon.mnu", "WEAPON", null)
	var texts := _combo_texts(_combo(menu, "PRIMARY"))
	assert_has(texts, expected,
			"PRIMARY rows show the resolved WepDes name, not the raw WPN_ id")


func test_owns_menu_detects_weapon_screen() -> void:
	var companion := ArmoryMenuCompanion.new()
	var menu := _make_menu()
	assert_true(companion.owns_menu(menu), "PLAYER_CLASS + PRIMARY_AMMO1 mark the WEAPON screen")
	# player.mnu's screen (PLAYERCLASS combo, no ammo combos) is NOT claimed.
	var player_info := Node.new()
	add_child_autofree(player_info)
	var cls := NovaMnuCombo.new()
	cls.name = "PLAYERCLASS"
	player_info.add_child(cls)
	assert_false(companion.owns_menu(player_info), "the PLAYER_INFO screen stays with its own companion")


func test_unclassed_default_shows_all_weapons() -> void:
	# An unclassed player (SP spawn, class 0) resolves to class 0 with NO class filter
	# — the witnessed switch default is mask -1 = ALL weapons — and the spin merely
	# shows row 0 [orig: Armory_ResolveSelectedClass @0x5642f0;
	# SpinList_SelectItemByValue @0x64ba50 falls back to row 0].
	var companion := ArmoryMenuCompanion.new()
	var wdb := _load_weapons()
	companion.set_weapon_database(wdb)
	var menu := _make_menu()
	companion.on_menu_built(menu, "weapon.mnu", "WEAPON", null)

	var spin := menu.find_child("PLAYER_CLASS", true, false) as NovaMnuSpinList
	assert_eq(spin.get_value_count(), 5, "the five soldier classes 5..9 fill PLAYER_CLASS")
	assert_eq(spin.get_value_index(), 0, "an unclassed resolve shows row 0")

	var primary := _combo(menu, "PRIMARY")
	var expected: Array = wdb.get_slot_weapons(NovaWeaponDatabase.SLOT_PRIMARY, -1, 2)
	assert_gt(expected.size(), 0, "the fixture has blue primaries")
	assert_eq(primary.get_item_count(), expected.size() + 1, "PRIMARY = NONE + ALL blue primaries")
	assert_eq(primary.get_selected(), 0, "no equipped weapon known -> NONE stays selected")


func test_populates_classes_slots_and_ammo() -> void:
	var companion := ArmoryMenuCompanion.new()
	var wdb := _load_weapons()
	companion.set_weapon_database(wdb)
	# The screen opens on the player's current class + equipped primary [orig:
	# Armory_ResolveSelectedClass @0x5642f0; the per-class buffer reselect @0x564930].
	companion.set_player_class(8)
	companion.set_current_loadout("WPN_M4AUTO")
	var menu := _make_menu()
	companion.on_menu_built(menu, "weapon.mnu", "WEAPON", null)

	var spin := menu.find_child("PLAYER_CLASS", true, false) as NovaMnuSpinList
	assert_eq(spin.get_value_index(), 3, "the spin selects the resolved class by value (8 = row 3)")

	# Rifleman (mask 8), blue (mask 2): NONE + the filtered primaries, sorted
	# case-insensitively [orig: cmp @0x6448a0 mode (string, asc)].
	var primary := _combo(menu, "PRIMARY")
	var expected: Array = wdb.get_slot_weapons(NovaWeaponDatabase.SLOT_PRIMARY, 8, 2)
	assert_gt(expected.size(), 0, "the fixture has rifleman/blue primaries")
	assert_eq(primary.get_item_count(), expected.size() + 1, "PRIMARY = NONE + filtered weapons")
	var texts := _combo_texts(primary)
	for i in range(2, texts.size()):
		assert_true(String(texts[i - 1]).nocasecmp_to(String(texts[i])) <= 0,
			"weapon rows sort ascending: %s <= %s" % [texts[i - 1], texts[i]])
	var sel := primary.get_selected()
	assert_gt(sel, 0, "the equipped primary's row is pre-selected")
	assert_eq(String(companion.selected_weapon("PRIMARY").get("name", "")), "WPN_M4AUTO",
		"the pre-selected row is the equipped M4")

	# Retail rows are zero-based UI indices for one-based clip counts: row 0 means
	# one clip and row maxclips-1 means a full load
	# [orig: populate_ammo_type_combo_boxes @0x564c7d..0x564ce4].
	var ammo := _combo(menu, "PRIMARY_AMMO1")
	var weapon := companion.selected_weapon("PRIMARY")
	var maxclips := int(weapon.get("maxclips", 0))
	assert_eq(ammo.get_item_count(), maxclips, "PRIMARY_AMMO1 has one row per 1..maxclips")
	assert_eq(ammo.get_selected(), maxclips - 1, "full clips selects the last zero-based row")
	assert_eq(ammo.get_item_text(0), "%d - %s" % [
		int(weapon.get("clipsize", 0)), String(weapon.get("round_type", ""))],
		"row 0 displays one clip's round quantity and ammo label")
	ammo.select(0)
	assert_eq(companion.selected_clips("PRIMARY"), 1,
		"the first zero-based UI row serializes as one clip")


func test_ammo_rows_resolve_the_shipped_wepdes_labels() -> void:
	var gametext := RtxtStringFile.new()
	assert_eq(gametext.load_from_byte_array(
			FileAccess.get_file_as_bytes("res://../fixtures/rtxt/gametext.bin")), OK,
			"the shipped GameText fixture loads")
	NovaStrings.register_table("gametext", gametext)

	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	companion.set_current_loadout("WPN_M4AUTO")
	var menu := _make_menu()
	companion.on_menu_built(menu, "weapon.mnu", "WEAPON", null)

	assert_eq(_combo(menu, "PRIMARY_AMMO1").get_item_text(0), "30 - 5.56x45",
			"parent ammo rows use the shipped WepDes round label")
	assert_eq(_combo(menu, "GRENADE_AMMO1").get_item_text(0), "0 - Flashbang",
			"grenade zero rows use the shipped WepDes round label")
	assert_eq(_combo(menu, "GRENADE_AMMO1").get_item_text(1), "1 - Flashbang",
			"grenade count rows keep the localized label")


func test_class_change_refilters_slots() -> void:
	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	var menu := _make_menu()
	companion.on_menu_built(menu, "weapon.mnu", "WEAPON", null)
	var primary := _combo(menu, "PRIMARY")

	# Rifleman: WPN_M4AUTO (charfilter medic|rifleman|engineer) is present.
	assert_true(_combo_texts(primary).has("WPN_M4AUTO"), "M4 shows for Rifleman")

	# Sniper (index 1, value 6): the class mask re-filters it out.
	# [orig: handle_team_class_selection @0x566f60 -> populate_three_category_lists @0x566db0]
	var spin := menu.find_child("PLAYER_CLASS", true, false) as NovaMnuSpinList
	spin.set_value_index(1)
	spin.value_changed.emit(1, spin.get_value())
	assert_false(_combo_texts(primary).has("WPN_M4AUTO"), "M4 is hidden for Sniper")


func test_weight_updates_from_selection() -> void:
	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	companion.set_current_loadout("WPN_M4AUTO")
	var menu := _make_menu()
	companion.on_menu_built(menu, "weapon.mnu", "WEAPON", null)
	var weight := menu.find_child("STATIC_TOTAL_WEIGHT", true, false) as Label
	# weight = weaponweight + clips * clipweight summed over selected slots, rendered
	# "<TOTAL_WEIGHT> <w> <LBS> (<encumbrance>)" with bands <33.3/<66.6
	# [orig: calculate_equipped_weapons_weight @0x565490;
	#  update_weapon_weight_display @0x565640 "%s %.1f %s (%s)"]
	assert_false(weight.text.is_empty(), "the weight readout renders")
	var w: Dictionary = companion.selected_weapon("PRIMARY")
	assert_false(w.is_empty(), "the equipped primary is selected")
	var clips := int(companion.selected_clips("PRIMARY"))
	var expected := float(w.get("weight", 0.0)) + clips * float(w.get("clip_weight", 0.0))
	var band := "Light"
	if expected >= 66.6:
		band = "Heavy"
	elif expected >= 33.3:
		band = "Normal"
	assert_eq(weight.text, "Total Weight %.1f lbs (%s)" % [expected, band],
		"the witnessed weight format with the encumbrance band")


func test_banned_grenade_keeps_its_table_order_control_as_zero_only() -> void:
	var wdb := _load_weapons()
	var grenades: Array = wdb.get_slot_weapons(
			NovaWeaponDatabase.SLOT_GRENADE, 8, 2)
	assert_eq(grenades.size(), 3, "the JO fixture has three blue rifleman grenade defs")
	if grenades.size() < 3:
		return
	var banned_name := String((grenades[0] as Dictionary).get("name", ""))
	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(wdb)
	companion.set_player_class(8)
	companion.set_availability_lookup(func(name: String) -> int:
		return 0 if name.nocasecmp_to(banned_name) == 0 else 1)
	var menu := _make_menu()
	companion.on_menu_built(menu, "weapon.mnu", "WEAPON", null)

	var grenade1 := _combo(menu, "GRENADE_AMMO1")
	var grenade2 := _combo(menu, "GRENADE_AMMO2")
	var grenade3 := _combo(menu, "GRENADE_AMMO3")
	assert_eq(grenade1.get_item_count(), 1,
		"a banned first grenade retains control 1 with only its zero row")
	assert_eq(grenade2.get_item_count(),
			int((grenades[1] as Dictionary).get("maxclips", 0)) + 1,
			"the second table-order grenade remains on control 2")
	assert_eq(grenade3.get_item_count(),
			int((grenades[2] as Dictionary).get("maxclips", 0)) + 1,
			"the third table-order grenade remains on control 3")

	watch_signals(companion)
	grenade2.select(1)
	companion.trigger_accept()
	var loadout: Dictionary = get_signal_parameters(companion, "loadout_accepted")[0]
	var selected: Array = loadout.get("grenades", [])
	assert_eq(selected.size(), 1, "only the explicitly selected second grenade serializes")
	if selected.is_empty():
		return
	assert_eq(String((selected[0] as Dictionary).get("name", "")),
			String((grenades[1] as Dictionary).get("name", "")),
			"availability does not compress the third grenade into control 2")


func test_grenade_rows_and_weight_follow_the_retail_extra_ammo_leg() -> void:
	var fixture_db := _load_weapons()
	var fixture_grenades: Array = fixture_db.get_slot_weapons(
			NovaWeaponDatabase.SLOT_GRENADE, 8, 2)
	assert_gt(fixture_grenades.size(), 0,
			"the JO fixture supplies a blue rifleman grenade")
	if fixture_grenades.is_empty():
		return
	var grenade_name := String((fixture_grenades[0] as Dictionary).get("name", ""))
	const GRENADE_WEAPON_WEIGHT_SENTINEL := 40.0
	var wdb := _load_weapons_with_weight(
			grenade_name, GRENADE_WEAPON_WEIGHT_SENTINEL)
	var grenades: Array = wdb.get_slot_weapons(
			NovaWeaponDatabase.SLOT_GRENADE, 8, 2)
	assert_gt(grenades.size(), 0,
			"the temporary weapon.def retains the blue rifleman grenade")
	if grenades.is_empty():
		return
	var grenade_def := grenades[0] as Dictionary
	assert_eq(float(grenade_def.get("weight", 0.0)),
			GRENADE_WEAPON_WEIGHT_SENTINEL,
			"the public weapon database carries the authored weaponweight sentinel")
	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(wdb)
	companion.set_player_class(8)
	companion.set_current_loadout("", "", "", [{
		"name": grenade_name,
		"ammo_primary": 2,
	}])
	var menu := _make_menu()
	companion.on_menu_built(menu, "weapon.mnu", "WEAPON", null)

	var grenade := _combo(menu, "GRENADE_AMMO1")
	var expected_rows: Array = []
	for row in range(0, int(grenade_def.get("maxclips", 0)) + 1):
		expected_rows.append("%d - %s" % [
			row * int(grenade_def.get("clipsize", 0)),
			String(grenade_def.get("round_type", ""))])
	assert_eq(_combo_texts(grenade), expected_rows,
			"grenade rows display row*clipsize and the ammo label")
	assert_eq(grenade.get_selected(), 2,
			"the canonical grenade clip count remains the zero-based row value")
	# The temporary real weapon.def gives this grenade a nonzero weaponweight.
	# Retail's extra-ammo leg ignores adm[85] and weighs only selected_row * adm[84]
	# [orig: @0x5655c9..0x56561c].
	grenade.select(1)
	var weight := menu.find_child("STATIC_TOTAL_WEIGHT", true, false) as Label
	assert_eq(weight.text, "Total Weight %.1f lbs (Light)" % [
			float(grenade_def.get("clip_weight", 0.0))],
			"grenade weight is clips*clipweight and excludes weaponweight")


func test_accept_emits_loadout_and_cancel_closes() -> void:
	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	companion.set_current_loadout("WPN_M4AUTO")
	watch_signals(companion)
	var menu := _make_menu()
	companion.on_menu_built(menu, "weapon.mnu", "WEAPON", null)

	menu.find_child("ACCEPT", true, false).emit_signal("pressed")
	assert_signal_emitted(companion, "loadout_accepted", "ACCEPT commits the loadout")
	var loadout: Dictionary = get_signal_parameters(companion, "loadout_accepted")[0]
	assert_eq(int(loadout.get("player_class", 0)), 8, "the loadout carries the class (rifleman)")
	var primary := String(loadout.get("primary", ""))
	assert_eq(primary, "WPN_M4AUTO", "the loadout carries the selected primary")
	assert_gt(int(loadout.get("primary_clips", -2)), -1, "the loadout carries the clip count")

	menu.find_child("CANCEL", true, false).emit_signal("pressed")
	assert_signal_emitted(companion, "armory_closed", "CANCEL closes without applying")


# The ACCEPT hotkey: the WEAPON screen's on-show registers the USE-ITEM binding row's
# runtime keys (the armory opener — Shift) on the ACCEPT control, debounced until the
# opener press releases once [orig: UI_InitTeamClassSelection @0x567370 adds
# g_useItemBindingKey0/1 to control "ACCEPT" via CUIWidget_AddScreenHotkey
# @0x5674a8/@0x5674c0; the open stamps g_weaponScreenOpenDebounce @0x4e0b21,
# cleared only by the row's KEYUP — Input_HandleMenuKeyRelease @0x4de2d0].
# on_menu_built = the on-show: it stamps the debounce; NovaArmoryPresenter routes the
# key edges here while its overlay is open.
func test_armory_accept_hotkey_debounces_until_release() -> void:
	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	companion.set_current_loadout("WPN_M4AUTO")
	var menu := _make_menu()
	companion.on_menu_built(menu, "weapon.mnu", "WEAPON", null)
	watch_signals(companion)

	assert_false(companion.accept_hotkey_edge(true),
		"the still-held opener press must not ACCEPT [orig: @0x4e0b21]")
	assert_signal_not_emitted(companion, "loadout_accepted")
	assert_false(companion.accept_hotkey_edge(false),
		"the release arms the key, no ACCEPT of its own [orig: @0x4de2d0]")
	assert_true(companion.accept_hotkey_edge(true),
		"the armed press is the ACCEPT accelerator [orig: @0x5674a8]")
	assert_signal_emitted(companion, "loadout_accepted")

	# A re-show re-stamps the debounce — the next press is swallowed again.
	companion.on_menu_built(menu, "weapon.mnu", "WEAPON", null)
	assert_false(companion.accept_hotkey_edge(true),
		"the on-show re-stamps the open debounce [orig: @0x4e0b21]")


func test_degrades_without_weapon_def() -> void:
	var companion := ArmoryMenuCompanion.new()
	var menu := _make_menu()
	companion.on_menu_built(menu, "weapon.mnu", "WEAPON", null)  # no db, no root
	assert_eq(_combo(menu, "PRIMARY").get_item_count(), 0, "no weapon.def -> empty slots, no crash")


# End-to-end against the REAL weapon.mnu (built controls + nesting), so population runs
# through the same control tree the runtime builds [orig: the WEAPON screen of
# weapon.mnu, a boot resource @0x49b3b1 family].
func test_real_weapon_mnu_populates() -> void:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes("res://../fixtures/mnu/jo_weapon.mnu"))
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_edit_mode(false)
	menu.menu = doc

	var companion := ArmoryMenuCompanion.new()
	assert_true(companion.owns_menu(menu), "the real weapon.mnu is claimed by the armory companion")
	companion.set_weapon_database(_load_weapons())
	companion.on_menu_built(menu, "weapon.mnu", "WEAPON", null)

	var primary := menu.find_child("PRIMARY", true, false) as NovaMnuCombo
	assert_not_null(primary, "the real weapon.mnu builds a PRIMARY combobox")
	assert_gt(primary.get_item_count(), 1, "PRIMARY populates (NONE + weapons)")
	var spin := menu.find_child("PLAYER_CLASS", true, false) as NovaMnuSpinList
	assert_not_null(spin, "the real weapon.mnu builds the PLAYER_CLASS spinlist")
	assert_eq(spin.get_value_count(), 5, "the companion fills the authored-empty class spinlist")


# First-show regression against the real authored WEAPON screen and weapon.def:
# M4 (5.5 + 10 * 1.5) plus two satchels (2 * 17.6) is 55.7 lbs / Normal.
# Selecting row zero means one satchel and must re-render 38.1 lbs / Normal.
func test_real_weapon_mnu_weight_tracks_ammo_and_encumbrance_on_first_open() -> void:
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(
			FileAccess.get_file_as_bytes("res://../fixtures/mnu/jo_weapon.mnu")), OK)
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_edit_mode(false)
	menu.menu = doc

	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	companion.set_current_loadout("WPN_M4AUTO", "", "WPN_SATCHEL_CHARGE")
	companion.on_menu_built(menu, "weapon.mnu", "WEAPON", null)

	var weight_window := menu.find_child("STATIC_TOTAL_WEIGHT", true, false)
	assert_not_null(weight_window, "the real weapon.mnu builds the weight window")
	if weight_window == null:
		return
	var weight := weight_window.find_child("Label", false, false) as Label
	assert_not_null(weight, "the authored weight window contains its visible label")
	if weight == null:
		return
	assert_eq(weight.text, "Total Weight 55.7 lbs (Normal)",
			"first open weighs the selected M4 plus two satchels")

	var satchel_ammo := menu.find_child("ACCESSORY_AMMO1", true, false) as NovaMnuCombo
	assert_not_null(satchel_ammo, "the real weapon.mnu builds the satchel ammo combo")
	assert_eq(satchel_ammo.get_selected(), 1,
			"two satchels preselect the last zero-based row")
	satchel_ammo.select(0)
	assert_eq(weight.text, "Total Weight 38.1 lbs (Normal)",
			"row zero means one satchel and updates both weight and encumbrance")
