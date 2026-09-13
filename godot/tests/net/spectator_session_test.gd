extends GutTest

# OpenNova-to-OpenNova over production loopback UDP. The byte-level native
# suites separately pin the same ClientAuth/ServerAuth fields against retail.


func _mission() -> MissionData:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_not_null(mission.add_entity(
			MissionData.KIND_MARKER, 6094,
			Vector3.ZERO, Vector3.ZERO))
	return mission


func _host(mission: MissionData) -> Simulation:
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.server_name = "Spectator Session"
	host_options.mission_name = "Spectator Session"
	host_options.mission_file = "SPECTATOR_TEST.BMS"
	host_options.game_type = 0x30020
	host_options.max_players = 4
	host_options.spectator_slots = -1
	host_options.spectator_password = "watch"
	host.configure_host_session(host_options)
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
			"the retail DPC 16 description punt becomes an actionable join error")
	# Retail rejects the spectator AFTER admission (the game-layer join gate),
	# then the punted joiner answers with CLIENT_GOODBYE and the host reaps the
	# node. Pump both ends until that teardown completes.
	var peers := host.get_host_peer_count()
	for _i in range(200):
		joiner.poll_join_preload()
		host.step()
		peers = host.get_host_peer_count()
		if peers == 0:
			break
		OS.delay_msec(2)
	assert_eq(peers, 0,
			"the rejected spectator's punted connection is reaped")


func _protected_host(mission: MissionData) -> Simulation:
	var host := Simulation.new()
	var options := HostSessionOptions.new()
	options.server_name = "Protected Teams"
	options.mission_file = "TEAM_PASSWORD_TEST.BMS"
	options.game_type = 0x10000
	options.mp_attributes = 4
	options.max_players = 4
	options.side_a_password = "BlueSecret"
	options.side_b_password = "RedSecret"
	host.configure_host_session(options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	return host


func test_two_same_named_players_join_protected_sides_with_distinct_identity() -> void:
	var mission := _mission()
	var host := _protected_host(mission)
	var blue := Simulation.new()
	var red := Simulation.new()
	var automatic := CharacterJoinProfile.new()
	blue.set_join_character_profile(automatic)
	red.set_join_character_profile(automatic)
	assert_true(blue.enable_join("127.0.0.1", host.get_host_listen_port(), "SharedName",
			JoinTarget.ROLE_PLAYER, "", "", "blueSECRET"))
	assert_true(red.enable_join("127.0.0.1", host.get_host_listen_port(), "SharedName",
			JoinTarget.ROLE_PLAYER, "", "", "redSECRET"))
	assert_true(blue.load_from_mission_data(mission))
	assert_true(red.load_from_mission_data(mission))
	var reached := false
	for _i in range(1000):
		host.step()
		blue.step()
		red.step()
		if blue.is_joined_in_match() and red.is_joined_in_match():
			reached = true
			break
		OS.delay_msec(2)
	assert_true(reached, "both JSP credentials survive runtime rebuild and admit over real UDP")
	assert_eq(blue.get_join_assigned_team(), 1)
	assert_eq(red.get_join_assigned_team(), 2)
	assert_ne(blue.get_joiner_self_handle(), red.get_joiner_self_handle(),
			"duplicate callsigns keep distinct authenticated entity handles")
	assert_ne(blue.get_joiner_self_handle(), host.get_local_player_wire_handle())
	assert_ne(red.get_joiner_self_handle(), host.get_local_player_wire_handle())


func test_wrong_side_password_surfaces_retail_join_failure() -> void:
	var mission := _mission()
	var host := _protected_host(mission)
	var joiner := Simulation.new()
	assert_true(joiner.enable_join("127.0.0.1", host.get_host_listen_port(), "WrongSide",
			JoinTarget.ROLE_PLAYER, "", "", "wrong"))
	var reason := ""
	for _i in range(400):
		joiner.poll_join_preload()
		host.step()
		reason = joiner.get_join_error()
		if not reason.is_empty():
			break
		OS.delay_msec(2)
	assert_string_contains(reason.to_lower(), "password")
