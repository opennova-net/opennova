extends GutTest

# The scenario driver's joiner setup pose (Simulation.debug_apply_local_pose
# over engine LocalPlayer::apply_pose): the retail bridge's ApplyPose write on
# the joiner's own L. Over a real loopback pair (coop_two_sim_test's shape:
# a listen host and a joiner free-running their own step()), the write lands
# on L and the joiner's ordinary C2S 0x0C uplink carries it, so the host's
# copy of the joiner reaches the pose. Anything but a joiner with L refuses
# it: teleport_local_player is the host's and single player's write.

const AI_TYPE := 0x14BF # Generic Soldier (items.def 105311, org1 Person)


func _mission() -> MissionData:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, AI_TYPE, Vector3(0, 0, 0), Vector3.ZERO)
	md.add_entity(3, AI_TYPE, Vector3(10, 0, 0), Vector3.ZERO)
	return md


func _drive_pair_to_match(host: Simulation, joiner: Simulation) -> bool:
	for _i in range(800):
		host.step()
		joiner.step()
		if joiner.is_joined_in_match() and joiner.get_joiner_self_handle() > 0:
			for _settle in range(8):
				host.step()
				joiner.step()
				OS.delay_msec(2)
			return true
		OS.delay_msec(2)
	return false


func test_a_joiner_pose_write_rides_its_uplink_to_the_host() -> void:
	var mission := _mission()
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	var joiner := Simulation.new()
	assert_true(joiner.enable_join("127.0.0.1", host.get_host_listen_port(), "PoseJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	assert_true(_drive_pair_to_match(host, joiner), "the joiner reached the real-UDP match")
	if not joiner.is_joined_in_match():
		return
	assert_true(joiner.has_local_player(), "the joiner spawned its own L")
	var self_handle := joiner.get_joiner_self_handle()
	var before: EntityCard = host.entity_card(self_handle)
	assert_not_null(before, "the host carries the joiner's player at its wire handle")
	if before == null:
		return
	var origin: Vector3 = before.get_mission_position()
	var target := Vector3(origin.x + 3.5, origin.y - 2.25, origin.z)

	assert_eq(joiner.debug_apply_local_pose(target.x, target.y, target.z, 45.0, -10.0), OK,
			"the joiner takes the ApplyPose write on L")
	assert_almost_eq(float(joiner.get_local_player_yaw_deg()), 45.0, 0.01,
			"L faces the written yaw")
	assert_almost_eq(float(joiner.get_local_player_pitch_deg()), -10.0, 0.01,
			"L takes the written pitch")
	var at := Vector3.INF
	for _i in range(120):
		# The sender first: its net leg ships L's pose ahead of its own motor.
		joiner.step()
		host.step()
		var card: EntityCard = host.entity_card(self_handle)
		if card != null:
			at = card.get_mission_position()
			if absf(at.x - target.x) < 0.05 and absf(at.y - target.y) < 0.05:
				break
		OS.delay_msec(2)
	assert_almost_eq(at.x, target.x, 0.05, "the host's copy of the joiner reached the written x")
	assert_almost_eq(at.y, target.y, 0.05, "and the written y, carried by the C2S 0x0C uplink")

	assert_eq(joiner.debug_apply_local_pose(target.x, target.y, target.z, 0.0, 120.0),
			ERR_INVALID_PARAMETER, "a pitch past 90 has no ApplyPose word")
	assert_eq(host.debug_apply_local_pose(0.0, 0.0, 0.0, 0.0, 0.0), ERR_UNAVAILABLE,
			"the host keeps teleport_local_player: no joiner pose write")


func test_no_session_and_single_player_refuse_the_joiner_write() -> void:
	var bare := Simulation.new()
	assert_eq(bare.debug_apply_local_pose(0.0, 0.0, 0.0, 0.0, 0.0), ERR_UNAVAILABLE,
			"no mission, no L")
	var world := WorldFixture.boot_minimal(self)
	var sim := world.get_sim()
	assert_true(sim.has_local_player())
	assert_eq(sim.debug_apply_local_pose(0.0, 0.0, 0.0, 0.0, 0.0), ERR_UNAVAILABLE,
			"single player keeps teleport_local_player")
