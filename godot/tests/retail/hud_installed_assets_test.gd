extends GutTest

const MountLook := preload("res://tests/support/mount_look.gd")

# Optional installed-data integration test over InstalledCombatHud: the
# shipped Javelin/Stinger/mortar HUD transitions (sight cards, fonts, the
# mortar's impact map) through the real presenter, and the tank/pilot seat
# HUD. The viewport pixel leg of the deploy is the windowed script
# tests/retail/windowed/hud_installed_assets_pixels_test.gd.
var _rig: InstalledCombatHud


func after_each() -> void:
	if _rig != null:
		_rig.release()
	_rig = null


func test_installed_launcher_and_mortar_hud_transitions() -> void:
	_rig = InstalledCombatHud.boot(self)
	if _rig == null:
		return
	for name: String in InstalledCombatHud.NAMES:
		_rig.deploy(self, name)
		var weapon := _rig.weapons.get_weapon(_rig.weapons.find_weapon(name))
		assert_eq(_rig.card.row_count(), weapon.get_sights().size())
		for row in _rig.card.get_children():
			assert_not_null(row.tex, name + " resolves its installed sight texture")
		var stats := _rig.hud.get_draw_list_stats()
		assert_gt(stats.glyphs, 0, "installed HUD fonts render")
		assert_false((_rig.hud.get_node("InsetScope") as HudInsetScope).is_scope_active(),
				"these installed weapons do not author Scoped + Inset")
		if name == "WPN_MORTAR":
			assert_true(stats.big_map_visible, "deploying the mortar opens its impact map")
			# The impact preview's rings are anti-aliased band sprites (nine for
			# the 0xD0 special slot's three layers; the ctest hud_combat pins
			# the exact bands) [orig: Render_DrawRingOverlay @0x5D4270].
			assert_gte(stats.big_map_sprites, 9, "the live impact ring reaches the map renderer")
			# LollyPop (0x8000) draws a world marker; UseDesignator (0x200000)
			# adjusts the 2D ring. These are independent authored flags.
			if (weapon.get_flags() & 0x8000) == 0:
				assert_eq(stats.tris, 0, "this mortar has no authored world impact marker")
		else:
			assert_true(_rig.card.is_card_up(), name + " shows its authored optical card")
		_rig.lower(self, name)
		assert_false(_rig.hud.get_draw_list_stats().big_map_visible,
				"lowering the mortar closes the map")


func test_installed_tank_and_pilot_hud_entry_exit() -> void:
	var art := InstalledCombatHud.combat_root(self)
	if art == null:
		return
	var items := ItemDatabase.new()
	assert_eq(items.load_from_resource_root(art, "items.def"), OK)
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load_from_resource_root(art, "weapon.def"), OK)
	var layout := HudPos.new()
	assert_eq(layout.load_from_resource_root(art, "hudpos.def"), OK)
	for item_id: int in [100164, 100165, 102010]:
		var mission := MissionData.new()
		assert_eq(mission.create_default(), OK)
		var vehicle_position := Vector3(0, 0, 30)
		var placed := mission.add_entity(MissionData.KIND_ITEM, item_id, vehicle_position, Vector3.ZERO)
		assert_not_null(placed)
		var sim := Simulation.new()
		sim.enable_listen_server(true)
		sim.set_asset_root(art)
		assert_true(sim.install_seat_specs_for_type_ids(items, PackedInt32Array([item_id - 100000])))
		assert_true(sim.load_from_mission_data(mission))
		var seat_card := sim.entity_card_by_net_id(placed.bms_id)
		var control_seat: EntityCardSeat = null
		for seat: EntityCardSeat in seat_card.get_seats():
			if seat.get_type() == 2 or seat.get_type() == 5:
				control_seat = seat
				break
		assert_not_null(control_seat, "%d has an authored controller/driver seat" % item_id)
		if control_seat == null:
			continue
		sim.resolve_item_traits(items)
		assert_eq(sim.load_weapon_table(art, "weapon.def"), OK)
		assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 1))
		sim.set_local_player_weapon(weapons.get_weapon(weapons.find_weapon("WPN_M4AUTO")), {})
		var seat_position := vehicle_position + control_seat.get_local()
		MountLook.face(self, sim, seat_position, seat_position - Vector3(1, 0, 0))
		assert_true(sim.local_player_toggle_mount(), "%d boards its control seat" % item_id)
		for _i in range(50):
			sim.step()
		var view := sim.get_local_player_view()
		assert_true(view.mounted, "%d remains mounted" % item_id)
		var hud := HudOverlay.new()
		hud.size = Vector2(1024, 768)
		add_child_autofree(hud)
		hud.configure(layout, art)
		hud.set_hud_detail_level(0)
		hud.set_player_context(view)
		hud.set_weapon_state(true, 0, 0, 0, 0, false, false, false, 0)
		hud.set_combat_state(view, Transform3D.IDENTITY, Projection.IDENTITY, false, null, "E")
		var mounted_stats := hud.get_draw_list_stats()
		assert_gt(mounted_stats.glyphs, 0, "the control seat displays its gear label")
		if item_id == 102010:
			assert_gte(mounted_stats.lines, 9, "the pilot sees the AGL ladder and readout connectors")
		assert_true(sim.local_player_toggle_mount())
		sim.step()
		view = sim.get_local_player_view()
		assert_false(view.mounted)
		hud.set_player_context(view)
		hud.set_combat_state(view, Transform3D.IDENTITY, Projection.IDENTITY, false, null, "E")
		assert_lt(hud.get_draw_list_stats().glyphs, mounted_stats.glyphs,
				"leaving the vehicle removes its control/instrument labels")
