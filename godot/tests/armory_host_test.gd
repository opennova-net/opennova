extends GutTest

const ArmoryHost := preload("res://engine/world/armory_host.gd")
const TMP_DIR := "res://.godot/armory_host_test"


class FakeSim:
	extends RefCounted
	var in_zone := true
	var multiplayer := false
	var player_class := 8
	var weapon := ""
	var applies: Array = []

	func local_player_in_armory_zone() -> bool:
		return in_zone

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
	var sim: FakeSim
	var entity_team := 2
	var set_weapon_calls: Array[String] = []
	var clear_calls := 0

	func get_sim() -> FakeSim:
		return sim

	func get_resource_root() -> NovaResourceRoot:
		return root

	func get_weapon_database() -> NovaWeaponDatabase:
		return weapons

	func local_player_team() -> int:
		return entity_team

	func local_player_viewmodel_def():
		return null

	func set_local_player_weapon_by_name(name: String) -> bool:
		set_weapon_calls.append(name)
		return true

	func clear_local_player_weapon() -> void:
		clear_calls += 1


class FakePlayerHost:
	extends RefCounted
	var refresh_calls := 0

	func refresh_viewmodel() -> void:
		refresh_calls += 1


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
	var player_host := FakePlayerHost.new()
	var overlay := Control.new()
	add_child_autofree(overlay)
	overlay.size = Vector2(1600, 900)
	var host := ArmoryHost.new()
	add_child_autofree(host)
	host.setup(world, player_host, overlay)

	assert_true(host.try_open(), "offline player in a type-6 zone opens the armory")
	var menu := overlay.get_node_or_null("ArmoryMenu") as NovaMnuMenu
	assert_not_null(menu)
	assert_eq(menu.size, Vector2(800, 600), "weapon.mnu keeps its authored design space")
	assert_eq(menu.scale, Vector2(2.0, 1.5), "the design space fills a 1600x900 host")
	overlay.size = Vector2(1200, 600)
	assert_eq(menu.scale, Vector2(1.5, 1.0), "the fit follows host resizes")

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

	assert_true(host.try_open(), "the same armory can reopen after ACCEPT")
	menu = overlay.get_node("ArmoryMenu") as NovaMnuMenu
	primary = menu.find_child("PRIMARY", true, false) as NovaMnuCombo
	primary.select_silent(0)
	menu.find_child("ACCEPT", true, false).emit_signal("pressed")
	assert_eq(String((sim.applies.back() as Dictionary)["primary"]), "",
		"the authored NONE row reaches the simulation")
	assert_eq(world.clear_calls, 1, "NONE clears the rendered/action weapon state")
	assert_eq(player_host.refresh_calls, 2, "both equip and unequip rebuild the FP view")


func test_multiplayer_open_is_gated_until_live_loadout_submission_exists() -> void:
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
	var host := ArmoryHost.new()
	add_child_autofree(host)
	host.setup(world, null, overlay)

	assert_false(host.try_open(),
		"LAN armory cannot bypass the unimplemented authoritative 0x2F/0x5A path")
	assert_null(overlay.get_node_or_null("ArmoryMenu"))
	assert_true(sim.applies.is_empty(), "no local-only loadout mutation occurred")
