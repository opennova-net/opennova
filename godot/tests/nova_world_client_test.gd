extends GutTest

# Smoke coverage for the NovaWorldClient GDExtension binding (the Godot-side
# novaworld client over libs/novacrypto + libs/napi + libs/novaworld).
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
	assert_eq(client.host, "127.0.0.1")
	assert_eq(client.gate_port, 7597, "gate probe port matches the retail gate")

	client.host = "203.0.113.10"
	client.gate_port = 17597
	client.player_name = "GutPlayer"
	assert_eq(client.host, "203.0.113.10")
	assert_eq(client.gate_port, 17597)
	assert_eq(client.player_name, "GutPlayer")
	client.free()


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


func test_server_info_empty_before_connect() -> void:
	var client := NovaWorldClient.new()
	var info: Dictionary = client.get_server_info()
	assert_eq(info.size(), 0, "no server info before a gate response")
	client.free()
