extends GutTest

# Smoke coverage for the NovaWorldClient GDExtension binding (the Godot-side
# novaworld client over engine/net/novacrypto + engine/net/napi + engine/net/novaworld).
# No live server: this pins registration, defaults, property round-trips,
# and that start()/stop() drive the state machine without crashing. The
# wire behavior itself is covered by the C++ net ctests and, end to end,
# by the live smokes recorded in plan/status.md.


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
	assert_eq(requests.size(), 3, "browser, login, and join each own one HTTP request")
	for request in requests:
		assert_gt(request.timeout, 0.0, "no Matchmaking HTTP leg can wait forever")
	client.stop()


func test_server_info_empty_before_connect() -> void:
	var client := NovaWorldClient.new()
	assert_null(client.get_server_info(), "no server info before a gate response")
	client.free()
