extends GutTest

# The SP-as-listen-server present path (ADR 0009/0011). With the listen server on,
# NovaSimulation.get_present_snapshot() returns the state the LOCAL CLIENT decoded off
# the in-process loopback — real entity state serialized through the wire codec
# (libs/netsim NetSystem) and decoded back (libs/novaworld ingame_decode) — instead of
# reading the authoritative AI pool directly. This is the in-Godot end of the Phase 1
# loopback identity guard (tests/netsim/loopback_identity_test).


func test_listen_server_present_reads_client_decoded_state() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	# Two standing organics at known, close positions (KIND_ORGANIC = 3). No route + no
	# anim clips -> they hold, so the decoded positions are their spawn positions.
	md.add_entity(3, 0, Vector3(0, 0, 0), Vector3.ZERO)
	md.add_entity(3, 0, Vector3(10, 0, 0), Vector3.ZERO)

	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	assert_true(sim.is_listen_server(), "listen server enabled before load")
	assert_true(sim.load_from_mission_data(md), "promoted with NetSystem registered ahead of WAC")

	# The AI-pool truth is still readable through the scalar getters (they read the sim
	# directly); key it by net_id so we can match the decoded records.
	var truth := {}  # net_id -> Vector3 (Godot space)
	for i in range(sim.get_entity_count()):
		truth[sim.get_entity_net_id(i)] = sim.get_entity_position(i)
	assert_eq(truth.size(), 2, "two organics got AI brains")

	# One host frame: NetSystem emits the S2C 0x0A frame -> loopback -> the local client
	# decodes it into the ClientState the present pass now reads.
	sim.advance_frame()

	var stride: int = sim.get_present_stride()
	var snap: PackedFloat32Array = sim.get_present_snapshot()
	var records: int = snap.size() / stride
	assert_eq(records, 2, "both organics replicated through the wire (Infantry class)")

	var matched := 0
	for rec in range(records):
		var base := rec * stride
		assert_eq(int(snap[base + NovaSimulation.PF_KIND]), 3,
			"kind resolves to KIND_ORGANIC from the registry behind the decoded handle")
		assert_eq(int(snap[base + NovaSimulation.PF_ALIVE]), 1, "decoded entity is alive")
		var nid := int(snap[base + NovaSimulation.PF_NET_ID])
		assert_true(truth.has(nid), "decoded net_id maps back to a sim entity")
		var p := Vector3(snap[base + NovaSimulation.PF_POS_X],
			snap[base + NovaSimulation.PF_POS_Y],
			snap[base + NovaSimulation.PF_POS_Z])
		# Position rides the wire compressed (lossy); the reconstruction lands within a
		# codec quantization step of the authoritative value.
		assert_lt(p.distance_to(truth[nid]), 0.5,
			"decoded position round-trips within codec tolerance")
		matched += 1
	assert_eq(matched, 2, "every decoded entity matched a sim entity")
	sim.free()


func test_listen_server_spawns_and_replicates_local_player() -> void:
	# Phase 2 (the moving player, net-re §5.2b/§5.38): spawn_local_player makes the host's own
	# player an authoritative pool-0 entity that replicates through the wire to its own client
	# view like any other entity, and set_player_input feeds its player-body input. (Headless
	# has no anim clips, so the soldier holds — motion comes from clips, as in the original;
	# clip-driven forward motion is covered by the C++ player_spawn_test.)
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, 0, Vector3(0, 0, 0), Vector3.ZERO)  # a standing organic for company

	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	assert_true(sim.load_from_mission_data(md), "loaded with the net seam")
	assert_false(sim.has_local_player(), "no local player before spawn")

	var spawn_pos := Vector3(5, 0, 7)
	assert_true(sim.spawn_local_player(spawn_pos, 0.0, 1), "player spawned into pool 0")
	assert_true(sim.has_local_player(), "local player now present")
	assert_lt(sim.get_local_player_position().distance_to(spawn_pos), 0.01,
		"player at the requested spawn position (Godot->mission->Godot round-trip)")

	# Drive forward for several frames (exercises input -> pre-tick hook -> motor -> present).
	sim.set_player_input(true, false, false, false, false, false, false, false, 0.0, 0.0)
	for _i in range(8):
		sim.advance_frame()

	# The player replicates through the wire as one more present record, keyed by its net_id.
	var stride: int = sim.get_present_stride()
	var snap: PackedFloat32Array = sim.get_present_snapshot()
	var records: int = snap.size() / stride
	var found_player := false
	for rec in range(records):
		var base := rec * stride
		if int(snap[base + NovaSimulation.PF_NET_ID]) == 0xFFF0:
			found_player = true
			assert_eq(int(snap[base + NovaSimulation.PF_ALIVE]), 1, "player is alive")
			# The player has no BMS placement, so it carries no (kind,index) origin (PF_KIND 0)
			# — unlike placed mission nodes, its avatar is host-managed by net_id.
			assert_eq(int(snap[base + NovaSimulation.PF_KIND]), 0,
				"player carries no BMS (kind,index) origin")
	assert_true(found_player, "the local player replicated into the client-decoded present")
	sim.free()


func test_listen_server_off_uses_ai_pool_present() -> void:
	# Default (no listen server): get_present_snapshot() is the direct AI-pool path, one
	# record per sim entity — unchanged from before the net seam.
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	assert_false(sim.is_listen_server(), "listen server off by default")
	var snap: PackedFloat32Array = sim.get_present_snapshot()
	assert_eq(snap.size(), sim.get_entity_count() * sim.get_present_stride(),
		"AI-pool present: one record per sim entity")
	sim.free()
