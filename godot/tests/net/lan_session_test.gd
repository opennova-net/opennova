extends GutTest

# Production LAN browser seam over real loopback UDP. The browser sends the
# retail game-session 0x41 probe and projects the host's 0x81 reply; it does not
# use an online service and intentionally cannot know the mission before join.


func _mission() -> MissionData:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_true(mission.set_header_string("mission_name", "Discovery Island"))
	mission.add_entity(3, 0, Vector3.ZERO, Vector3.ZERO)
	return mission


func test_browse_defaults_pin_the_retail_game_port_range() -> void:
	# Stock JO's enumerator walks 32768..32787 by default, broadcasting to the
	# configured discovery target [orig: CNapiNPConnection_PumpEnumeratorAndSend
	# @ 0x6290c0 port walk; docs/net/novaworld-net-re.md 5.0c]. The registered
	# defaults are the seam every menu browse rides.
	var found := false
	for m in ClassDB.class_get_method_list("LanSession"):
		if String(m.get("name", "")) != "start_browsing":
			continue
		found = true
		var defaults: Array = m.get("default_args", [])
		assert_eq(defaults.size(), 4,
				"destination/port_min/port_max/connect_type all carry registered defaults")
		if defaults.size() == 4:
			assert_eq(String(defaults[0]), "255.255.255.255",
					"the default discovery target is the local broadcast address")
			assert_eq(int(defaults[1]), 32768, "retail range start")
			assert_eq(int(defaults[2]), 32787, "retail range end")
			assert_eq(int(defaults[3]), JoinTarget.NETWORK_LAN,
					"the menu browse runs as the LAN screen's network connect type")
	assert_true(found, "start_browsing is registered")


func test_browser_discovers_live_host_without_pre_auth_mission_metadata() -> void:
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.server_name = "Kitchen LAN"
	host_options.mission_name = "Discovery Island"
	host_options.mission_file = "DISCOVERY_A1.BMS"
	host_options.expansion = "jox01"
	host_options.game_type = 0x30020
	host_options.max_players = 6
	host_options.spectator_slots = -1
	host_options.spectator_password = "watch"
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(_mission()))
	var port := host.get_host_listen_port()
	assert_gt(port, 0)

	var browser := LanSession.new()
	add_child_autofree(browser)
	assert_eq(browser.start_browsing("127.0.0.1", port, port), OK)
	assert_true(browser.is_browsing())

	var rows: Array = []
	for _i in range(180):
		host.step()
		await get_tree().process_frame
		rows = browser.get_servers()
		if not rows.is_empty():
			break
		OS.delay_msec(2)
	assert_eq(rows.size(), 1, "one endpoint reply becomes one browser row")
	if rows.size() == 1:
		var row: LanServerRow = rows[0]
		assert_eq(row.server_name, "Kitchen LAN")
		assert_eq(row.host_ip, "127.0.0.1")
		assert_eq(row.port, port)
		assert_eq(row.players, 1, "listen host occupies one player slot")
		assert_eq(row.max_players, 6)
		assert_eq(row.gametype, 0x30020)
		var flags := row.server_flags
		assert_ne(flags & JoinTarget.FLAG_ALLOW_SPECTATORS, 0,
				"ServerHello.P2 advertises that spectators are enabled")
		assert_ne(flags & JoinTarget.FLAG_SPECTATOR_PASSWORD, 0,
				"ServerHello.P2 advertises the spectator-password prompt")
		assert_eq(row.expansion, "jox01")
		# The row carries the browse's connection index, which a join from it goes
		# on with (D-NET-521).
		assert_gt(browser.get_client_index(), 0, "the browse took a connection index")
		assert_eq(row.client_index, browser.get_client_index(),
				"the row carries the browse's CI")
		assert_eq(row.session_id, "",
				"an unmodeled SUS1 is omitted instead of fabricated")
		assert_false("mission" in row,
				"retail discovery does not invent pre-auth mission metadata")
	browser.stop()
	assert_false(browser.is_browsing())


# A browse lists only the sessions whose ServerHello.P2 admits its network
# connect type: a LAN host's P2 carries the LAN transport bit, so a NovaWorld
# browse (the NovaWorld preflight's type) never lists it, as the LAN screen never
# lists a NovaWorld host (the engine's lan_session_admits_connect_type; ctest
# lan_discovery pins the full matrix).
func test_novaworld_browse_does_not_list_a_lan_host() -> void:
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.server_name = "Typed LAN"
	host_options.game_type = 0x30020
	host_options.max_players = 4
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(_mission()))
	var port := host.get_host_listen_port()
	assert_gt(port, 0)

	var nw_browser := LanSession.new()
	add_child_autofree(nw_browser)
	assert_eq(nw_browser.start_browsing("127.0.0.1", port, port, JoinTarget.NETWORK_NOVAWORLD), OK)
	var lan_browser := LanSession.new()
	add_child_autofree(lan_browser)
	assert_eq(lan_browser.start_browsing("127.0.0.1", port, port, JoinTarget.NETWORK_LAN), OK)
	for _i in range(180):
		host.step()
		await get_tree().process_frame
		if not lan_browser.get_servers().is_empty():
			break
		OS.delay_msec(2)
	# Give the NovaWorld browse the same replies a few more frames.
	for _i in range(10):
		host.step()
		await get_tree().process_frame
		OS.delay_msec(2)
	assert_eq(lan_browser.get_servers().size(), 1, "the LAN browse lists the LAN host")
	assert_eq(nw_browser.get_servers().size(), 0, "a NovaWorld browse does not list a LAN host")
	nw_browser.stop()
	lan_browser.stop()


func test_duplicate_replies_from_one_endpoint_collapse_to_one_row() -> void:
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.server_name = "Duplicate Reply LAN"
	host_options.game_type = 0x30020
	host_options.max_players = 4
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(_mission()))
	var host_port := host.get_host_listen_port()
	assert_gt(host_port, 0)

	# Relay one browser probe through a live production host to obtain its real
	# ServerHello, then send that exact 0x81 twice from one controlled endpoint.
	# This drives LanSession's endpoint de-duplication without depending on
	# whether the platform loops global broadcast back to the local machine.
	var responder := PacketPeerUDP.new()
	assert_eq(responder.bind(0, "127.0.0.1"), OK)
	var responder_port := responder.get_local_port()
	assert_gt(responder_port, 0)

	var browser := LanSession.new()
	add_child_autofree(browser)
	assert_eq(browser.start_browsing("127.0.0.1", responder_port, responder_port), OK)

	var probe := PackedByteArray()
	var browser_ip := ""
	var browser_port := 0
	for _i in range(180):
		if responder.get_available_packet_count() > 0:
			probe = responder.get_packet()
			browser_ip = responder.get_packet_ip()
			browser_port = responder.get_packet_port()
			break
		await get_tree().process_frame
		OS.delay_msec(2)
	assert_false(probe.is_empty(), "controlled responder received the browser's 0x41")
	assert_gt(browser_port, 0)
	if probe.is_empty() or browser_port <= 0:
		browser.stop()
		responder.close()
		return

	assert_eq(responder.set_dest_address("127.0.0.1", host_port), OK)
	assert_eq(responder.put_packet(probe), OK)
	var reply := PackedByteArray()
	for _i in range(180):
		host.step()
		await get_tree().process_frame
		while responder.get_available_packet_count() > 0:
			var candidate := responder.get_packet()
			var source_port := responder.get_packet_port()
			if source_port == host_port:
				reply = candidate
				break
		if not reply.is_empty():
			break
		OS.delay_msec(2)
	assert_false(reply.is_empty(), "live host returned a production 0x81 through the responder")
	if reply.is_empty():
		browser.stop()
		responder.close()
		return

	assert_eq(responder.set_dest_address(browser_ip, browser_port), OK)
	assert_eq(responder.put_packet(reply), OK)
	assert_eq(responder.put_packet(reply), OK)

	# Let both loopback datagrams reach the socket before observing the final
	# snapshot. Breaking on the first visible row would let a late duplicate
	# arrive after the assertion and weaken this regression.
	for _i in range(30):
		await get_tree().process_frame
		OS.delay_msec(2)
	var rows: Array = browser.get_servers()
	assert_eq(rows.size(), 1, "duplicate 0x81 replies from one endpoint remain one browser row")
	if rows.size() == 1:
		var row: LanServerRow = rows[0]
		assert_eq(row.server_name, "Duplicate Reply LAN")
		assert_eq(row.host_ip, "127.0.0.1")
		assert_eq(row.port, responder_port)

	browser.stop()
	responder.close()


# D-NET-521: the browse row's CI rides the join end to end. JoinTarget.from_lan_row
# carries it as session_client_index, Simulation.set_join_discovered_session (the seam
# SessionDrive's join calls with its target) hands it to the joiner's discovered
# session, and the joiner's 0x42 goes out under it: the host echoes that CI in its
# 0x82, which the joiner accepts.
func test_browse_row_ci_reaches_the_joiner_connection() -> void:
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.server_name = "Row CI LAN"
	host_options.game_type = 0x30020
	host_options.max_players = 4
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(_mission()))
	var port := host.get_host_listen_port()
	assert_gt(port, 0)

	var browser := LanSession.new()
	add_child_autofree(browser)
	assert_eq(browser.start_browsing("127.0.0.1", port, port), OK)
	var rows: Array = []
	for _i in range(180):
		host.step()
		await get_tree().process_frame
		rows = browser.get_servers()
		if not rows.is_empty():
			break
		OS.delay_msec(2)
	browser.stop()
	assert_eq(rows.size(), 1, "the browse lists the host")
	if rows.size() != 1:
		return
	var row: LanServerRow = rows[0]
	assert_gt(row.client_index, 0)
	var target := JoinTarget.from_lan_row(row)
	assert_eq(target.session_client_index, row.client_index,
			"the join target carries the browse's CI")

	var joiner := Simulation.new()
	joiner.set_join_discovered_session(target)
	assert_true(joiner.enable_join("127.0.0.1", port, "RowCiJoiner"))
	joiner.poll_join_preload()
	assert_eq(int(joiner.get_joiner_network_diagnostics()["client_index"]), row.client_index,
			"the joiner's connection goes on with the browse's CI")
	var accepted := false
	for _i in range(180):
		host.step()
		joiner.poll_join_preload()
		# Phase 3 is Driving, 4 InMatch (5 is Error): the host's 0x82 for this CI was
		# accepted.
		var phase := joiner.get_joiner_phase()
		if phase == 3 or phase == 4:
			accepted = true
			break
		OS.delay_msec(2)
	assert_true(accepted, "the host's 0x82 echoes the browse's CI and the joiner takes it")
	assert_eq(int(joiner.get_joiner_network_diagnostics()["client_index"]), row.client_index)


# `--lan-join browse:<port>` (the parity runner's joiner) joins as a player does
# from the LAN screen: a search on LanSession's defaults, then the join from the
# row on that port as its discovered session (JoinTarget.from_lan_row). A row on
# another port is passed over; the row taken ends the search. This row's
# spectator flag stops the join at the role question, so no world load starts.
func test_launch_browse_joins_the_row_on_its_port() -> void:
	LaunchFlags.set_args_override(PackedStringArray(["--lan-join", "browse:32786"]))
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := NetSessionController.new()
	add_child_autofree(controller)
	controller.setup(null, null, null, layer)
	assert_true(controller.maybe_launch_lan_from_flags(), "the browse form is a join launch")
	var browse := controller.get_node_or_null("LaunchBrowse") as LanSession
	assert_not_null(browse, "the launch starts a LAN search")
	if browse == null:
		LaunchFlags.clear_args_override()
		return
	assert_null(controller.get_node_or_null("SpectatorPreflight"),
			"the search is the join's only enumeration")
	var other := LanServerRow.make("Other Host", "192.0.2.7", 32770)
	browse.servers_changed.emit([other])
	assert_null(layer.get_node_or_null("JoinRolePrompt"), "a row on another port is passed over")
	var row := LanServerRow.make("Untitled ", "192.0.2.8", 32786)
	row.server_flags = JoinTarget.FLAG_ALLOW_SPECTATORS
	row.client_index = 5
	browse.servers_changed.emit([other, row])
	assert_false(browse.is_browsing(), "the row taken ends the search")
	var prompt := layer.get_node_or_null("JoinRolePrompt")
	assert_not_null(prompt, "the row's session goes on to the join")
	if prompt != null:
		var labels := prompt.find_children("*", "Label", true, false)
		assert_gt(labels.size(), 0)
		if labels.size() > 0:
			assert_eq((labels[0] as Label).text, "Join Untitled ", "the join is the row's")
	LaunchFlags.clear_args_override()


func test_invalid_port_range_fails_without_browsing() -> void:
	var browser := LanSession.new()
	add_child_autofree(browser)
	assert_eq(browser.start_browsing("127.0.0.1", 32788, 32768), ERR_INVALID_PARAMETER)
	assert_false(browser.is_browsing())
