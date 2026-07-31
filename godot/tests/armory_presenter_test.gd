extends GutTest

const ArmoryPresenter := preload("res://engine/world/armory_presenter.gd")
const TMP_DIR := "res://.godot/armory_presenter_test"


class FakeSim:
	extends RefCounted
	var in_zone := true
	var multiplayer := false
	var player_class := 8
	var weapon := ""
	var entity_team := 2
	var applies: Array = []

	func local_player_in_armory_zone() -> bool:
		return in_zone

	func get_local_player_team() -> int:
		return entity_team

	func is_host_listening() -> bool:
		return multiplayer

	func is_joiner() -> bool:
		return false

	func get_local_player_class() -> int:
		return player_class

	func get_local_player_weapon_name() -> String:
		return weapon

	# The ACCEPT now carries the full multi-slot kit (Array[Dictionary] rows
	# {name, ammo_primary, ammo_secondary, flags}); the first row is the primary
	# [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 tuple parse].
	func apply_local_player_loadout(kit: Array, selected_class: int) -> bool:
		var primary := ""
		if kit.size() > 0:
			primary = String((kit[0] as Dictionary).get("name", ""))
		applies.append({
			"kit": kit.duplicate(true),
			"primary": primary,
			"player_class": selected_class,
		})
		weapon = primary
		player_class = selected_class
		return true


class FakeWorld:
	extends Node
	var root: NovaResourceRoot
	var weapons: NovaWeaponDatabase
	var sim
	var set_weapon_calls: Array[String] = []
	var clear_calls := 0

	func get_sim():
		return sim

	func get_resource_root() -> NovaResourceRoot:
		return root

	func get_weapon_database() -> NovaWeaponDatabase:
		return weapons

	func local_player_viewmodel_def():
		return null

	func set_local_player_weapon_by_name(name: String) -> bool:
		set_weapon_calls.append(name)
		return true

	func clear_local_player_weapon() -> void:
		clear_calls += 1


class FakePlayerPresenter:
	extends RefCounted
	var refresh_calls := 0

	func refresh_viewmodel() -> void:
		refresh_calls += 1


class ArmoryZoneSimProxy:
	extends RefCounted
	var inner: NovaSimulation

	func _init(p_inner: NovaSimulation) -> void:
		inner = p_inner

	func local_player_in_armory_zone() -> bool:
		return true

	func is_host_listening() -> bool:
		return false

	func is_joiner() -> bool:
		return false

	func get_local_player_team() -> int:
		return inner.get_local_player_team()

	func get_local_player_class() -> int:
		return inner.get_local_player_class()

	func get_local_player_weapon_name() -> String:
		return inner.get_local_player_weapon_name()

	func get_local_player_inventory() -> Dictionary:
		return inner.get_local_player_inventory()

	func get_local_player_loadout() -> Array:
		return inner.get_local_player_loadout()

	func apply_local_player_loadout(kit: Array, selected_class: int) -> bool:
		return inner.apply_local_player_loadout(kit, selected_class)


func before_each() -> void:
	NovaStrings.clear()
	NovaMusicService.set_var(2, 0)
	var dir := ProjectSettings.globalize_path(TMP_DIR)
	if not DirAccess.dir_exists_absolute(dir):
		assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	_copy_fixture("res://../fixtures/mnu/jo_weapon.mnu", dir.path_join("weapon.mnu"))
	_copy_fixture("res://../fixtures/def/weapon.def", dir.path_join("weapon.def"))
	_copy_fixture("res://../fixtures/rtxt/menutxt.bin", dir.path_join("menutxt.BIN"))
	_copy_fixture("res://../fixtures/rtxt/gametext.bin", dir.path_join("gametext.bin"))


func after_each() -> void:
	NovaStrings.clear()


func after_all() -> void:
	var dir := ProjectSettings.globalize_path(TMP_DIR)
	for name in ["weapon.mnu", "weapon.def", "menutxt.BIN", "gametext.bin"]:
		var path := dir.path_join(name)
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(path)
	DirAccess.remove_absolute(dir)


func _copy_fixture(source: String, target: String) -> void:
	var output := FileAccess.open(target, FileAccess.WRITE)
	assert_not_null(output, "temporary armory fixture opens for write")
	if output != null:
		output.store_buffer(FileAccess.get_file_as_bytes(source))
		output.close()


func _make_root() -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(TMP_DIR)), OK)
	return root


func _make_weapons() -> NovaWeaponDatabase:
	var weapons := NovaWeaponDatabase.new()
	assert_eq(weapons.load(ProjectSettings.globalize_path(TMP_DIR).path_join("weapon.def")), OK)
	return weapons


func _label_text(node: Node) -> String:
	if node is Label:
		return (node as Label).text
	for child in node.get_children():
		var text := _label_text(child)
		if not text.is_empty():
			return text
	return ""


func _weapon_display_text(row: Dictionary) -> String:
	var textid := String(row.get("display_textid", ""))
	var gametext: RtxtStringFile = NovaStrings.get_table("gametext")
	if gametext != null and not textid.is_empty() \
			and gametext.has_string_in_section("WepDes", textid):
		return gametext.get_string_in_section("WepDes", textid)
	return String(row.get("name", ""))


func test_sp_open_uses_authoritative_context_and_full_menu_protocol() -> void:
	var weapons := _make_weapons()
	var red_rifleman: Array = weapons.get_slot_weapons(
			NovaWeaponDatabase.SLOT_PRIMARY, 8, 1)
	assert_gt(red_rifleman.size(), 0, "fixture has a red rifleman primary")
	var equipped := String((red_rifleman[0] as Dictionary).get("name", ""))

	var sim := FakeSim.new()
	sim.weapon = equipped
	var world := FakeWorld.new()
	world.root = _make_root()
	world.weapons = weapons
	world.sim = sim
	add_child_autofree(world)
	var player_presenter := FakePlayerPresenter.new()
	var overlay := Control.new()
	add_child_autofree(overlay)
	overlay.size = Vector2(1600, 900)
	var presenter := ArmoryPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, player_presenter, overlay)

	assert_true(presenter.try_open(), "offline player in a type-6 zone opens the armory")
	var menu := overlay.get_node_or_null("ArmoryMenu") as NovaMnuMenu
	assert_not_null(menu)
	assert_eq(menu.size, Vector2(800, 600), "weapon.mnu keeps its authored design space")
	assert_eq(menu.scale, Vector2(2.0, 1.5), "the design space fills a 1600x900 presenter")
	overlay.size = Vector2(1200, 600)
	assert_eq(menu.scale, Vector2(1.5, 1.0), "the fit follows presenter resizes")

	var spin := menu.find_child("PLAYER_CLASS", true, false) as NovaMnuSpinList
	assert_eq(spin.get_value_index(), 3, "entity class 8 selects Rifleman by value")
	assert_ne(spin.process_mode, Node.PROCESS_MODE_DISABLED,
		"the class selector is LIVE offline — D-MNU-10 (retail enables it only "
		+ "in-session [orig: UI_InitTeamClassSelection @0x567370]; deliberate "
		+ "divergence under the ADR 0009 listen-server model, user decision)")
	var primary := menu.find_child("PRIMARY", true, false) as NovaMnuCombo
	assert_eq(primary.get_item_count(), red_rifleman.size() + 1,
		"entity team 2 maps to the red weapon-filter domain")
	assert_gt(primary.get_selected(), 0, "the authoritative equipped primary is reselected")

	var menutxt: RtxtStringFile = NovaStrings.get_table("menutxt")
	var cancel := menu.find_child("CANCEL", true, false)
	assert_eq(_label_text(cancel), menutxt.get_string("WD_NOHOT_CANCEL"),
		"standalone weapon.mnu resolves button IDs through menutxt")
	assert_ne(_label_text(cancel), "WD_NOHOT_CANCEL", "raw RTXT IDs are never shown")
	assert_eq(NovaMusicService.get_var(2), 14, "WEAPON MUSICVAR drives retail menu Var2")

	menu.find_child("ACCEPT", true, false).emit_signal("pressed")
	assert_eq(String((sim.applies.back() as Dictionary)["primary"]), equipped,
		"unchanged ACCEPT preserves the entity's equipped weapon")
	assert_eq(world.set_weapon_calls, [equipped])

	assert_true(presenter.try_open(), "the same armory can reopen after ACCEPT")
	menu = overlay.get_node("ArmoryMenu") as NovaMnuMenu
	primary = menu.find_child("PRIMARY", true, false) as NovaMnuCombo
	primary.select_silent(0)
	menu.find_child("ACCEPT", true, false).emit_signal("pressed")
	assert_eq(String((sim.applies.back() as Dictionary)["primary"]), "",
		"the authored NONE row reaches the simulation")
	assert_eq(world.clear_calls, 1, "NONE clears the rendered/action weapon state")
	assert_eq(player_presenter.refresh_calls, 2, "both equip and unequip rebuild the FP view")


func test_open_preselects_the_authoritative_satchel_loadout() -> void:
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	var simulation := NovaSimulation.new()
	assert_true(simulation.load_from_mission_data(mission))
	assert_true(simulation.spawn_local_player(Vector3.ZERO, 0.0, 2))
	var resource_root := _make_root()
	assert_eq(simulation.load_weapon_table(resource_root, "weapon.def"), OK)
	var weapons := _make_weapons()
	var rows: Array = weapons.get_slot_weapons(NovaWeaponDatabase.SLOT_ACCESSORY, 8, 1)
	assert_gt(rows.size(), 0, "the fixture has an equippable red rifleman accessory")
	var expected := String((rows[0] as Dictionary).get("name", ""))
	assert_eq(expected, "WPN_SATCHEL_CHARGE", "the minimized fixture row is the satchel")
	var primary_rows: Array = weapons.get_slot_weapons(NovaWeaponDatabase.SLOT_PRIMARY, 8, 1)
	assert_gt(primary_rows.size(), 0, "the fixture has a primary beside the satchel")
	var primary := String((primary_rows[0] as Dictionary).get("name", ""))
	assert_eq(primary, "WPN_AK47AUTO",
		"the minimized primary expands the non-selectable WPN_AK47 subclass")
	const PRIMARY_CLIPS := 3
	const ACCESSORY_CLIPS := 1
	assert_true(simulation.apply_local_player_loadout([
		{"name": primary, "ammo_primary": PRIMARY_CLIPS},
		{"name": expected, "ammo_primary": ACCESSORY_CLIPS}], 8),
		"the real simulation owns the primary + satchel kit before the armory opens")
	var canonical_names: Array[String] = []
	for value in simulation.get_local_player_loadout():
		canonical_names.append(String((value as Dictionary).get("name", "")))
	assert_eq(canonical_names, [primary, expected],
		"the armory transport preserves only the selectable parent tuples")
	assert_does_not_have(canonical_names, "WPN_AK47",
		"the primary's hidden subclass is not part of the canonical armory buffer")
	var inventory: Dictionary = simulation.get_local_player_inventory()
	var inventory_names: Array[String] = []
	for value in inventory.get("slots", []):
		inventory_names.append(String((value as Dictionary).get("name", "")))
	assert_has(inventory_names, expected, "the authoritative inventory still contains the satchel")
	assert_has(inventory_names, "WPN_AK47",
		"the expanded runtime pool contains the misleading hidden subclass")

	var sim := ArmoryZoneSimProxy.new(simulation)
	var world := FakeWorld.new()
	world.root = resource_root
	world.weapons = weapons
	world.sim = sim
	add_child_autofree(world)
	var overlay := Control.new()
	add_child_autofree(overlay)
	overlay.size = Vector2(800, 600)
	var presenter := ArmoryPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, null, overlay)

	assert_true(presenter.try_open(), "a fresh armory presenter opens for the equipped local player")
	var menu := overlay.get_node("ArmoryMenu") as NovaMnuMenu
	var accessory := menu.find_child("ACCESSORY", true, false) as NovaMnuCombo
	assert_gt(accessory.get_selected(), 0,
		"ACCESSORY pre-selects the satchel that is already in the player's loadout")
	assert_eq(accessory.get_item_text(accessory.get_selected()),
		_weapon_display_text(rows[0] as Dictionary),
		"the selected accessory row is exactly the canonical satchel parent")
	var primary_combo := menu.find_child("PRIMARY", true, false) as NovaMnuCombo
	assert_gt(primary_combo.get_selected(), 0,
		"PRIMARY pre-selects the canonical AK parent instead of relying on fallback")
	assert_eq(primary_combo.get_item_text(primary_combo.get_selected()),
		_weapon_display_text(primary_rows[0] as Dictionary),
		"the selected primary row is exactly WPN_AK47AUTO")
	var primary_ammo := menu.find_child("PRIMARY_AMMO1", true, false) as NovaMnuCombo
	var accessory_ammo := menu.find_child("ACCESSORY_AMMO1", true, false) as NovaMnuCombo
	assert_eq(primary_ammo.get_selected(), PRIMARY_CLIPS - 1,
		"first open converts the canonical primary clip count to its zero-based row")
	assert_eq(accessory_ammo.get_selected(), ACCESSORY_CLIPS - 1,
		"first open converts the canonical satchel clip count to its zero-based row")

	menu.find_child("ACCEPT", true, false).emit_signal("pressed")
	var accepted_by_name := {}
	for value in simulation.get_local_player_loadout():
		var row := value as Dictionary
		accepted_by_name[String(row.get("name", ""))] = int(row.get("ammo_primary", -1))
	assert_eq(int(accepted_by_name.get(primary, -1)), PRIMARY_CLIPS,
		"untouched ACCEPT converts the primary row back to the canonical clip count")
	assert_eq(int(accepted_by_name.get(expected, -1)), ACCESSORY_CLIPS,
		"untouched ACCEPT converts the satchel row back to the canonical clip count")
	simulation.free()


func test_open_populates_the_authored_grenade_combo_from_weapon_def() -> void:
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	var simulation := NovaSimulation.new()
	assert_true(simulation.load_from_mission_data(mission))
	assert_true(simulation.spawn_local_player(Vector3.ZERO, 0.0, 2))
	var resource_root := _make_root()
	assert_eq(simulation.load_weapon_table(resource_root, "weapon.def"), OK)
	var weapons := _make_weapons()
	var grenade_rows: Array = weapons.get_slot_weapons(
			NovaWeaponDatabase.SLOT_GRENADE, 8, 1)
	assert_eq(grenade_rows.size(), 3,
		"the real weapon.def fixture has three selectable red rifleman grenades")
	var primary_rows: Array = weapons.get_slot_weapons(
			NovaWeaponDatabase.SLOT_PRIMARY, 8, 1)
	assert_gt(primary_rows.size(), 0, "the canonical kit has a normal equipped primary")
	var primary := String((primary_rows[0] as Dictionary).get("name", ""))
	var kit: Array[Dictionary] = [{"name": primary}]
	for i in grenade_rows.size():
		kit.append({
			"name": String((grenade_rows[i] as Dictionary).get("name", "")),
			"ammo_primary": i + 1,
		})
	assert_true(simulation.apply_local_player_loadout(kit, 8),
		"the authoritative simulation owns all three grenade tuples before first open")

	var sim := ArmoryZoneSimProxy.new(simulation)
	var world := FakeWorld.new()
	world.root = resource_root
	world.weapons = weapons
	world.sim = sim
	add_child_autofree(world)
	var overlay := Control.new()
	add_child_autofree(overlay)
	var presenter := ArmoryPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, null, overlay)

	assert_true(presenter.try_open(), "the real weapon.mnu armory opens")
	var menu := overlay.get_node("ArmoryMenu") as NovaMnuMenu
	for i in grenade_rows.size():
		var combo_name := "GRENADE_AMMO%d" % (i + 1)
		var grenade_combo := menu.find_child(combo_name, true, false) as NovaMnuCombo
		assert_not_null(grenade_combo, "weapon.mnu authors %s" % combo_name)
		assert_eq(grenade_combo.get_item_count(),
			int((grenade_rows[i] as Dictionary).get("maxclips", 0)) + 1,
			"%s exposes selectable 0..maxclips rows from weapon.def" % combo_name)
		assert_eq(grenade_combo.get_selected(), i + 1,
			"%s preselects the authoritative canonical grenade count" % combo_name)

	menu.find_child("ACCEPT", true, false).emit_signal("pressed")
	var accepted_by_name := {}
	for value in simulation.get_local_player_loadout():
		var row := value as Dictionary
		accepted_by_name[String(row.get("name", ""))] = int(row.get("ammo_primary", -1))
	assert_true(accepted_by_name.has(primary),
		"untouched ACCEPT keeps the normal primary beside the grenade tuples")
	var inventory_names: Array[String] = []
	for value in simulation.get_local_player_inventory().get("slots", []):
		inventory_names.append(String((value as Dictionary).get("name", "")))
	for i in grenade_rows.size():
		var grenade_name := String((grenade_rows[i] as Dictionary).get("name", ""))
		assert_eq(int(accepted_by_name.get(grenade_name, -1)), i + 1,
			"untouched ACCEPT preserves %s and its selected count" % grenade_name)
		assert_has(inventory_names, grenade_name,
			"the ArmoryPresenter ACCEPT rebuild keeps %s equipped in the slot pool" % grenade_name)
	simulation.free()


func test_multiplayer_open_is_live() -> void:
	# The 0x2F loadout service exists now: a joiner's ACCEPT re-submits from the
	# applied kit (the sim's in-match queue leg) and the listen presenter applies
	# server-authoritatively, so the armory opens in MP like retail
	# [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 is_in_session leg @0x565d94].
	var sim := FakeSim.new()
	sim.multiplayer = true
	var world := FakeWorld.new()
	world.root = _make_root()
	world.weapons = _make_weapons()
	world.sim = sim
	add_child_autofree(world)
	var overlay := Control.new()
	add_child_autofree(overlay)
	overlay.size = Vector2(800, 600)
	var presenter := ArmoryPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, null, overlay)

	assert_true(presenter.try_open(), "the MP armory opens over live play")
	assert_not_null(overlay.get_node_or_null("ArmoryMenu"))
