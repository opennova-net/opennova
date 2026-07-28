extends GutTest

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const ItemSeatSpecs := preload("res://engine/world/item_seat_specs.gd")

const MISSION := "00TRc.bms"
const GUN_BMS_ID := 88
const GUN_ITEM_ID := 101881
const GUN_GRAPHIC := "E50triB"


func test_00trc_e50trib_mounted_avatar_root_follows_live_usegun_frame() -> void:
	var install_dir := OS.get_environment("OPENNOVA_JO_DIR").strip_edges()
	if install_dir.is_empty() or not DirAccess.dir_exists_absolute(install_dir):
		pending("OPENNOVA_JO_DIR / retail JO PFFs are required for the 00TRc E50triB witness")
		return

	var root := NovaResourceRoot.new()
	assert_eq(root.mount_runtime(install_dir, "", false, "jo"), OK)
	var mission := NovaMissionData.new()
	assert_eq(mission.open_from_resource_root(root, MISSION), OK)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)

	var gun: Dictionary = {}
	for raw in mission.get_all_entities():
		var entity: Dictionary = raw
		if int(entity.get("bms_id", 0)) == GUN_BMS_ID:
			gun = entity
			break
	assert_false(gun.is_empty(), "00TRc contains the near-spawn .50 cal")
	assert_eq(int(gun.get("item_id", 0)), GUN_ITEM_ID)
	assert_eq(String(item_db.get_graphic(GUN_ITEM_ID)), GUN_GRAPHIC)

	var data := NovaObjectData.new()
	assert_eq(data.open_from_resource_root(root, GUN_GRAPHIC + ".3di"), OK)
	# Build the expected frame from the retail USRP row directly. The production
	# seat-spec/provider path below is only the system under test, never the oracle.
	var userpoint_index := -1
	for index in range(data.get_user_point_count()):
		var candidate: Dictionary = data.get_user_point_info(index)
		if String(candidate.get("name", "")).nocasecmp_to("Usegun") == 0:
			userpoint_index = index
			break
	assert_gte(userpoint_index, 0, "E50triB carries its retail Usegun USRP row")
	if userpoint_index < 0:
		return
	var userpoint: Dictionary = data.get_user_point_info(userpoint_index)
	var part_index := int(userpoint.get("subobject", -1))
	assert_eq(part_index, 1, "E50triB Usegun is owned by the articulated gun part")

	var seats := ItemSeatSpecs.seat_specs_from_model(data, true)
	assert_eq(seats.size(), 1)
	var seat: Dictionary = seats[0]
	assert_eq(String(seat.get("source_name", "")).to_lower(), "usegun")
	assert_eq(int(seat.get("bone_index", 0)) - 1, userpoint_index,
			"the runtime selected the exact retail Usegun row")

	var sim := NovaSimulation.new()
	sim.set_item_seat_specs(ItemSeatSpecs.build_item_seat_specs(
			mission, root, item_db, true))
	assert_true(sim.load_from_mission_data(mission))
	assert_eq(sim.spawn_local_player_at_start(), 1)
	sim.resolve_item_traits(item_db)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([{"name": "WPN_M4AUTO"}], 1))
	var weapons := NovaWeaponDatabase.new()
	assert_eq(weapons.load_from_resource_root(root, "weapon.def"), OK)
	var personal_index := weapons.find_weapon("WPN_M4AUTO")
	assert_gte(personal_index, 0)
	sim.set_local_player_weapon(weapons.get_weapon(personal_index), {})
	sim.drain_local_player_weapon_events()

	assert_true(sim.local_player_toggle_mount(), "the 00TRc spawn can mount E50triB")
	sim.step()
	sim.set_local_player_mouse(511, false)
	sim.add_local_player_look(300.0, 0.0)
	sim.step()
	var weapon_state: Dictionary = sim.get_local_player_weapon_state()
	var yaw_control := int(weapon_state.get("emplaced_gun_yaw", 0))
	var pitch_control := int(weapon_state.get("emplaced_gun_pitch", 0))
	assert_ne(yaw_control, 0, "look input drives E50triB's EWEAP_GUNYAW")

	var rest_parts: Dictionary = data.evaluate_panm(0, 0, {})
	var live_parts: Dictionary = data.evaluate_panm(0, 0, {
		"EWEAP_GUNYAW": yaw_control,
		"EWEAP_GUNPITCH": pitch_control,
	})
	assert_true(rest_parts.has(part_index))
	assert_true(live_parts.has(part_index))
	var authored_model_position: Vector3 = userpoint.get("position", Vector3.ZERO)
	var point_in_part := (rest_parts[part_index] as Transform3D).affine_inverse() \
			* authored_model_position
	var live_usegun_model := (live_parts[part_index] as Transform3D) * point_in_part
	var gun_world := MissionObjectPlacer.entity_transform(
			gun.get("position", Vector3.ZERO), gun.get("rotation_deg", Vector3.ZERO))
	var live_usegun_world := gun_world * live_usegun_model
	var avatar_root_world := sim.get_local_player_position()
	var drift := avatar_root_world.distance_to(live_usegun_world)
	assert_lt(drift, 0.001,
			"mounted avatar root %s must follow live E50triB Usegun %s (drift %.6f m)" % [
				str(avatar_root_world), str(live_usegun_world), drift])
	var overlay: Dictionary = sim.get_local_player_aim_overlay()
	assert_true(bool(overlay.get("valid", false)),
			"the mounted local player exports its authoritative body frame")
	var expected_body_basis := gun_world.basis \
			* (live_parts[part_index] as Transform3D).basis
	var actual_body_basis := MissionObjectPlacer.bms_to_godot_basis(
			overlay.get("body", Vector3.ZERO))
	var basis_error_deg := rad_to_deg(expected_body_basis.get_rotation_quaternion().angle_to(
			actual_body_basis.get_rotation_quaternion()))
	assert_lt(basis_error_deg, 0.51,
			"mounted avatar body must follow E50triB's live yaw frame (error %.6f deg)" % \
					basis_error_deg)
	sim.free()
