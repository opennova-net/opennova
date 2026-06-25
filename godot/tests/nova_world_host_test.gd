extends GutTest

# Smoke coverage for the NovaWorldHost GDExtension binding — the host-direction
# sibling of NovaWorldClient that registers a locally-hosted game with the gate
# (ClientHostRequest / ClientHostUpdate over the witnessed NWU lobby session).
# No live gate here: this pins registration, defaults, property round-trips, and
# that start()/stop()/set_player_count() drive the state machine without
# crashing. The wire shape is covered by the C++ net ctests
# (client_session_loopback_test steps 8-9, session_test).


func test_class_is_registered() -> void:
	assert_true(ClassDB.class_exists("NovaWorldHost"),
		"NovaWorldHost is registered by the GDExtension")


func test_defaults_and_property_roundtrip() -> void:
	var host := NovaWorldHost.new()
	assert_eq(host.get_state(), NovaWorldHost.STATE_IDLE, "starts Idle")
	assert_false(host.is_hosting(), "not hosting before start()")
	assert_eq(host.host, "127.0.0.1")
	assert_eq(host.gate_port, 7597, "gate probe port matches the retail gate")
	assert_eq(host.game_port, 32768, "default game port is the NW session port")
	assert_eq(host.max_players, 32)
	assert_eq(host.get_player_count(), 1, "the host itself is the first player")

	host.host = "203.0.113.10"
	host.gate_port = 17597
	host.server_name = "Taylor's Game"
	host.mission_name = "ASH_G11A"
	host.game_port = 40000
	host.max_players = 16
	host.region = "eu"
	host.player_name = "Taylor"
	host.advertise_ip = "203.0.113.10"
	host.app_id = "28"
	host.lobby_name = "jop_2_consumer"
	assert_eq(host.host, "203.0.113.10")
	assert_eq(host.gate_port, 17597)
	assert_eq(host.server_name, "Taylor's Game")
	assert_eq(host.mission_name, "ASH_G11A")
	assert_eq(host.game_port, 40000)
	assert_eq(host.max_players, 16)
	assert_eq(host.region, "eu")
	assert_eq(host.player_name, "Taylor")
	assert_eq(host.advertise_ip, "203.0.113.10")
	host.free()


func test_start_probes_gate_and_stop_is_idempotent() -> void:
	var host := NovaWorldHost.new()
	add_child_autofree(host)
	host.host = "127.0.0.1"
	host.gate_port = 1  # nothing listens here; the probe must not crash

	host.start()
	assert_eq(host.get_state(), NovaWorldHost.STATE_GATE_PROBING,
		"start() enters GateProbing even with no gate")

	# A few frames of _process with no reply must not crash or register.
	await wait_frames(3)
	assert_false(host.is_hosting())

	host.stop()
	assert_eq(host.get_state(), NovaWorldHost.STATE_DISCONNECTED,
		"stop() lands in Disconnected")
	host.stop()
	assert_eq(host.get_state(), NovaWorldHost.STATE_DISCONNECTED,
		"stop() is idempotent")

	# start() is restartable from Disconnected.
	host.start()
	assert_eq(host.get_state(), NovaWorldHost.STATE_GATE_PROBING,
		"start() works again after a stop()")
	host.stop()


func test_set_player_count_before_hosting_just_stores() -> void:
	var host := NovaWorldHost.new()
	add_child_autofree(host)
	# Not hosting yet: set_player_count just records the value (no send, no crash).
	host.set_player_count(3)
	assert_eq(host.get_player_count(), 3, "player count stored while idle")
	host.set_player_count(-5)
	assert_eq(host.get_player_count(), 0, "player count clamps at 0")
