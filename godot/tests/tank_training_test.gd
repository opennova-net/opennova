extends GutTest

# The authored course, including its landing-craft crews and instructor.
# Only positioning/aim use a debug seam; boarding, weapons, damage and BMS
# progression use the normal runtime. No objectives or deaths are injected.
const MountLook := preload("res://tests/support/mount_look.gd")
const ENEMY_VEHICLES := [416, 318, 321, 415, 317, 320, 568, 572]
const RUNNING_APCS := [568, 572]
var _presenter: LocalPlayerPresenter
var _world: GameWorld
var _camera: Camera3D


func after_each() -> void:
	if is_instance_valid(_presenter):
		_presenter.teardown()
	_presenter = null
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


func _advance(sim: Simulation, ticks := 125) -> void:
	for i in ticks:
		sim.step()
		_presenter.after_world_tick()


func _right_click(sim: Simulation) -> void:
	# Headless Godot cannot capture the mouse. A windowed run exercises the
	# default binding and router; headless starts at the router's request seam.
	if DisplayServer.get_name() == "headless":
		assert_true(sim.request_local_player_scope_toggle())
		return
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	assert_eq(Input.get_mouse_mode(), Input.MOUSE_MODE_CAPTURED)
	for pressed: bool in [true, false]:
		var event := InputEventMouseButton.new()
		event.button_index = MOUSE_BUTTON_RIGHT
		event.pressed = pressed
		Input.parse_input_event(event)
		Input.flush_buffered_events()
		_presenter.before_world_tick(Simulation.tick_dt(), false, true)


func _sights() -> PackedStringArray:
	var textures := PackedStringArray()
	for row: WeaponSightRow in _world.local_player_hud_weapon_def().sights:
		textures.append(row.get_texture())
	return textures


func _fire_gun(sim: Simulation) -> int:
	var serial := sim.get_local_player_weapon_state().fired_serial
	sim.drain_fire_presentation_events()
	var ammo := -1
	var alignment := 1.0
	for tick in 125:
		sim.set_local_player_weapon_input(tick < 16, tick == 0, false)
		_advance(sim, 1)
		for shot: FirePresentationEvent in sim.drain_fire_presentation_events():
			if shot.get_is_local_player():
				ammo = shot.get_ammo_index()
				alignment = minf(alignment, shot.get_forward().dot(-_camera.global_basis.z))
	assert_gt(sim.get_local_player_weapon_state().fired_serial, serial, "the equipped tank gun fires")
	assert_gte(ammo, 0, "firing produces a real local projectile")
	assert_gt(alignment, 0.999, "the selected gun fires along its sight")
	return ammo


func _seat_target(handle: int, bone: int) -> Vector3:
	# Independently read the rendered live userpoint, not the native USE scorer.
	var frame := _presenter.before_world_tick(Simulation.tick_dt(), false, true)
	_world.tick(_camera.global_position, _camera.global_transform, Simulation.tick_dt(), frame)
	_presenter.after_world_tick()
	var model := _world.get_runtime().get_entity_presenter().resolve_wire_handle(handle) as ObjectModel
	var point := model.get_object_data().get_user_point_info(bone - 1)
	var part := model.get_render_part_nodes()[point.subobject] as Node3D
	var pos := part.to_global(point.position)
	return Vector3(pos.x, -pos.z, pos.y)


func _combat_target(sim: Simulation) -> EntityCard:
	var nearest: EntityCard
	var score := INF
	var hull := sim.entity_card_by_net_id(33)
	for ssn in ENEMY_VEHICLES:
		var card := sim.entity_card_by_net_id(ssn)
		if card == null or not card.has_ai() or not card.is_alive() or card.get_health() <= 0:
			continue
		var candidate := card.get_mission_position().distance_to(hull.get_mission_position())
		# These authored EngineRunning APCs can respawn. Weaken both before
		# finishing them together so the course can enter its victory window.
		if card.get_net_id() in RUNNING_APCS:
			candidate += 10000.0
			if card.get_health() <= 6000:
				candidate += 1000.0
		if candidate < score:
			score = candidate
			nearest = card
	return nearest


func _fight_convoy(sim: Simulation) -> void:
	var serial := sim.get_local_player_weapon_state().fired_serial
	for combat_step in 100:
		var target := _combat_target(sim)
		if target == null:
			break
		MountLook.face(sim, target.get_mission_position() + Vector3(0, 0, 1.5))
		_advance(sim, 125)
		var weaken_apc := target.get_net_id() in RUNNING_APCS and target.get_health() > 6000
		for tick in 375:
			# Repeated presses operate the semi-automatic cannon after its reload.
			sim.set_local_player_weapon_input(tick % 12 < 6, tick % 12 == 0, false)
			_advance(sim, 1)
			var live := sim.entity_card_by_net_id(target.get_net_id())
			if live == null or not live.is_alive() or live.get_health() <= 0:
				break
			if weaken_apc and live.get_health() <= 6000:
				break
		sim.set_local_player_weapon_input(false, false, false)
		if sim.get_round_outcome_debug().get_ended():
			break
	assert_gt(sim.get_local_player_weapon_state().fired_serial, serial, "combat fires real cannon rounds")
	_advance(sim, 1250)
	assert_true(sim.has_event_fired(92), "all six enemy groups satisfy the destruction objective")
	assert_true(sim.has_event_fired(93), "the authored delayed victory event fires")
	assert_true(sim.get_round_outcome_debug().get_ended(), "tank training ends in victory")
	assert_eq(sim.get_round_outcome_debug().get_winner_team(), 1)


func test_07tr_tank_course_from_boarding_to_victory() -> void:
	if RetailData.install().is_empty():
		pending("OPENNOVA_JO_DIR with 07TR.bms is required")
		return
	var root := ResourceRoot.new()
	# Use the installed revx02 course when available, matching the reference
	# data and configured JOTAC game; ordinary JO installs use the base course.
	var expansion := "revx02" if "revx02" in RetailData.expansions() else ""
	assert_eq(root.mount_runtime(RetailData.install(), expansion), OK)
	gut.p("07TR course mount: base" if expansion.is_empty() else "07TR course mount: " + expansion)
	_world = WorldFixture.make_world(self)
	_world.set_resource_root(root)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, "07TR.bms"), OK)
	assert_eq(_world.load_mission_data(mission, "07TR.bms"), OK)
	_world.set_process(false)
	var sim := _world.get_sim()
	_camera = Camera3D.new()
	add_child_autofree(_camera)
	_camera.make_current()
	_presenter = LocalPlayerPresenter.new()
	add_child_autofree(_presenter)
	_presenter.setup(_world, _camera, null, ControlsModel.new())
	for ssn in [37, 38, 202, 203]:
		assert_gt(sim.entity_card_by_net_id(ssn).get_mission_position().z, 11.5,
				"landing craft starts afloat before its crew boards")
	_advance(sim)
	var tank := sim.entity_card_by_net_id(33)
	assert_not_null(tank)
	if tank == null:
		return
	var cannon: EntityCard
	var snapshot := sim.get_present_snapshot()
	for base in range(0, snapshot.size(), sim.get_present_stride()):
		var card := sim.entity_card(int(snapshot[base + Simulation.PF_WIRE_HANDLE]))
		if card != null and card.get_item_id() == 166 and card.get_mission_position().distance_to(tank.get_mission_position()) < 10.0:
			cannon = card
	assert_not_null(cannon)
	if cannon == null:
		return
	var seat: EntityCardSeat = cannon.get_seats()[0]
	var target := _seat_target(cannon.get_wire_handle(), seat.get_bone_index())
	MountLook.face(sim, target, target + Vector3(1.5, 0, 0))
	_advance(sim, 18) # refresh USE's proximity list after positioning
	target = _seat_target(cannon.get_wire_handle(), seat.get_bone_index())
	MountLook.face(sim, target, target + Vector3(1.5, 0, 0))
	var boarded := sim.local_player_toggle_mount()
	assert_true(boarded, "USE boards the live cannon seat on the authored tank")
	if not boarded:
		return
	_advance(sim)
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M1TURRET")
	for tick in 1250:
		if sim.has_event_fired(15):
			break
		_advance(sim, 1)
	assert_true(sim.has_event_fired(15), "boarding starts the instructor within 20 seconds")
	_advance(sim)
	var cannon_sights := _sights()
	# Fire clear of the instructor and landing craft after the turret settles.
	MountLook.face(sim, sim.entity_card_by_net_id(33).get_mission_position() + Vector3(0, -150, 45))
	_advance(sim)
	var cannon_ammo := _fire_gun(sim)
	# Stock mounts and mods differ: only an authored G attachment switches
	# slots. The existing mounted_weapon_switch suite also tests helicopters.
	var has_alternate := false
	var definitions := AuthoredItemFixture.read_rows(root)
	for attachment: Dictionary in definitions[100164]["attachments"]:
		has_alternate = has_alternate or attachment["kind"] == "addeweapg"
	if has_alternate:
		_right_click(sim)
		_advance(sim)
		assert_eq(sim.get_local_player_weapon_name(), definitions[100164]["weapon"])
		assert_eq(_world.local_player_hud_weapon_def().weapon_name, definitions[100164]["weapon"],
				"right-click selects the alternate gun HUD definition")
		assert_false(_sights().is_empty(), "the alternate gun has its authored sight")
		assert_ne(_fire_gun(sim), cannon_ammo, "the alternate gun fires its own ammunition")
		_right_click(sim)
		_advance(sim)
		assert_eq(sim.get_local_player_weapon_name(), "WPN_M1TURRET")
		assert_eq(_sights(), cannon_sights, "switching back restores the cannon sight")
	else:
		gut.p("Installed M1A1 has no G attachment; validating its cannon and course.")
	if get_fail_count() > 0:
		return
	var start := sim.entity_card_by_net_id(33).get_mission_position()
	_advance(sim, 3750)
	assert_gt(sim.entity_card_by_net_id(33).get_mission_position().distance_to(start), 15.0,
			"the instructor drives the tank along the course")
	for leg in 30:
		_advance(sim, 1250)
		if sim.has_event_fired(49):
			break
	assert_true(sim.has_event_fired(49), "the convoy reaches its final firing position")
	if not sim.has_event_fired(49):
		return
	_advance(sim, 6250) # let the full enemy convoy reach its firing positions
	assert_true(sim.has_event_fired(48), "the other allied tank releases the APC wave")
	if not sim.has_event_fired(48):
		return
	_fight_convoy(sim)
	_presenter.teardown()
	_presenter = null
	_world.unload()
