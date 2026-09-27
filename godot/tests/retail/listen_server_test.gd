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
		MountLook.face(self, sim, Vector3(2, 0, 0))
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
