extends GutTest

# The scenario driver's joiner setup pose (Simulation.debug_apply_local_pose
# over engine LocalPlayer::apply_pose): the retail bridge's ApplyPose write on
# the joiner's own L. Over a real loopback pair (coop_two_sim_test's shape:
# a listen host and a joiner free-running their own step()), the write lands
# on L and the joiner's ordinary C2S 0x0C uplink carries it. The host's copy
# of the joiner chases each uplink as retail's authority does: a gap within
# 2 units steps a tenth of the way per uplink until it falls inside the 0x2AAA
# deadband, and a gap over 2 units is dropped with no snap (D-NET-NEW-LP-2).
# Anything but a joiner with L refuses it: teleport_local_player is the host's
# and single player's write.

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
	# 1.5 units off: inside the authority chase's 2-unit band.
	var target := Vector3(origin.x + 1.2, origin.y - 0.9, origin.z)

	assert_eq(joiner.debug_apply_local_pose(target.x, target.y, target.z, 45.0, -10.0), OK,
			"the joiner takes the ApplyPose write on L")
	assert_almost_eq(float(joiner.get_local_player_yaw_deg()), 45.0, 0.01,
			"L faces the written yaw")
	assert_almost_eq(float(joiner.get_local_player_pitch_deg()), -10.0, 0.01,
			"L takes the written pitch")
	var at := _host_copy_after(host, joiner, self_handle, 120)
	# Each uplink restarts the chase: a step of ((10 >> 1) + gap) / 10 per tick until the
	# gap falls inside the 0x2AAA deadband (0.1667 units), short of the written pose.
	# [orig: NetPacket_SerializePlayerState @0x4C09C0 - the staging @0x4C2042..0x4C205E,
	#  progress 0 @0x4C20A9; Entity_UpdateInfantryPlayerBody @0x4B40E0 - the deadband
	#  @0x4B438E..0x4B4393, the bucket 10 @0x4B4466, the steps @0x4B458A..0x4B45CA]
	assert_true(at.x - origin.x > 0.9 and origin.y - at.y > 0.6,
			"the host's copy of the joiner chased the pose the C2S 0x0C uplink carried")
	assert_almost_eq(at.x, target.x, 0.2, "into the deadband of the written x")
	assert_almost_eq(at.y, target.y, 0.2, "and of the written y")

	# Past 2 units the authority drops the gap with no snap: the host's copy holds.
	# [orig: Entity_UpdateInfantryPlayerBody - the drop @0x4B432A..0x4B4330, the cleared
	#  target and steps @0x4B4367..0x4B4386]
	var held := at
	assert_eq(joiner.debug_apply_local_pose(held.x + 3.5, held.y - 2.25, held.z, 45.0, -10.0),
			OK, "the joiner takes a write 4.2 units off")
	at = _host_copy_after(host, joiner, self_handle, 30)
	assert_almost_eq(at.x, held.x, 0.2, "the host's copy holds against a gap over 2 units")
	assert_almost_eq(at.y, held.y, 0.2, "on both axes")

	assert_eq(joiner.debug_apply_local_pose(target.x, target.y, target.z, 0.0, 120.0),
			ERR_INVALID_PARAMETER, "a pitch past 90 has no ApplyPose word")
	assert_eq(host.debug_apply_local_pose(0.0, 0.0, 0.0, 0.0, 0.0), ERR_UNAVAILABLE,
			"the host keeps teleport_local_player: no joiner pose write")


# Steps the pair `frames` times (the sender first: its net leg ships L's pose ahead of its
# own motor) and returns the host's copy of the joiner where it stands.
func _host_copy_after(host: Simulation, joiner: Simulation, handle: int, frames: int) -> Vector3:
	var at := Vector3.INF
	for _i in range(frames):
		joiner.step()
		host.step()
		var card: EntityCard = host.entity_card(handle)
		if card != null:
			at = card.get_mission_position()
		OS.delay_msec(2)
	return at


func test_no_session_and_single_player_refuse_the_joiner_write() -> void:
	var bare := Simulation.new()
	assert_eq(bare.debug_apply_local_pose(0.0, 0.0, 0.0, 0.0, 0.0), ERR_UNAVAILABLE,
			"no mission, no L")
	var world := WorldFixture.boot_minimal(self)
	var sim := world.get_sim()
	assert_true(sim.has_local_player())
	assert_eq(sim.debug_apply_local_pose(0.0, 0.0, 0.0, 0.0, 0.0), ERR_UNAVAILABLE,
			"single player keeps teleport_local_player")
