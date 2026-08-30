extends GutTest

# OpenNova-to-OpenNova over production loopback UDP. The byte-level native
# suites separately pin the same ClientAuth/ServerAuth fields against retail.


func _mission() -> MissionData:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_entity(
			MissionData.KIND_MARKER, 6094,
			Vector3.ZERO, Vector3.ZERO).is_empty())
	return mission


func _host(mission: MissionData) -> Simulation:
	var host := Simulation.new()
	host.configure_host_session({
		"server_name": "Spectator Session",
		"mission_name": "Spectator Session",
		"mission_file": "SPECTATOR_TEST.BMS",
		"gametype": 0x30020,
		"max_players": 4,
		"spectator_slots": -1,
		"spectator_password": "watch",
	})
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	return host


func test_spectator_role_reaches_in_match_and_latches_from_tag_75() -> void:
	var mission := _mission()
	var host := _host(mission)
	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "SpectatorJoiner",
			JoinTarget.ROLE_SPECTATOR, "watch"))
	assert_true(joiner.load_from_mission_data(mission))

	var reached := false
	var host_tick := host.get_logic_tick()
	for _i in range(800):
		host.step()
		joiner.step()
		if joiner.is_joined_in_match() and joiner.is_local_spectator():
			reached = true
			break
		OS.delay_msec(2)

	assert_true(reached,
			"JSR/JSPP admission reaches the spectator in-match state")
	assert_gt(host.get_logic_tick(), host_tick,
			"the host world keeps ticking throughout spectator admission")
	assert_eq(joiner.get_join_assigned_team(), 0,
			"S2C 0x75 assigns the spectator's retail team 0")
	assert_false(joiner.set_local_spectator(false),
			"a joiner cannot overwrite host-owned spectator state through F3")
	host.free()
	joiner.free()


func test_wrong_spectator_password_surfaces_retail_join_failure() -> void:
	var mission := _mission()
	var host := _host(mission)
	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "RejectedSpectator",
			JoinTarget.ROLE_SPECTATOR, "wrong"))

	var reason := ""
	for _i in range(400):
		joiner.poll_join_preload()
		host.step()
		reason = joiner.get_join_error()
		if not reason.is_empty():
			break
		OS.delay_msec(2)

	assert_string_contains(reason.to_lower(), "spectator password",
			"retail JFC 16 becomes an actionable join error")
	assert_eq(host.get_host_peer_count(), 0,
			"a rejected spectator never consumes a connection slot")
	host.free()
	joiner.free()
