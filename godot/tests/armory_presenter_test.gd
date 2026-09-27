extends GutTest

# ArmoryPresenter on the typed surfaces (ADR 0034): a REAL Simulation owns the
# player/loadout state (mission boot + spawn + weapon table — the same recipe
# the satchel/grenade tests always used), the world seam is an ArmoryWorldView
# interface harness, and the zone gate is exercised through try_open() while the
# staged tests drive the post-gate open() directly (no armory volume is
# authored in the in-memory mission).

const ArmoryPresenter := preload("res://game/world/armory_presenter.gd")
const TMP_DIR := "res://.godot/armory_presenter_test"

# ArmoryFixture authors the small menu, catalog and string tables used here.
const STAGED_FIXTURES := {
	"mnu/jo_weapon.mnu": "weapon.mnu",
	"def/weapon.def": "weapon.def",
	"rtxt/menutxt.bin": "menutxt.BIN",
	"rtxt/gametext.bin": "gametext.bin",
}


class FakeArmoryView:
	extends ArmoryWorldView
	var root: ResourceRoot
	var weapons: WeaponDatabase
	var sim_value: Simulation
	var set_weapon_calls: Array[String] = []
	var clear_calls := 0

	func _sim() -> Simulation:
		return sim_value

	func _resource_root() -> ResourceRoot:
		return root

	func _weapon_database() -> WeaponDatabase:
		return weapons

	func _set_local_player_weapon_by_name(weapon_name: String,
			_preserve_slot_state: bool) -> bool:
		set_weapon_calls.append(weapon_name)
		return true

	func _clear_local_player_weapon() -> void:
		clear_calls += 1


func before_each() -> void:
	Strings.clear()
	MusicService.set_var(2, 0)
	ArmoryFixture.stage(self, TMP_DIR, STAGED_FIXTURES)


func after_each() -> void:
	Strings.clear()


func after_all() -> void:
	PresenterFixture.unstage(TMP_DIR, STAGED_FIXTURES)


func _make_weapons() -> WeaponDatabase:
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(ProjectSettings.globalize_path(TMP_DIR).path_join("weapon.def")), OK)
	return weapons


# A REAL spawned local player over the in-memory default mission, with the
# fixture weapon.def loaded — the authoritative context every open reads.
# The offline recipe (no listen session) keeps apply_local_player_loadout on
# its SP-local leg; the MP tests layer configure_host_session + a real listen
# socket on top AFTER staging the kit.
func _real_sim(entity_team: int = 2) -> Simulation:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var sim := Simulation.new()
	autofree(sim)
	assert_true(sim.load_from_mission_data(mission))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, entity_team))
	assert_eq(sim.load_weapon_table(PresenterFixture.root_over(self, TMP_DIR), "weapon.def"), OK)
	return sim


func _make_world(sim: Simulation, weapons: WeaponDatabase) -> FakeArmoryView:
	var view := FakeArmoryView.new()
	view.root = PresenterFixture.root_over(self, TMP_DIR)
	view.weapons = weapons
	view.sim_value = sim
	return view


func _weapon_display_text(row: WeaponDef) -> String:
	var textid := row.display_textid
	var gametext: RtxtStringFile = Strings.get_table("gametext")
	if gametext != null and not textid.is_empty() \
			and gametext.has_string_in_section("WepDes", textid):
		return gametext.get_string_in_section("WepDes", textid)
	return row.name


func _loadout_names(sim: Simulation) -> Array[String]:
	var names: Array[String] = []
	for value in sim.get_local_player_loadout():
		names.append((value as WeaponKitEntry).name)
	return names


func test_out_of_zone_try_open_is_the_silent_gate() -> void:
	# No type-6 armory volume exists in the in-memory mission, so the spawned
	# player is out of zone and the armory key is silently ignored
	# [orig: Flags & 0x400000 gate @0x4e0b4d].
	var sim := _real_sim()
	var world := _make_world(sim, _make_weapons())
	var overlay := Control.new()
	add_child_autofree(overlay)
	var presenter := ArmoryPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, null, overlay)

	assert_false(presenter.try_open(), "out of zone the key is ignored")
	assert_null(overlay.get_node_or_null("ArmoryMenu"))


func test_sp_open_uses_authoritative_context_and_full_menu_protocol() -> void:
	var weapons := _make_weapons()
	var red_rifleman: Array[WeaponDef] = weapons.get_slot_weapons(
			WeaponDatabase.SLOT_PRIMARY, 8, 1)
	assert_gt(red_rifleman.size(), 0, "fixture has a red rifleman primary")
	var equipped := red_rifleman[0].name

	var sim := _real_sim(2)
	assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make(equipped)], 8),
			"the real simulation owns the equipped rifleman context")
	var world := _make_world(sim, weapons)
	# A REAL local-player presenter (no world behind it): the FP refreshes the
	# armory drives are read through its viewmodel generation.
	var player_presenter := LocalPlayerPresenter.new()
	add_child_autofree(player_presenter)
	var refresh_start := player_presenter.viewmodel_generation()
	var overlay := Control.new()
	add_child_autofree(overlay)
	overlay.size = Vector2(1600, 900)
	var presenter := ArmoryPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, player_presenter, overlay)

	assert_true(presenter.open(), "the staged player opens the armory")
	var frame := overlay.get_node_or_null("ArmoryMenu") as MenuFrame
	assert_not_null(frame)
	var driver: MenuDriver = presenter.get_menu_driver()
	assert_not_null(driver)
	# The frame scales its fixed 800x600 design space to its OWN rect internally
	# [orig: CUIScene_SetScreenScale @0x639480]; the presenter fit just fills the
	# ui parent (replaces the old Control size/scale math).
	assert_eq(frame.position, Vector2.ZERO, "the armory frame anchors at the parent origin")
	assert_eq(frame.size, Vector2(1600, 900), "the design space fills a 1600x900 presenter")
	overlay.size = Vector2(1200, 600)
	assert_eq(frame.size, Vector2(1200, 600), "the fit follows presenter resizes")

	var spin := driver.widget_id("PLAYER_CLASS")
	assert_gte(spin, 0, "weapon.mnu authors the class spin")
	assert_eq(driver.selected_row(spin), 3, "entity class 8 selects Rifleman by value")
	assert_false(driver.is_widget_disabled(spin),
		"the class selector is LIVE offline — D-MNU-10 (retail enables it only "
		+ "in-session [orig: UI_InitTeamClassSelection @0x567370]; deliberate "
		+ "divergence under the ADR 0009 listen-server model, user decision)")
	var primary := driver.widget_id("PRIMARY")
	assert_eq(driver.item_count(primary), red_rifleman.size() + 1,
		"entity team 2 maps to the red weapon-filter domain")
	assert_gt(driver.selected_row(primary), 0, "the authoritative equipped primary is reselected")

	var menutxt: RtxtStringFile = Strings.get_table("menutxt")
	var cancel := driver.widget_id("CANCEL")
	assert_gte(cancel, 0, "weapon.mnu authors the CANCEL button")
	assert_eq(driver.get_widget_text(cancel), menutxt.get_string("WD_NOHOT_CANCEL"),
		"standalone weapon.mnu resolves button IDs through menutxt")
	assert_ne(driver.get_widget_text(cancel), "WD_NOHOT_CANCEL", "raw RTXT IDs are never shown")
	assert_eq(MusicService.get_var(2), 14, "WEAPON MUSICVAR drives retail menu Var2")

	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	assert_has(_loadout_names(sim), equipped,
		"unchanged ACCEPT preserves the entity's equipped weapon")
	assert_eq(world.set_weapon_calls, [equipped] as Array[String])

	assert_true(presenter.open(), "the same armory can reopen after ACCEPT")
	driver.select_row(primary, 0, false)  # the authored NONE row, silently
	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	assert_false(_loadout_names(sim).has(equipped),
		"the authored NONE row reaches the simulation")
	assert_eq(world.clear_calls, 1, "NONE clears the rendered/action weapon state")
	assert_eq(player_presenter.viewmodel_generation() - refresh_start, 2,
			"both equip and unequip rebuild the FP view")


func test_open_preselects_the_authoritative_satchel_loadout() -> void:
	var simulation := _real_sim(2)
	var weapons := _make_weapons()
	var rows: Array[WeaponDef] = weapons.get_slot_weapons(WeaponDatabase.SLOT_ACCESSORY, 8, 1)
	assert_gt(rows.size(), 0, "the fixture has an equippable red rifleman accessory")
	var expected := rows[0].name
	assert_eq(expected, "WPN_SATCHEL_CHARGE", "the minimized fixture row is the satchel")
	var primary_rows: Array[WeaponDef] = weapons.get_slot_weapons(WeaponDatabase.SLOT_PRIMARY, 8, 1)
	assert_gt(primary_rows.size(), 0, "the fixture has a primary beside the satchel")
	var primary := primary_rows[0].name
	assert_eq(primary, "WPN_AK47AUTO",
		"the minimized primary expands the non-selectable WPN_AK47 subclass")
	const PRIMARY_CLIPS := 3
	const ACCESSORY_CLIPS := 1
	assert_true(simulation.apply_local_player_loadout([
		WeaponKitEntry.make(primary, PRIMARY_CLIPS),
		WeaponKitEntry.make(expected, ACCESSORY_CLIPS)], 8),
		"the real simulation owns the primary + satchel kit before the armory opens")
	var canonical_names := _loadout_names(simulation)
	assert_eq(canonical_names, [primary, expected] as Array[String],
		"the armory transport preserves only the selectable parent tuples")
	assert_does_not_have(canonical_names, "WPN_AK47",
		"the primary's hidden subclass is not part of the canonical armory buffer")
	var inventory := simulation.get_local_player_inventory()
	var inventory_names: Array[String] = []
	for value in inventory.slots:
		inventory_names.append((value as PlayerInventorySlot).name)
	assert_has(inventory_names, expected, "the authoritative inventory still contains the satchel")
	assert_has(inventory_names, "WPN_AK47",
		"the expanded runtime pool contains the misleading hidden subclass")

	var world := _make_world(simulation, weapons)
	var overlay := Control.new()
	add_child_autofree(overlay)
	overlay.size = Vector2(800, 600)
	var presenter := ArmoryPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, null, overlay)

	assert_true(presenter.open(), "a fresh armory presenter opens for the equipped local player")
	var driver: MenuDriver = presenter.get_menu_driver()
	assert_not_null(driver)
	var accessory := driver.widget_id("ACCESSORY")
	assert_gt(driver.selected_row(accessory), 0,
		"ACCESSORY pre-selects the satchel that is already in the player's loadout")
	assert_eq(driver.item_text(accessory, driver.selected_row(accessory)),
		_weapon_display_text(rows[0] as WeaponDef),
		"the selected accessory row is exactly the canonical satchel parent")
	var primary_combo := driver.widget_id("PRIMARY")
	assert_gt(driver.selected_row(primary_combo), 0,
		"PRIMARY pre-selects the canonical AK parent instead of relying on fallback")
	assert_eq(driver.item_text(primary_combo, driver.selected_row(primary_combo)),
		_weapon_display_text(primary_rows[0] as WeaponDef),
		"the selected primary row is exactly WPN_AK47AUTO")
	var primary_ammo := driver.widget_id("PRIMARY_AMMO1")
	var accessory_ammo := driver.widget_id("ACCESSORY_AMMO1")
	assert_eq(driver.selected_row(primary_ammo), PRIMARY_CLIPS - 1,
		"first open converts the canonical primary clip count to its zero-based row")
	assert_eq(driver.selected_row(accessory_ammo), ACCESSORY_CLIPS - 1,
		"first open converts the canonical satchel clip count to its zero-based row")

	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	var accepted_by_name := {}
	for row: WeaponKitEntry in simulation.get_local_player_loadout():
		accepted_by_name[row.name] = row.ammo_primary
	assert_eq(int(accepted_by_name.get(primary, -1)), PRIMARY_CLIPS,
		"untouched ACCEPT converts the primary row back to the canonical clip count")
	assert_eq(int(accepted_by_name.get(expected, -1)), ACCESSORY_CLIPS,
		"untouched ACCEPT converts the satchel row back to the canonical clip count")


func test_open_populates_the_authored_grenade_combo_from_weapon_def() -> void:
	var simulation := _real_sim(2)
	var weapons := _make_weapons()
	var grenade_rows: Array[WeaponDef] = weapons.get_slot_weapons(
			WeaponDatabase.SLOT_GRENADE, 8, 1)
	assert_eq(grenade_rows.size(), 3,
		"the real weapon.def fixture has three selectable red rifleman grenades")
	var primary_rows: Array[WeaponDef] = weapons.get_slot_weapons(
			WeaponDatabase.SLOT_PRIMARY, 8, 1)
	assert_gt(primary_rows.size(), 0, "the canonical kit has a normal equipped primary")
	var primary := primary_rows[0].name
	var kit: Array[WeaponKitEntry] = [WeaponKitEntry.make(primary)]
	for i in grenade_rows.size():
		kit.append(WeaponKitEntry.make(grenade_rows[i].name, i + 1))
	assert_true(simulation.apply_local_player_loadout(kit, 8),
		"the authoritative simulation owns all three grenade tuples before first open")

	var world := _make_world(simulation, weapons)
	var overlay := Control.new()
	add_child_autofree(overlay)
	var presenter := ArmoryPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, null, overlay)

	assert_true(presenter.open(), "the real weapon.mnu armory opens")
	var driver: MenuDriver = presenter.get_menu_driver()
	assert_not_null(driver)
	for i in grenade_rows.size():
		var combo_name := "GRENADE_AMMO%d" % (i + 1)
		var grenade_combo := driver.widget_id(combo_name)
		assert_gte(grenade_combo, 0, "weapon.mnu authors %s" % combo_name)
		assert_eq(driver.item_count(grenade_combo),
			grenade_rows[i].maxclips + 1,
			"%s exposes selectable 0..maxclips rows from weapon.def" % combo_name)
		assert_eq(driver.selected_row(grenade_combo), i + 1,
			"%s preselects the authoritative canonical grenade count" % combo_name)

	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	var accepted_by_name := {}
	for row: WeaponKitEntry in simulation.get_local_player_loadout():
		accepted_by_name[row.name] = row.ammo_primary
	assert_true(accepted_by_name.has(primary),
		"untouched ACCEPT keeps the normal primary beside the grenade tuples")
	var inventory_names: Array[String] = []
	for value in simulation.get_local_player_inventory().slots:
		inventory_names.append((value as PlayerInventorySlot).name)
	for i in grenade_rows.size():
		var grenade_name := grenade_rows[i].name
		assert_eq(int(accepted_by_name.get(grenade_name, -1)), i + 1,
			"untouched ACCEPT preserves %s and its selected count" % grenade_name)
		assert_has(inventory_names, grenade_name,
			"the ArmoryPresenter ACCEPT rebuild keeps %s equipped in the slot pool" % grenade_name)


func test_multiplayer_open_is_live() -> void:
	# The 0x2F loadout service exists now: a joiner's ACCEPT re-submits from the
	# applied kit (the sim's in-match queue leg) and the listen presenter applies
	# server-authoritatively, so the armory opens in MP like retail
	# [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 is_in_session leg @0x565d94].
	var sim := _real_sim(2)
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	sim.configure_host_session(host_options)
	assert_true(sim.enable_host_listen(0), "the MP armory rides a real listen host")
	var world := _make_world(sim, _make_weapons())
	var overlay := Control.new()
	add_child_autofree(overlay)
	overlay.size = Vector2(800, 600)
	var presenter := ArmoryPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, null, overlay)

	assert_true(presenter.open(), "the MP armory opens over live play")
	assert_not_null(overlay.get_node_or_null("ArmoryMenu"))


func test_multiplayer_open_uses_retail_server_class_allow_mask() -> void:
	var sim := _real_sim(2)
	assert_true(sim.apply_local_player_loadout([], 8),
			"the staged player carries the disallowed rifleman class")
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host_options.class_allow_mask = 1 << 9  # rifleman disallowed; engineer is next allowed
	sim.configure_host_session(host_options)
	assert_true(sim.enable_host_listen(0), "the session class policy rides a real host")
	var world := _make_world(sim, _make_weapons())
	var overlay := Control.new()
	add_child_autofree(overlay)
	overlay.size = Vector2(800, 600)
	var presenter := ArmoryPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, null, overlay)

	assert_true(presenter.open(), "the MP armory opens with a session class policy")
	assert_not_null(overlay.get_node_or_null("ArmoryMenu"))
	var driver: MenuDriver = presenter.get_menu_driver()
	assert_not_null(driver)
	if driver == null:
		return
	var spin := driver.widget_id("PLAYER_CLASS")
	assert_gte(spin, 0)
	if spin >= 0:
		assert_eq(driver.selected_row(spin), 4,
				"S2C 0x76 policy advances disallowed rifleman to allowed engineer")
