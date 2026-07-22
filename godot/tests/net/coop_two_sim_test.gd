extends GutTest

# Co-op LAN bidirectional bring-up (D.2) at the NovaSimulation layer: a HOST listen server
# (enable_host_listen) and a JOINER (enable_join) run in the same headless process, each on a
# real loopback UDP socket (NovaUdpPump), and free-run their own step() — no manual
# byte carry. This exercises the full witnessed JOIN end to end: the joiner handshakes
# (ClientHello/Auth), drives the spawn-gate burst, name-matches the host's S2C 0x0C organic
# spawn to learn its wire handle H, spawns its local player L, and uplinks C2S 0x0C; the host
# admits it and SNAPs it from the uplink. (The handshake bytes are unit-tested in libs —
# tests/novaworld/joiner_session_test; this is the Godot-layer glue + the two-handle present.)
#
# Asserts the DATA layer (the present snapshots both sides decode), not rendered nodes —
# headless has no assets, so wire_present_pass builds nothing. The §5.38b two-handle (L vs H)
# reconciliation shows up as: the host's present carries the joiner (SSN 0xFFEF), and the
# joiner's wire present carries the host player (type 0x14B9) while self-filtering its own echo H.


# A tiny world covering every pool the host streams: two AI organics (pool 0, type 0x0816),
# one static building (pool 2, type 0x0123) and one marker (pool 3, type 0x1773). The AI carry a
# RESOLVABLE non-zero type id so they survive the present's `type_id != 0` filter — with the old
# item_id 0 they were invisible by construction, which is why the joiner only ever saw the host
# player. The witnessed initial-state burst streams EVERY entity pool to the joiner at world-load
# [orig: Server_SendInitialGameStateToPlayer @0x51bba0]: pool-2 statics (0x10), pool-1 (0x0D, omitted
# by our host — D-NET-97), pool-0 dynamics (0x0C), pool-3 markers (0x20). Only client-local BMS map
# geometry (rebuilt from the 0x0B header) is not re-streamed.
const AI_TYPE := 0x0816       # AI infantry
const BUILDING_TYPE := 0x0123 # a static structure
const MARKER_TYPE := 0x1773   # a start marker


func _anim_root() -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/anim")), OK)
	return root


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
	# Captured retail Co-op g_GameType: bit 0x20000 makes every phase-3
	# 0x0A carry a 16-byte objective block before the local-health tail.
	host.configure_host_session({"gametype": 0x30020})
	assert_true(host.enable_host_listen(0), "host bound an OS-assigned UDP port")
	assert_true(host.load_from_mission_data(_two_organics()), "host promoted with the net seam")
	# A co-op host is playable — it spawns its own pool-0 player (0x14B9), which the joiner must
	# see over the wire. Without it the only player entity would be the joiner's own echo.
	assert_true(host.spawn_local_player(Vector3(5, 0, 5), 0.0, 1), "host spawned its own player")
	var host_anim_root := _anim_root()
	assert_gt(host.set_infantry_anim_map(host_anim_root, "soldier.adm"), 0)
	var host_item_db := NovaItemDatabase.new()
	assert_eq(host_item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	host.resolve_infantry_adm_ids(host_anim_root, host_item_db)
	var host_port: int = host.get_host_listen_port()
	assert_gt(host_port, 0, "host got a real bound port")

	var joiner := NovaSimulation.new()
	assert_true(joiner.enable_join("127.0.0.1", host_port, "JoinerOne"), "joiner dialed the host")
	assert_true(joiner.is_joiner(), "joiner flag set before load")
	assert_eq(joiner.get_joiner_phase(), 0, "joiner phase Idle before the first frame")
	assert_true(joiner.load_from_mission_data(_two_organics()), "joiner promoted as a client")
	var joiner_anim_root := _anim_root()
	assert_gt(joiner.set_infantry_anim_map(joiner_anim_root, "soldier.adm"), 0)
	var joiner_item_db := NovaItemDatabase.new()
	assert_eq(joiner_item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	joiner.resolve_infantry_adm_ids(joiner_anim_root, joiner_item_db)

	var before_peers: int = host.get_host_peer_count()

	# Free-run both sims (real loopback UDP) until the joiner name-matches into the match.
	var reached := false
	for _i in range(800):
		host.step()
		joiner.step()
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
	joiner.set_player_input(true, false, false, false, false, false, false)
	for _i in range(30):
		host.step()
		joiner.step()
		OS.delay_msec(2)

	var stride: int = host.get_present_stride()

	# HOST sees the JOINER: its client-decoded present carries the admitted joiner. [D-NET-112] Both
	# players carry net_id 0 (no SSN); a player is identified by its WIRE HANDLE, so the joiner is the
	# player row (PF_KIND 0, no BMS origin) whose handle is NOT the host's own player handle.
	var host_own: int = host.get_local_player_wire_handle()
	var hsnap: PackedFloat32Array = host.get_present_snapshot()
	var host_sees_joiner := false
	for rec in range(hsnap.size() / stride):
		var base := rec * stride
		if int(hsnap[base + NovaSimulation.PF_KIND]) == 0 \
				and int(hsnap[base + NovaSimulation.PF_WIRE_HANDLE]) != host_own:
			host_sees_joiner = true
	assert_true(host_sees_joiner, "host's present includes the admitted joiner (a player row that isn't the host's own)")
	var host_remote_adm := ""
	var host_remote_index := -1
	for ai_index in range(host.get_entity_count()):
		var card: Dictionary = host.get_entity_debug(ai_index)
		if int(card.get("item_id", 0)) == 0x14B9 \
				and int(card.get("wire_handle", 0)) != host_own:
			host_remote_adm = String(card.get("adm_name", ""))
			host_remote_index = ai_index
			break
	assert_eq(host_remote_adm, "US01.adm",
			"the real host admission path binds the remote player's body ADM")

	# JOINER sees the HOST: its wire-decoded present carries a player row (type 0x14B9) that is
	# NOT its own echo, and its own wire echo (handle H) is self-filtered out.
	var jsnap: PackedFloat32Array = joiner.get_present_snapshot()
	var joiner_sees_host := false
	var saw_self_echo := false
	var host_player_anim_state := -1
	var host_player_anim_phase := -2
	for rec in range(jsnap.size() / stride):
		var base := rec * stride
		var tid := int(jsnap[base + NovaSimulation.PF_TYPE_ID])
		var whandle := int(jsnap[base + NovaSimulation.PF_WIRE_HANDLE])
		if whandle == h:
			saw_self_echo = true
		if tid == 0x14B9 and whandle != h and whandle != 0:
			joiner_sees_host = true
			host_player_anim_state = int(
					jsnap[base + NovaSimulation.PF_ANIM_STATE])
			host_player_anim_phase = int(
					jsnap[base + NovaSimulation.PF_ANIM_PHASE_TICKS])
	assert_true(joiner_sees_host, "joiner's wire present includes the host player (0x14B9), not itself")
	assert_false(saw_self_echo, "the joiner's own wire echo (handle H) is self-filtered from its present")
	assert_gte(host_player_anim_state, 0,
			"the host player's authoritative body state reaches presentation")
	assert_gte(host_player_anim_phase, 0,
			"the player compact's channel-phase byte reaches presentation")
	var joiner_local_adm := ""
	for ai_index in range(joiner.get_entity_count()):
		var card: Dictionary = joiner.get_entity_debug(ai_index)
		# Joiner get_local_player_wire_handle() deliberately returns host identity H,
		# not local simulation handle L. Its local World contains only L as a player.
		if int(card.get("item_id", 0)) == 0x14B9:
			joiner_local_adm = String(card.get("adm_name", ""))
			break
	assert_eq(joiner_local_adm, "US01.adm",
			"the real joiner name-match path binds local L's body ADM")

	# JOINER sees the host's full ENTITY world: the witnessed initial-state burst streams EVERY entity
	# pool to a joining player [orig: Server_SendInitialGameStateToPlayer @0x51bba0, sync-state 4] —
	# phase 1 pool-2 statics under S2C 0x10, phase 2 pool-1 under 0x0D, phase 3 pool-0 dynamics (AI +
	# players) under 0x0C, phase 4 pool-3 markers under 0x20 (each bounded by Pool_GetUsedCount(idx)).
	# So the joiner's present carries the host AI organics AND the pool-2 building. The thing that is NOT
	# re-streamed is client-local BMS MAP GEOMETRY (terrain/props the joiner rebuilds from the 0x0B
	# mission header) — distinct from pool-2 entity instances, which DO stream. (D-NET-98 corrected;
	# net-re §5.38c.)
	var ai_seen := 0
	var ai_with_anim_state := 0
	var ai_without_wire_phase := 0
	var building_seen := false
	for rec in range(jsnap.size() / stride):
		var base := rec * stride
		var tid := int(jsnap[base + NovaSimulation.PF_TYPE_ID])
		if tid == AI_TYPE:
			ai_seen += 1
			if int(jsnap[base + NovaSimulation.PF_ANIM_STATE]) >= 0:
				ai_with_anim_state += 1
			if int(jsnap[base + NovaSimulation.PF_ANIM_PHASE_TICKS]) == -1:
				ai_without_wire_phase += 1
		elif tid == BUILDING_TYPE:
			building_seen = true
	assert_eq(ai_seen, 2, "joiner's present carries both host AI organics (type 0x0816) from the 0x0C stream")
	assert_eq(ai_with_anim_state, 2,
			"both compact infantry records carry their body state")
	assert_eq(ai_without_wire_phase, 2,
			"infantry compacts expose the absent phase for local free-running playback")
	assert_true(building_seen, "pool-2 static building IS wire-streamed to the joiner under S2C 0x10 (the join burst's phase 1) [orig: @0x51bba0]")

	# The recipient-specific 0x0A tail, not the lossy self-echo H, owns the
	# joiner's health. Kill H on the authority and verify the same received
	# frame is applied to local motor entity L for HUD and death state.
	assert_gte(host_remote_index, 0, "host resolves the joiner's authoritative player H")
	assert_gt(joiner.get_local_player_health(), 0, "joiner starts alive before the authority kill")
	host.debug_set_entity_health(host_remote_index, 0)
	var death_arrived := false
	for _i in range(120):
		host.step()
		joiner.step()
		if joiner.get_local_player_health() == 0:
			death_arrived = true
			break
		OS.delay_msec(2)
	assert_true(death_arrived,
			"the authoritative 0x0A tail health reaches the joiner's local player L")
	var local_death_card: Dictionary = {}
	for ai_index in range(joiner.get_entity_count()):
		var card: Dictionary = joiner.get_entity_debug(ai_index)
		if int(card.get("item_id", 0)) == 0x14B9:
			local_death_card = card
			break
	assert_false(local_death_card.is_empty(), "joiner retains its local player L after death")
	assert_eq(int(local_death_card.get("health", -1)), 0)
	assert_eq(int(local_death_card.get("ai_health", -1)), 0)
	assert_false(bool(local_death_card.get("alive", true)))

	# A positive tail without a new deploy edge is not a respawn. This also
	# protects the next infantry tick from hydrating its motor copy back to life.
	host.debug_set_entity_health(host_remote_index, 100)
	for _i in range(8):
		host.step()
		joiner.step()
		OS.delay_msec(2)
	var after_stale_positive: Dictionary = {}
	for ai_index in range(joiner.get_entity_count()):
		var card: Dictionary = joiner.get_entity_debug(ai_index)
		if int(card.get("item_id", 0)) == 0x14B9:
			after_stale_positive = card
			break
	assert_eq(int(after_stale_positive.get("health", -1)), 0)
	assert_eq(int(after_stale_positive.get("ai_health", -1)), 0)
	assert_false(bool(after_stale_positive.get("alive", true)),
			"positive health without a deploy edge cannot partially revive local L")

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
