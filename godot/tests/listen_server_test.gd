extends GutTest

# The USE scan admits a seat only inside the player's view cone (just under
# 90 deg standing, 5 deg seated); a test that presses USE looks at the seat first.
const MountLook := preload("res://tests/support/mount_look.gd")

# The SP-as-listen-server present path (ADR 0009/0011). With the listen server on,
# Simulation.get_present_snapshot() returns the state the LOCAL CLIENT decoded off
# the in-process loopback — real entity state serialized through the wire codec
# (engine/runtime/replication connection fan) and decoded back (engine/net/novaworld ingame_decode) — instead of
# reading the authoritative AI pool directly. This is the in-Godot end of the Phase 1
# loopback identity guard (tests/netsim/loopback_identity_test).

# Native seat/mount fixtures (S16): the Dictionary seat seam is gone — the sim
# extracts seats/attachments from items.def rows + model userpoints through its
# own asset root. ResourceRoot rejects user:// paths, so the composed loose
# roots live under OS.get_cache_dir().

var _native_fixture_dirs: Array[String] = []


func after_each() -> void:
	for dir in _native_fixture_dirs:
		TestFs.remove_dir_recursive(dir)
	_native_fixture_dirs.clear()


func _native_fixture_dir() -> String:
	var dir := OS.get_cache_dir().path_join("listen_native_%d_%d" % [
			Time.get_ticks_usec(), _native_fixture_dirs.size()])
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	_native_fixture_dirs.append(dir)
	return dir


func _fixture_items_text() -> String:
	return FileAccess.get_file_as_bytes(
			"res://../fixtures/def/items.def").get_string_from_ascii()


# Compose <dir>/items.def from the fixture superset text, load it, wire the dir
# as the sim's asset root, and run the native seat-spec install for type_ids.
func _install_native_seats(sim: Simulation, dir: String, items_text: String,
		type_ids: PackedInt32Array) -> ItemDatabase:
	TestFs.write_text(self, dir.path_join("items.def"), items_text)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(dir.path_join("items.def")), OK)
	var seat_root := ResourceRoot.new()
	assert_eq(seat_root.set_root_dir(dir), OK)
	sim.set_asset_root(seat_root)
	assert_true(sim.install_seat_specs_for_type_ids(item_db, type_ids),
			"native seat-spec install over the composed fixture root")
	return item_db


func test_listen_server_present_reads_client_decoded_state() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	# Two standing organics at known, close positions (KIND_ORGANIC = 3). No route + no
	# anim clips -> they hold, so the decoded positions are their spawn positions.
	md.add_entity(3, 0, Vector3(0, 0, 0), Vector3.ZERO)
	md.add_entity(3, 0, Vector3(10, 0, 0), Vector3.ZERO)

	var sim := Simulation.new()
	sim.enable_listen_server(true)
	assert_true(sim.is_listen_server(), "listen server enabled before load")
	assert_true(sim.load_from_mission_data(md), "promoted as the inmatch in-process listen server")

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

	# One host frame: Server_TickUpdate fans the host loopback retail's header-only S2C 0x0A
	# (D-NET-140 closed); the present pass reads the host's own pools.
	sim.step()

	var stride: int = sim.get_present_stride()
	var snap: PackedFloat32Array = sim.get_present_snapshot()
	var records: int = snap.size() / stride
	assert_eq(records, 3, "both organics + the host player presented from the pools")

	var matched := 0
	for rec in range(records):
		var base := rec * stride
		assert_eq(int(snap[base + Simulation.PF_ALIVE]), 1, "decoded entity is alive")
		var wh := int(snap[base + Simulation.PF_WIRE_HANDLE])
		assert_true(truth.has(wh), "decoded wire handle maps back to a sim entity")
		# Organics resolve KIND_ORGANIC (3) from the registry behind the decoded handle; the host
		# player has no BMS placement so it carries no (kind,index) origin (kind 0).
		assert_eq(int(snap[base + Simulation.PF_KIND]), int(truth[wh]["kind"]),
			"kind matches the AI-pool truth for the entity behind the decoded handle")
		var p := Vector3(snap[base + Simulation.PF_POS_X],
			snap[base + Simulation.PF_POS_Y],
			snap[base + Simulation.PF_POS_Z])
		# The host presents its own registry: the pose is the authoritative value.
		assert_lt(p.distance_to(truth[wh]["pos"]), 0.5,
			"presented position is the authoritative pool position")
		matched += 1
	assert_eq(matched, 3, "every decoded entity matched a sim entity")


func test_items_attachment_follows_through_listen_client() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(
			MissionData.KIND_ITEM, 101291,
			Vector3(10, 0, 0), Vector3.ZERO)
	assert_not_null(vehicle)
	var sim := Simulation.new()
	sim.enable_listen_server(true)
	# The authored addeweap row names a userpoint its (unresolved Dbuggy1)
	# graphic cannot anchor: the child keeps the parent-root fallback frame and
	# the first authored row's stored slot — the old missing-anchor spec.
	var dir := _native_fixture_dir()
	var items := _install_native_seats(sim, dir, _fixture_items_text().replace(
			"id 101291", "id 101291\n  addeweap gunanchor 101419"),
			PackedInt32Array([1291]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(items)

	# Fold the initial 0x0D pool stream and capture the child's spawn pose.
	sim.step()
	var stride := sim.get_present_stride()
	var snapshot := sim.get_present_snapshot()
	var child_base := -1
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_TYPE_ID]) == 1419:
			child_base = base
			break
	assert_gte(child_base, 0, "the ewep child reached the listen client")
	var spawn_x := snapshot[child_base + Simulation.PF_POS_X]

	# The child has no 0x0A callback of its own. Its presented motion comes from
	# the stock 0x0D parent relation recomposed against the decoded vehicle row.
	sim.debug_set_world_entity_position(
			vehicle.bms_id, Vector3(30, 0, 0))
	sim.step()
	snapshot = sim.get_present_snapshot()
	child_base = -1
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_TYPE_ID]) == 1419:
			child_base = base
			break
	assert_gte(child_base, 0, "the moving child remains presented")
	if child_base >= 0:
		assert_gt(absf(snapshot[child_base + Simulation.PF_POS_X] - spawn_x), 15.0,
				"the child follows the decoded carrier instead of freezing at spawn")


func test_present_effect_lookup_matches_client_snapshot_and_reloads_cleanly() -> void:
	var first_mission := MissionData.new()
	assert_eq(first_mission.create_default(), OK)
	first_mission.add_entity(3, 5311, Vector3(12, 4, -3), Vector3.ZERO)
	# Use the second pool-0 organic so its wire handle is non-zero; handle zero is
	# the presentation sentinel even though slot zero is valid in the registry.
	first_mission.add_entity(3, 5311, Vector3(18, 4, -3), Vector3(10, 20, 30))

	var sim := Simulation.new()
	sim.enable_listen_server(true)
	assert_true(sim.load_from_mission_data(first_mission))
	var items := ItemDatabase.new()
	assert_eq(items.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	sim.resolve_item_traits(items)
	sim.step()

	var stride := sim.get_present_stride()
	var snapshot: PackedFloat32Array = sim.get_present_snapshot()
	var layout_revision := sim.get_present_layout_revision()
	sim.get_present_snapshot()
	assert_eq(sim.get_present_layout_revision(), layout_revision,
			"re-reading an unchanged ordered identity layout keeps its revision")
	var row_base := -1
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_KIND]) == 3 \
				and int(snapshot[base + Simulation.PF_INDEX]) == 1:
			row_base = base
			break
	assert_gte(row_base, 0, "the placed organic reached the client view")
	var expected_position := Vector3(
			snapshot[row_base + Simulation.PF_POS_X],
			snapshot[row_base + Simulation.PF_POS_Y],
			snapshot[row_base + Simulation.PF_POS_Z])
	var expected_rotation := Vector3(
			snapshot[row_base + Simulation.PF_PITCH_DEG],
			snapshot[row_base + Simulation.PF_YAW_DEG],
			snapshot[row_base + Simulation.PF_ROLL_DEG])
	assert_almost_eq(expected_rotation.x, 0.0, 0.001,
			"the host snapshot carries the organic initializer's level pitch")
	assert_almost_eq(expected_rotation.y, 20.0, 1.5,
			"yaw remains the retail compact-byte view")
	assert_almost_eq(expected_rotation.z, 0.0, 0.001,
			"the host snapshot carries the organic initializer's level roll")
	var wire_handle := int(snapshot[row_base + Simulation.PF_WIRE_HANDLE])
	var bms_id := int(snapshot[row_base + Simulation.PF_BMS_ID])
	var ssn := int(snapshot[row_base + Simulation.PF_NET_ID])
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
		assert_eq(state.size(), Simulation.EFFECT_STATE_COUNT)
		assert_true(state[Simulation.EFFECT_STATE_POSITION].is_equal_approx(
				expected_position), "compact lookup keeps the decoded wire position")
		assert_true(state[Simulation.EFFECT_STATE_ROTATION_DEG].is_equal_approx(
				expected_rotation), "compact lookup keeps the present-pass yaw conversion")
	# A second identity in the same epoch resolves independently of the first
	# lazily materialized owner.
	var other_state: PackedVector3Array = \
			sim.get_present_effect_state_for_origin(3, 0)
	assert_eq(other_state.size(), Simulation.EFFECT_STATE_COUNT)
	assert_eq(sim.get_present_effect_state_for_wire_handle(-1).size(), 0,
		"invalid wire identity stays absent")
	assert_eq(sim.get_present_effect_state_for_bms_id(0).size(), 0,
		"zero BMS identity stays absent")
	assert_eq(sim.get_present_effect_state_for_ssn(0).size(), 0,
		"zero SSN identity stays absent")
	assert_eq(sim.get_present_effect_state_for_origin(-1, 0).size(), 0,
		"invalid origin identity stays absent")

	# Every catch-up tick is a new decoded-client epoch. Re-resolve the moving
	# owner from that tick's row.
	sim.debug_set_entity_position(1, Vector3(70, 4, -3))
	sim.step()
	sim.get_present_snapshot()
	assert_eq(sim.get_present_layout_revision(), layout_revision,
			"pose-only movement does not invalidate presentation row routing")
	var moved_state: PackedVector3Array = \
			sim.get_present_effect_state_for_origin(3, 1)
	assert_eq(moved_state.size(), Simulation.EFFECT_STATE_COUNT)
	assert_gt(moved_state[Simulation.EFFECT_STATE_POSITION].distance_to(
			expected_position), 30.0, "the next epoch resolves the moved client pose")

	# A replacement world restarts both generation counters at the same values.
	# The cache must still belong to the new presentation epoch.
	var replacement := MissionData.new()
	assert_eq(replacement.create_default(), OK)
	replacement.add_entity(3, 5311, Vector3(96, 4, -3), Vector3.ZERO)
	assert_true(sim.load_from_mission_data(replacement))
	sim.resolve_item_traits(items)
	sim.step()
	sim.get_present_snapshot()
	assert_ne(sim.get_present_layout_revision(), layout_revision,
			"world replacement invalidates the exact ordered row topology")
	var replacement_state: PackedVector3Array = \
			sim.get_present_effect_state_for_origin(3, 0)
	assert_eq(replacement_state.size(), Simulation.EFFECT_STATE_COUNT)
	assert_gt(replacement_state[Simulation.EFFECT_STATE_POSITION].distance_to(
			expected_position), 40.0,
			"world replacement invalidates an equal-tick pose cache")


func test_present_effect_lookup_accepts_zero_wire_handle() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_not_null(mission.add_entity(
			MissionData.KIND_ORGANIC, 5311,
			Vector3(12, 4, -3), Vector3.ZERO))

	var sim := Simulation.new()
	sim.enable_listen_server(true)
	assert_true(sim.load_from_mission_data(mission))
	sim.step()

	var stride := sim.get_present_stride()
	var snapshot: PackedFloat32Array = sim.get_present_snapshot()
	var row_base := -1
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_TYPE_ID]) != 0 \
				and int(snapshot[base + Simulation.PF_WIRE_HANDLE]) == 0:
			row_base = base
			break
	assert_gte(row_base, 0, "the first organic keeps its valid packed handle zero")
	var state := sim.get_present_effect_state_for_wire_handle(0)
	assert_eq(state.size(), Simulation.EFFECT_STATE_COUNT,
			"the compact effect lookup accepts packed handle zero")
	if row_base >= 0 and state.size() == Simulation.EFFECT_STATE_COUNT:
		var expected := Vector3(
				snapshot[row_base + Simulation.PF_POS_X],
				snapshot[row_base + Simulation.PF_POS_Y],
				snapshot[row_base + Simulation.PF_POS_Z])
		assert_true(state[Simulation.EFFECT_STATE_POSITION].is_equal_approx(expected))


func test_present_effect_missing_handle_retries_on_the_next_client_epoch() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	mission.add_entity(3, 0, Vector3(8, 0, 0), Vector3.ZERO)

	var sim := Simulation.new()
	assert_true(sim.enable_host_listen(0))
	assert_true(sim.load_from_mission_data(mission))
	# Establish the host client's initial decoded epoch before admitting a peer.
	sim.step()

	var admitted_pos := Vector3(40, 0, 24)
	var before := sim.get_entity_count()
	assert_true(sim.admit_test_remote_peer(admitted_pos, 0.0, 2))
	assert_eq(sim.get_entity_count(), before + 1)
	var admitted_handle := 0
	for entity_index in range(sim.get_entity_count()):
		var entity_pos: Vector3 = sim.get_entity_position(entity_index)
		if entity_pos.distance_to(admitted_pos) < 0.5 \
				and sim.get_entity_owner_connection_id(entity_index) != 0:
			admitted_handle = sim.get_entity_wire_handle(entity_index)
			break
	assert_gt(admitted_handle, 0, "the new authoritative peer has a wire identity")

	# A handle nothing owns misses, and the repeat is served by the native
	# negative cache for the current epoch rather than a registry walk.
	var absent_handle := 0x0FFF
	assert_true(sim.get_present_effect_state_for_wire_handle(
			absent_handle).is_empty())
	assert_true(sim.get_present_effect_state_for_wire_handle(
			absent_handle).is_empty(),
			"a repeated missing owner stays absent for the current epoch")

	# The listen host presents its own pools (D-NET-140 closed): admission
	# mutates the authoritative World and the effect lookup resolves the new
	# row at once, before any loopback pump — retail's local client reads
	# process memory.
	var admitted_state: PackedVector3Array = \
			sim.get_present_effect_state_for_wire_handle(admitted_handle)
	assert_eq(admitted_state.size(), Simulation.EFFECT_STATE_COUNT,
			"the host resolves an admitted peer from its pools immediately")
	if admitted_state.size() == Simulation.EFFECT_STATE_COUNT:
		assert_lt(admitted_state[Simulation.EFFECT_STATE_POSITION].distance_to(
				admitted_pos), 0.5)

	# The next host tick opens a new epoch; the remembered miss is discarded and
	# the same identity is looked up again (still absent here).
	sim.step()
	assert_true(sim.get_present_effect_state_for_wire_handle(
			absent_handle).is_empty(),
			"a new epoch retries a formerly missing identity")


func test_listen_server_restart_preserves_auto_spawned_local_identity() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, 0, Vector3(8, 0, 0), Vector3.ZERO)

	var sim := Simulation.new()
	sim.enable_listen_server(true)
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.has_local_player())
	var player_handle := sim.get_local_player_wire_handle()

	sim.reset_session()
	assert_true(sim.has_local_player(),
			"restart restores the listen baseline's host-player identity")
	assert_eq(sim.get_local_player_wire_handle(), player_handle)
	sim.step()
	var stride := sim.get_present_stride()
	var snapshot := sim.get_present_snapshot()
	var found_player := false
	for record in range(snapshot.size() / stride):
		if int(snapshot[record * stride + Simulation.PF_WIRE_HANDLE]) == player_handle:
			found_player = true
			break
	assert_true(found_player,
			"the restored host player still replicates through the loopback client")


func test_listen_server_auto_spawns_and_replicates_local_player() -> void:
	# Phase 2 (the moving player, net-re §5.2b/§5.38) + the faithful §5.0 mode-3 bring-up: the host's
	# own player is created as part of session bring-up (ADR 0012) — an authoritative pool-0 entity
	# that replicates through the wire to its own client view like any other entity. set_player_input
	# feeds its player-body input. (Headless has no anim clips, so the soldier holds — motion comes
	# from clips, as in the original; clip-driven forward motion is covered by the C++ player_spawn_test.)
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, 0, Vector3(0, 0, 0), Vector3.ZERO)  # a standing organic for company

	var sim := Simulation.new()
	sim.enable_listen_server(true)
	assert_true(sim.load_from_mission_data(md), "loaded as the inmatch listen server")
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
		if int(snap[base + Simulation.PF_WIRE_HANDLE]) == player_handle:
			found_player = true
			assert_eq(int(snap[base + Simulation.PF_ALIVE]), 1, "player is alive")
			# The player has no BMS placement: it carries the explicit none/synthetic
			# origin (kind 255, index 0xFFFFFF), which the wire present pass renders
			# wire-direct instead of deferring to a placed node. (The old Entity
			# default 0 read as authored kind 0/index 0 and defer-swallowed the row.)
			assert_eq(int(snap[base + Simulation.PF_KIND]), 255,
				"player carries the synthetic origin kind, not a fake authored identity")
			assert_eq(int(snap[base + Simulation.PF_INDEX]), 0xFFFFFF,
				"player carries the synthetic origin index sentinel")
	assert_true(found_player, "the auto-spawned local player replicated into the client-decoded present")


# (P7: test_listen_server_off_uses_ai_pool_present deleted — the no-net AI-pool present is retired;
#  the present is always the listen-server ClientState now.)
