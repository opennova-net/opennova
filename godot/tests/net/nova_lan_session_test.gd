extends GutTest

# Production LAN browser seam over real loopback UDP. The browser sends the
# retail game-session 0x41 probe and projects the host's 0x81 reply; it does not
# use an online service and intentionally cannot know the mission before join.


func _mission() -> NovaMissionData:
	var mission := NovaMissionData.new()
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
	for m in ClassDB.class_get_method_list("NovaLanSession"):
		if String(m.get("name", "")) != "start_browsing":
			continue
		found = true
		var defaults: Array = m.get("default_args", [])
		assert_eq(defaults.size(), 3,
				"destination/port_min/port_max all carry registered defaults")
		if defaults.size() == 3:
			assert_eq(String(defaults[0]), "255.255.255.255",
					"the default discovery target is the local broadcast address")
			assert_eq(int(defaults[1]), 32768, "retail range start")
			assert_eq(int(defaults[2]), 32787, "retail range end")
	assert_true(found, "start_browsing is registered")


func test_browser_discovers_live_host_without_pre_auth_mission_metadata() -> void:
	var host := NovaSimulation.new()
	host.configure_host_session({
		"server_name": "Kitchen LAN",
		"mission_name": "Discovery Island",
		"mission_file": "DISCOVERY_A1.BMS",
		"expansion": "jox01",
		"gametype": 0x30020,
		"max_players": 6,
	})
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(_mission()))
	var port := host.get_host_listen_port()
	assert_gt(port, 0)

	var browser := NovaLanSession.new()
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
		var row: Dictionary = rows[0]
		assert_eq(String(row.get("server_name", "")), "Kitchen LAN")
		assert_eq(String(row.get("host_ip", "")), "127.0.0.1")
		assert_eq(int(row.get("port", 0)), port)
		assert_eq(int(row.get("players", 0)), 1, "listen host occupies one player slot")
		assert_eq(int(row.get("max_players", 0)), 6)
		assert_eq(int(row.get("gametype", 0)), 0x30020)
		assert_eq(String(row.get("expansion", "")), "jox01")
		assert_eq(String(row.get("session_id", "")), "",
				"an unmodeled SUS1 is omitted instead of fabricated")
		assert_false(row.has("mission"),
				"retail discovery does not invent pre-auth mission metadata")
	browser.stop()
	assert_false(browser.is_browsing())
	host.free()


func test_duplicate_replies_from_one_endpoint_collapse_to_one_row() -> void:
	var host := NovaSimulation.new()
	host.configure_host_session({
		"server_name": "Duplicate Reply LAN",
		"gametype": 0x30020,
		"max_players": 4,
	})
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(_mission()))
	var host_port := host.get_host_listen_port()
	assert_gt(host_port, 0)

	# Relay one browser probe through a live production host to obtain its real
	# ServerHello, then send that exact 0x81 twice from one controlled endpoint.
	# This drives NovaLanSession's endpoint de-duplication without depending on
	# whether the platform loops global broadcast back to the local machine.
	var responder := PacketPeerUDP.new()
	assert_eq(responder.bind(0, "127.0.0.1"), OK)
	var responder_port := responder.get_local_port()
	assert_gt(responder_port, 0)

	var browser := NovaLanSession.new()
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
		host.free()
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
		host.free()
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
		assert_eq(String(rows[0].get("server_name", "")), "Duplicate Reply LAN")
		assert_eq(String(rows[0].get("host_ip", "")), "127.0.0.1")
		assert_eq(int(rows[0].get("port", 0)), responder_port)

	browser.stop()
	responder.close()
	host.free()


func test_invalid_port_range_fails_without_browsing() -> void:
	var browser := NovaLanSession.new()
	add_child_autofree(browser)
	assert_eq(browser.start_browsing("127.0.0.1", 32788, 32768), ERR_INVALID_PARAMETER)
	assert_false(browser.is_browsing())
