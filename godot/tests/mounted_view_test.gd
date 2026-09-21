extends GutTest

# Drive the authored 03TR mounts through the same input/world/view path as
# the shell. Camera direction is measured after the real mounted pose fold.
const MISSION := "03TR.bms"
const SPAWN_ATTACK_VEHICLE := 32

var _presenter: LocalPlayerPresenter
var _camera: Camera3D


func after_each() -> void:
	if is_instance_valid(_presenter):
		_presenter.teardown()
	_presenter = null
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


func _frame(world: GameWorld, camera: Camera3D, ticks: int = 1) -> void:
	for i in ticks:
		var frame_input := _presenter.before_world_tick(Simulation.tick_dt(), false, true)
		world.tick(camera.global_position, camera.global_transform, Simulation.tick_dt(), frame_input)
		_presenter.after_world_tick()


func _load_world() -> GameWorld:
	var installed := RetailData.install()
	if installed.is_empty():
		pending("OPENNOVA_JO_DIR required for the authored 03TR mounted camera")
		return null
	var art := ResourceRoot.new()
	assert_eq(art.mount_runtime(installed, ""), OK)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(art, MISSION), OK)
	var world := WorldFixture.make_world(self)
	world.set_resource_root(art)
	assert_eq(world.load_mission_data(mission, MISSION), OK)
	world.set_process(false)
	var camera := Camera3D.new()
	_camera = camera
	add_child_autofree(camera)
	camera.make_current()
	_presenter = LocalPlayerPresenter.new()
	add_child_autofree(_presenter)
	_presenter.setup(world, camera, null, ControlsModel.new())
	_presenter.set_input_override(PlayerMoveIntent.new())
	await get_tree().process_frame
	return world


func test_03tr_truck_50cal_mouse_up_raises_the_camera() -> void:
	var world := await _load_world()
	if world == null:
		return
	var camera := _camera
	var sim := world.get_sim()
	assert_eq(sim.debug_crew_local_player(SPAWN_ATTACK_VEHICLE), OK)
	assert_true(sim.local_player_select_seat(1), "select the truck's 50cal gunner seat")
	_frame(world, camera, 24)
	var player := sim.entity_card(sim.get_local_player_wire_handle())
	assert_true(player.is_mounted())
	assert_eq(player.get_mount_type(), 3, "the local player is operating UseGun")
	var before := -camera.global_basis.z
	var motion := InputEventMouseMotion.new()
	motion.relative = Vector2(0.0, -400.0)
	assert_true(_presenter.handle_input(motion, true))
	_frame(world, camera, 16)
	var after := -camera.global_basis.z
	assert_gt(after.y, before.y + 0.01,
			"moving the mouse up raises the mounted view instead of lowering it")


func test_03tr_blackhawk_passengers_and_miniguns_stay_fixed_to_the_cabin() -> void:
	var world := await _load_world()
	if world == null:
		return
	var sim := world.get_sim()
	assert_eq(sim.debug_crew_local_player(44), OK)
	assert_true(sim.local_player_select_seat(1), "select the objective helicopter minigun")
	# Let the authored NPC boarding and engine spool-up finish. The following
	# interval contains the first banking turn of the real training flight.
	_frame(world, _camera, 1124)
	var index := world.get_runtime().get_entity_index()
	var heli := index.resolve_single(44)
	var riders: Array[Node3D] = [index.resolve_single(1748), index.resolve_single(1750)]
	for ssn in [1748, 1750]:
		assert_true(sim.entity_card_by_net_id(ssn).is_mounted(), "NPC has boarded before measuring")
	var gun_handle := 0
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for base in range(0, snapshot.size(), stride):
		if int(snapshot[base + Simulation.PF_WIRE_HANDLE]) == sim.get_local_player_wire_handle():
			gun_handle = int(snapshot[base + Simulation.PF_CARRIER_HANDLE])
	riders.append(world.get_runtime().get_entity_presenter().resolve_wire_handle(gun_handle))
	assert_gt(gun_handle, 0, "the local gunner's live minigun carrier")
	assert_not_null(heli)
	for rider in riders:
		assert_not_null(rider)
	if heli == null or riders.has(null):
		return
	var start := heli.global_position
	var previous_gun_basis := heli.global_basis.inverse() * riders[2].global_basis
	var largest_gun_angle_step := 0.0
	var previous_camera_forward := -_camera.global_basis.z
	var previous_camera_delta := Vector3.ZERO
	var largest_camera_delta_change := 0.0
	var previous: Array[Vector3] = []
	var largest_steps: Array[float] = []
	for rider in riders:
		previous.append(heli.global_transform.affine_inverse() * rider.global_position)
		largest_steps.append(0.0)
	for tick in 400:
		_frame(world, _camera)
		var gun_basis := heli.global_basis.inverse() * riders[2].global_basis
		largest_gun_angle_step = maxf(largest_gun_angle_step,
				previous_gun_basis.get_rotation_quaternion().angle_to(gun_basis.get_rotation_quaternion()))
		previous_gun_basis = gun_basis
		var camera_forward := -_camera.global_basis.z
		var camera_delta := camera_forward - previous_camera_forward
		if tick > 0:
			largest_camera_delta_change = maxf(largest_camera_delta_change,
					(camera_delta - previous_camera_delta).length())
		previous_camera_forward = camera_forward
		previous_camera_delta = camera_delta
		for i in riders.size():
			var relative: Vector3 = heli.global_transform.affine_inverse() * riders[i].global_position
			largest_steps[i] = maxf(largest_steps[i], relative.distance_to(previous[i]))
			previous[i] = relative
	gut.p("03TR flight: moved %.3f; largest cabin-relative steps %s" % [heli.global_position.distance_to(start), largest_steps])
	assert_gt(heli.global_position.distance_to(start), 1.0, "the mission flies the helicopter")
	for step in largest_steps:
		assert_lt(step, 0.002, "a seated body or gun stays fixed to the moving cabin")
	assert_lt(largest_gun_angle_step, 0.001,
			"the minigun's rigid frame turns with the cabin without one-degree snaps")
	gut.p("03TR camera: largest forward-vector acceleration %.6f; gun angle step %.6f" % [largest_camera_delta_change, largest_gun_angle_step])
	assert_lt(largest_camera_delta_change, 0.005,
			"the minigun camera follows the flight without discrete pose snaps")


func test_03tr_minigun_barrel_rotates_while_firing() -> void:
	var world := await _load_world()
	if world == null:
		return
	var sim := world.get_sim()
	assert_eq(sim.debug_crew_local_player(44), OK)
	assert_true(sim.local_player_select_seat(1))
	_frame(world, _camera, 96)
	var gun_handle := 0
	var snapshot := sim.get_present_snapshot()
	for base in range(0, snapshot.size(), sim.get_present_stride()):
		if int(snapshot[base + Simulation.PF_WIRE_HANDLE]) == sim.get_local_player_wire_handle():
			gun_handle = int(snapshot[base + Simulation.PF_CARRIER_HANDLE])
	var gun := world.get_runtime().get_entity_presenter().resolve_wire_handle(gun_handle) as ObjectModel
	assert_not_null(gun, "the mounted minigun is presented")
	if gun == null:
		return
	var data := gun.get_object_data()
	var rest := data.evaluate_panm(0, 0, {"WEAP_SPIN": 0})
	var rotated := data.evaluate_panm(0, 0, {"WEAP_SPIN": 16384})
	var barrel_id := -1
	var parts := gun.get_render_part_nodes()
	for key in rest:
		if parts.has(key) and not (rest[key] as Transform3D).basis.is_equal_approx(
				(rotated[key] as Transform3D).basis):
			barrel_id = int(key)
			break
	assert_gte(barrel_id, 0, "the installed minigun authors a WEAP_SPIN barrel")
	if barrel_id < 0:
		return
	var barrel := parts[barrel_id] as Node3D
	var clock := PanmClock.new()
	clock.set_time_ms_for_test(0)
	gun.set_panm_clock(clock)
	gun.advance_runtime_frame(0.0)
	var greatest_turn := 0.0
	var fired_before := sim.get_local_player_weapon_state().fired_serial
	for tick in 96:
		var frame_input := _presenter.before_world_tick(Simulation.tick_dt(), false, true)
		frame_input.set_weapon_input(true, tick == 0, false)
		world.tick(_camera.global_position, _camera.global_transform, Simulation.tick_dt(), frame_input)
		_presenter.after_world_tick()
		clock.set_time_ms_for_test((tick + 1) * 16)
		gun.advance_runtime_frame(Simulation.tick_dt())
		# Remove the live yaw/pitch contribution: only the barrel's authored
		# spin may satisfy this assertion, never recoil or helicopter motion.
		var without_spin := gun.get_ctrl_values().duplicate()
		without_spin["WEAP_SPIN"] = 0
		var unspun := data.evaluate_panm(0, clock.time_ms, without_spin)[barrel_id] as Transform3D
		greatest_turn = maxf(greatest_turn, unspun.basis.get_rotation_quaternion().angle_to(
				barrel.transform.basis.get_rotation_quaternion()))
	gut.p("03TR minigun barrel: greatest turn %.6f; controls %s" % [greatest_turn, gun.get_ctrl_values()])
	assert_gt(sim.get_local_player_weapon_state().fired_serial, fired_before,
			"the mounted gun actually fires rounds")
	assert_gt(greatest_turn, 0.1, "the minigun barrel visibly rotates while firing")

	var phase_at_release := int(gun.get_ctrl_values().get("WEAP_SPIN", 0))
	_frame(world, _camera, 8)
	assert_ne(int(gun.get_ctrl_values().get("WEAP_SPIN", 0)), phase_at_release,
			"the barrel keeps turning after the trigger is released")
	_frame(world, _camera, 120)
	var stopped_phase := int(gun.get_ctrl_values().get("WEAP_SPIN", 0))
	_frame(world, _camera, 16)
	assert_eq(int(gun.get_ctrl_values().get("WEAP_SPIN", 0)), stopped_phase,
			"the barrel holds its final angle after coasting down")
