extends GutTest

# Drive authored mounts through the same input/world/view path as the shell.
# Camera direction is measured after the real mounted pose fold. The mounts
# are VehicleFixture's, placed beside the minimal world's spawn: the attack
# truck with its .50 cal, the Little Bird (an EWeap carrier its pilot fires),
# the Blackhawk with its two door miniguns (the minted minigun: a WEAP_SPIN
# barrel, MFlash01 at its end, the Effect_MiniMuz fire action). The retail
# witness these replaced crewed the same mounts in the 03TR training mission.

var _presenter: LocalPlayerPresenter
var _camera: Camera3D
var _dir := ""
var _vehicles := {}


func after_each() -> void:
	if is_instance_valid(_presenter):
		_presenter.teardown()
	_presenter = null
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	if not _dir.is_empty():
		TestFs.remove_dir_recursive(_dir)
	_dir = ""
	_vehicles.clear()
	_flight.clear()


# One shell frame: the input sample, then the world tick. The tick's local-view
# leg presents the frame (GameWorld::present_local_view_frame), so the camera is
# composed once a frame, as retail's render frame composes it once
# [orig: GameLoop_RenderFrame @0x521310 once per outer frame from Game_MainLoop
#  @0x52BAC6; Render_ProcessMainSceneFrame @0x5CA34D]. A second
# after_world_tick would compose again and ease the mounted chase look-ahead twice.
func _frame(world: GameWorld, camera: Camera3D, ticks: int = 1) -> void:
	for i in ticks:
		var frame_input := _presenter.before_world_tick(Simulation.tick_dt(), false, true)
		world.tick(camera.global_position, camera.global_transform, Simulation.tick_dt(), frame_input)


# The mounts' placements beside the spawn (mission x, y, yaw), each on the
# ground of the Tmap relief.
const PLACEMENTS := {
	"truck": [VehicleFixture.ATTACK_TRUCK, Vector2(14.0, 6.0), -90.0],
	"little_bird": [VehicleFixture.LITTLE_BIRD, Vector2(-16.0, 10.0), 0.0],
	"blackhawk": [VehicleFixture.BLACKHAWK, Vector2(-14.0, -14.0), 90.0],
}


func _load_world() -> GameWorld:
	_dir = VehicleFixture.stage_world(self, "mounted_view")
	var terrain := VehicleFixture.terrain(self, _dir)
	var rows := []
	for key: String in PLACEMENTS:
		var p: Array = PLACEMENTS[key]
		var at: Vector2 = p[1]
		rows.append({"item": p[0], "pos": Vector3(at.x, at.y,
				VehicleFixture.ground_z(terrain, at.x, at.y)), "yaw": p[2]})
	var ids := []
	var world := VehicleFixture.boot(self, _dir, rows, ids)
	var keys := PLACEMENTS.keys()
	for i in keys.size():
		_vehicles[keys[i]] = ids[i]
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


# The training flight (the 03TR objective helicopter's authoring, minted): the
# Blackhawk with two door miniguns, two riders whose AI runs the board order
# (waypoint_id 125, "Goto SSN and board", at the helicopter: one takes the
# pilot's seat, the other the free minigun), and one event: the local
# player crewing a minigun of the helicopter (PLYRONGUN) redirects its group
# onto the route and sets its patrol speed, which each stop raises (03TR's
# events 44..47: RedirectGroupTo + ChangeGroupAI PATROLSPEED, GroupAtWaypoint). The route's stops are waypoint
# markers (type 6005) on path 1, the second off the first leg's heading so the
# flight banks. The helicopter's own profile is a follow-waypoint HELO one at
# the shipped H_BHawk.aip's values.
const FLIGHT_HELI_AT := Vector2(-14.0, -14.0)
const FLIGHT_SETTLE_FRAMES := 1124
const FLIGHT_MEASURE_FRAMES := 400
# The patrol speed the takeoff event sets and each stop then raises: 03TR's
# events 44..47 (5, then 10, 20 and 60 at its route's nodes 1..3).
const FLIGHT_SPEEDS := [5, 10, 20, 60]
const FLIGHT_GROUP := 3
const FLIGHT_PATH := 1
# The helicopter faces mission +x (yaw 90); the first leg runs straight ahead,
# the second turns off it.
const FLIGHT_ROUTE := [Vector3(46.0, -14.0, 30.0), Vector3(100.0, -60.0, 40.0),
		Vector3(180.0, -70.0, 40.0), Vector3(240.0, 10.0, 40.0)]
const FLIGHT_PROFILE := """type HELO
default_state HELO_FOLLOWWP
combat_flags FOLLOW_WP COUNTER
patrol_speed 70
patrol_altitude 25
patrol_climb 5
combat_speed 90
combat_altitude 40
combat_climb 10
flight_skill 4
turn_rate 40
accel_time 10
"""

var _flight := {}


func _load_flight_world() -> GameWorld:
	_dir = VehicleFixture.stage_world(self, "mounted_flight")
	VehicleFixture.stage_infantry_clips(_dir)
	assert_eq(DirAccess.copy_absolute(ProjectSettings.globalize_path(
			VehicleFixture.SYNTH_MODELS + "/person.3di"), _dir.path_join("US01.3di")), OK)
	WorldFixture.write_file(_dir.path_join("hbhawk.aip"), TestFs.crlf(FLIGHT_PROFILE))
	var terrain := VehicleFixture.terrain(self, _dir)
	var root := VehicleFixture.root_over(self, _dir)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, WorldFixture.MINIMAL_MISSION), OK)
	var ground := VehicleFixture.ground_z(terrain, FLIGHT_HELI_AT.x, FLIGHT_HELI_AT.y)
	var heli := mission.add_entity(MissionData.KIND_ITEM, VehicleFixture.BLACKHAWK,
			Vector3(FLIGHT_HELI_AT.x, FLIGHT_HELI_AT.y, ground), Vector3(0, 90, 0))
	assert_true(mission.set_entity_property_int(MissionData.KIND_ITEM, heli.index, "group", FLIGHT_GROUP))
	assert_true(mission.set_entity_property_string(MissionData.KIND_ITEM, heli.index, "name2", "hbhawk"))
	for i in FLIGHT_ROUTE.size():
		var stop: Vector3 = FLIGHT_ROUTE[i]
		stop.z += VehicleFixture.ground_z(terrain, stop.x, stop.y)
		var marker := mission.add_entity(MissionData.KIND_MARKER, 106005, stop, Vector3.ZERO)
		assert_true(mission.set_entity_property_int(MissionData.KIND_MARKER, marker.index, "waypoint_id", FLIGHT_PATH))
		assert_true(mission.set_entity_property_int(MissionData.KIND_MARKER, marker.index, "wp_number", i + 1))
	# A marker removal lays every path out again from its waypoint markers, as
	# the original editor's writer does (bms_edit lay_out_waypoint_path, D-MIS-6).
	var scratch := mission.add_entity(MissionData.KIND_MARKER, 106005, Vector3.ZERO, Vector3.ZERO)
	assert_true(mission.remove_entity(MissionData.KIND_MARKER, scratch.index))
	var riders := []
	for offset in [Vector3(0.5, 0.0, 0.0), Vector3(-0.5, 0.0, 0.0)]:
		var at: Vector3 = Vector3(FLIGHT_HELI_AT.x, FLIGHT_HELI_AT.y, 0.0) + offset
		at.z = VehicleFixture.ground_z(terrain, at.x, at.y)
		var rider := mission.add_entity(MissionData.KIND_ORGANIC, VehicleFixture.RIDER, at, Vector3.ZERO)
		assert_true(mission.set_entity_property_int(MissionData.KIND_ORGANIC, rider.index, "team", 1))
		assert_true(mission.set_entity_property_int(MissionData.KIND_ORGANIC, rider.index, "waypoint_id", 125))
		assert_true(mission.set_entity_property_int(MissionData.KIND_ORGANIC, rider.index, "wp_number", heli.bms_id))
		riders.append(rider.bms_id)
	var event := mission.add_event(0, 0, 0)
	assert_true(mission.add_event_trigger(event, 7, 41, heli.bms_id), "PLYRONGUN of the helicopter")
	assert_true(mission.add_event_action(event, 1, 0, FLIGHT_GROUP, FLIGHT_PATH, 0), "RedirectGroupTo")
	assert_true(mission.add_event_action(event, 3, 30, FLIGHT_GROUP, FLIGHT_SPEEDS[0]),
			"ChangeGroupAI PATROLSPEED")
	# Each stop reached raises the patrol speed (GroupAtWaypoint -> PATROLSPEED).
	for node in range(1, FLIGHT_SPEEDS.size()):
		var at_stop := mission.add_event(0, 0, 0)
		assert_true(mission.add_event_trigger(at_stop, 1, 7, FLIGHT_GROUP, FLIGHT_PATH, node))
		assert_true(mission.add_event_action(at_stop, 3, 30, FLIGHT_GROUP, FLIGHT_SPEEDS[node]))
	assert_eq(mission.save_as(_dir.path_join("flight.bms")), OK)
	# The board order's think gate staggers by net id ((tick + 36 * net_id) & 15):
	# the first rider thinks second and finds the pilot's seat taken.
	_flight = {"heli": heli.bms_id, "gunner": riders[0], "pilot": riders[1]}
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, _dir, "flight.bms"), OK, "the flight mission loads")
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


func test_truck_50cal_mouse_up_raises_the_camera() -> void:
	var world := await _load_world()
	if world == null:
		return
	var camera := _camera
	var sim := world.get_sim()
	assert_eq(sim.debug_crew_local_player(_vehicles["truck"]), OK)
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


func test_little_bird_pilot_fires_the_carrier_weapon() -> void:
	var world := await _load_world()
	if world == null:
		return
	var sim := world.get_sim()
	assert_eq(sim.debug_crew_local_player(_vehicles["little_bird"]), OK)
	_frame(world, _camera, 96)
	var player := sim.entity_card(sim.get_local_player_wire_handle())
	assert_true(player.is_mounted())
	assert_eq(player.get_mount_type(), 2, "the Little Bird pilot is a controller")
	var fired_before := sim.get_local_player_weapon_state().fired_serial
	for tick in 96:
		var frame_input := _presenter.before_world_tick(Simulation.tick_dt(), false, true)
		frame_input.set_weapon_input(true, tick == 0, false)
		world.tick(_camera.global_position, _camera.global_transform, Simulation.tick_dt(), frame_input)
	assert_gt(sim.get_local_player_weapon_state().fired_serial, fired_before,
			"pilot input reaches the carrier weapon through the real presenter")
	_frame(world, _camera, 96)
	var released := sim.get_local_player_weapon_state().fired_serial
	_frame(world, _camera, 32)
	assert_eq(sim.get_local_player_weapon_state().fired_serial, released,
			"releasing the pilot trigger stops firing")


func test_blackhawk_passengers_and_miniguns_stay_fixed_to_the_cabin() -> void:
	var world := await _load_flight_world()
	if world == null:
		return
	var sim := world.get_sim()
	assert_eq(sim.debug_crew_local_player(_flight["heli"]), OK)
	assert_true(sim.local_player_select_seat(1), "select the objective helicopter minigun")
	# Let the riders' boarding and the engine spool-up finish; the following
	# interval holds the takeoff, the climb and the bank onto the second leg.
	_frame(world, _camera, FLIGHT_SETTLE_FRAMES)
	var index := world.get_runtime().get_entity_index()
	var heli := index.resolve_single(_flight["heli"])
	var riders: Array[Node3D] = [index.resolve_single(_flight["pilot"]),
			index.resolve_single(_flight["gunner"])]
	for ssn in [_flight["pilot"], _flight["gunner"]]:
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
	var start_heading := heli.global_basis.z
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
	for tick in FLIGHT_MEASURE_FRAMES:
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
	var turned_deg := rad_to_deg(Vector2(start_heading.x, start_heading.z).angle_to(
			Vector2(heli.global_basis.z.x, heli.global_basis.z.z)))
	gut.p("training flight: moved %.3f, turned %.2f deg; largest cabin-relative steps %s" % [
			heli.global_position.distance_to(start), turned_deg, largest_steps])
	assert_gt(heli.global_position.distance_to(start), 1.0, "the mission flies the helicopter")
	assert_gt(absf(turned_deg), 10.0,
			"the interval holds the bank onto the second leg (a straight flight would prove less)")
	for step in largest_steps:
		assert_lt(step, 0.002, "a seated body or gun stays fixed to the moving cabin")
	assert_lt(largest_gun_angle_step, 0.001,
			"the minigun's rigid frame turns with the cabin without one-degree snaps")
	gut.p("training flight camera: largest forward-vector acceleration %.6f; gun angle step %.6f" % [largest_camera_delta_change, largest_gun_angle_step])
	assert_lt(largest_camera_delta_change, 0.005,
			"the minigun camera follows the flight without discrete pose snaps")


func test_minigun_barrel_rotates_while_firing() -> void:
	var world := await _load_world()
	if world == null:
		return
	var sim := world.get_sim()
	assert_eq(sim.debug_crew_local_player(_vehicles["blackhawk"]), OK)
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
	assert_gte(barrel_id, 0, "the minigun authors a WEAP_SPIN barrel")
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
		clock.set_time_ms_for_test((tick + 1) * 16)
		gun.advance_runtime_frame(Simulation.tick_dt())
		# Remove the live yaw/pitch contribution: only the barrel's authored
		# spin may satisfy this assertion, never recoil or helicopter motion.
		var without_spin := gun.get_ctrl_values().duplicate()
		without_spin["WEAP_SPIN"] = 0
		var unspun := data.evaluate_panm(0, clock.time_ms, without_spin)[barrel_id] as Transform3D
		greatest_turn = maxf(greatest_turn, unspun.basis.get_rotation_quaternion().angle_to(
				barrel.transform.basis.get_rotation_quaternion()))
	gut.p("minigun barrel: greatest turn %.6f; controls %s" % [greatest_turn, gun.get_ctrl_values()])
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


func test_minigun_flash_follows_the_mounted_muzzle() -> void:
	var world := await _load_world()
	if world == null:
		return
	var sim := world.get_sim()
	assert_eq(sim.debug_crew_local_player(_vehicles["blackhawk"]), OK)
	assert_true(sim.local_player_select_seat(1))
	_frame(world, _camera, 96)
	assert_true(world.local_player_viewmodel_parts().is_empty(),
			"this mounted weapon renders its world model without a first-person gun")
	var gun_handle := 0
	var snapshot := sim.get_present_snapshot()
	for base in range(0, snapshot.size(), sim.get_present_stride()):
		if int(snapshot[base + Simulation.PF_WIRE_HANDLE]) == sim.get_local_player_wire_handle():
			gun_handle = int(snapshot[base + Simulation.PF_CARRIER_HANDLE])
	var gun := world.get_runtime().get_entity_presenter().resolve_wire_handle(gun_handle) as ObjectModel
	assert_not_null(gun)
	if gun == null:
		return
	var data := gun.get_object_data()
	var muzzle: ModelUserPoint
	for i in data.get_user_point_count():
		var point := data.get_user_point_info(i)
		if point.name.nocasecmp_to("MFlash01") == 0:
			muzzle = point
	assert_not_null(muzzle, "the mounted model authors the muzzle point")
	if muzzle == null:
		return
	var part := gun.get_render_part_nodes().get(muzzle.subobject) as Node3D
	assert_not_null(part)
	if part == null:
		return
	# The spinning barrel provides a geometry check independent of the
	# userpoint transform. Rigid vertices and userpoints share model space.
	var parts := gun.get_render_part_nodes()
	var rest := data.evaluate_panm(0, 0, {"WEAP_SPIN": 0})
	var rotated := data.evaluate_panm(0, 0, {"WEAP_SPIN": 16384})
	var barrel: Node3D
	for key in rest:
		if parts.has(key) and not (rest[key] as Transform3D).basis.is_equal_approx(
				(rotated[key] as Transform3D).basis):
			barrel = parts[key]
			break
	assert_not_null(barrel, "the model has a spinning barrel")
	if barrel == null:
		return
	var barrel_bounds := AABB()
	var have_barrel_mesh := false
	for child in barrel.get_children():
		if child is MeshInstance3D and child.mesh != null and child.visible:
			var bounds: AABB = child.transform * child.get_aabb()
			barrel_bounds = barrel_bounds.merge(bounds) if have_barrel_mesh else bounds
			have_barrel_mesh = true
	assert_true(have_barrel_mesh, "the barrel has rendered geometry")
	if not have_barrel_mesh:
		return
	var effects := world.get_effect_world()
	var fired_before := sim.get_local_player_weapon_state().fired_serial
	var flash_id := -1
	for tick in 16:
		var frame_input := _presenter.before_world_tick(Simulation.tick_dt(), false, true)
		frame_input.set_weapon_input(true, tick == 0, false)
		world.tick(_camera.global_position, _camera.global_transform, Simulation.tick_dt(), frame_input)
		for group in effects.get_debug_group_report():
			if group.name == "Effect_MiniMuz":
				flash_id = group.id
		if flash_id >= 0:
			break
	assert_gt(sim.get_local_player_weapon_state().fired_serial, fired_before,
			"the real mounted weapon fires")
	assert_gte(flash_id, 0, "firing creates the authored muzzle-flash effect")
	if flash_id < 0:
		return
	var first_muzzle := Vector3.ZERO
	for pose_step in 2:
		if pose_step == 1:
			# Move the rendered carrier and articulate its gun without firing
			# again: the SAME live group must follow position and direction.
			gun.position += Vector3(1.0, 0.0, 0.0)
			gun.set_ctrl_override("test:muzzle", "EWEAP_GUNYAW", 8192)
			gun.set_ctrl_override("test:muzzle", "EWEAP_GUNPITCH", 4096)
		gun.advance_runtime_frame(0.0)
		effects.render_frame(GameWorld.current_frame_clock_ms())
		var expected := part.to_global(muzzle.position)
		var expected_forward := (part.global_basis * muzzle.rotation).normalized()
		if pose_step == 0:
			first_muzzle = expected
			assert_true(_camera.is_position_in_frustum(expected), "the muzzle is in the firing view")
		else:
			assert_gt(expected.distance_to(first_muzzle), 0.5, "the live muzzle actually moved")
		var live_particles := 0
		for group in effects.get_debug_group_report():
			if group.id != flash_id:
				continue
			var flash_in_barrel := barrel.to_local(group.transform.origin)
			var center := barrel_bounds.get_center()
			var radial_error := Vector2(flash_in_barrel.x - center.x,
					flash_in_barrel.y - center.y).length()
			assert_lt(radial_error, 0.10, "the flash lies on the rendered barrel axis")
			assert_lt(absf(flash_in_barrel.z - barrel_bounds.end.z), 0.12,
					"the flash starts at the end of the rendered barrel")
			assert_lt(group.transform.origin.distance_to(expected), 0.002,
					"the live flash follows the mounted gun's posed muzzle")
			# The weapon action effect's frame is the one forward pose every
			# effect spawn shares: retail hands the group the direction alone.
			var frame: Basis = group.transform.basis
			var shared: Basis = EffectWorld.forward_pose(group.transform.origin, frame.z).basis
			assert_lt((frame.x - shared.x).length() + (frame.y - shared.y).length()
					+ (frame.z - shared.z).length(), 0.0003,
					"the flash's frame is EffectWorld.forward_pose of its forward")
			for emitter in group.emitters:
				live_particles += emitter.alive
				assert_lt(emitter.position.distance_to(expected), 0.002)
				assert_gt(emitter.forward.dot(expected_forward), 0.9999,
						"the flash direction follows the gun part")
		assert_gt(live_particles, 0, "the same muzzle-flash group still has visible particles")


func test_npc_minigunner_remains_attached_during_takeoff() -> void:
	var world := await _load_flight_world()
	if world == null:
		return
	var sim := world.get_sim()
	assert_eq(sim.debug_crew_local_player(_flight["heli"]), OK)
	assert_true(sim.local_player_select_seat(1), "crew the minigun that starts the flight objective")
	_frame(world, _camera, 500)
	var gunner := sim.entity_card_by_net_id(_flight["gunner"])
	assert_true(gunner.is_mounted(), "the NPC boards the other authored minigun")
	assert_eq(gunner.get_mount_type(), 3)
	if not gunner.is_mounted():
		return
	var start := sim.entity_card_by_net_id(_flight["heli"]).get_mission_position()
	var lost_seat := false
	var max_distance := 0.0
	# Eighty seconds covers takeoff, the first bank and sustained flight.
	for tick in 5000:
		_frame(world, _camera)
		gunner = sim.entity_card_by_net_id(_flight["gunner"])
		var heli := sim.entity_card_by_net_id(_flight["heli"])
		lost_seat = lost_seat or not gunner.is_mounted()
		max_distance = maxf(max_distance,
				gunner.get_mission_position().distance_to(heli.get_mission_position()))
		var cabin := world.get_runtime().get_entity_index().resolve_single(_flight["heli"])
		var rider := world.get_runtime().get_entity_index().resolve_single(_flight["gunner"])
		max_distance = maxf(max_distance, rider.global_position.distance_to(cabin.global_position))
		if tick % 64 == 0:
			await get_tree().process_frame
	assert_gt(sim.entity_card_by_net_id(_flight["heli"]).get_mission_position().distance_to(start), 100.0,
			"the objective helicopter takes off and follows its route")
	assert_false(lost_seat, "the NPC retains the minigun seat throughout flight")
	assert_lt(max_distance, 6.0, "both the simulated and rendered NPC travel with the helicopter")
