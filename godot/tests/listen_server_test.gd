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
		for file_name in DirAccess.get_files_at(dir):
			DirAccess.remove_absolute(dir.path_join(file_name))
		DirAccess.remove_absolute(dir)
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


func _present_row_base_for_handle(
		sim: Simulation, snapshot: PackedFloat32Array, wire_handle: int) -> int:
	var stride := sim.get_present_stride()
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_WIRE_HANDLE]) == wire_handle:
			return base
	return -1


func _packed_aim_angles(
		snapshot: PackedFloat32Array, base: int) -> PackedVector3Array:
	var angles := PackedVector3Array()
	angles.resize(9)
	for overlay_class in range(9):
		var offset: int = (base + Simulation.PF_AIM_ANGLES
				+ overlay_class * Simulation.PF_AIM_CLASS_STRIDE)
		angles[overlay_class] = Vector3(
				snapshot[offset], snapshot[offset + 1], snapshot[offset + 2])
	return angles


func test_mounted_local_overlay_matches_packed_present_for_valid_zero_and_six() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# The local avatar and the listen-server client view are two consumers of the
	# same authoritative selector result. Config 0 is deliberately included: an
	# explicit zero must survive as a real counter-lean branch, not become unknown.
	for config_value in [0, 6]:
		var md := MissionData.new()
		assert_eq(md.create_default(), OK)
		assert_not_null(md.add_entity(
				MissionData.KIND_ITEM, 101294,
				Vector3(2, 0, 0), Vector3.ZERO))

		var sim := Simulation.new()
		sim.enable_listen_server(true)
		# mount's authored Usegun row (bone 6) + the fixture def's WPN_AVENGER
		# primary; phrase_set is authored per config — an explicit 0 must
		# survive the def parser as a real value, not become unknown.
		var dir := _native_fixture_dir()
		var model_file := FileAccess.open(
				dir.path_join("mount.3di"), FileAccess.WRITE)
		assert_not_null(model_file)
		model_file.store_buffer(FileAccess.get_file_as_bytes(
				"res://../fixtures/threedi/synth/mount.3di"))
		model_file.close()
		_install_native_seats(sim, dir, _fixture_items_text().replace(
				"id 101294",
				"id 101294\n  graphic mount\n  phrase_set %d" % config_value),
				PackedInt32Array([1294]))
		assert_true(sim.load_from_mission_data(md))
		assert_true(sim.has_local_player())
		# NAPI authority bypasses the offline player's null-EquippedSlot reject
		# [orig: Entity_AttachToUseGunSlot @0x546c07], but this fixture has not
		# loaded weapon.def yet. Complete the normal mission-start weapon/loadout
		# leg so the parent's embedded MountSlot and its presentation resolve.
		var root := ResourceRoot.new()
		assert_eq(root.set_root_dir(RetailData.def_root()), OK)
		assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
		var weapons := WeaponDatabase.new()
		assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
		var personal_index := weapons.find_weapon("WPN_M4AUTO")
		assert_gte(personal_index, 0)
		if personal_index >= 0:
			sim.set_local_player_weapon(weapons.get_weapon(personal_index), {})
		sim.drain_local_player_weapon_events()
		MountLook.face(sim, Vector3(2, 0, 0))
		assert_true(sim.local_player_toggle_mount(),
				"the listen-server player mounts the config-%d gun" % config_value)
		sim.set_local_player_mouse(511, false)
		sim.add_local_player_look(80.0, 100.0)
		sim.step()

		var local := sim.get_local_player_aim_overlay()
		assert_true(local != null)
		assert_eq(local.mount_mode, 2,
				"UseGun selects the animation-owned Gunner mode")
		assert_true(local.mount_config_valid)
		assert_eq(local.mount_config, config_value)
		var local_angles := local.segment_angles
		assert_eq(local_angles.size(), 9)

		var snapshot := sim.get_present_snapshot()
		var base := _present_row_base_for_handle(
				sim, snapshot, sim.get_local_player_wire_handle())
		assert_gte(base, 0, "the mounted local player reached its decoded client view")
		if base >= 0:
			assert_eq(int(snapshot[base + Simulation.PF_AIM_OVERLAY_VALID]), 1)
			assert_eq(int(snapshot[base + Simulation.PF_RIGHT_HAND_COLLAPSED]), 0,
					"retail Flags 0x100 keeps the player BN17 row live on UseGun")
			var packed_body := Vector3(
					snapshot[base + Simulation.PF_AIM_BODY_PITCH_DEG],
					snapshot[base + Simulation.PF_AIM_BODY_YAW_DEG],
					snapshot[base + Simulation.PF_AIM_BODY_ROLL_DEG])
			assert_lt(packed_body.distance_to(local.body_angles), 0.001,
					"packed body orientation equals the local selector result")
			var packed_angles := _packed_aim_angles(snapshot, base)
			for overlay_class in range(9):
				assert_lt(packed_angles[overlay_class].distance_to(
						local_angles[overlay_class]), 0.001,
						"config %d class %d has local/present parity" % [
								config_value, overlay_class])

			# Keep each row branch-distinguishing rather than accepting nine equal
			# zeroes: config 0 counter-leans its arms, while config 6 keeps the
			# upper body on the body matrix and lets only the head take aim.
			if config_value == 0:
				assert_gt(packed_angles[4].distance_to(packed_angles[0]), 0.1,
						"config 0 exports its witnessed arm counter-lean")
				assert_gt(packed_angles[4].distance_to(packed_angles[8]), 0.1,
						"config 0 arm is neither the body nor full-aim matrix")
			else:
				for overlay_class in [1, 2, 3, 4, 7]:
					assert_lt(packed_angles[overlay_class].distance_to(
							packed_angles[0]), 0.001,
							"config 6 class %d stays on body" % overlay_class)
				assert_gt(packed_angles[8].distance_to(packed_angles[0]), 0.1,
						"config 6 head alone preserves full aim")

		assert_true(sim.local_player_toggle_mount(),
				"the same player can leave the emplaced seat")
		sim.step()
		var dismounted_snapshot := sim.get_present_snapshot()
		var dismounted_base := _present_row_base_for_handle(
				sim, dismounted_snapshot, sim.get_local_player_wire_handle())
		assert_gte(dismounted_base, 0)
		if dismounted_base >= 0:
			assert_eq(int(dismounted_snapshot[dismounted_base
					+ Simulation.PF_RIGHT_HAND_COLLAPSED]), 0,
					"dismount clears the transient bone-collapse verdict")


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
