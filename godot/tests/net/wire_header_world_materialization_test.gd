extends GutTest

# Production parity seam: the joiner authenticates on real loopback UDP, takes
# the host's exact S2C 0x0B 616-byte header, loads that body-empty mission on the
# SAME ClientRuntime/socket, then relies on 0x10/0x0D/0x20 to create retail's
# exact local pool rows for deploy/mount/collision consumers.

const VEHICLE_DEF_ID := 101291
const VEHICLE_TYPE := 1291
const ZONE_DEF_ID := 101359
const ZONE_TYPE := 1359
const MARKER_DEF_ID := 6002
const DESIGNATED_G_PARENT_DEF_ID := 105005
const DESIGNATED_G_PARENT_TYPE := 5005
const DESIGNATED_G_CHILD_TYPE := 1419


func _mission_fixture() -> Dictionary:
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	# First row in each independent pool: exact authority handles 0x1000,
	# 0x2000, and 0x3000. The joiner starts at x=0, deploys to x=12, then is
	# within the retail four-unit seat scan of the vehicle at x=14.
	var vehicle: Dictionary = mission.add_entity(
			NovaMissionData.KIND_ITEM, VEHICLE_DEF_ID,
			Vector3(14, 0, 0), Vector3.ZERO)
	var zone: Dictionary = mission.add_entity(
			NovaMissionData.KIND_BUILDING, ZONE_DEF_ID,
			Vector3(12, 0, 0), Vector3.ZERO)
	var marker: Dictionary = mission.add_entity(
			NovaMissionData.KIND_MARKER, MARKER_DEF_ID,
			Vector3.ZERO, Vector3.ZERO)
	assert_false(vehicle.is_empty())
	assert_false(zone.is_empty())
	assert_false(marker.is_empty())
	if not zone.is_empty():
		assert_true(mission.set_entity_property_int(
				NovaMissionData.KIND_BUILDING,
				int(zone.get("index", -1)), "team", 1))
	return {
		"mission": mission,
		"vehicle_bms_id": int(vehicle.get("bms_id", 0)),
		"zone_bms_id": int(zone.get("bms_id", 0)),
	}


func _designated_g_mission_fixture() -> Dictionary:
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	# The body-empty joiner deploys beside a vehicle-EWeap parent. Promotion
	# creates its designated-G B50 child at the next exact pool-1 handle.
	var parent: Dictionary = mission.add_entity(
			NovaMissionData.KIND_ITEM, DESIGNATED_G_PARENT_DEF_ID,
			Vector3(14, 0, 0), Vector3.ZERO)
	var zone: Dictionary = mission.add_entity(
			NovaMissionData.KIND_BUILDING, ZONE_DEF_ID,
			Vector3(12, 0, 0), Vector3.ZERO)
	var marker: Dictionary = mission.add_entity(
			NovaMissionData.KIND_MARKER, MARKER_DEF_ID,
			Vector3.ZERO, Vector3.ZERO)
	assert_false(parent.is_empty())
	assert_false(zone.is_empty())
	assert_false(marker.is_empty())
	if not zone.is_empty():
		assert_true(mission.set_entity_property_int(
				NovaMissionData.KIND_BUILDING,
				int(zone.get("index", -1)), "team", 1))
	return {
		"mission": mission,
		"parent_bms_id": int(parent.get("bms_id", 0)),
	}


func _item_db() -> NovaItemDatabase:
	var base_path := ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")
	var base_file := FileAccess.open(base_path, FileAccess.READ)
	assert_not_null(base_file)
	if base_file == null:
		return null
	var text := base_file.get_as_text().replace("\r\n", "\n")
	base_file.close()
	var path := ProjectSettings.globalize_path(
			"res://.godot/wire_header_world_items.def")
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	if file == null:
		return null
	file.store_string(text)
	if not text.ends_with("\n"):
		file.store_string("\n")
	file.store_string("""begin "Wire Header Spawn Zone"
  id 101359
  type object
  graphic MrkAlpha
  sid wire_header_zone
  hp 100
  attrib: SpawnPoint
end

begin "Wire Header Designated-G Parent"
  id 105005
  type vehicle
  sid wire_header_designated_g_parent
  hp 1000
  attrib: EWeap
  ai_function cveh
  render_function cveh
  move_function cveh
  primary_weapon WPN_EMPLCD50NA
end
""")
	file.close()
	var db := NovaItemDatabase.new()
	assert_eq(db.load(path), OK)
	return db


func _vehicle_seats() -> Array:
	return [{
		"type_id": VEHICLE_TYPE,
		"seats": [{
			"type": 2,
			"retail_slot": 8,
			"bone_index": 1,
			"position": Vector3.ZERO,
			"source_name": "ctrlx00",
		}],
	}]


func _designated_g_specs() -> Array:
	return [{
		"type_id": DESIGNATED_G_PARENT_TYPE,
		"primary_weapon": "WPN_EMPLCD50NA",
		"emplacement_attachments": [{
			"item_id": 101419,
			"stored_slot": 1,
			"designated_g": true,
			"local": Vector3(0, 3, 0),
		}],
	}, {
		"type_id": DESIGNATED_G_CHILD_TYPE,
		"seats": [{
			"type": NovaSimulation.SEAT_GUNNER,
			"retail_slot": 9,
			"bone_index": 1,
			"position": Vector3.ZERO,
			"source_name": "usegunx00",
		}],
		"primary_weapon": "WPN_EMPLCD50NA",
	}]


func _install_combat_tables(sim: NovaSimulation, db: NovaItemDatabase) -> void:
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/def")), OK)
	sim.resolve_item_traits(db)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_eq(sim.load_ammo_table(root, "ammo.def"), OK)


func _weapon(name: String) -> Dictionary:
	var weapons := NovaWeaponDatabase.new()
	assert_eq(weapons.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/weapon.def")), OK)
	var index := weapons.find_weapon(name)
	assert_gte(index, 0)
	return weapons.get_weapon(index) if index >= 0 else {}


func _apply_weapon_switch_events(sim: NovaSimulation,
		defs_by_name: Dictionary) -> void:
	for value in sim.drain_local_player_weapon_events():
		var name := String((value as Dictionary).get("switch_to_weapon", ""))
		if name.is_empty() or not defs_by_name.has(name):
			continue
		sim.set_local_player_weapon(
				defs_by_name[name], {}, name == "WPN_EMPLCD50NA")


func _present_wire_handle_for_type(sim: NovaSimulation, type_id: int) -> int:
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for row in range(snapshot.size() / stride):
		var base := row * stride
		if int(snapshot[base + NovaSimulation.PF_TYPE_ID]) == type_id:
			return int(snapshot[base + NovaSimulation.PF_WIRE_HANDLE])
	return -1


func _present_field_for_type(sim: NovaSimulation, type_id: int, field: int) -> int:
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for row in range(snapshot.size() / stride):
		var base := row * stride
		if int(snapshot[base + NovaSimulation.PF_TYPE_ID]) == type_id:
			return int(snapshot[base + field])
	return -1


func _present_position_for_type(sim: NovaSimulation, type_id: int) -> Vector3:
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for row in range(snapshot.size() / stride):
		var base := row * stride
		if int(snapshot[base + NovaSimulation.PF_TYPE_ID]) == type_id:
			return Vector3(
					snapshot[base + NovaSimulation.PF_POS_X],
					snapshot[base + NovaSimulation.PF_POS_Y],
					snapshot[base + NovaSimulation.PF_POS_Z])
	return Vector3.INF


func _player_index(sim: NovaSimulation) -> int:
	for index in range(sim.get_entity_count()):
		if int(sim.get_entity_debug(index).get("item_id", 0)) == 0x14B9:
			return index
	return -1


func _configure_dedicated_host(host: NovaSimulation) -> void:
	host.configure_host_session({
		"serve_and_play": false,
		"gametype": 0x30020,
		"mission_file": "WIRE_HEADER_PARITY.BMS",
	})


func test_true_wire_header_materializes_exact_deploy_and_vehicle_rows() -> void:
	var fixture := _mission_fixture()
	var mission: NovaMissionData = fixture["mission"]
	var db := _item_db()
	assert_not_null(db)
	if db == null:
		return
	var seats := _vehicle_seats()

	var host := NovaSimulation.new()
	_configure_dedicated_host(host)
	assert_true(host.enable_host_listen(0))
	host.set_item_seat_specs(seats)
	assert_true(host.load_from_mission_data(mission))
	host.resolve_item_traits(db)
	var host_vehicle: Dictionary = host.get_world_entity_debug(
			int(fixture["vehicle_bms_id"]))
	var host_zone: Dictionary = host.get_world_entity_debug(
			int(fixture["zone_bms_id"]))
	assert_eq(int(host_vehicle.get("handle", -1)), 0x1000)
	assert_eq(int(host_zone.get("handle", -1)), 0x2000)

	var joiner := NovaSimulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "WireHeaderJoiner"))
	joiner.set_join_world_ready(false)
	var header := PackedByteArray()
	for _tick in range(800):
		joiner.poll_join_preload()
		host.step()
		header = joiner.get_join_mission_header()
		if header.size() == 616:
			break
		OS.delay_msec(2)
	assert_eq(header.size(), 616,
			"the test consumes the host's real S2C 0x0B BMS header")
	if header.size() != 616:
		joiner.free()
		host.free()
		return

	var wire_mission := NovaMissionData.new()
	assert_eq(wire_mission.open_wire_header(header), OK)
	assert_true(wire_mission.is_wire_header_only())
	for kind in [NovaMissionData.KIND_MARKER, NovaMissionData.KIND_ITEM,
			NovaMissionData.KIND_BUILDING, NovaMissionData.KIND_ORGANIC]:
		assert_eq(wire_mission.get_entity_count(kind), 0,
				"the 616-byte mission has no locally authored body rows")
	assert_true(joiner.load_from_mission_data(wire_mission))
	joiner.resolve_item_traits(db)
	# Prime the body-empty cache. A later streamed zone must invalidate it.
	assert_eq(joiner.get_deploy_spawn_zones().size(), 0)

	var streamed := false
	for _tick in range(1000):
		host.step()
		joiner.step()
		if joiner.is_joined_in_match() \
				and joiner.is_join_deploy_pick_pending() \
				and _present_wire_handle_for_type(joiner, VEHICLE_TYPE) == 0x1000 \
				and _present_wire_handle_for_type(joiner, ZONE_TYPE) == 0x2000 \
				and joiner.get_deploy_spawn_zones().size() == 1:
			streamed = true
			break
		OS.delay_msec(2)
	assert_true(streamed,
			"wire pools materialized before deploy/vehicle consumers run")
	if not streamed:
		joiner.free()
		host.free()
		return
	assert_eq(joiner.get_join_terrain_til_state(),
			NovaSimulation.JOIN_TERRAIN_TIL_ABSENT,
			"a host with no terrain payload proves explicit 0x45 absence")
	assert_true(joiner.get_join_terrain_til().is_empty(),
			"Absent terrain never exposes a synthetic byte image")
	assert_eq(_present_wire_handle_for_type(joiner, VEHICLE_TYPE),
			int(host_vehicle.get("handle", -1)))
	assert_eq(_present_wire_handle_for_type(joiner, ZONE_TYPE),
			int(host_zone.get("handle", -1)))

	var deploy_rows := joiner.get_deploy_spawn_zones()
	var zone_param := int((deploy_rows[0] as Dictionary).get("param", 0))
	assert_gt(zone_param, 0)
	assert_true(joiner.send_deployment_pick(zone_param))
	var deployed := false
	for _tick in range(300):
		joiner.step()
		host.step()
		if not joiner.is_join_deploy_pick_pending() \
				and absf(joiner.get_local_player_position().x - 12.0) < 1.0:
			deployed = true
			break
		OS.delay_msec(2)
	assert_true(deployed,
			"the pool-2 exact handle round-trips through authoritative deployment")
	if not deployed:
		joiner.free()
		host.free()
		return

	# Install model-derived metadata AFTER the pool-1 row exists. This must
	# refresh that same registry row; presentation-only seat knowledge is not
	# sufficient for the local scan or the authority's carrier validation.
	joiner.set_item_seat_specs(seats)
	for _settle in range(12):
		joiner.step()
		host.step()
	assert_true(joiner.local_player_toggle_mount(),
			"late seat specs make the streamed vehicle mountable")
	var host_player_index := _player_index(host)
	var joiner_player_index := _player_index(joiner)
	assert_gte(host_player_index, 0)
	assert_gte(joiner_player_index, 0)
	var mounted := false
	for _tick in range(240):
		joiner.step()
		host.step()
		if bool(joiner.get_local_player_view().get("mounted", false)) \
				and host_player_index >= 0 \
				and bool(host.get_entity_debug(host_player_index).get(
						"mounted", false)):
			mounted = true
			break
		OS.delay_msec(2)
	assert_true(mounted,
			"the authority echoes the joiner's exact pool-1 carrier identity")
	if mounted:
		# Dense list order is presentation metadata. Retail occupancy is keyed by
		# the fixed mountHandles slot, so a late model refresh that inserts a
		# passenger before the controller must move both the occupant and its
		# mount_seat index to the controller's new dense row.
		joiner.set_item_seat_specs([{
			"type_id": VEHICLE_TYPE,
			"seats": [{
				"type": 1,
				"retail_slot": 0,
				"bone_index": 3,
				"position": Vector3.ZERO,
				"source_name": "sitex00",
			}, {
				"type": 2,
				"retail_slot": 8,
				"bone_index": 2,
				"position": Vector3.ZERO,
				"source_name": "ctrlx01",
			}],
		}])
	if mounted and joiner_player_index >= 0:
		var local_card: Dictionary = joiner.get_entity_debug(joiner_player_index)
		assert_true(bool(local_card.get("mounted", false)))
		assert_eq(int(local_card.get("mount_seat", -1)), 1,
				"the occupant's dense index follows retail slot 8")
		assert_eq(String(local_card.get("mount_seat_source_name", "")),
				"ctrlx01")
		assert_eq(int(local_card.get("mount_target_seat_count", 0)), 2)
		var target_seats: Array = local_card.get("mount_target_seats", [])
		assert_eq(target_seats.size(), 2)
		if target_seats.size() == 2:
			var seat: Dictionary = target_seats[1]
			assert_eq(int(seat.get("retail_slot", -1)), 8)
			assert_eq(String(seat.get("source_name", "")), "ctrlx01")
			assert_true(bool(seat.get("occupied", false)),
					"late seat refresh preserves the occupant by retail slot")
	if mounted and host_player_index >= 0:
		assert_eq(int(host.get_entity_debug(host_player_index).get(
				"mount_target_net_id", -1)),
				int(host_vehicle.get("net_id", -2)))
	joiner.free()
	host.free()


func test_true_wire_header_recovers_designated_g_parent_ammo_route() -> void:
	# Retail's 0x0D addeweap row carries child type + parent handle, but no
	# authored slot identity or designated-G bit. The body-empty client must
	# derive that capability from the same parent seat-spec table the authority
	# used, then honor compact seat_type and phase-8 ammo in both directions.
	var fixture := _designated_g_mission_fixture()
	var mission: NovaMissionData = fixture["mission"]
	var parent_bms_id := int(fixture["parent_bms_id"])
	var db := _item_db()
	assert_not_null(db)
	if db == null:
		return
	var specs := _designated_g_specs()

	var host := NovaSimulation.new()
	_configure_dedicated_host(host)
	assert_true(host.enable_host_listen(0))
	host.set_item_seat_specs(specs)
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host, db)
	var host_parent: Dictionary = host.get_world_entity_debug(parent_bms_id)
	assert_eq(int(host_parent.get("handle", -1)), 0x1000)

	var joiner := NovaSimulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "DesignatedGHeaderJoiner"))
	joiner.set_join_world_ready(false)
	var header := PackedByteArray()
	for _tick in range(800):
		joiner.poll_join_preload()
		host.step()
		header = joiner.get_join_mission_header()
		if header.size() == 616:
			break
		OS.delay_msec(2)
	assert_eq(header.size(), 616,
			"joiner consumes the authority's real body-empty 0x0B header")
	if header.size() != 616:
		joiner.free()
		host.free()
		return

	var wire_mission := NovaMissionData.new()
	assert_eq(wire_mission.open_wire_header(header), OK)
	assert_true(wire_mission.is_wire_header_only())
	assert_true(joiner.load_from_mission_data(wire_mission))
	_install_combat_tables(joiner, db)

	var streamed := false
	for _tick in range(1000):
		host.step()
		joiner.step()
		if joiner.is_joined_in_match() \
				and joiner.is_join_deploy_pick_pending() \
				and _present_wire_handle_for_type(
						joiner, DESIGNATED_G_PARENT_TYPE) == 0x1000 \
				and _present_wire_handle_for_type(
						joiner, DESIGNATED_G_CHILD_TYPE) == 0x1001 \
				and joiner.get_deploy_spawn_zones().size() == 1:
			streamed = true
			break
		OS.delay_msec(2)
	assert_true(streamed,
			"0x0D materialized the exact parent and synthetic child handles")
	if not streamed:
		joiner.free()
		host.free()
		return

	# Model/seat resolution commonly completes after the stock 0x0D rows. First
	# install a deliberately ambiguous same-type sibling. The joiner cannot know
	# which authored slot produced its child, so its exact rigid 0x0D pose remains
	# authoritative. The promoted host does know stored slot 1 and must retain
	# that identity across the same duplicate-table refresh.
	var ambiguous_specs: Array = specs.duplicate(true)
	var ambiguous_attachments: Array = \
			ambiguous_specs[0]["emplacement_attachments"]
	ambiguous_attachments.append({
		"item_id": 101419,
		"stored_slot": 2,
		"local": Vector3(7, 0, 0),
	})
	host.set_item_seat_specs(ambiguous_specs)
	joiner.set_item_seat_specs(ambiguous_specs)
	var joiner_parent_before := _present_position_for_type(
			joiner, DESIGNATED_G_PARENT_TYPE)
	var joiner_child_before := _present_position_for_type(
			joiner, DESIGNATED_G_CHILD_TYPE)
	assert_true(joiner_parent_before.is_finite() and joiner_child_before.is_finite())
	var joiner_rigid_offset := joiner_child_before - joiner_parent_before
	assert_gt(joiner_rigid_offset.length(), 2.5,
			"the authored child starts separated from the carrier root")

	# Move the authority carrier after both late refreshes and advance real network
	# ticks. A frozen child or a parent-root fallback fails the offset checks.
	host.debug_set_world_entity_position(parent_bms_id, Vector3(14.25, 0, 0))
	var rigid_motion_observed := false
	var moved_joiner_parent := joiner_parent_before
	var moved_joiner_child := joiner_child_before
	for _tick in range(240):
		host.step()
		joiner.step()
		moved_joiner_parent = _present_position_for_type(
				joiner, DESIGNATED_G_PARENT_TYPE)
		moved_joiner_child = _present_position_for_type(
				joiner, DESIGNATED_G_CHILD_TYPE)
		if moved_joiner_parent.distance_to(joiner_parent_before) > 0.15:
			rigid_motion_observed = true
			break
		OS.delay_msec(1)
	assert_true(rigid_motion_observed,
			"the moved carrier reached the header-only joiner")
	assert_lt((moved_joiner_child - moved_joiner_parent).distance_to(
			joiner_rigid_offset), 0.05,
			"ambiguous joiner child preserves its exact rigid wire offset")

	# Restore the fixture's scan distance, then replace the ambiguous table with
	# the unique authored row so the remaining route/ammo assertions exercise the
	# recoverable designated-G capability.
	host.debug_set_world_entity_position(parent_bms_id, Vector3(14, 0, 0))
	for _settle in range(48):
		host.step()
		joiner.step()
		OS.delay_msec(1)
	joiner.set_item_seat_specs(specs)

	var deploy_rows := joiner.get_deploy_spawn_zones()
	var zone_param := int((deploy_rows[0] as Dictionary).get("param", 0))
	assert_gt(zone_param, 0)
	assert_true(joiner.send_deployment_pick(zone_param))
	var deployed := false
	for _tick in range(300):
		joiner.step()
		host.step()
		if not joiner.is_join_deploy_pick_pending() \
				and absf(joiner.get_local_player_position().x - 12.0) < 1.0:
			deployed = true
			break
		OS.delay_msec(2)
	assert_true(deployed)
	if not deployed:
		joiner.free()
		host.free()
		return
	# Give the 17-tick retail proximity slice a full post-deploy refresh before
	# pressing USE. The short weapon-idle loop below can otherwise break at tick 1.
	for _settle in range(24):
		joiner.step()
		host.step()
		OS.delay_msec(1)

	var personal := _weapon("WPN_M4AUTO")
	var mounted_weapon := _weapon("WPN_EMPLCD50NA")
	var weapon_defs := {
		"WPN_M4AUTO": personal,
		"WPN_EMPLCD50NA": mounted_weapon,
	}
	assert_true(joiner.apply_local_player_loadout(
			[{"name": "WPN_M4AUTO"}], 8))
	joiner.set_local_player_weapon(personal, {})
	for _settle in range(80):
		joiner.step()
		host.step()
		_apply_weapon_switch_events(joiner, weapon_defs)
		if int(joiner.get_local_player_weapon_state().get("current", -1)) < 2:
			break
		OS.delay_msec(1)
	assert_lt(int(joiner.get_local_player_weapon_state().get("current", 99)), 2,
			"personal weapon settled before the mount action")
	assert_eq(_present_field_for_type(
			joiner, DESIGNATED_G_CHILD_TYPE, NovaSimulation.PF_ALIVE), 1)
	assert_lt(_present_position_for_type(
			joiner, DESIGNATED_G_CHILD_TYPE).distance_to(
					joiner.get_local_player_position()), 4.0,
			"the decoded child remains inside retail's seat-scan radius")
	var attach_labels: Array = joiner.get_attach_labels()
	assert_eq(attach_labels.size(), 1,
			"the decoded child contributes one in-range UseGun label")
	if attach_labels.size() == 1:
		assert_eq(int((attach_labels[0] as Dictionary).get("seat_type", 0)),
				NovaSimulation.SEAT_GUNNER)
	assert_true(joiner.local_player_toggle_mount(),
			"the decoded child exposes its authored UseGun seat")
	var host_player_index := _player_index(host)
	assert_gte(host_player_index, 0)
	var mounted_echoed := false
	for _tick in range(240):
		joiner.step()
		host.step()
		_apply_weapon_switch_events(joiner, weapon_defs)
		if bool(joiner.get_local_player_view().get("mounted", false)) \
				and host_player_index >= 0 \
				and bool(host.get_entity_debug(host_player_index).get(
						"mounted", false)):
			mounted_echoed = true
			break
		OS.delay_msec(2)
	assert_true(mounted_echoed,
			"authority echoes the exact decoded child carrier")
	if not mounted_echoed:
		joiner.free()
		host.free()
		return

	for _settle in range(100):
		joiner.step()
		host.step()
		_apply_weapon_switch_events(joiner, weapon_defs)
		if int(joiner.get_local_player_weapon_state().get("current", -1)) < 2:
			break
		OS.delay_msec(1)
	var child_ammo: Dictionary = joiner.get_local_player_weapon_state()
	var child_clip := int(child_ammo.get("clip", -999))
	var child_reserve := int(child_ammo.get("reserve", -999))
	assert_true(child_clip != 7 or child_reserve != 19,
			"the direct child slot differs from the parent witness")

	assert_true(joiner.request_local_player_scope_toggle(),
			"header-only child recovered designated-G action-6 capability")
	# Let the authority consume action 6 and prepare the parent slot before
	# stamping the non-default phase-8 witness; first-time preparation seeds the
	# weapon.def defaults and would intentionally replace an earlier debug value.
	joiner.step()
	host.step()
	_apply_weapon_switch_events(joiner, weapon_defs)
	assert_eq(host.debug_set_world_entity_weapon_ammo(
			parent_bms_id, 7, 19), OK)
	var parent_ammo_applied := false
	# Phase 8 is every sixteenth emitted network frame, not every simulation
	# step. Allow one complete low-rate network cycle after the route echo.
	for _tick in range(640):
		joiner.step()
		host.step()
		_apply_weapon_switch_events(joiner, weapon_defs)
		var active: Dictionary = joiner.get_local_player_weapon_state()
		if int(active.get("clip", -999)) == 7 \
				and int(active.get("reserve", -999)) == 19:
			parent_ammo_applied = true
			break
		OS.delay_msec(2)
	assert_true(parent_ammo_applied,
			"compact route echo and phase 8 select the vehicle-parent MountSlot")

	assert_true(joiner.request_local_player_scope_toggle(),
			"action 6 queues the return to the child MountSlot")
	var child_route_restored := false
	for _tick in range(120):
		joiner.step()
		host.step()
		_apply_weapon_switch_events(joiner, weapon_defs)
		var active: Dictionary = joiner.get_local_player_weapon_state()
		if int(active.get("clip", -999)) == child_clip \
				and int(active.get("reserve", -999)) == child_reserve:
			child_route_restored = true
			break
		OS.delay_msec(1)
	assert_true(child_route_restored,
			"returning to the child recovers its untouched ammo words")
	var host_parent_after: Dictionary = host.get_world_entity_debug(parent_bms_id)
	assert_eq(int(host_parent_after.get("primary_weapon_clip", -999)), 7)
	assert_eq(int(host_parent_after.get("primary_weapon_reserve", -999)), 19)
	joiner.free()
	host.free()


func test_complete_bms_joiner_keeps_authored_promotion_identity() -> void:
	var fixture := _mission_fixture()
	var mission: NovaMissionData = fixture["mission"]
	assert_false(mission.is_wire_header_only())
	var db := _item_db()
	assert_not_null(db)
	if db == null:
		return

	var host := NovaSimulation.new()
	_configure_dedicated_host(host)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	host.resolve_item_traits(db)
	var joiner := NovaSimulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "FullBmsJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	joiner.resolve_item_traits(db)

	var before: Dictionary = joiner.get_world_entity_debug(
			int(fixture["vehicle_bms_id"]))
	assert_eq(int(before.get("handle", -1)), 0x1000)
	assert_eq(int(before.get("kind", -1)), NovaMissionData.KIND_ITEM)
	assert_eq(int(before.get("index", -1)), 0)
	assert_eq(int(before.get("bms_id", 0)), int(fixture["vehicle_bms_id"]))
	for _tick in range(800):
		host.step()
		joiner.step()
		if joiner.is_joined_in_match() \
				and _present_wire_handle_for_type(joiner, VEHICLE_TYPE) == 0x1000:
			break
		OS.delay_msec(2)
	var after: Dictionary = joiner.get_world_entity_debug(
			int(fixture["vehicle_bms_id"]))
	assert_eq(int(after.get("handle", -1)), 0x1000)
	assert_eq(int(after.get("kind", -1)), NovaMissionData.KIND_ITEM,
			"full-BMS joiners retain authored spawn_origin semantics")
	assert_eq(int(after.get("index", -1)), 0)
	assert_eq(int(after.get("bms_id", 0)), int(fixture["vehicle_bms_id"]))
	joiner.free()
	host.free()
