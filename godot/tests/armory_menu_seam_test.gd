extends GutTest

# The in-game armory seam: weapon.mnu's WEAPON screen driven by the ArmoryMenuCompanion
# companion (godot/game/world/armory_menu_companion.gd) riding a MenuDriver over a real
# MnuDocument (the compiled-menu surface; the Control tree is gone). Pins the witnessed
# wiring [orig: WeaponDef_RegisterUICallbacks @0x567020 registers PLAYER_CLASS / PRIMARY /
# SECONDARY / ACCESSORY / *_AMMO / ACCEPT / CANCEL on the "WEAPON" screen; population
# populate_three_category_lists @0x566db0 (sorted rows, NONE at 0); class resolution
# Armory_ResolveSelectedClass @0x5642f0; weight update_weapon_weight_display @0x565640;
# ACCEPT WeaponLoadout_ApplyFromBuffer @0x565cd0]. The zone-gated OPEN path (the
# useitem key, action 177, on entity Flags 0x400000 @0x4e0b4d) lives in main_game +
# the collision resolver (ctest collision_test) — here the screen itself is the unit.

const WEAPON_FIXTURE := "res://../fixtures/def/weapon.def"


func before_each() -> void:
	Strings.clear()


func after_all() -> void:
	Strings.clear()


func _load_weapons() -> WeaponDatabase:
	var wdb := WeaponDatabase.new()
	var path := ProjectSettings.globalize_path(WEAPON_FIXTURE)
	assert_eq(wdb.load(path), OK, "weapon.def fixture loads")
	return wdb


func _load_weapons_with_weight(weapon_name: String, weight: float) -> WeaponDatabase:
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
		return WeaponDatabase.new()
	source = (source.substr(0, weight_start)
			+ "\tweaponweight\t%.1f" % weight
			+ source.substr(weight_end))

	var temp_path := OS.get_temp_dir().path_join(
			"opennova_armory_grenade_weight_%d.def" % Time.get_ticks_usec())
	var output := FileAccess.open(temp_path, FileAccess.WRITE)
	assert_not_null(output, "temporary weapon.def variant opens for writing")
	if output == null:
		return WeaponDatabase.new()
	output.store_string(source)
	output.close()
	var wdb := WeaponDatabase.new()
	var err := wdb.load(temp_path)
	DirAccess.remove_absolute(temp_path)
	assert_eq(err, OK, "temporary weapon.def variant loads through WeaponDatabase")
	return wdb


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


# A synthetic WEAPON screen: the named controls the original registers, authored
# as a small .mnu document (spinlist + combos + buttons + the weight static).
func _make_weapon_driver() -> MenuDriver:
	var body := _wnd("spinlist", "PLAYER_CLASS", 10)
	var y := 40
	for n in ["PRIMARY", "SECONDARY", "ACCESSORY",
			"PRIMARY_AMMO1", "SECONDARY_AMMO1", "ACCESSORY_AMMO1",
			"GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"]:
		body += _wnd("combo", n, y)
		y += 24
	body += _wnd("button", "ACCEPT", y)
	body += _wnd("button", "CANCEL", y + 24)
	body += _wnd("static", "STATIC_TOTAL_WEIGHT", y + 48)
	return _driver_over(_doc_from_xml(_screen_xml("WEAPON", body)), "weapon.mnu")


func _items(driver: MenuDriver, name: String) -> Array:
	return Array(driver.get_widget_items(driver.widget_id(name)))


# The armory row label = LOADOUT_MENU_TEXTID resolved in the gametext table's WepDes
# section [orig: WeaponDef_ParseProperty @0x54d730 — GameText_GetString("WepDes",
# textid) -> g_loadoutWeaponTable entry+40, raw weapon name fallback; g_TextGameText
# loads gametext.bin @0x4a6cd0 — NOT Game.bin, the separate menu resource @0x552510].
func test_weapon_labels_resolve_from_gametext_wepdes() -> void:
	var t := RtxtStringFile.new()
	assert_eq(t.load_from_byte_array(
			FileAccess.get_file_as_bytes("res://../fixtures/rtxt/gametext.bin")), OK,
			"gametext.bin fixture loads")
	Strings.register_table("gametext", t)
	var expected := t.get_string_in_section("WepDes", "WEAP_SHORT_M4")
	assert_true(not expected.is_empty(), "the fixture carries WepDes/WEAP_SHORT_M4")
	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	var driver := _make_weapon_driver()
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)
	assert_has(_items(driver, "PRIMARY"), expected,
			"PRIMARY rows show the resolved WepDes name, not the raw WPN_ id")


func test_owns_menu_detects_weapon_screen() -> void:
	var companion := ArmoryMenuCompanion.new()
	assert_true(companion.owns_menu(_make_weapon_driver()),
			"PLAYER_CLASS + PRIMARY_AMMO1 mark the WEAPON screen")
	# player.mnu's screen (PLAYERCLASS combo, no ammo combos) is NOT claimed.
	var player_info := _driver_over(_doc_from_xml(_screen_xml("PLAYER_INFO",
			_wnd("combo", "PLAYERCLASS", 10))), "player.mnu")
	assert_false(companion.owns_menu(player_info),
			"the PLAYER_INFO screen stays with its own companion")


func test_unclassed_default_shows_all_weapons() -> void:
	# An unclassed player (SP spawn, class 0) resolves to class 0 with NO class filter
	# — the witnessed switch default is mask -1 = ALL weapons — and the spin merely
	# shows row 0 [orig: Armory_ResolveSelectedClass @0x5642f0;
	# SpinList_SelectItemByValue @0x64ba50 falls back to row 0].
	var companion := ArmoryMenuCompanion.new()
	var wdb := _load_weapons()
	companion.set_weapon_database(wdb)
	var driver := _make_weapon_driver()
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)

	var spin := driver.widget_id("PLAYER_CLASS")
	assert_eq(driver.item_count(spin), 5, "the five soldier classes 5..9 fill PLAYER_CLASS")
	assert_eq(driver.selected_row(spin), 0, "an unclassed resolve shows row 0")
	# Outside an MP session the class spin is inert [orig: the is_in_session
	# branch of the WEAPON on-show handler @0x567370].
	assert_true(driver.is_widget_disabled(spin), "SP leaves the class spin disabled")

	var primary := driver.widget_id("PRIMARY")
	var expected: Array = wdb.get_slot_weapons(WeaponDatabase.SLOT_PRIMARY, -1, 2)
	assert_gt(expected.size(), 0, "the fixture has blue primaries")
	assert_eq(driver.item_count(primary), expected.size() + 1, "PRIMARY = NONE + ALL blue primaries")
	assert_eq(driver.selected_row(primary), 0, "no equipped weapon known -> NONE stays selected")


func test_populates_classes_slots_and_ammo() -> void:
	var companion := ArmoryMenuCompanion.new()
	var wdb := _load_weapons()
	companion.set_weapon_database(wdb)
	# The screen opens on the player's current class + equipped primary [orig:
	# Armory_ResolveSelectedClass @0x5642f0; the per-class buffer reselect @0x564930].
	companion.set_player_class(8)
	companion.set_current_loadout("WPN_M4AUTO")
	var driver := _make_weapon_driver()
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)

	var spin := driver.widget_id("PLAYER_CLASS")
	assert_eq(driver.selected_row(spin), 3, "the spin selects the resolved class by value (8 = row 3)")

	# Rifleman (mask 8), blue (mask 2): NONE + the filtered primaries, sorted
	# case-insensitively [orig: cmp @0x6448a0 mode (string, asc)].
	var primary := driver.widget_id("PRIMARY")
	var expected: Array = wdb.get_slot_weapons(WeaponDatabase.SLOT_PRIMARY, 8, 2)
	assert_gt(expected.size(), 0, "the fixture has rifleman/blue primaries")
	assert_eq(driver.item_count(primary), expected.size() + 1, "PRIMARY = NONE + filtered weapons")
	var texts := _items(driver, "PRIMARY")
	for i in range(2, texts.size()):
		assert_true(String(texts[i - 1]).nocasecmp_to(String(texts[i])) <= 0,
			"weapon rows sort ascending: %s <= %s" % [texts[i - 1], texts[i]])
	var sel := driver.selected_row(primary)
	assert_gt(sel, 0, "the equipped primary's row is pre-selected")
	assert_eq(String(companion.selected_weapon("PRIMARY").get("name", "")), "WPN_M4AUTO",
		"the pre-selected row is the equipped M4")

	# Retail rows are zero-based UI indices for one-based clip counts: row 0 means
	# one clip and row maxclips-1 means a full load
	# [orig: populate_ammo_type_combo_boxes @0x564c7d..0x564ce4].
	var ammo := driver.widget_id("PRIMARY_AMMO1")
	var weapon := companion.selected_weapon("PRIMARY")
	var maxclips := int(weapon.get("maxclips", 0))
	assert_eq(driver.item_count(ammo), maxclips, "PRIMARY_AMMO1 has one row per 1..maxclips")
	assert_eq(driver.selected_row(ammo), maxclips - 1, "full clips selects the last zero-based row")
	assert_eq(driver.item_text(ammo, 0), "%d - %s" % [
		int(weapon.get("clipsize", 0)), String(weapon.get("round_type", ""))],
		"row 0 displays one clip's round quantity and ammo label")
	driver.select_row(ammo, 0)  # the user pick relays as a "combo" value change
	assert_eq(companion.selected_clips("PRIMARY"), 1,
		"the first zero-based UI row serializes as one clip")


func test_ammo_rows_resolve_the_shipped_wepdes_labels() -> void:
	var gametext := RtxtStringFile.new()
	assert_eq(gametext.load_from_byte_array(
			FileAccess.get_file_as_bytes("res://../fixtures/rtxt/gametext.bin")), OK,
			"the shipped GameText fixture loads")
	Strings.register_table("gametext", gametext)

	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	companion.set_current_loadout("WPN_M4AUTO")
	var driver := _make_weapon_driver()
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)

	assert_eq(driver.item_text(driver.widget_id("PRIMARY_AMMO1"), 0), "30 - 5.56x45",
			"parent ammo rows use the shipped WepDes round label")
	assert_eq(driver.item_text(driver.widget_id("GRENADE_AMMO1"), 0), "0 - Flashbang",
			"grenade zero rows use the shipped WepDes round label")
	assert_eq(driver.item_text(driver.widget_id("GRENADE_AMMO1"), 1), "1 - Flashbang",
			"grenade count rows keep the localized label")


func test_class_change_refilters_slots() -> void:
	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	# The flip is MP-only in the original; enable it the way an MP session does
	# [orig: @0x567370 enables PLAYER_CLASS only in-session].
	companion.set_class_selection_enabled(true)
	var driver := _make_weapon_driver()
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)

	# Rifleman: WPN_M4AUTO (charfilter medic|rifleman|engineer) is present.
	assert_true(_items(driver, "PRIMARY").has("WPN_M4AUTO"), "M4 shows for Rifleman")

	# Sniper (index 1, value 6): the class mask re-filters it out. select_row
	# emits the "spinlist" value change — the driver relay of the spin flip.
	# [orig: handle_team_class_selection @0x566f60 -> populate_three_category_lists @0x566db0]
	driver.select_row(driver.widget_id("PLAYER_CLASS"), 1)
	assert_false(_items(driver, "PRIMARY").has("WPN_M4AUTO"), "M4 is hidden for Sniper")


func test_weight_updates_from_selection() -> void:
	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	companion.set_current_loadout("WPN_M4AUTO")
	var driver := _make_weapon_driver()
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)
	# weight = weaponweight + clips * clipweight summed over selected slots, rendered
	# "<TOTAL_WEIGHT> <w> <LBS> (<encumbrance>)" with bands <33.3/<66.6
	# [orig: calculate_equipped_weapons_weight @0x565490;
	#  update_weapon_weight_display @0x565640 "%s %.1f %s (%s)"]
	assert_false(companion.weight_line().is_empty(), "the weight readout renders")
	var w: Dictionary = companion.selected_weapon("PRIMARY")
	assert_false(w.is_empty(), "the equipped primary is selected")
	var clips := int(companion.selected_clips("PRIMARY"))
	var expected := float(w.get("weight", 0.0)) + clips * float(w.get("clip_weight", 0.0))
	var band := "Light"
	if expected >= 66.6:
		band = "Heavy"
	elif expected >= 33.3:
		band = "Normal"
	assert_eq(companion.weight_line(), "Total Weight %.1f lbs (%s)" % [expected, band],
		"the witnessed weight format with the encumbrance band")


func test_banned_grenade_keeps_its_table_order_control_as_zero_only() -> void:
	var wdb := _load_weapons()
	var grenades: Array = wdb.get_slot_weapons(
			WeaponDatabase.SLOT_GRENADE, 8, 2)
	assert_eq(grenades.size(), 3, "the JO fixture has three blue rifleman grenade defs")
	if grenades.size() < 3:
		return
	var banned_name := String((grenades[0] as Dictionary).get("name", ""))
	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(wdb)
	companion.set_player_class(8)
	companion.set_availability_lookup(func(name: String) -> int:
		return 0 if name.nocasecmp_to(banned_name) == 0 else 1)
	var driver := _make_weapon_driver()
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)

	var grenade1 := driver.widget_id("GRENADE_AMMO1")
	var grenade2 := driver.widget_id("GRENADE_AMMO2")
	var grenade3 := driver.widget_id("GRENADE_AMMO3")
	assert_eq(driver.item_count(grenade1), 1,
		"a banned first grenade retains control 1 with only its zero row")
	assert_eq(driver.item_count(grenade2),
			int((grenades[1] as Dictionary).get("maxclips", 0)) + 1,
			"the second table-order grenade remains on control 2")
	assert_eq(driver.item_count(grenade3),
			int((grenades[2] as Dictionary).get("maxclips", 0)) + 1,
			"the third table-order grenade remains on control 3")

	watch_signals(companion)
	driver.select_row(grenade2, 1)
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
			WeaponDatabase.SLOT_GRENADE, 8, 2)
	assert_gt(fixture_grenades.size(), 0,
			"the JO fixture supplies a blue rifleman grenade")
	if fixture_grenades.is_empty():
		return
	var grenade_name := String((fixture_grenades[0] as Dictionary).get("name", ""))
	const GRENADE_WEAPON_WEIGHT_SENTINEL := 40.0
	var wdb := _load_weapons_with_weight(
			grenade_name, GRENADE_WEAPON_WEIGHT_SENTINEL)
	var grenades: Array = wdb.get_slot_weapons(
			WeaponDatabase.SLOT_GRENADE, 8, 2)
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
	var driver := _make_weapon_driver()
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)

	var grenade := driver.widget_id("GRENADE_AMMO1")
	var expected_rows: Array = []
	for row in range(0, int(grenade_def.get("maxclips", 0)) + 1):
		expected_rows.append("%d - %s" % [
			row * int(grenade_def.get("clipsize", 0)),
			String(grenade_def.get("round_type", ""))])
	assert_eq(_items(driver, "GRENADE_AMMO1"), expected_rows,
			"grenade rows display row*clipsize and the ammo label")
	assert_eq(driver.selected_row(grenade), 2,
			"the canonical grenade clip count remains the zero-based row value")
	# The temporary real weapon.def gives this grenade a nonzero weaponweight.
	# Retail's extra-ammo leg ignores adm[85] and weighs only selected_row * adm[84]
	# [orig: @0x5655c9..0x56561c].
	driver.select_row(grenade, 1)
	assert_eq(companion.weight_line(), "Total Weight %.1f lbs (Light)" % [
			float(grenade_def.get("clip_weight", 0.0))],
			"grenade weight is clips*clipweight and excludes weaponweight")


func test_accept_emits_loadout_and_cancel_closes() -> void:
	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	companion.set_current_loadout("WPN_M4AUTO")
	watch_signals(companion)
	var driver := _make_weapon_driver()
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)

	# The activation seam: the driver emits widget_activated(id, NAME) on the
	# click/hotkey edge and companions route by NAME [orig: @0x5671f6 arg 0].
	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	assert_signal_emitted(companion, "loadout_accepted", "ACCEPT commits the loadout")
	var loadout: Dictionary = get_signal_parameters(companion, "loadout_accepted")[0]
	assert_eq(int(loadout.get("player_class", 0)), 8, "the loadout carries the class (rifleman)")
	var primary := String(loadout.get("primary", ""))
	assert_eq(primary, "WPN_M4AUTO", "the loadout carries the selected primary")
	assert_gt(int(loadout.get("primary_clips", -2)), -1, "the loadout carries the clip count")

	driver.widget_activated.emit(driver.widget_id("CANCEL"), "CANCEL")
	assert_signal_emitted(companion, "armory_closed", "CANCEL closes without applying")


# The ACCEPT hotkey: the WEAPON screen's on-show registers the USE-ITEM binding row's
# runtime keys (the armory opener — Shift) on the ACCEPT control, debounced until the
# opener press releases once [orig: UI_InitTeamClassSelection @0x567370 adds
# g_useItemBindingKey0/1 to control "ACCEPT" via CUIWidget_AddScreenHotkey
# @0x5674a8/@0x5674c0; the open stamps g_weaponScreenOpenDebounce @0x4e0b21,
# cleared only by the row's KEYUP — Input_HandleMenuKeyRelease @0x4de2d0].
# on_menu_built = the on-show: it stamps the debounce; ArmoryPresenter routes the
# key edges here while its overlay is open.
func test_armory_accept_hotkey_debounces_until_release() -> void:
	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	companion.set_current_loadout("WPN_M4AUTO")
	var driver := _make_weapon_driver()
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)
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
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)
	assert_false(companion.accept_hotkey_edge(true),
		"the on-show re-stamps the open debounce [orig: @0x4e0b21]")


func test_degrades_without_weapon_def() -> void:
	var companion := ArmoryMenuCompanion.new()
	var driver := _make_weapon_driver()
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)  # no db, no root
	assert_eq(driver.item_count(driver.widget_id("PRIMARY")), 0,
			"no weapon.def -> empty slots, no crash")


# End-to-end against the REAL weapon.mnu (authored widget tree), so population runs
# through the same document the runtime opens [orig: the WEAPON screen of
# weapon.mnu, a boot resource @0x49b3b1 family].
func test_real_weapon_mnu_populates() -> void:
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(
			FileAccess.get_file_as_bytes("res://../fixtures/mnu/jo_weapon.mnu")), OK,
			"the shipped jo_weapon.mnu fixture loads")
	var driver := _driver_over(doc, "weapon.mnu", "WEAPON")

	var companion := ArmoryMenuCompanion.new()
	assert_true(companion.owns_menu(driver), "the real weapon.mnu is claimed by the armory companion")
	companion.set_weapon_database(_load_weapons())
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)

	var primary := driver.widget_id("PRIMARY")
	assert_gte(primary, 0, "the real weapon.mnu authors a PRIMARY combobox")
	assert_gt(driver.item_count(primary), 1, "PRIMARY populates (NONE + weapons)")
	var spin := driver.widget_id("PLAYER_CLASS")
	assert_gte(spin, 0, "the real weapon.mnu authors the PLAYER_CLASS spinlist")
	assert_eq(driver.item_count(spin), 5, "the companion fills the authored-empty class spinlist")


# First-show regression against the real authored WEAPON screen and weapon.def:
# M4 (5.5 + 10 * 1.5) plus two satchels (2 * 17.6) is 55.7 lbs / Normal.
# Selecting row zero means one satchel and must re-render 38.1 lbs / Normal.
func test_real_weapon_mnu_weight_tracks_ammo_and_encumbrance_on_first_open() -> void:
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(
			FileAccess.get_file_as_bytes("res://../fixtures/mnu/jo_weapon.mnu")), OK)
	var driver := _driver_over(doc, "weapon.mnu", "WEAPON")

	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	companion.set_current_loadout("WPN_M4AUTO", "", "WPN_SATCHEL_CHARGE")
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)

	assert_gte(driver.widget_id("STATIC_TOTAL_WEIGHT"), 0,
			"the real weapon.mnu authors the weight readout")
	assert_eq(companion.weight_line(), "Total Weight 55.7 lbs (Normal)",
			"first open weighs the selected M4 plus two satchels")

	var satchel_ammo := driver.widget_id("ACCESSORY_AMMO1")
	assert_gte(satchel_ammo, 0, "the real weapon.mnu authors the satchel ammo combo")
	assert_eq(driver.selected_row(satchel_ammo), 1,
			"two satchels preselect the last zero-based row")
	driver.select_row(satchel_ammo, 0)
	assert_eq(companion.weight_line(), "Total Weight 38.1 lbs (Normal)",
			"row zero means one satchel and updates both weight and encumbrance")
