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

# S16 native seat tables: the Dictionary install seam is gone. The tests
# compose a flat asset dir under the gitignored res://.godot (ResourceRoot
# rejects user:// roots), wire it with sim.set_asset_root FIRST, then
# install_seat_specs_for_type_ids(item_db, ...) runs the ONE engine extractor
# (mission::extract_item_seat_specs) over items.def rows + .3di userpoints.
# Three composed models re-author committed fixtures at the byte level
# (48-byte USRP records; the name field at +32):
# - tank.3di      (verbatim)  one ctrlx control seat, retail slot 8, bone 1.
# - carrierswap.3di   (carrier)     ctrlx13 -> sitex13 and sitex00d -> ctrlx01, so
#                              a passenger row precedes the controller in
#                              dense extraction order (the refresh pin).
# - tankgp.3di    (tank)     ctrlx25 retired and ewep01 moved to authored
#                              (0, 3, 0) mission-local — the designated-G
#                              parent anchor the old hand table carried.
const NATIVE_MODEL_DIR := "res://.godot/native_3dp_wire_header"


func should_skip_script():
	return RetailData.def_root_skip()


func before_all() -> void:
	DirAccess.make_dir_recursive_absolute(
			ProjectSettings.globalize_path(NATIVE_MODEL_DIR))
	assert_true(DirAccess.dir_exists_absolute(
			ProjectSettings.globalize_path(NATIVE_MODEL_DIR)),
			"created the flat native asset dir")
	assert_true(NativeModelFixture.write_native_model(NATIVE_MODEL_DIR, "tank.3di",
			NativeModelFixture.repo_file_bytes("res://../fixtures/threedi/synth/tank.3di")),
			"composed the single-control-seat vehicle fixture")
	assert_true(NativeModelFixture.write_native_model(NATIVE_MODEL_DIR, "mount.3di",
			NativeModelFixture.repo_file_bytes("res://../fixtures/threedi/synth/mount.3di")),
			"composed the B50 child fixture")
	var swap := NativeModelFixture.with_renamed_user_point(
			NativeModelFixture.repo_file_bytes("res://../fixtures/threedi/synth/carrier.3di"),
			"ctrlx13", "sitex13")
	swap = NativeModelFixture.with_renamed_user_point(swap, "sitex00d", "ctrlx01")
	assert_true(NativeModelFixture.write_native_model(NATIVE_MODEL_DIR, "carrierswap.3di", swap),
			"composed the passenger-before-controller refresh fixture")
	# seat_local reads (-y, x, z)/65536 over the raw authored ints, so raw
	# (196608, 0, 0) lands the anchor at mission-local (0, 3, 0) — the exact
	# offset the retired hand table authored for this fixture.
	var parent := NativeModelFixture.with_renamed_user_point(
			NativeModelFixture.repo_file_bytes("res://../fixtures/threedi/synth/tank.3di"),
			"ctrlx25", "cxrlx25")
	parent = NativeModelFixture.with_user_point_position(parent, "ewep01", 196608, 0, 0)
	assert_true(NativeModelFixture.write_native_model(NATIVE_MODEL_DIR, "tankgp.3di", parent),
			"composed the seatless designated-G parent fixture")


func _native_asset_root() -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path(NATIVE_MODEL_DIR)), OK)
	assert_true(root.has_file("tank.3di"),
			"the composed flat asset dir indexes the fixture models")
	return root


func _mission_fixture() -> Dictionary:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	# First row in each independent pool: exact authority handles 0x1000,
	# 0x2000, and 0x3000. The joiner starts at x=0, deploys to x=12, then is
	# within the retail four-unit seat scan of the vehicle at x=14.
	var vehicle: EntityRef = mission.add_entity(
			MissionData.KIND_ITEM, VEHICLE_DEF_ID,
			Vector3(14, 0, 0), Vector3.ZERO)
	var zone: EntityRef = mission.add_entity(
			MissionData.KIND_BUILDING, ZONE_DEF_ID,
			Vector3(12, 0, 0), Vector3.ZERO)
	var marker: EntityRef = mission.add_entity(
			MissionData.KIND_MARKER, MARKER_DEF_ID,
			Vector3.ZERO, Vector3.ZERO)
	assert_not_null(vehicle)
	assert_not_null(zone)
	assert_not_null(marker)
	if zone != null:
		assert_true(mission.set_entity_property_int(
				MissionData.KIND_BUILDING,
				zone.index, "team", 1))
	return {
		"mission": mission,
		"vehicle_bms_id": vehicle.bms_id,
		"zone_bms_id": zone.bms_id,
	}


func _designated_g_mission_fixture() -> Dictionary:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	# The body-empty joiner deploys beside a vehicle-EWeap parent. Promotion
	# creates its designated-G B50 child at the next exact pool-1 handle.
	var parent: EntityRef = mission.add_entity(
			MissionData.KIND_ITEM, DESIGNATED_G_PARENT_DEF_ID,
			Vector3(14, 0, 0), Vector3.ZERO)
	var zone: EntityRef = mission.add_entity(
			MissionData.KIND_BUILDING, ZONE_DEF_ID,
			Vector3(12, 0, 0), Vector3.ZERO)
	var marker: EntityRef = mission.add_entity(
			MissionData.KIND_MARKER, MARKER_DEF_ID,
			Vector3.ZERO, Vector3.ZERO)
	assert_not_null(parent)
	assert_not_null(zone)
	assert_not_null(marker)
	if zone != null:
		assert_true(mission.set_entity_property_int(
				MissionData.KIND_BUILDING,
				zone.index, "team", 1))
	return {
		"mission": mission,
		"parent_bms_id": parent.bms_id,
	}


# The designated-G parent is moved explicitly by the test. Its null mover
# keeps it at the authored seat-scan position in this terrain-free wire fixture.
# variant "" = the authoritative table; "refresh" swaps the vehicle graphic to
# carrierswap (the dense-reorder refresh); "ambiguous" authors a second addeweap
# row of the same child type on the parent (stored slot 2, missing anchor).
func _item_db(variant := "") -> ItemDatabase:
	var text := ItemDbFixture.fixture_text(self)
	if text.is_empty():
		return null
	# Native seat extraction walks the graphic's .3di userpoints; the Dune
	# Buggy row rides the committed tank model (one ctrlx control seat).
	assert_true(text.contains("  graphic Dbuggy1\n"))
	text = text.replace("  graphic Dbuggy1\n",
			"  graphic carrierswap\n" if variant == "refresh"
			else "  graphic tank\n")
	var rows := ItemDbFixture.SPAWN_ZONE_ROW + """
begin "Wire Header Designated-G Parent"
  id 105005
  type vehicle
  graphic tankgp
  sid wire_header_designated_g_parent
  hp 1000
  attrib: EWeap
  ai_function cveh
  render_function cveh
  move_function null
  primary_weapon WPN_EMPLCD50NA
  addeweapG ewep01 101419
"""
	if variant == "ambiguous":
		rows += "  addeweap missing 101419\n"
	rows += "end\n"
	return ItemDbFixture.load_with(self, "wire_header_world_items%s.def" % (
			"" if variant.is_empty() else "_" + variant), text, rows)


func _install_combat_tables(sim: Simulation, db: ItemDatabase) -> void:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	sim.resolve_item_traits(db)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_eq(sim.load_ammo_table(root, "ammo.def"), OK)


func _weapon(name: String) -> WeaponDef:
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var index := weapons.find_weapon(name)
	assert_gte(index, 0)
	return weapons.get_weapon(index) if index >= 0 else null


func _present_wire_handle_for_type(sim: Simulation, type_id: int) -> int:
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for row in range(snapshot.size() / stride):
		var base := row * stride
		if int(snapshot[base + Simulation.PF_TYPE_ID]) == type_id:
			return int(snapshot[base + Simulation.PF_WIRE_HANDLE])
	return -1


func _present_field_for_type(sim: Simulation, type_id: int, field: int) -> int:
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for row in range(snapshot.size() / stride):
		var base := row * stride
		if int(snapshot[base + Simulation.PF_TYPE_ID]) == type_id:
			return int(snapshot[base + field])
	return -1


func _present_position_for_type(sim: Simulation, type_id: int) -> Vector3:
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for row in range(snapshot.size() / stride):
		var base := row * stride
		if int(snapshot[base + Simulation.PF_TYPE_ID]) == type_id:
			return Vector3(
					snapshot[base + Simulation.PF_POS_X],
					snapshot[base + Simulation.PF_POS_Y],
					snapshot[base + Simulation.PF_POS_Z])
	return Vector3.INF


func _player_index(sim: Simulation) -> int:
	for index in range(sim.get_entity_count()):
		var card: EntityCard = sim.entity_card_by_ai_index(index)
		if card != null and card.get_item_id() == 0x14B9:
			return index
	return -1


func _configure_dedicated_host(host: Simulation) -> void:
	var host_options := HostSessionOptions.new()
	host_options.serve_and_play = false
	host_options.game_type = 0x30020
	host_options.mission_file = "WIRE_HEADER_PARITY.BMS"
	host.configure_host_session(host_options)


func test_true_wire_header_materializes_exact_deploy_and_vehicle_rows() -> void:
	var fixture := _mission_fixture()
	var mission: MissionData = fixture["mission"]
	var db := _item_db()
	assert_not_null(db)
	if db == null:
		return
	var root := _native_asset_root()

	var host := Simulation.new()
	_configure_dedicated_host(host)
	assert_true(host.enable_host_listen(0))
	host.set_asset_root(root)
	assert_true(host.install_seat_specs_for_type_ids(
			db, PackedInt32Array([VEHICLE_TYPE])))
	assert_gt(host.get_mounted_graphic_source_count(), 0,
			"the native install resolved the vehicle model source")
	assert_true(host.load_from_mission_data(mission))
	host.resolve_item_traits(db)
	var host_vehicle: EntityCard = host.entity_card_by_net_id(
			int(fixture["vehicle_bms_id"]))
	var host_zone: EntityCard = host.entity_card_by_net_id(
			int(fixture["zone_bms_id"]))
	assert_eq(host_vehicle.get_wire_handle(), 0x1000)
	assert_eq(host_zone.get_wire_handle(), 0x2000)

	var joiner := Simulation.new()
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
		return

	var wire_mission := MissionData.new()
	assert_eq(wire_mission.open_wire_header(header), OK)
	assert_true(wire_mission.is_wire_header_only())
	for kind in [MissionData.KIND_MARKER, MissionData.KIND_ITEM,
			MissionData.KIND_BUILDING, MissionData.KIND_ORGANIC]:
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
				and _present_wire_handle_for_type(joiner, VEHICLE_TYPE) == 0x1000 \
				and _present_wire_handle_for_type(joiner, ZONE_TYPE) == 0x2000 \
				and joiner.get_deploy_spawn_zones().size() == 1:
			streamed = true
			break
		OS.delay_msec(2)
	assert_true(streamed,
			"wire pools materialized before deploy/vehicle consumers run")
	if not streamed:
		return
	assert_eq(joiner.get_join_terrain_til_state(),
			Simulation.JOIN_TERRAIN_TIL_ABSENT,
			"a host with no terrain payload proves explicit 0x45 absence")
	assert_true(joiner.get_join_terrain_til().is_empty(),
			"Absent terrain never exposes a synthetic byte image")
	assert_eq(_present_wire_handle_for_type(joiner, VEHICLE_TYPE),
			host_vehicle.get_wire_handle())
	assert_eq(_present_wire_handle_for_type(joiner, ZONE_TYPE),
			host_zone.get_wire_handle())

	# Admission can finish while the authority still holds initial deployment.
	# Exercise the actual zone pick with the body-empty client and streamed rows.
	# [orig: Server_OnPlayerJoin @0x51A680; Input_HandleActionBinding @0x49AD40]
	var overlay := false
	for _tick in range(240):
		host.step()
		joiner.step()
		if joiner.is_join_deploy_overlay_active():
			overlay = true
			break
		OS.delay_msec(2)
	assert_true(overlay, "the streamed-world joiner receives its initial deployment hold")
	if not overlay:
		return
	assert_false(joiner.is_join_deploy_pick_pending(), "no selection has been sent yet")
	var zones := joiner.get_deploy_spawn_zones()
	var zone_param := (zones[0] as DeployZoneRow).param
	assert_true(joiner.send_deployment_pick(zone_param))
	var deployed := false
	for _tick in range(400):
		host.step()
		joiner.step()
		if not joiner.is_join_deploy_pick_pending() \
				and not joiner.is_join_deploy_overlay_active() \
				and absf(joiner.get_local_player_position().x - 12.0) < 1.0:
			deployed = true
			break
		OS.delay_msec(2)
	assert_true(deployed, "the streamed-zone pick releases and places the body-empty joiner")


func test_true_wire_header_recovers_designated_g_parent_ammo_route() -> void:
	# Retail's 0x0D addeweap row carries child type + parent handle, but no
	# authored slot identity or designated-G bit. The body-empty client must
	# derive that capability from the same parent seat-spec table the authority
	# used, then honor compact seat_type and phase-8 ammo in both directions.
	var fixture := _designated_g_mission_fixture()
	var mission: MissionData = fixture["mission"]
	var parent_bms_id := int(fixture["parent_bms_id"])
	var db := _item_db()
	assert_not_null(db)
	if db == null:
		return
	var root := _native_asset_root()

	var host := Simulation.new()
	_configure_dedicated_host(host)
	assert_true(host.enable_host_listen(0))
	host.set_asset_root(root)
	# The addeweapG row recurses the B50 child type into the same table, so
	# the parent seed installs both halves of the designated-G family.
	assert_true(host.install_seat_specs_for_type_ids(
			db, PackedInt32Array([DESIGNATED_G_PARENT_TYPE])))
	assert_gt(host.get_mounted_graphic_source_count(), 0,
			"the native install resolved the parent and child model sources")
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host, db)
	var host_parent: EntityCard = host.entity_card_by_net_id(parent_bms_id)
	assert_eq(host_parent.get_wire_handle(), 0x1000)

	var joiner := Simulation.new()
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
		return

	var wire_mission := MissionData.new()
	assert_eq(wire_mission.open_wire_header(header), OK)
	assert_true(wire_mission.is_wire_header_only())
	assert_true(joiner.load_from_mission_data(wire_mission))
	_install_combat_tables(joiner, db)

	var streamed := false
	for _tick in range(1000):
		host.step()
		joiner.step()
		if joiner.is_joined_in_match() \
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
		return

	# Model/seat resolution commonly completes after the stock 0x0D rows. First
	# install a deliberately ambiguous same-type sibling (a second authored
	# addeweap row of the same child type, stored slot 2, missing anchor). The
	# joiner cannot know which authored slot produced its child, so its exact
	# rigid 0x0D pose remains authoritative. The promoted host does know stored
	# slot 1 and must retain that identity across the same duplicate-table
	# refresh.
	var ambiguous_db := _item_db("ambiguous")
	assert_not_null(ambiguous_db)
	if ambiguous_db == null:
		return
	assert_true(host.install_seat_specs_for_type_ids(
			ambiguous_db, PackedInt32Array([DESIGNATED_G_PARENT_TYPE])))
	joiner.set_asset_root(root)
	assert_true(joiner.install_seat_specs_for_type_ids(
			ambiguous_db, PackedInt32Array([DESIGNATED_G_PARENT_TYPE])))
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
	assert_true(joiner.install_seat_specs_for_type_ids(
			db, PackedInt32Array([DESIGNATED_G_PARENT_TYPE])))

	# The zone pick is the DEATH flow now (the initial join deploys with no
	# C2S 0x0E — retail sends none): kill on the authority, wait out the
	# 3-second fresh-death pick penalty, then re-pick the streamed zone to
	# land beside the carrier. The lone joiner on this dedicated host is
	# pool-0 slot-0: wire handle 0 (get_joiner_self_handle's unbound sentinel
	# is indistinguishable here).
	assert_eq(host.debug_kill_player_entity(0), OK)
	var death_pick_pending := false
	for _tick in range(240):
		host.step()
		joiner.step()
		if joiner.is_join_deploy_pick_pending():
			death_pick_pending = true
			break
		OS.delay_msec(2)
	assert_true(death_pick_pending,
			"the death edge re-arms the deploy pick for the streamed zone")
	for _settle in range(260):
		host.step()
		joiner.step()
	var deploy_rows := joiner.get_deploy_spawn_zones()
	var zone_param := (deploy_rows[0] as DeployZoneRow).param
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
			[WeaponKitEntry.make("WPN_M4AUTO")], 8))
	joiner.set_local_player_weapon(personal, {})
	for _settle in range(80):
		joiner.step()
		host.step()
		NativeModelFixture.apply_weapon_switch_events(joiner, weapon_defs)
		if joiner.get_local_player_weapon_state().current_action < 2:
			break
		OS.delay_msec(1)
	assert_lt(joiner.get_local_player_weapon_state().current_action, 2,
			"personal weapon settled before the mount action")
	assert_eq(_present_field_for_type(
			joiner, DESIGNATED_G_CHILD_TYPE, Simulation.PF_ALIVE), 1)
	assert_lt(_present_position_for_type(
			joiner, DESIGNATED_G_CHILD_TYPE).distance_to(
					joiner.get_local_player_position()), 4.0,
			"the decoded child remains inside retail's seat-scan radius")
	var attach_hud := HudOverlay.new()
	autofree(attach_hud)
	attach_hud.set_attach_labels(
			Transform3D(Basis.looking_at(Vector3.RIGHT), Vector3(-2.0, 0.0, 0.0)),
			Projection.create_perspective(70.0, 1.0, 0.05, 4000.0), null, joiner)
	assert_eq(attach_hud.get_attach_label_count(), 1,
			"the decoded child contributes one in-range UseGun label")
	assert_eq(attach_hud.get_attach_label_selected(), 0,
			"the single in-range candidate is the full-bright nearest label")
	assert_true(joiner.local_player_toggle_mount(),
			"the decoded child exposes its authored UseGun seat")
	var host_player_index := _player_index(host)
	assert_gte(host_player_index, 0)
	var mounted_echoed := false
	for _tick in range(240):
		joiner.step()
		host.step()
		NativeModelFixture.apply_weapon_switch_events(joiner, weapon_defs)
		if joiner.get_local_player_view().mounted \
				and host_player_index >= 0 \
				and host.entity_card_by_ai_index(host_player_index).is_mounted():
			mounted_echoed = true
			break
		OS.delay_msec(2)
	assert_true(mounted_echoed,
			"authority echoes the exact decoded child carrier")
	if not mounted_echoed:
		return

	for _settle in range(100):
		joiner.step()
		host.step()
		NativeModelFixture.apply_weapon_switch_events(joiner, weapon_defs)
		if joiner.get_local_player_weapon_state().current_action < 2:
			break
		OS.delay_msec(1)

	# The first-person UseGun parent cull is the LOCAL render verdict and is
	# role-blind in retail: with the embedded MountSlot live as EquippedSlot and
	# the FP model resolved, the mounted gun's own world model is suppressed so
	# the world pass and the FP pass never draw the gun twice
	# [orig: Entity_RenderVehicleModel @0x4407d0 predicate @0x4407f6..0x44084c].
	# The joiner path used to skip this entirely (host-only `ent` enrichment) —
	# the live "two 50cal models while mounted" symptom.
	assert_eq(_present_field_for_type(joiner, DESIGNATED_G_CHILD_TYPE,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"without a resolved FP model the world gun still renders")
	joiner.set_local_player_first_person_model_available(true)
	assert_eq(_present_field_for_type(joiner, DESIGNATED_G_CHILD_TYPE,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 1,
			"the mounted joiner suppresses the gun's duplicate world model")
	joiner.set_local_player_debug_third_person(true)
	assert_eq(_present_field_for_type(joiner, DESIGNATED_G_CHILD_TYPE,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"third person restores the world gun")
	joiner.set_local_player_debug_third_person(false)
	assert_eq(_present_field_for_type(joiner, DESIGNATED_G_CHILD_TYPE,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 1)

	var child_ammo := joiner.get_local_player_weapon_state()
	var child_clip := child_ammo.clip
	var child_reserve := child_ammo.reserve
	assert_true(child_clip != 7 or child_reserve != 19,
			"the direct child slot differs from the parent witness")

	assert_true(joiner.request_local_player_scope_toggle(),
			"header-only child recovered designated-G action-6 capability")
	# Let the authority consume action 6 and prepare the parent slot before
	# stamping the non-default phase-8 witness; first-time preparation seeds the
	# weapon.def defaults and would intentionally replace an earlier debug value.
	joiner.step()
	host.step()
	NativeModelFixture.apply_weapon_switch_events(joiner, weapon_defs)
	assert_eq(host.debug_set_world_entity_weapon_ammo(
			parent_bms_id, 7, 19), OK)
	var parent_ammo_applied := false
	# Phase 8 is every sixteenth emitted network frame, not every simulation
	# step. Allow one complete low-rate network cycle after the route echo.
	for _tick in range(640):
		joiner.step()
		host.step()
		NativeModelFixture.apply_weapon_switch_events(joiner, weapon_defs)
		var active := joiner.get_local_player_weapon_state()
		if active.clip == 7 \
				and active.reserve == 19:
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
		NativeModelFixture.apply_weapon_switch_events(joiner, weapon_defs)
		var active := joiner.get_local_player_weapon_state()
		if active.clip == child_clip \
				and active.reserve == child_reserve:
			child_route_restored = true
			break
		OS.delay_msec(1)
	assert_true(child_route_restored,
			"returning to the child recovers its untouched ammo words")
	var host_parent_after: EntityCard = host.entity_card_by_net_id(parent_bms_id)
	assert_eq(host_parent_after.get_primary_weapon_clip(), 7)
	assert_eq(host_parent_after.get_primary_weapon_reserve(), 19)


func test_complete_bms_joiner_keeps_authored_promotion_identity() -> void:
	var fixture := _mission_fixture()
	var mission: MissionData = fixture["mission"]
	assert_false(mission.is_wire_header_only())
	var db := _item_db()
	assert_not_null(db)
	if db == null:
		return

	var host := Simulation.new()
	_configure_dedicated_host(host)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	host.resolve_item_traits(db)
	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "FullBmsJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	joiner.resolve_item_traits(db)

	var before: EntityCard = joiner.entity_card_by_net_id(
			int(fixture["vehicle_bms_id"]))
	assert_eq(before.get_wire_handle(), 0x1000)
	assert_eq(before.get_kind(), MissionData.KIND_ITEM)
	assert_eq(before.get_source_index(), 0)
	assert_eq(before.get_bms_id(), int(fixture["vehicle_bms_id"]))
	for _tick in range(800):
		host.step()
		joiner.step()
		if joiner.is_joined_in_match() \
				and _present_wire_handle_for_type(joiner, VEHICLE_TYPE) == 0x1000:
			break
		OS.delay_msec(2)
	var after: EntityCard = joiner.entity_card_by_net_id(
			int(fixture["vehicle_bms_id"]))
	assert_eq(after.get_wire_handle(), 0x1000)
	assert_eq(after.get_kind(), MissionData.KIND_ITEM,
			"full-BMS joiners retain authored spawn_origin semantics")
	assert_eq(after.get_source_index(), 0)
	assert_eq(after.get_bms_id(), int(fixture["vehicle_bms_id"]))
