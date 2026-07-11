extends GutTest

# The in-game armory seam: weapon.mnu's WEAPON screen driven by the ArmoryMenuHost
# companion (godot/engine/world/armory_menu_host.gd). Pins the witnessed wiring [orig:
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
			"PRIMARY_AMMO1", "SECONDARY_AMMO1", "ACCESSORY_AMMO1"]:
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


func test_owns_menu_detects_weapon_screen() -> void:
	var host := ArmoryMenuHost.new()
	var menu := _make_menu()
	assert_true(host.owns_menu(menu), "PLAYER_CLASS + PRIMARY_AMMO1 mark the WEAPON screen")
	# player.mnu's screen (PLAYERCLASS combo, no ammo combos) is NOT claimed.
	var player_info := Node.new()
	add_child_autofree(player_info)
	var cls := NovaMnuCombo.new()
	cls.name = "PLAYERCLASS"
	player_info.add_child(cls)
	assert_false(host.owns_menu(player_info), "the PLAYER_INFO screen stays with its own companion")


func test_unclassed_default_shows_all_weapons() -> void:
	# An unclassed player (SP spawn, class 0) resolves to class 0 with NO class filter
	# — the witnessed switch default is mask -1 = ALL weapons — and the spin merely
	# shows row 0 [orig: Armory_ResolveSelectedClass @0x5642f0;
	# SpinList_SelectItemByValue @0x64ba50 falls back to row 0].
	var host := ArmoryMenuHost.new()
	var wdb := _load_weapons()
	host.set_weapon_database(wdb)
	var menu := _make_menu()
	host.on_menu_built(menu, "weapon.mnu", "WEAPON", null)

	var spin := menu.find_child("PLAYER_CLASS", true, false) as NovaMnuSpinList
	assert_eq(spin.get_value_count(), 5, "the five soldier classes 5..9 fill PLAYER_CLASS")
	assert_eq(spin.get_value_index(), 0, "an unclassed resolve shows row 0")

	var primary := _combo(menu, "PRIMARY")
	var expected: Array = wdb.get_slot_weapons(NovaWeaponDatabase.SLOT_PRIMARY, -1, 2)
	assert_gt(expected.size(), 0, "the fixture has blue primaries")
	assert_eq(primary.get_item_count(), expected.size() + 1, "PRIMARY = NONE + ALL blue primaries")
	assert_eq(primary.get_selected(), 0, "no equipped weapon known -> NONE stays selected")


func test_populates_classes_slots_and_ammo() -> void:
	var host := ArmoryMenuHost.new()
	var wdb := _load_weapons()
	host.set_weapon_database(wdb)
	# The screen opens on the player's current class + equipped primary [orig:
	# Armory_ResolveSelectedClass @0x5642f0; the per-class buffer reselect @0x564930].
	host.set_player_class(8)
	host.set_current_loadout("WPN_M4AUTO")
	var menu := _make_menu()
	host.on_menu_built(menu, "weapon.mnu", "WEAPON", null)

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
	assert_eq(String(host.selected_weapon("PRIMARY").get("name", "")), "WPN_M4AUTO",
		"the pre-selected row is the equipped M4")

	# The ammo combo carries 0..maxclips of the selected weapon, full clips pre-selected
	# (the per-class buffer's remembered counts are the tracked deferral)
	# [orig: the ACCEPT clamp clips <= adm[83] @0x565cd0].
	var ammo := _combo(menu, "PRIMARY_AMMO1")
	var maxclips := int(host.selected_weapon("PRIMARY").get("maxclips", 0))
	assert_eq(ammo.get_item_count(), maxclips + 1, "PRIMARY_AMMO1 = counts 0..maxclips")
	assert_eq(ammo.get_selected(), maxclips, "full clips is the default")


func test_class_change_refilters_slots() -> void:
	var host := ArmoryMenuHost.new()
	host.set_weapon_database(_load_weapons())
	host.set_player_class(8)
	var menu := _make_menu()
	host.on_menu_built(menu, "weapon.mnu", "WEAPON", null)
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
	var host := ArmoryMenuHost.new()
	host.set_weapon_database(_load_weapons())
	host.set_player_class(8)
	host.set_current_loadout("WPN_M4AUTO")
	var menu := _make_menu()
	host.on_menu_built(menu, "weapon.mnu", "WEAPON", null)
	var weight := menu.find_child("STATIC_TOTAL_WEIGHT", true, false) as Label
	# weight = weaponweight + clips * clipweight summed over selected slots, rendered
	# "<TOTAL_WEIGHT> <w> <LBS> (<encumbrance>)" with bands <33.3/<66.6
	# [orig: calculate_equipped_weapons_weight @0x565490;
	#  update_weapon_weight_display @0x565640 "%s %.1f %s (%s)"]
	assert_false(weight.text.is_empty(), "the weight readout renders")
	var w: Dictionary = host.selected_weapon("PRIMARY")
	assert_false(w.is_empty(), "the equipped primary is selected")
	var clips := int(host.selected_clips("PRIMARY"))
	var expected := float(w.get("weight", 0.0)) + clips * float(w.get("clip_weight", 0.0))
	var band := "Light"
	if expected >= 66.6:
		band = "Heavy"
	elif expected >= 33.3:
		band = "Normal"
	assert_eq(weight.text, "Total Weight %.1f lbs (%s)" % [expected, band],
		"the witnessed weight format with the encumbrance band")


func test_accept_emits_loadout_and_cancel_closes() -> void:
	var host := ArmoryMenuHost.new()
	host.set_weapon_database(_load_weapons())
	host.set_player_class(8)
	host.set_current_loadout("WPN_M4AUTO")
	watch_signals(host)
	var menu := _make_menu()
	host.on_menu_built(menu, "weapon.mnu", "WEAPON", null)

	menu.find_child("ACCEPT", true, false).emit_signal("pressed")
	assert_signal_emitted(host, "loadout_accepted", "ACCEPT commits the loadout")
	var loadout: Dictionary = get_signal_parameters(host, "loadout_accepted")[0]
	assert_eq(int(loadout.get("player_class", 0)), 8, "the loadout carries the class (rifleman)")
	var primary := String(loadout.get("primary", ""))
	assert_eq(primary, "WPN_M4AUTO", "the loadout carries the selected primary")
	assert_gt(int(loadout.get("primary_clips", -2)), -1, "the loadout carries the clip count")

	menu.find_child("CANCEL", true, false).emit_signal("pressed")
	assert_signal_emitted(host, "armory_closed", "CANCEL closes without applying")


# The ACCEPT hotkey: the WEAPON screen's on-show registers the USE-ITEM binding row's
# runtime keys (the armory opener — Shift) on the ACCEPT control, debounced until the
# opener press releases once [orig: UI_InitTeamClassSelection @0x567370 adds
# g_useItemBindingKey0/1 to control "ACCEPT" via CUIWidget_AddScreenHotkey
# @0x5674a8/@0x5674c0; the open stamps g_weaponScreenOpenDebounce @0x4e0b21,
# cleared only by the row's KEYUP — Input_HandleMenuKeyRelease @0x4de2d0].
# on_menu_built = the on-show: it stamps the debounce; NovaArmoryHost routes the
# key edges here while its overlay is open.
func test_armory_accept_hotkey_debounces_until_release() -> void:
	var host := ArmoryMenuHost.new()
	host.set_weapon_database(_load_weapons())
	host.set_player_class(8)
	host.set_current_loadout("WPN_M4AUTO")
	var menu := _make_menu()
	host.on_menu_built(menu, "weapon.mnu", "WEAPON", null)
	watch_signals(host)

	assert_false(host.accept_hotkey_edge(true),
		"the still-held opener press must not ACCEPT [orig: @0x4e0b21]")
	assert_signal_not_emitted(host, "loadout_accepted")
	assert_false(host.accept_hotkey_edge(false),
		"the release arms the key, no ACCEPT of its own [orig: @0x4de2d0]")
	assert_true(host.accept_hotkey_edge(true),
		"the armed press is the ACCEPT accelerator [orig: @0x5674a8]")
	assert_signal_emitted(host, "loadout_accepted")

	# A re-show re-stamps the debounce — the next press is swallowed again.
	host.on_menu_built(menu, "weapon.mnu", "WEAPON", null)
	assert_false(host.accept_hotkey_edge(true),
		"the on-show re-stamps the open debounce [orig: @0x4e0b21]")


func test_degrades_without_weapon_def() -> void:
	var host := ArmoryMenuHost.new()
	var menu := _make_menu()
	host.on_menu_built(menu, "weapon.mnu", "WEAPON", null)  # no db, no root
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

	var host := ArmoryMenuHost.new()
	assert_true(host.owns_menu(menu), "the real weapon.mnu is claimed by the armory companion")
	host.set_weapon_database(_load_weapons())
	host.on_menu_built(menu, "weapon.mnu", "WEAPON", null)

	var primary := menu.find_child("PRIMARY", true, false) as NovaMnuCombo
	assert_not_null(primary, "the real weapon.mnu builds a PRIMARY combobox")
	assert_gt(primary.get_item_count(), 1, "PRIMARY populates (NONE + weapons)")
	var spin := menu.find_child("PLAYER_CLASS", true, false) as NovaMnuSpinList
	assert_not_null(spin, "the real weapon.mnu builds the PLAYER_CLASS spinlist")
	assert_eq(spin.get_value_count(), 5, "the host fills the authored-empty class spinlist")
