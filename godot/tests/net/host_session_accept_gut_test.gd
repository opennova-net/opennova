extends GutTest

# Co-op LAN host wiring (Increment C) on NovaSimulation: enable_host_listen binds a real UDP
# socket and stands up the witnessed host-accept handshake (HostSessionAccept) against the
# live World; on a joiner reaching Spawned the host admits it as a pool-0 player bound to a
# netsim connection. The full handshake->spawn LOGIC is unit-tested in libs
# (tests/novaworld/host_session_accept_test); this exercises the Godot-layer glue —
# socket ownership/binding, the SP path staying undisturbed, and the admit_peer->World-entity
# wiring (admit_test_remote_peer drives the same PeerSpawned reaction without crafting wire
# bytes in GDScript).


func _host_sim(md: NovaMissionData) -> NovaSimulation:
	var sim := NovaSimulation.new()
	assert_true(sim.enable_host_listen(0), "host bound an OS-assigned UDP port")
	assert_true(sim.is_host_listening(), "host listening flag set")
	assert_true(sim.is_listen_server(), "host listen implies the in-process listen server")
	assert_true(sim.load_from_mission_data(md), "promoted with the net seam + host socket")
	return sim


func _two_organics() -> NovaMissionData:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, 0, Vector3(0, 0, 0), Vector3.ZERO)   # KIND_ORGANIC
	md.add_entity(3, 0, Vector3(10, 0, 0), Vector3.ZERO)
	return md


func test_enable_host_listen_binds_and_keeps_sp_present() -> void:
	var sim := _host_sim(_two_organics())
	assert_gt(sim.get_host_listen_port(), 0, "host got a real bound port")
	assert_eq(sim.get_host_peer_count(), 0, "no joiners yet")

	# The host's own SP listen-server present path is unchanged: one host frame replicates the
	# two organics through the loopback wire into the client-decoded present.
	sim.advance_frame()
	var stride: int = sim.get_present_stride()
	var records: int = sim.get_present_snapshot().size() / stride
	assert_eq(records, 2, "host's own client present still replicates the two organics")
	sim.free()


func test_host_receives_joiner_datagrams_without_disturbing_sp() -> void:
	var sim := _host_sim(_two_organics())
	var host_port: int = sim.get_host_listen_port()

	# A joiner dials the host's bound port and sends raw (non-handshake) datagrams. The host's
	# per-frame poll drains them through the accept component; malformed bytes register no JO
	# peer and must not crash or disturb the host's own present.
	var join := NovaUdpPump.new()
	autofree(join)
	assert_eq(join.dial("127.0.0.1", host_port), OK, "joiner dialed the host")
	assert_eq(join.send_to_host(PackedByteArray([0xEE, 0x01, 0x02, 0x03])), OK)

	for _i in range(20):
		sim.advance_frame()
		OS.delay_msec(2)

	assert_eq(sim.get_host_peer_count(), 0, "garbage datagram registered no JointOperations peer")
	var stride: int = sim.get_present_stride()
	var records: int = sim.get_present_snapshot().size() / stride
	assert_eq(records, 2, "host present undisturbed by the inbound socket traffic")
	sim.free()


func test_admit_remote_peer_spawns_a_world_entity() -> void:
	var sim := _host_sim(_two_organics())
	var before: int = sim.get_entity_count()

	# Drive the same PeerSpawned reaction the handshake would: spawn the joiner into the live
	# World + bind it to a connection. (The handshake that produces this event is unit-tested
	# in libs/host_session_accept_test.)
	var spawn_pos := Vector3(20, 0, 30)
	assert_true(sim.admit_test_remote_peer(spawn_pos, 0.0, 2), "joiner admitted into the World")

	var after: int = sim.get_entity_count()
	assert_eq(after, before + 1, "admitting a joiner adds exactly one pool-0 entity")

	# The joiner carries a distinct SSN (0xFFF1) and sits at the admitted position (authoritative
	# registry read; mission<->Godot round-trip). Z may settle on terrain (none here, so exact).
	var found := false
	for i in range(after):
		if sim.get_entity_net_id(i) == 0xFFF1:
			found = true
			var p: Vector3 = sim.get_entity_position(i)
			assert_almost_eq(p.x, spawn_pos.x, 0.5, "joiner spawned at the admitted X")
			assert_almost_eq(p.z, spawn_pos.z, 0.5, "joiner spawned at the admitted Z")
	assert_true(found, "the admitted joiner is present with its own SSN")

	# The host's own player is NOT stolen — admit_peer uses spawn_remote_player (no local_player).
	assert_false(sim.has_local_player(), "admitting a joiner does not claim the host's local player")
	sim.free()


func test_host_listen_off_by_default() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	assert_false(sim.is_host_listening(), "host listening off by default")
	assert_eq(sim.get_host_listen_port(), 0, "no bound port when not listening")
	assert_false(sim.admit_test_remote_peer(Vector3.ZERO, 0.0, 1),
		"admit is a no-op when host listening is off")
	sim.free()
