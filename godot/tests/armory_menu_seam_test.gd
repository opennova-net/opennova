extends GutTest

# The in-game armory seam: weapon.mnu's WEAPON screen driven by the ArmoryMenuHost
# companion (godot/game/armory_menu_host.gd). Pins the witnessed wiring [orig:
# WeaponDef_RegisterUICallbacks @0x567020 registers PLAYER_CLASS / PRIMARY / SECONDARY /
# ACCESSORY / *_AMMO / ACCEPT / CANCEL on the "WEAPON" screen; population
# populate_weapon_slot_lists @0x560430; weight calculate_loadout_weight @0x55f1f0;
# ACCEPT WeaponLoadout_ApplyFromBuffer @0x565cd0]. The zone-gated OPEN path (input
# action 218 on entity Flags 0x400000 @0x49b848) lives in main_game + the collision
# resolver (ctest collision_test) — here the screen itself is the unit.

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


func test_populates_classes_slots_and_ammo() -> void:
	var host := ArmoryMenuHost.new()
	var wdb := _load_weapons()
	host.set_weapon_database(wdb)
	var menu := _make_menu()
	host.on_menu_built(menu, "weapon.mnu", "WEAPON", null)

	var spin := menu.find_child("PLAYER_CLASS", true, false) as NovaMnuSpinList
	assert_eq(spin.get_value_count(), 5, "the five soldier classes 5..9 fill PLAYER_CLASS")
	assert_eq(spin.get_value_index(), 3, "rifleman (8) is the default class")

	# Rifleman (mask 8), blue (mask 2): NONE + the filtered primaries, first weapon pre-selected.
	var primary := _combo(menu, "PRIMARY")
	var expected: Array = wdb.get_slot_weapons(NovaWeaponDatabase.SLOT_PRIMARY, 8, 2)
	assert_gt(expected.size(), 0, "the fixture has rifleman/blue primaries")
	assert_eq(primary.get_item_count(), expected.size() + 1, "PRIMARY = NONE + filtered weapons")
	assert_eq(primary.get_selected(), 1, "the first real weapon is pre-selected")

	# The ammo combo carries 0..maxclips of the selected weapon, full clips pre-selected.
	# [orig: populate_ammo_combo_boxes @0x55def0; the ACCEPT clamp clips <= adm[83]]
	var ammo := _combo(menu, "PRIMARY_AMMO1")
	var maxclips := int(expected[0].get("maxclips", 0))
	assert_eq(ammo.get_item_count(), maxclips + 1, "PRIMARY_AMMO1 = counts 0..maxclips")
	assert_eq(ammo.get_selected(), maxclips, "full clips is the default")


func test_class_change_refilters_slots() -> void:
	var host := ArmoryMenuHost.new()
	host.set_weapon_database(_load_weapons())
	var menu := _make_menu()
	host.on_menu_built(menu, "weapon.mnu", "WEAPON", null)
	var primary := _combo(menu, "PRIMARY")

	# Rifleman default: WPN_M4AUTO (charfilter medic|rifleman|engineer) is present.
	assert_true(_combo_texts(primary).has("WPN_M4AUTO"), "M4 shows for Rifleman")

	# Sniper (index 1, value 6): the class mask re-filters it out.
	# [orig: handle_team_class_selection @0x566f60 -> populate_weapon_slot_lists @0x560430]
	var spin := menu.find_child("PLAYER_CLASS", true, false) as NovaMnuSpinList
	spin.set_value_index(1)
	spin.value_changed.emit(1, spin.get_value())
	assert_false(_combo_texts(primary).has("WPN_M4AUTO"), "M4 is hidden for Sniper")


func test_weight_updates_from_selection() -> void:
	var host := ArmoryMenuHost.new()
	host.set_weapon_database(_load_weapons())
	var menu := _make_menu()
	host.on_menu_built(menu, "weapon.mnu", "WEAPON", null)
	var weight := menu.find_child("STATIC_TOTAL_WEIGHT", true, false) as Label
	# weight = weaponweight + clips * clipweight summed over selected slots
	# [orig: calculate_loadout_weight @0x55f1f0]
	assert_false(weight.text.is_empty(), "the weight readout renders")
	var w: Dictionary = host.selected_weapon("PRIMARY")
	var clips := int(host.selected_clips("PRIMARY"))
	var expected := float(w.get("weight", 0.0)) + clips * float(w.get("clip_weight", 0.0))
	var sec: Dictionary = host.selected_weapon("SECONDARY")
	if not sec.is_empty():
		expected += float(sec.get("weight", 0.0)) \
			+ host.selected_clips("SECONDARY") * float(sec.get("clip_weight", 0.0))
	var acc: Dictionary = host.selected_weapon("ACCESSORY")
	if not acc.is_empty():
		expected += float(acc.get("weight", 0.0)) \
			+ host.selected_clips("ACCESSORY") * float(acc.get("clip_weight", 0.0))
	assert_true(weight.text.ends_with("%.1f" % expected), "weight = sum of slot + clip weights")


func test_accept_emits_loadout_and_cancel_closes() -> void:
	var host := ArmoryMenuHost.new()
	host.set_weapon_database(_load_weapons())
	watch_signals(host)
	var menu := _make_menu()
	host.on_menu_built(menu, "weapon.mnu", "WEAPON", null)

	menu.find_child("ACCEPT", true, false).emit_signal("pressed")
	assert_signal_emitted(host, "loadout_accepted", "ACCEPT commits the loadout")
	var loadout: Dictionary = get_signal_parameters(host, "loadout_accepted")[0]
	assert_eq(int(loadout.get("player_class", 0)), 8, "the loadout carries the class (rifleman)")
	var primary := String(loadout.get("primary", ""))
	assert_false(primary.is_empty(), "the loadout carries the selected primary")
	assert_true(primary.begins_with("WPN_"), "the primary is the raw weapon.def id")
	assert_gt(int(loadout.get("primary_clips", -2)), -1, "the loadout carries the clip count")

	menu.find_child("CANCEL", true, false).emit_signal("pressed")
	assert_signal_emitted(host, "armory_closed", "CANCEL closes without applying")


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
