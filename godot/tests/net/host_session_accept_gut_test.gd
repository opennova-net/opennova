extends GutTest

# Co-op LAN host wiring (Increment C) on Simulation: enable_host_listen binds a real UDP
# socket and stands up the witnessed host-accept handshake (the inmatch host runtime,
# P7) against the live World; on a joiner reaching Spawned the host admits it as a pool-0
# player bound to a replication connection. The full handshake->spawn LOGIC is unit-tested in
# libs (tests/npruntime/handshake_server_test, the P2 retarget of the retired novaworld
# host_session_accept lib); this exercises the Godot-layer glue —
# socket ownership/binding, the SP path staying undisturbed, and the admit_peer->World-entity
# wiring (admit_test_remote_peer drives the same PeerSpawned reaction without crafting wire
# bytes in GDScript).


func _host_sim(md: MissionData) -> Simulation:
	var sim := Simulation.new()
	assert_true(sim.enable_host_listen(0), "host bound an OS-assigned UDP port")
	assert_true(sim.is_host_listening(), "host listening flag set")
	assert_true(sim.is_listen_server(), "host listen implies the in-process listen server")
	assert_true(sim.load_from_mission_data(md), "promoted with the net seam + host socket")
	return sim


func _two_organics() -> MissionData:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, 0, Vector3(0, 0, 0), Vector3.ZERO)   # KIND_ORGANIC
	md.add_entity(3, 0, Vector3(10, 0, 0), Vector3.ZERO)
	return md


func test_enable_host_listen_binds_and_keeps_sp_present() -> void:
	var sim := _host_sim(_two_organics())
	assert_gt(sim.get_host_listen_port(), 0, "host got a real bound port")
	assert_eq(sim.get_host_peer_count(), 0, "no joiners yet")

	# The host's own listen-server present path replicates the two organics + the auto-spawned host
	# player (faithful §5.0 mode-3 bring-up) through the loopback wire into the client-decoded present.
	sim.step()
	var stride: int = sim.get_present_stride()
	var records: int = sim.get_present_snapshot().size() / stride
	assert_eq(records, 3, "host's own client present replicates the two organics + the host player")


func test_host_session_config_survives_native_accept_start() -> void:
	var sim := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.server_name = "Configured Host"
	host_options.mission_name = "Custom Island Test"
	host_options.mission_file = "CUSTOM_A1.BMS"
	host_options.player_name = "HostPlayer"
	host_options.spawn_names = PackedStringArray(["Custom Island Test"])
	sim.configure_host_session(host_options)
	assert_true(sim.enable_host_listen(0), "host bound an OS-assigned UDP port")
	var config := sim.get_host_session_config()
	assert_eq(config.server_name, "Configured Host")
	assert_eq(config.mission_name, "Custom Island Test")
	assert_eq(config.mission_file, "CUSTOM_A1.BMS")
	assert_eq(config.player_name, "HostPlayer")
	assert_eq(config.spawn_names.size(), 1)


func test_loaded_host_session_installs_bms_header() -> void:
	var sim := _host_sim(_two_organics())
	assert_eq(sim.get_mission_header_size(), 616,
		"loaded host session has the BMS header sent by tag=0x0B")


func test_host_receives_joiner_datagrams_without_disturbing_sp() -> void:
	var sim := _host_sim(_two_organics())
	var host_port: int = sim.get_host_listen_port()

	# A joiner dials the host's bound port and sends raw (non-handshake) datagrams. The host's
	# per-frame poll drains them through the accept component; malformed bytes register no JO
	# peer and must not crash or disturb the host's own present.
	var join := UdpPump.new()
	autofree(join)
	assert_eq(join.dial("127.0.0.1", host_port), OK, "joiner dialed the host")
	assert_eq(join.send_to_host(PackedByteArray([0xEE, 0x01, 0x02, 0x03])), OK)

	for _i in range(20):
		sim.step()
		OS.delay_msec(2)

	assert_eq(sim.get_host_peer_count(), 0, "garbage datagram registered no JointOperations peer")
	var stride: int = sim.get_present_stride()
	var records: int = sim.get_present_snapshot().size() / stride
	assert_eq(records, 3, "host present (2 organics + host player) undisturbed by the inbound socket traffic")


func test_admit_remote_peer_spawns_a_world_entity() -> void:
	var sim := _host_sim(_two_organics())
	var before: int = sim.get_entity_count()

	# Drive the same PeerSpawned reaction the handshake would: spawn the joiner into the live
	# World + bind it to a connection. (The handshake that produces this event is unit-tested
	# in engine/host_session_accept_test.)
	var spawn_pos := Vector3(20, 0, 30)
	assert_true(sim.admit_test_remote_peer(spawn_pos, 0.0, 2), "joiner admitted into the World")

	var after: int = sim.get_entity_count()
	assert_eq(after, before + 1, "admitting a joiner adds exactly one pool-0 entity")

	# [D-NET-112] The joiner carries NO SSN (net_id 0) — faithful: a networked player is identified by
	# its handle + ownerConnectionId(dcb), not an allocated id. Find it by the admitted position + a
	# nonzero ownerConnectionId (the host player is elsewhere). Z may settle on terrain (none here, exact).
	var found := false
	for i in range(after):
		var p: Vector3 = sim.get_entity_position(i)
		if abs(p.x - spawn_pos.x) < 0.5 and abs(p.z - spawn_pos.z) < 0.5 \
				and sim.get_entity_owner_connection_id(i) != 0:
			found = true
			assert_eq(sim.get_entity_net_id(i), 0, "joiner carries net_id 0 (no SSN — D-NET-112)")
			assert_almost_eq(p.x, spawn_pos.x, 0.5, "joiner spawned at the admitted X")
			assert_almost_eq(p.z, spawn_pos.z, 0.5, "joiner spawned at the admitted Z")
	assert_true(found, "the admitted joiner is present at the admitted position with its own dcb")

	# The host's own (auto-spawned) player is untouched — admit uses spawn_remote_player, so the joiner
	# is a distinct entity and the host keeps its own local player.
	assert_true(sim.has_local_player(), "the host keeps its own auto-spawned local player after admitting a joiner")


func test_host_listen_off_by_default() -> void:
	var sim := Simulation.new()
	sim.build_demo_mission()
	assert_false(sim.is_host_listening(), "host listening off by default")
	assert_eq(sim.get_host_listen_port(), 0, "no bound port when not listening")
	assert_false(sim.admit_test_remote_peer(Vector3.ZERO, 0.0, 1),
		"admit is a no-op when host listening is off")
