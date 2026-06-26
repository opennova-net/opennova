extends GutTest

# Co-op LAN bidirectional bring-up (D.2) at the NovaSimulation layer: a HOST listen server
# (enable_host_listen) and a JOINER (enable_join) run in the same headless process, each on a
# real loopback UDP socket (NovaUdpPump), and free-run their own advance_frame() — no manual
# byte carry. This exercises the full witnessed JOIN end to end: the joiner handshakes
# (ClientHello/Auth), drives the spawn-gate burst, name-matches the host's S2C 0x0C organic
# spawn to learn its wire handle H, spawns its local player L, and uplinks C2S 0x0C; the host
# admits it and SNAPs it from the uplink. (The handshake bytes are unit-tested in libs —
# tests/novaworld/joiner_session_test; this is the Godot-layer glue + the two-handle present.)
#
# Asserts the DATA layer (the present snapshots both sides decode), not rendered nodes —
# headless has no assets, so wire_present_pass builds nothing. The §5.38b two-handle (L vs H)
# reconciliation shows up as: the host's present carries the joiner (SSN 0xFFF1), and the
# joiner's wire present carries the host player (type 0x14B9) while self-filtering its own echo H.


# A tiny world covering every pool the host streams: two AI organics (pool 0, type 0x0816),
# one static building (pool 2, type 0x0123) and one marker (pool 3, type 0x1773). The AI carry a
# RESOLVABLE non-zero type id so they survive the present's `type_id != 0` filter — with the old
# item_id 0 they were invisible by construction, which is why the joiner only ever saw the host
# player. The host streams only the dynamic set at the joiner's world-load (pool-0 organics via
# S2C 0x0C, empty 0x10, spawn-marker 0x20); pool-1 vehicles + pool-2 buildings are client-local.
const AI_TYPE := 0x0816       # AI infantry
const BUILDING_TYPE := 0x0123 # a static structure
const MARKER_TYPE := 0x1773   # a start marker

func _two_organics() -> NovaMissionData:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, AI_TYPE, Vector3(0, 0, 0), Vector3.ZERO)    # KIND_ORGANIC (pool 0)
	md.add_entity(3, AI_TYPE, Vector3(10, 0, 0), Vector3.ZERO)
	md.add_entity(2, BUILDING_TYPE, Vector3(20, 0, 0), Vector3.ZERO) # KIND_BUILDING (pool 2)
	md.add_entity(0, MARKER_TYPE, Vector3(30, 0, 0), Vector3.ZERO)   # KIND_MARKER (pool 3)
	return md


func test_joiner_handshakes_and_sees_host_bidirectional() -> void:
	var host := NovaSimulation.new()
	assert_true(host.enable_host_listen(0), "host bound an OS-assigned UDP port")
	assert_true(host.load_from_mission_data(_two_organics()), "host promoted with the net seam")
	# A co-op host is playable — it spawns its own pool-0 player (0x14B9), which the joiner must
	# see over the wire. Without it the only player entity would be the joiner's own echo.
	assert_true(host.spawn_local_player(Vector3(5, 0, 5), 0.0, 1), "host spawned its own player")
	var host_port: int = host.get_host_listen_port()
	assert_gt(host_port, 0, "host got a real bound port")

	var joiner := NovaSimulation.new()
	assert_true(joiner.enable_join("127.0.0.1", host_port, "JoinerOne"), "joiner dialed the host")
	assert_true(joiner.is_joiner(), "joiner flag set before load")
	assert_eq(joiner.get_joiner_phase(), 0, "joiner phase Idle before the first frame")
	assert_true(joiner.load_from_mission_data(_two_organics()), "joiner promoted as a client")

	var before_peers: int = host.get_host_peer_count()

	# Free-run both sims (real loopback UDP) until the joiner name-matches into the match.
	var reached := false
	for _i in range(800):
		host.advance_frame()
		joiner.advance_frame()
		if joiner.is_joined_in_match():
			reached = true
			break
		OS.delay_msec(2)
	assert_true(reached, "joiner reached in-match (handshake -> spawn gate -> 0x0C name-match)")
	assert_eq(host.get_host_peer_count(), before_peers + 1, "host registered exactly one joiner peer")
	assert_true(joiner.has_local_player(), "joiner spawned its local player L at the H-learned pose")
	var h: int = joiner.get_joiner_self_handle()
	assert_gt(h, 0, "joiner learned its wire handle H")

	# Drive the joiner forward + settle: its C2S 0x0C uplink reaches the host (SNAP), and the
	# host's S2C 0x0A carries everyone (incl. the host player + the joiner) back to the joiner.
	joiner.set_player_input(true, false, false, false, false, false, false, false, 0.0, 0.0)
	for _i in range(30):
		host.advance_frame()
		joiner.advance_frame()
		OS.delay_msec(2)

	var stride: int = host.get_present_stride()

	# HOST sees the JOINER: its client-decoded present carries the admitted joiner (SSN 0xFFF1).
	var hsnap: PackedFloat32Array = host.get_present_snapshot()
	var host_sees_joiner := false
	for rec in range(hsnap.size() / stride):
		if int(hsnap[rec * stride + NovaSimulation.PF_NET_ID]) == 0xFFF1:
			host_sees_joiner = true
	assert_true(host_sees_joiner, "host's present includes the admitted joiner (SSN 0xFFF1)")

	# JOINER sees the HOST: its wire-decoded present carries a player row (type 0x14B9) that is
	# NOT its own echo, and its own wire echo (handle H) is self-filtered out.
	var jsnap: PackedFloat32Array = joiner.get_present_snapshot()
	var joiner_sees_host := false
	var saw_self_echo := false
	for rec in range(jsnap.size() / stride):
		var base := rec * stride
		var tid := int(jsnap[base + NovaSimulation.PF_TYPE_ID])
		var whandle := int(jsnap[base + NovaSimulation.PF_WIRE_HANDLE])
		if whandle == h:
			saw_self_echo = true
		if tid == 0x14B9 and whandle != h and whandle != 0:
			joiner_sees_host = true
	assert_true(joiner_sees_host, "joiner's wire present includes the host player (0x14B9), not itself")
	assert_false(saw_self_echo, "the joiner's own wire echo (handle H) is self-filtered from its present")

	# JOINER sees the DYNAMIC set: the host streams the networked entities (pool-0 organics via
	# S2C 0x0C) during load, so the joiner's wire present now carries the host's AI organics, not
	# just the host player. Statics (pool-2 building) and markers (pool-3) are deliberately NOT
	# wire-streamed — a real host streams only the small dynamic set (golden host_and_join_lan.pcapng:
	# empty 0x10, ~5 organics in 0x0C, empty 0x20); static geometry is client-local mission data.
	# Streaming the full static mission floods + crashes a stock client (D-NET-98). (net-re §5.38c)
	var ai_seen := 0
	var building_seen := false
	for rec in range(jsnap.size() / stride):
		var tid := int(jsnap[rec * stride + NovaSimulation.PF_TYPE_ID])
		if tid == AI_TYPE:
			ai_seen += 1
		elif tid == BUILDING_TYPE:
			building_seen = true
	assert_eq(ai_seen, 2, "joiner's present carries both host AI organics (type 0x0816) from the 0x0C stream")
	assert_false(building_seen, "static building is NOT wire-streamed (client-local geometry, not a host broadcast)")

	host.free()
	joiner.free()


func test_joiner_off_by_default() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	assert_false(sim.is_joiner(), "joiner off by default")
	assert_eq(sim.get_joiner_phase(), -1, "no joiner phase when not joining")
	assert_false(sim.is_joined_in_match(), "not in a match")
	assert_eq(sim.get_joiner_self_handle(), 0, "no wire handle when not joining")
	sim.free()
