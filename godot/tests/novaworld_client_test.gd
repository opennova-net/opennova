extends GutTest

# Smoke coverage for the NovaWorldClient GDExtension binding (the Godot-side
# novaworld client over engine/net/novacrypto + engine/net/napi + engine/net/novaworld).
# No live server: this pins registration, defaults, property round-trips,
# and that start()/stop() drive the state machine without crashing. The
# wire behavior itself is covered by the C++ net ctests and, end to end,
# by the live two-client smokes.


func test_class_is_registered() -> void:
	assert_true(ClassDB.class_exists("NovaWorldClient"),
		"NovaWorldClient is registered by the GDExtension")


func test_defaults_and_property_roundtrip() -> void:
	var client := NovaWorldClient.new()
	assert_eq(client.get_state(), NovaWorldClient.STATE_IDLE, "starts Idle")
	assert_false(client.is_session_active(), "no session before start()")
	assert_false(client.is_authenticated(), "a transport session is not an account login")
	assert_eq(client.host, "127.0.0.1")
	assert_eq(client.gate_port, 7597, "gate probe port matches the retail gate")

	client.host = "203.0.113.10"
	client.gate_port = 17597
	client.player_name = "GutPlayer"
	assert_eq(client.host, "203.0.113.10")
	assert_eq(client.gate_port, 17597)
	assert_eq(client.player_name, "GutPlayer")
	client.free()


func test_browser_and_join_are_rejected_before_authentication() -> void:
	var client := NovaWorldClient.new()
	add_child_autofree(client)
	watch_signals(client)
	client.refresh_servers()
	assert_signal_emitted(client, "server_list_failed")
	assert_eq(get_signal_parameters(client, "server_list_failed")[0],
			"Sign in before loading games.")
	client.join(77)
	assert_signal_emitted(client, "join_failed")
	assert_eq(get_signal_parameters(client, "join_failed")[0],
			"Sign in before joining a game.")
	assert_eq(client.get_server_rows().size(), 0, "no pre-login list is exposed")


func test_start_probes_gate_and_stop_is_idempotent() -> void:
	var client := NovaWorldClient.new()
	add_child_autofree(client)
	client.host = "127.0.0.1"
	client.gate_port = 1  # nothing listens here; the probe must not crash

	client.start()
	assert_eq(client.get_state(), NovaWorldClient.STATE_GATE_PROBING,
		"start() enters GateProbing even with no server")

	# A few frames of _process with no reply must not crash or connect.
	await wait_frames(3)
	assert_false(client.is_session_active())

	client.stop()
	assert_eq(client.get_state(), NovaWorldClient.STATE_DISCONNECTED,
		"stop() lands in Disconnected")
	client.stop()
	assert_eq(client.get_state(), NovaWorldClient.STATE_DISCONNECTED,
		"stop() is idempotent")

	# start() is restartable from Disconnected.
	client.start()
	assert_eq(client.get_state(), NovaWorldClient.STATE_GATE_PROBING,
		"start() works again after a stop()")
	client.stop()


# The session socket opens first and stands as the process's transport socket; the gate
# worker's socket, bound after it, is the gate manager's own and takes nothing from it
# (engine: inmatch::gate_port_quad carries the witness).
func test_the_session_socket_stays_the_process_transport_past_the_gate_bind() -> void:
	var client := NovaWorldClient.new()
	add_child_autofree(client)
	client.host = "127.0.0.1"
	client.gate_port = 1
	client.start()
	var pump: UdpPump = client.get_game_pump()
	assert_not_null(pump, "start() made the session socket")
	if pump != null:
		assert_true(pump.is_open(), "the session socket bound")
		assert_true(pump.is_process_transport(),
			"the session socket, not the gate's, is the process's transport socket")
	client.stop()


# A session socket that binds nothing goes on, as retail's failed mode-4 open does: the
# gate's start runs as ever over a closed session socket, whose datagrams go nowhere
# (engine: inmatch::write_novaworld_socket_open_lines carries the witness).
func test_a_failed_session_socket_open_goes_on_to_the_gate_probe() -> void:
	var blocker := UdpPump.new()
	assert_eq(blocker.bind_listen(0), OK)
	var occupied_port := blocker.local_port()
	assert_gt(occupied_port, 0)
	var block := GameConfigBlock.new()
	block.set_word("mpnovaworldportmin", occupied_port)
	block.set_word("mpnovaworldportmax", occupied_port)
	block.set_word("mpnovaworldportdelta", 1)
	block.set_word("mpnovaworldportrandom", 0)
	var client := NovaWorldClient.new()
	add_child_autofree(client)
	client.set_game_config(block)
	client.host = "127.0.0.1"
	client.gate_port = 1
	client.start()
	assert_eq(client.get_state(), NovaWorldClient.STATE_GATE_PROBING,
		"the failed session socket open goes on to the gate's start")
	var pump: UdpPump = client.get_game_pump()
	assert_not_null(pump)
	if pump != null:
		assert_false(pump.is_open(), "the session socket stays closed")
	assert_true(blocker.is_process_transport(),
		"neither the closed session socket nor the gate's took the process transport")
	client.stop()
	assert_eq(client.get_state(), NovaWorldClient.STATE_DISCONNECTED)
	blocker.close()


func test_late_join_http_completion_cannot_resurrect_a_stopped_or_restarted_client() -> void:
	var client := NovaWorldClient.new()
	add_child_autofree(client)
	client.host = "127.0.0.1"
	client.gate_port = 1
	watch_signals(client)
	client.start()
	client.stop()

	client.on_join_request_completed(
			HTTPRequest.RESULT_REQUEST_FAILED, 0,
			PackedStringArray(), PackedByteArray())

	assert_eq(client.get_state(), NovaWorldClient.STATE_DISCONNECTED,
			"a cancelled join completion cannot reconnect a stopped client")
	assert_signal_not_emitted(client, "connected")
	assert_signal_not_emitted(client, "join_failed")

	client.start()
	client.on_join_request_completed(
			HTTPRequest.RESULT_REQUEST_FAILED, 0,
			PackedStringArray(), PackedByteArray())
	assert_eq(client.get_state(), NovaWorldClient.STATE_GATE_PROBING,
			"an old join completion cannot supersede a restarted lifecycle")
	assert_signal_not_emitted(client, "connected")
	assert_signal_not_emitted(client, "join_failed")
	client.stop()


func test_late_host_http_completion_cannot_resurrect_a_stopped_or_restarted_client() -> void:
	var client := NovaWorldClient.new()
	add_child_autofree(client)
	client.host = "127.0.0.1"
	client.gate_port = 1
	watch_signals(client)
	client.start()
	client.stop()

	client.on_host_request_completed(
			HTTPRequest.RESULT_REQUEST_FAILED, 0,
			PackedStringArray(), PackedByteArray())
	assert_eq(client.get_state(), NovaWorldClient.STATE_DISCONNECTED,
			"a cancelled hosting-page completion cannot reconnect a stopped client")
	assert_signal_not_emitted(client, "host_failed")

	client.start()
	client.on_host_request_completed(
			HTTPRequest.RESULT_REQUEST_FAILED, 0,
			PackedStringArray(), PackedByteArray())
	assert_eq(client.get_state(), NovaWorldClient.STATE_GATE_PROBING,
			"an old hosting-page completion cannot supersede a restarted lifecycle")
	assert_signal_not_emitted(client, "host_failed")
	client.stop()


func test_late_login_http_completion_cannot_fail_a_restarted_client() -> void:
	var client := NovaWorldClient.new()
	add_child_autofree(client)
	client.host = "127.0.0.1"
	client.gate_port = 1
	watch_signals(client)
	client.start()
	client.stop()

	client.on_login_request_completed(
			HTTPRequest.RESULT_REQUEST_FAILED, 0,
			PackedStringArray(), PackedByteArray())
	assert_eq(client.get_state(), NovaWorldClient.STATE_DISCONNECTED)
	assert_signal_not_emitted(client, "login_failed")

	client.start()
	client.on_login_request_completed(
			HTTPRequest.RESULT_REQUEST_FAILED, 0,
			PackedStringArray(), PackedByteArray())
	assert_eq(client.get_state(), NovaWorldClient.STATE_GATE_PROBING,
			"an old login completion cannot supersede a restarted lifecycle")
	assert_signal_not_emitted(client, "login_failed")
	client.stop()


func test_every_http_request_has_a_finite_timeout() -> void:
	var client := NovaWorldClient.new()
	add_child_autofree(client)
	client.host = "127.0.0.1"
	client.gate_port = 1
	client.start()

	var requests: Array[HTTPRequest] = []
	for child in client.get_children():
		if child is HTTPRequest:
			requests.append(child as HTTPRequest)
	assert_eq(requests.size(), 4, "browser, login, join, and host each own one HTTP request")
	for request in requests:
		assert_gt(request.timeout, 0.0, "no Matchmaking HTTP leg can wait forever")
	client.stop()


func test_server_info_empty_before_connect() -> void:
	var client := NovaWorldClient.new()
	assert_null(client.get_server_info(), "no server info before a gate response")
	client.free()


# The proxy-assisted join crosses the shell typed (copy_join_proxy_to hands the
# engine's ProxyRendezvousConfig to the JoinTarget the mission root installs):
# with no resolved join the client has none and the copy leaves the target's
# disabled, and the "ip:port" string form is gone. The engine half
# (proxy_rendezvous_from_join, the 48-byte rendezvous, the all-six gate) is
# the http_flow and joiner_role ctests.
func test_join_proxy_crosses_typed_and_starts_disabled() -> void:
	var client := NovaWorldClient.new()
	var target := JoinTarget.new()
	assert_false(client.has_join_proxy(), "no resolved join: no proxy")
	client.copy_join_proxy_to(target)
	assert_false(target.has_join_proxy(), "the copy of no proxy is no proxy")
	assert_false("proxy_node" in target, "no string endpoint property")
	assert_false("proxy_relay" in target, "no string relay property")
	client.free()
