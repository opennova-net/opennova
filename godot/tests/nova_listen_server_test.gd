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
	assert_true(sim.load_from_mission_data(md), "promoted as the npruntime in-process listen server")

	# The faithful §5.0 mode-3 bring-up auto-spawns the host's own player (ADR 0012), so the world is
	# the 2 organics + the host player. The AI-pool truth is readable through the scalar getters (they
	# read the sim directly); key it by WIRE HANDLE (pool<<12|slot — unique per entity) so we can match
	# the decoded records. [D-NET-112: a player carries NO SSN (net_id 0); it is identified by its handle
	# + ownerConnectionId(dcb), not a reserved net_id — net_id is no longer a unique key.]
	var truth := {}  # wire_handle -> { pos: Vector3 (Godot space), kind: int }
	var host_handle := -1
	for i in range(sim.get_entity_count()):
		var wh := sim.get_entity_wire_handle(i)
		truth[wh] = {
			"pos": sim.get_entity_position(i),
			"kind": sim.get_entity_kind(i),
		}
		if sim.get_entity_owner_connection_id(i) != 0:  # the host's own player carries the host dcb
			host_handle = wh
	assert_eq(truth.size(), 3, "two organics + the auto-spawned host player")
	assert_true(host_handle != -1,
		"the host player is identified by its ownerConnectionId (dcb), not an SSN (D-NET-112)")

	# One host frame: Server_TickUpdate fans the host loopback its whole-world S2C 0x0A; the host's
	# own ClientRuntime folds it into the ClientState the present pass now reads.
	sim.step()

	var stride: int = sim.get_present_stride()
	var snap: PackedFloat32Array = sim.get_present_snapshot()
	var records: int = snap.size() / stride
	assert_eq(records, 3, "both organics + the host player replicated through the wire")

	var matched := 0
	for rec in range(records):
		var base := rec * stride
		assert_eq(int(snap[base + NovaSimulation.PF_ALIVE]), 1, "decoded entity is alive")
		var wh := int(snap[base + NovaSimulation.PF_WIRE_HANDLE])
		assert_true(truth.has(wh), "decoded wire handle maps back to a sim entity")
		# Organics resolve KIND_ORGANIC (3) from the registry behind the decoded handle; the host
		# player has no BMS placement so it carries no (kind,index) origin (kind 0).
		assert_eq(int(snap[base + NovaSimulation.PF_KIND]), int(truth[wh]["kind"]),
			"kind matches the AI-pool truth for the entity behind the decoded handle")
		var p := Vector3(snap[base + NovaSimulation.PF_POS_X],
			snap[base + NovaSimulation.PF_POS_Y],
			snap[base + NovaSimulation.PF_POS_Z])
		# Position rides the wire compressed (lossy); the reconstruction lands within a
		# codec quantization step of the authoritative value.
		assert_lt(p.distance_to(truth[wh]["pos"]), 0.5,
			"decoded position round-trips within codec tolerance")
		matched += 1
	assert_eq(matched, 3, "every decoded entity matched a sim entity")
	sim.free()


func test_present_effect_lookup_matches_client_snapshot_and_reloads_cleanly() -> void:
	var first_mission := NovaMissionData.new()
	assert_eq(first_mission.create_default(), OK)
	first_mission.add_entity(3, 0, Vector3(12, 4, -3), Vector3.ZERO)
	# Use the second pool-0 organic so its wire handle is non-zero; handle zero is
	# the presentation sentinel even though slot zero is valid in the registry.
	first_mission.add_entity(3, 0, Vector3(18, 4, -3), Vector3.ZERO)

	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	assert_true(sim.load_from_mission_data(first_mission))
	sim.step()

	var stride := sim.get_present_stride()
	var snapshot: PackedFloat32Array = sim.get_present_snapshot()
	var row_base := -1
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + NovaSimulation.PF_KIND]) == 3 \
				and int(snapshot[base + NovaSimulation.PF_INDEX]) == 1:
			row_base = base
			break
	assert_gte(row_base, 0, "the placed organic reached the client view")
	var expected_position := Vector3(
			snapshot[row_base + NovaSimulation.PF_POS_X],
			snapshot[row_base + NovaSimulation.PF_POS_Y],
			snapshot[row_base + NovaSimulation.PF_POS_Z])
	var expected_rotation := Vector3(
			snapshot[row_base + NovaSimulation.PF_PITCH_DEG],
			snapshot[row_base + NovaSimulation.PF_YAW_DEG],
			snapshot[row_base + NovaSimulation.PF_ROLL_DEG])
	var wire_handle := int(snapshot[row_base + NovaSimulation.PF_WIRE_HANDLE])
	var bms_id := int(snapshot[row_base + NovaSimulation.PF_BMS_ID])
	var ssn := int(snapshot[row_base + NovaSimulation.PF_NET_ID])
	var lookups: Array = [
		sim.get_present_effect_state_for_wire_handle(wire_handle),
		sim.get_present_effect_state_for_origin(3, 1),
	]
	assert_gt(wire_handle, 0, "the fixture carries a usable wire identity")
	if bms_id > 0:
		lookups.append(sim.get_present_effect_state_for_bms_id(bms_id))
	if ssn > 0:
		lookups.append(sim.get_present_effect_state_for_ssn(ssn))
	for state_v in lookups:
		var state: PackedVector3Array = state_v
		assert_eq(state.size(), NovaSimulation.EFFECT_STATE_COUNT)
		assert_true(state[NovaSimulation.EFFECT_STATE_POSITION].is_equal_approx(
				expected_position), "compact lookup keeps the decoded wire position")
		assert_true(state[NovaSimulation.EFFECT_STATE_ROTATION_DEG].is_equal_approx(
				expected_rotation), "compact lookup keeps the present-pass yaw conversion")
	assert_eq(sim.get_present_effect_state_for_wire_handle(-1).size(), 0,
		"invalid wire identity stays absent")
	assert_eq(sim.get_present_effect_state_for_bms_id(0).size(), 0,
		"zero BMS identity stays absent")
	assert_eq(sim.get_present_effect_state_for_ssn(0).size(), 0,
		"zero SSN identity stays absent")
	assert_eq(sim.get_present_effect_state_for_origin(-1, 0).size(), 0,
		"invalid origin identity stays absent")

	# A replacement world restarts both generation counters at the same values.
	# The cache must still belong to the new presentation epoch.
	var replacement := NovaMissionData.new()
	assert_eq(replacement.create_default(), OK)
	replacement.add_entity(3, 0, Vector3(96, 4, -3), Vector3.ZERO)
	assert_true(sim.load_from_mission_data(replacement))
	sim.step()
	var replacement_state: PackedVector3Array = \
			sim.get_present_effect_state_for_origin(3, 0)
	assert_eq(replacement_state.size(), NovaSimulation.EFFECT_STATE_COUNT)
	assert_gt(replacement_state[NovaSimulation.EFFECT_STATE_POSITION].distance_to(
			expected_position), 40.0,
		"world replacement invalidates an equal-tick pose cache")
	sim.free()


func test_listen_server_auto_spawns_and_replicates_local_player() -> void:
	# Phase 2 (the moving player, net-re §5.2b/§5.38) + the faithful §5.0 mode-3 bring-up: the host's
	# own player is created as part of session bring-up (ADR 0012) — an authoritative pool-0 entity
	# that replicates through the wire to its own client view like any other entity. set_player_input
	# feeds its player-body input. (Headless has no anim clips, so the soldier holds — motion comes
	# from clips, as in the original; clip-driven forward motion is covered by the C++ player_spawn_test.)
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, 0, Vector3(0, 0, 0), Vector3.ZERO)  # a standing organic for company

	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	assert_true(sim.load_from_mission_data(md), "loaded as the npruntime listen server")
	# Faithful §5.0: the host's own player auto-spawns at bring-up (no explicit spawn call needed).
	assert_true(sim.has_local_player(), "the host's own player auto-spawned at load")

	# Drive forward for several frames (exercises input -> pre-tick hook -> motor -> 0x0A -> present).
	sim.set_player_input(true, false, false, false, false, false, false)
	for _i in range(8):
		sim.step()

	# The player replicates through the wire as one present record, keyed by its WIRE HANDLE.
	# [D-NET-112: the player carries no SSN (net_id 0); it is identified by its handle, not 0xFFF0.]
	var player_handle := sim.get_local_player_wire_handle()
	var stride: int = sim.get_present_stride()
	var snap: PackedFloat32Array = sim.get_present_snapshot()
	var records: int = snap.size() / stride
	var found_player := false
	for rec in range(records):
		var base := rec * stride
		if int(snap[base + NovaSimulation.PF_WIRE_HANDLE]) == player_handle:
			found_player = true
			assert_eq(int(snap[base + NovaSimulation.PF_ALIVE]), 1, "player is alive")
			# The player has no BMS placement, so it carries no (kind,index) origin (PF_KIND 0)
			# — unlike placed mission nodes, its avatar is host-managed by handle + ownerConnectionId.
			assert_eq(int(snap[base + NovaSimulation.PF_KIND]), 0,
				"player carries no BMS (kind,index) origin")
	assert_true(found_player, "the auto-spawned local player replicated into the client-decoded present")
	sim.free()


# (P7: test_listen_server_off_uses_ai_pool_present deleted — the no-net AI-pool present is retired;
#  the present is always the listen-server ClientState now.)
