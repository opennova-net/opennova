extends RefCounted
# Looking at the seat before pressing USE.
#
# The USE scan (Entity_FindNearestSeatOrArmory) admits a seat only inside the
# player's view cone: just under 90 deg for a standing player, 5.0 deg once
# seated, measured from the entity yaw/pitch to the seat point lifted 0.1875 u,
# and it scores by 3D reach plus that angular offset. A test that presses USE
# therefore first looks at what it means to board, as a player does. The
# debug teleport is the one seam that writes the local look directly; passing
# the current position keeps the player where it stands. Once seated, the
# right click switches the mounted alternate gun.

const SEAT_LIFT := 0.1875


static func _mission(godot_vec: Vector3) -> Vector3:
	return Vector3(godot_vec.x, -godot_vec.z, godot_vec.y)


# The local player's position in mission space (Z-up).
static func local_player_mission_position(sim: Simulation) -> Vector3:
	return _mission(sim.get_local_player_position())


# Point the local player at `target` (mission space, Z-up). `from` moves the
# player there first (mission space); the default keeps its current position.
static func face(test: GutTest, sim: Simulation, target: Vector3, from = null) -> void:
	var here: Vector3
	if from == null:
		here = local_player_mission_position(sim)
	else:
		here = from
	# The scan measures from Position + CameraOffset, the entity's eye offset:
	# the body tick stamps it from the head clip, and a clip-less headless
	# fixture leaves it at zero, so read the live value rather than assume one.
	var eye := here + _mission(sim.get_local_player_eye_offset())
	var dx := target.x - eye.x
	var dy := target.y - eye.y
	var dz := target.z + SEAT_LIFT - eye.z
	var yaw_deg := rad_to_deg(atan2(dx, dy))
	var pitch_deg := rad_to_deg(atan2(dz, sqrt(dx * dx + dy * dy)))
	test.assert_eq(sim.debug_teleport_local_player(here, yaw_deg, pitch_deg), OK,
			"the debug teleport writes the look")


# Action 6, the configurable scope row (RMB by default), switches the mounted
# alternate gun. Headless Godot cannot capture the mouse, and
# PlayerInputRouter::sample_weapon_input samples the weapon actions only while
# it is captured, so a headless run calls
# Simulation.request_local_player_scope_toggle() itself: the router's one-line
# forward, everything after it (the loopback request, the host handler, the
# presenter's event) being the same. A windowed run drives the actual default
# RMB binding through the input router.
static func right_click(test: GutTest, sim: Simulation,
		presenter: LocalPlayerPresenter) -> void:
	if DisplayServer.get_name() == "headless":
		test.assert_true(sim.request_local_player_scope_toggle())
		return
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	test.assert_eq(Input.get_mouse_mode(), Input.MOUSE_MODE_CAPTURED)
	for pressed: bool in [true, false]:
		var event := InputEventMouseButton.new()
		event.button_index = MOUSE_BUTTON_RIGHT
		event.pressed = pressed
		Input.parse_input_event(event)
		Input.flush_buffered_events()
		presenter.before_world_tick(Simulation.tick_dt(), false, true)
