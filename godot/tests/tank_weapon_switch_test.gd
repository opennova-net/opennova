extends GutTest

const MountLook := preload("res://tests/support/mount_look.gd")

var _presenter: LocalPlayerPresenter

func after_each() -> void:
	if is_instance_valid(_presenter):
		_presenter.teardown()
	_presenter = null
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)

# Stock Escalation does not author G attachments on these tanks. JOTAC does;
# discover that capability instead of inventing an alternate gun for stock data.
# The ungated host_role ctest covers the same queue with synthetic slot records.
func _tank_root(item_id: int) -> ResourceRoot:
	var installed := RetailData.install()
	if installed.is_empty():
		pending("OPENNOVA_JO_DIR is required for installed tank switching")
		return null
	var expansions := PackedStringArray([""])
	expansions.append_array(RetailData.expansions())
	for expansion: String in expansions:
		var art := ResourceRoot.new()
		assert_eq(art.mount_runtime(installed, expansion), OK)
		var items := ItemDatabase.new()
		assert_eq(items.load_from_resource_root(art, "items.def"), OK)
		for attachment: ItemEmplacementAttachment in items.get_emplacement_attachments(item_id):
			if attachment.is_designated_g():
				return art
	pending("Installed tank has no authored G attachment for alternate-gun switching")
	return null

func _advance(sim: Simulation, ticks: int = 90) -> void:
	for i in range(ticks):
		sim.step()
		_presenter.after_world_tick()

func _right_click(sim: Simulation) -> void:
	if DisplayServer.get_name() == "headless":
		assert_true(sim.request_local_player_scope_toggle())
		return
	# Windowed runs exercise the actual default RMB binding and input router.
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	assert_eq(Input.get_mouse_mode(), Input.MOUSE_MODE_CAPTURED)
	for pressed: bool in [true, false]:
		var event := InputEventMouseButton.new()
		event.button_index = MOUSE_BUTTON_RIGHT
		event.pressed = pressed
		Input.parse_input_event(event)
		Input.flush_buffered_events()
		_presenter.before_world_tick(Simulation.tick_dt(), false, true)

func _check_training_tank(item_id: int, cannon: String) -> void:
	var art := _tank_root(item_id)
	if art == null:
		return
	var world := WorldFixture.make_world(self)
	world.set_resource_root(art)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(art, "07TR.bms"), OK)
	var tank_bms_id := -1
	for record: MissionEntityRecord in mission.get_all_entities():
		if record.item_id == 100164:
			tank_bms_id = record.bms_id
			# The original T80 targets have enemy crews. Add an unoccupied T80
			# nearby through the mission API, retaining its installed rig/defs.
			if item_id != 100164:
				var added := mission.add_entity(MissionData.KIND_ITEM, item_id,
						record.position + Vector3(12, 0, 0), record.rotation_deg)
				assert_not_null(added)
				if added == null:
					return
				tank_bms_id = added.bms_id
			break
	assert_gte(tank_bms_id, 0)
	assert_eq(world.load_mission_data(mission, "07TR.bms"), OK)
	world.set_process(false)
	var sim := world.get_sim()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	camera.make_current()
	_presenter = LocalPlayerPresenter.new()
	add_child_autofree(_presenter)
	_presenter.setup(world, camera, null, ControlsModel.new())
	var ui := Control.new()
	ui.size = Vector2(1024, 768)
	add_child_autofree(ui)
	var hud_presenter := GameHudPresenter.new()
	add_child_autofree(hud_presenter)
	hud_presenter.setup(world, _presenter, ui)
	hud_presenter.ensure_game_hud()
	hud_presenter.set_hud_detail_level(0)
	var card := hud_presenter.get_game_hud().get_node("SightsCard") as HudSightsCard
	var hull := sim.entity_card_by_net_id(tank_bms_id)
	assert_not_null(hull, "the training mission contains the tank")
	if hull == null:
		return
	assert_eq(hull.get_item_id(), item_id - 100000)
	var seat: EntityCardSeat = hull.get_seats()[0]
	var target := hull.get_mission_position() + seat.get_local().rotated(
			Vector3(0, 0, 1), deg_to_rad(-hull.get_yaw_deg()))
	MountLook.face(sim, target, target + Vector3(1, 0, 0))
	# Proximity tables rebuild every 17 ticks. Refresh them after moving to a
	# tank away from the authored spawn, then aim using its current carrier pose.
	if item_id != 100164:
		_advance(sim, 18)
		hull = sim.entity_card_by_net_id(tank_bms_id)
		target = hull.get_mission_position() + seat.get_local().rotated(
				Vector3(0, 0, 1), deg_to_rad(-hull.get_yaw_deg()))
		MountLook.face(sim, target, target + Vector3(1, 0, 0))
	var boarded := sim.local_player_toggle_mount()
	assert_true(boarded, "board the training tank")
	if not boarded:
		return
	var selected := sim.local_player_select_seat(1)
	assert_true(selected, "select its turret gunner seat")
	if not selected:
		return
	_advance(sim)
	assert_eq(sim.entity_card(sim.get_local_player_wire_handle()).get_mount_type(), 3)
	assert_eq(sim.get_local_player_weapon_name(), cannon)
	assert_eq(world.local_player_hud_weapon_def().weapon_name, cannon)
	hud_presenter.tick()
	assert_true(card.is_card_up(), "the cannon's forced sights are visible")
	assert_eq(card.row_count(), world.local_player_hud_weapon_def().sights.size())
	var cannon_clip := sim.get_local_player_weapon_state().clip
	var cannon_reserve := sim.get_local_player_weapon_state().reserve

	# Action 6 is the configurable scope row (RMB by default). Its request must
	# survive the host loopback, and the REAL presenter consumes the weapon event.
	_right_click(sim)
	_advance(sim)
	var alternate := sim.get_local_player_weapon_name()
	assert_ne(alternate, cannon, "right-click selects the alternate tank gun")
	assert_eq(world.local_player_hud_weapon_def().weapon_name, alternate)
	assert_eq(sim.debug_set_world_entity_weapon_ammo(hull.get_net_id(), 7, 19), OK)
	assert_eq(world.local_player_weapon_view().clip, 7)
	assert_eq(world.local_player_weapon_view().reserve, 19)
	hud_presenter.tick()
	assert_true(card.is_card_up(), "switching retains the mounted optical HUD")
	assert_eq(card.row_count(), world.local_player_hud_weapon_def().sights.size())
	assert_gt(hud_presenter.get_game_hud().get_draw_list_stats().glyphs, 0)

	_right_click(sim)
	_advance(sim)
	assert_eq(sim.get_local_player_weapon_name(), cannon)
	assert_eq(world.local_player_hud_weapon_def().weapon_name, cannon)
	assert_eq(world.local_player_weapon_view().clip, cannon_clip)
	assert_eq(world.local_player_weapon_view().reserve, cannon_reserve)
	_right_click(sim)
	_advance(sim)
	assert_eq(sim.get_local_player_weapon_name(), alternate)
	assert_eq(world.local_player_weapon_view().clip, 7, "returning preserves alternate ammo")
	assert_eq(world.local_player_weapon_view().reserve, 19)

func test_m1a1_training_right_click_changes_gun_and_hud() -> void:
	_check_training_tank(100164, "WPN_M1TURRET")

func test_t80_training_right_click_changes_gun_and_hud() -> void:
	_check_training_tank(100165, "WPN_T80TURRET")
