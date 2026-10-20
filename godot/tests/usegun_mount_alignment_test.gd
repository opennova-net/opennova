extends GutTest

# A mounted gunner's avatar root follows the live `Usegun` frame of an
# articulated emplacement: the seat point rides the gun's yawing cradle, so the
# look input that turns the gun carries the seated body with it, within a
# millimetre, and the body's basis turns with the cradle's live yaw. The gun is
# the minted tripod (fixtures/gut/tripod_usegun_cradle.3di), shaped after the
# retail 00TRc witness this replaced, E50triB (the 50cal on 360 tripod, item
# 101881): its Usegun USRP row is owned by the articulated cradle (part 1).

const MountLook := preload("res://tests/support/mount_look.gd")

const GUN_POSITION := Vector3(2.2, 0.4, 0.0)
const GUN_ROTATION := Vector3(0.0, 20.0, 0.0)

var _dir := ""


func after_each() -> void:
	if not _dir.is_empty():
		TestFs.remove_dir_recursive(_dir)
	_dir = ""


func _mission_type_ids(mission: MissionData) -> PackedInt32Array:
	# The S16 boot seeding: every mission entity type id, deduplicated. The
	# native extractor walks authored addeweap children transitively and drops
	# rows without runtime metadata itself.
	var seen := {}
	var type_ids := PackedInt32Array()
	for raw in mission.get_all_entity_refs():
		var entity: EntityRef = raw
		var type_id := entity.runtime_type_id
		if type_id > 0 and not seen.has(type_id):
			seen[type_id] = true
			type_ids.append(type_id)
	return type_ids


func test_mounted_avatar_root_follows_live_usegun_frame() -> void:
	_dir = VehicleFixture.stage(self, "usegun_alignment")
	var root := VehicleFixture.root_over(self, _dir)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_not_null(mission.add_entity(MissionData.KIND_MARKER, 106001, Vector3.ZERO, Vector3.ZERO))
	var gun: EntityRef = mission.add_entity(MissionData.KIND_ITEM, VehicleFixture.TRIPOD,
			GUN_POSITION, GUN_ROTATION)
	assert_not_null(gun)
	assert_eq(gun.item_id, VehicleFixture.TRIPOD)
	assert_eq(String(item_db.get_graphic(VehicleFixture.TRIPOD)), "tripod")

	var data := ObjectData.new()
	assert_eq(data.open_from_resource_root(root, "tripod.3di"), OK)
	# Build the expected frame from the model's USRP row directly. The production
	# seat-spec/provider path below is only the system under test, never the oracle.
	var userpoint_index := -1
	for index in range(data.get_user_point_count()):
		var candidate := data.get_user_point_info(index)
		if candidate.name.nocasecmp_to("Usegun") == 0:
			userpoint_index = index
			break
	assert_gte(userpoint_index, 0, "the tripod carries its Usegun USRP row")
	if userpoint_index < 0:
		return
	var userpoint := data.get_user_point_info(userpoint_index)
	var part_index := userpoint.subobject
	assert_eq(part_index, 1, "the tripod's Usegun is owned by the articulated cradle part")

	# S16: the seat/mount table is the native extraction over items.def rows +
	# .3di userpoints — the asset root must be installed before the seed walk.
	var sim := Simulation.new()
	sim.set_asset_root(root)
	assert_true(sim.install_seat_specs_for_type_ids(
			item_db, _mission_type_ids(mission)))
	assert_gt(sim.get_mounted_graphic_source_count(), 0,
			"the native install fed the mounted-pose resolver")
	assert_true(sim.load_from_mission_data(mission))
	var card := sim.entity_card_by_net_id(gun.bms_id)
	var seats := card.get_seats()
	assert_eq(seats.size(), 1)
	var seat: EntityCardSeat = seats[0]
	assert_eq(seat.get_source_name().to_lower(), "usegun")
	assert_eq(seat.get_bone_index() - 1, userpoint_index,
			"the runtime selected the exact Usegun row")

	assert_eq(sim.spawn_local_player_at_start(), 1)
	sim.resolve_item_traits(item_db)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 1))
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load_from_resource_root(root, "weapon.def"), OK)
	var personal_index := weapons.find_weapon("WPN_M4AUTO")
	assert_gte(personal_index, 0)
	sim.set_local_player_weapon(weapons.get_weapon(personal_index), {})
	sim.drain_local_player_weapon_events()

	MountLook.face(self, sim, gun.position)
	assert_true(sim.local_player_toggle_mount(), "the spawn can mount the tripod")
	sim.step()
	sim.set_local_player_mouse(511, false)
	sim.add_local_player_look(300.0, 0.0)
	sim.step()
	var weapon_state := sim.get_local_player_weapon_state()
	var yaw_control := weapon_state.emplaced_gun_yaw
	var pitch_control := weapon_state.emplaced_gun_pitch
	assert_ne(yaw_control, 0, "look input drives the tripod's EWEAP_GUNYAW")

	var rest_parts: Dictionary = data.evaluate_panm(0, 0, {})
	var live_parts: Dictionary = data.evaluate_panm(0, 0, {
		"EWEAP_GUNYAW": yaw_control,
		"EWEAP_GUNPITCH": pitch_control,
	})
	assert_true(rest_parts.has(part_index))
	assert_true(live_parts.has(part_index))
	assert_false((live_parts[part_index] as Transform3D).is_equal_approx(rest_parts[part_index]),
			"the live yaw moves the seat's part: a static seat would prove nothing")
	var authored_model_position: Vector3 = userpoint.position
	var point_in_part := (rest_parts[part_index] as Transform3D).affine_inverse() \
			* authored_model_position
	var live_usegun_model := (live_parts[part_index] as Transform3D) * point_in_part
	var gun_world := MissionObjectPlacer.entity_transform(
			gun.position, mission.get_entity_rotation(gun.kind, gun.index))
	var live_usegun_world := gun_world * live_usegun_model
	var avatar_root_world := sim.get_local_player_position()
	var drift := avatar_root_world.distance_to(live_usegun_world)
	assert_lt(drift, 0.001,
			"mounted avatar root %s must follow the live Usegun %s (drift %.6f m)" % [
				str(avatar_root_world), str(live_usegun_world), drift])
	var overlay := sim.get_local_player_aim_overlay()
	assert_true(overlay != null,
			"the mounted local player exports its authoritative body frame")
	var expected_body_basis := gun_world.basis \
			* (live_parts[part_index] as Transform3D).basis
	var actual_body_basis := MissionObjectPlacer.bms_to_godot_basis(
			overlay.body_angles)
	var basis_error_deg := rad_to_deg(expected_body_basis.get_rotation_quaternion().angle_to(
			actual_body_basis.get_rotation_quaternion()))
	assert_lt(basis_error_deg, 0.51,
			"mounted avatar body must follow the tripod's live yaw frame (error %.6f deg)" % \
					basis_error_deg)
