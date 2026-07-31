extends GutTest

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const ItemSeatSpecs := preload("res://engine/world/item_seat_specs.gd")
const NovaObjectModelScript := preload("res://engine/object/nova_object_model.gd")
const PresentAimOverlay := preload("res://engine/world/aim_overlay_present_pass.gd")
const NATIVE_RUNTIME_TIMING_KEYS := [
	"sim_tick_us",
	"net_tick_us",
	"present_snapshot_us",
	"occlusion_build_us",
	"occlusion_probe_us",
]

# NovaSimulation (the GDExtension binding): promote a synthetic BMS mission into a live
# world + AI system, tick it, and confirm the AI walks entities along their authored route.
# This is the in-Godot end of step 1 (promotion) + step 2 (locomotion).


func _model_with_special1_in_local_slot_zero(
		source_res_path: String) -> NovaObjectData:
	var result := NovaObjectData.new()
	assert_eq(result.open_file(
			ProjectSettings.globalize_path(source_res_path)), OK)
	assert_true(result.set_control_register_name(0, "VEHICLE_SPECIAL1"),
			"author the semantic local CTRL name through NovaObjectData")
	return result


func _fast_rope_item_db() -> NovaItemDatabase:
	var path := ProjectSettings.globalize_path(
			"res://.godot/ctrl_fast_rope_items.def")
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	if file == null:
		return null
	file.store_string("""begin "Fast Rope Control Fixture"
  id 105006
  type object
  graphic StaticCrate1
  sid fastropectrl
  hp 50
  attrib: FastRope
end
""")
	file.close()
	var result := NovaItemDatabase.new()
	assert_eq(result.load(path), OK)
	assert_eq(int(result.get_attrib(105006)) & 0x1000, 0x1000,
			"fixture carries retail's FastRope item attribute")
	return result


func _vehicle_ctrl_item_db() -> NovaItemDatabase:
	var path := ProjectSettings.globalize_path(
			"res://.godot/ctrl_vehicle_items.def")
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	if file == null:
		return null
	file.store_string("""begin "CTRL Vehicle Fixture"
  id 105007
  type vehicle
  graphic StaticCrate1
  sid ctrlvehicle
  ai_function cveh
  render_function cveh
  move_function cveh
  attrib: AIData neutral PlayerControl
  hp 3000
  turn_rate 65
  turn_rate2 41
  acceleration 15
  deceleration 70
  player_speed 94
  physics 1
  torque 3
end
""")
	file.close()
	var result := NovaItemDatabase.new()
	assert_eq(result.load(path), OK)
	assert_eq(int(result.get_vehicle_physics(105007)[0]), 1)
	return result


func test_host_projectile_options_roundtrip() -> void:
	var sim := NovaSimulation.new()
	sim.configure_host_session({"fat_bullets": true, "one_shot_kill": true})
	var options: Dictionary = sim.get_host_session_config()
	assert_true(bool(options.get("fat_bullets", false)))
	assert_true(bool(options.get("one_shot_kill", false)))
	sim.free()

func test_demo_mission_promotes() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	assert_true(sim.is_loaded(), "demo mission promoted")
	assert_eq(sim.get_entity_count(), 2, "two organics got AI brains")
	assert_eq(sim.get_brain_count(), 2, "two AI brains attached")
	assert_eq(sim.get_spawned_count(), 6, "one building + three markers + two organics spawned into pools")
	assert_eq(sim.get_entity_state(0), 16, "a routed organic starts in GROUND_FOLLOWWP (16)")
	sim.free()


func test_runtime_profiling_is_opt_in_reset_stable_and_behavior_neutral() -> void:
	var sim := NovaSimulation.new()
	assert_false(sim.is_runtime_profiling_enabled(),
			"retail/default play does not own the profiling clocks")
	sim.build_demo_mission()
	sim.occlusion_init_mission()
	assert_true(sim.step())
	sim.run_occlusion_frame(
			Transform3D.IDENTITY, 90.0, 1.0, 0.05, 500.0, -100.0, false)
	var unprofiled_snapshot: PackedFloat32Array = sim.get_present_snapshot()
	var unprofiled_buildings: PackedInt64Array = sim.get_building_visibility()
	var unprofiled_culled: PackedInt32Array = sim.get_render_culled_bms_ids()
	var unprofiled_positions: Array[Vector3] = []
	for i in range(sim.get_entity_count()):
		unprofiled_positions.append(sim.get_entity_position(i))
	var counters: Dictionary = sim.get_runtime_perf_counters()
	_assert_native_runtime_timings_zero(counters)
	assert_false(bool(counters.get("runtime_profiling_enabled", true)))
	assert_false(bool(counters.get("trace_profiling_enabled", true)))

	sim.set_runtime_profiling_enabled(true)
	assert_true(sim.is_runtime_profiling_enabled())
	sim.run_occlusion_frame(
			Transform3D.IDENTITY, 90.0, 1.0, 0.05, 500.0, -100.0, false)
	assert_eq(sim.get_building_visibility(), unprofiled_buildings,
			"profiling does not change building submission")
	assert_eq(sim.get_render_culled_bms_ids(), unprofiled_culled,
			"profiling does not change entity render gates")
	assert_eq(sim.get_present_snapshot(), unprofiled_snapshot,
			"profiling does not change the client-view snapshot")
	for i in range(unprofiled_positions.size()):
		assert_eq(sim.get_entity_position(i), unprofiled_positions[i],
				"turning profiling on does not mutate simulation state")
	assert_true(sim.step())
	counters = sim.get_runtime_perf_counters()
	assert_true(bool(counters.get("runtime_profiling_enabled", false)))
	assert_true(bool(counters.get("trace_profiling_enabled", false)))
	var sampled_us := 0
	for key in NATIVE_RUNTIME_TIMING_KEYS:
		sampled_us += int(counters.get(key, 0))
	assert_gt(sampled_us, 0,
			"the enabled gate records at least one native runtime span")

	# Mission reload rebuilds CollisionWorld, so this pins reapplication of the
	# one profiling request as well as the cleared timing snapshot.
	sim.build_demo_mission()
	assert_true(sim.is_runtime_profiling_enabled(),
			"a mission reset preserves the active consumer request")
	counters = sim.get_runtime_perf_counters()
	_assert_native_runtime_timings_zero(counters)
	assert_true(bool(counters.get("trace_profiling_enabled", false)),
			"the rebuilt CollisionWorld inherits the unified gate")

	sim.set_runtime_profiling_enabled(false)
	assert_false(sim.is_runtime_profiling_enabled())
	_assert_native_runtime_timings_zero(sim.get_runtime_perf_counters())
	sim.occlusion_init_mission()
	sim.run_occlusion_frame(
			Transform3D.IDENTITY, 90.0, 1.0, 0.05, 500.0, -100.0, false)
	assert_true(sim.step())
	sim.get_present_snapshot()
	counters = sim.get_runtime_perf_counters()
	_assert_native_runtime_timings_zero(counters)
	assert_false(bool(counters.get("trace_profiling_enabled", true)))
	sim.free()


func _assert_native_runtime_timings_zero(counters: Dictionary) -> void:
	for key in NATIVE_RUNTIME_TIMING_KEYS:
		assert_eq(int(counters.get(key, -1)), 0,
				"%s stays zero while native profiling is closed" % key)


func test_binocular_and_nvg_requests_drive_effective_view_state() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))

	assert_true(sim.request_local_player_binoculars_toggle())
	var view: Dictionary = sim.get_local_player_view()
	assert_true(bool(view.get("binoculars_requested", false)))
	assert_true(bool(view.get("binoculars_raised", false)))
	assert_true(bool(view.get("binoculars_view_active", false)))
	assert_almost_eq(float(view.get("fov_h_deg", 0.0)), 20.0, 0.001)
	var jitter := Vector2(
			float(view.get("binocular_yaw_offset_deg", 0.0)),
			float(view.get("binocular_pitch_offset_deg", 0.0)))
	assert_almost_eq(jitter.length(), 2.8125, 0.0001,
			"the toggle seeds the fixed 0x02000000-BAM displacement")

	sim.set_player_input(true, false, false, false, false, false, false)
	view = sim.get_local_player_view()
	assert_true(bool(view.get("binoculars_requested", false)),
			"movement suppresses rather than destroys raw intent")
	assert_false(bool(view.get("binoculars_raised", true)))
	assert_false(bool(view.get("binoculars_view_active", true)))
	sim.set_player_input(false, false, false, false, false, false, false)
	sim.set_local_player_camera_third_person(true)
	view = sim.get_local_player_view()
	assert_true(bool(view.get("binoculars_raised", false)),
			"third person retains the remote-visible body pose")
	assert_false(bool(view.get("binoculars_view_active", true)))
	sim.set_local_player_camera_third_person(false)
	sim.request_local_player_binoculars_toggle()
	assert_false(bool(sim.get_local_player_view().get("binoculars_requested", true)))

	assert_eq(sim.request_local_player_nvg_gain(99), 4)
	assert_eq(int(sim.get_local_player_view().get("nvg_gain", -1)), 4,
			"gain is adjustable while NVG is inactive")
	assert_true(sim.request_local_player_nvg_toggle())
	assert_true(bool(sim.get_local_player_view().get("nvg_visible", false)))
	sim.set_local_player_camera_third_person(true)
	view = sim.get_local_player_view()
	assert_true(bool(view.get("nvg_active", false)))
	assert_false(bool(view.get("nvg_visible", true)),
			"third person suppresses treatment without clearing NVG")
	sim.free()


func test_start_with_nvg_reseeds_on_player_init() -> void:
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_true(mission.set_header_flag(
			NovaMissionData.ATTRIB_START_WITH_NVG_ON, true))
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(mission))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var view: Dictionary = sim.get_local_player_view()
	assert_true(bool(view.get("nvg_active", false)))
	assert_eq(int(view.get("nvg_gain", -1)), 0)

	assert_false(sim.request_local_player_nvg_toggle())
	assert_eq(sim.request_local_player_nvg_gain(3), 3)
	sim.respawn_local_player_loadout()
	view = sim.get_local_player_view()
	assert_true(bool(view.get("nvg_active", false)),
			"Player_InitPlayer reseeds the mission's StartWithNVGOn bit")
	assert_eq(int(view.get("nvg_gain", -1)), 0)
	sim.free()


func test_nvg_inset_scope_drop_refusal_and_restore_latch() -> void:
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(mission))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var weapon := {
		"name": "WPN_INSET_NVG_TEST",
		"actions": [{"name": "idle", "delaystart": 0, "delayend": 0}],
		"flags": 0x1,
		"flags2": 0x200,
		"clipsize": 30,
		"startrounds": 60,
	}
	sim.set_local_player_weapon(weapon, {})
	sim.step()
	assert_true(sim.request_local_player_scope_toggle())
	for _i in range(7):
		sim.step()
	assert_true(bool(sim.get_local_player_view().get("scope_engaged", false)))

	assert_true(sim.request_local_player_nvg_toggle())
	assert_false(bool(sim.get_local_player_view().get("scope_engaged", true)),
			"enabling NVG drops a settled Inset scope")
	for _i in range(7):
		sim.step()
	assert_false(sim.request_local_player_scope_toggle(),
			"Inset scope-up is refused while NVG remains active")
	sim.rebake_local_player_weapon(weapon, {})

	assert_false(sim.request_local_player_nvg_toggle())
	assert_true(bool(sim.get_local_player_view().get("scope_engaged", false)),
			"a render-only same-weapon rebake preserves the scope restore latch")

	assert_true(sim.request_local_player_nvg_toggle())
	sim.set_local_player_weapon(weapon, {})
	assert_false(sim.request_local_player_nvg_toggle())
	assert_false(bool(sim.get_local_player_view().get("scope_engaged", true)),
			"a real weapon mount invalidates the stale scope restore latch")
	sim.free()


# The HUD waypoint track: the demo mission's BLUE route becomes the player track;
# a spawned local player latches waypoint 0 on the first tick and walking into the
# radius advances. [orig chain: NetPacket_WriteWorldStateLoad0x0F @0x502e41 list ->
# Player_UpdatePerFrame @0x4de5f7 advance; docs/interface/hud-re.md §Waypoint HUD]
func test_waypoint_hud_view_tracks_the_demo_route() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	var wp: Dictionary = sim.get_waypoint_hud_view()
	assert_true(bool(wp.get("show", false)), "waypoints visible by default")
	assert_eq(int(wp.get("count", 0)), 3, "the blue route's three markers")
	assert_eq(int(wp.get("current", 0)), -1, "no selection before a player tick")

	# Spawn the local player far from marker 0 (demo marker 0 = mission (100,0,0)
	# = Godot (100, 0, 0); radius 25). The first tick latches entry 0.
	assert_true(sim.spawn_local_player(Vector3(0, 0, 0), 0.0, 1))
	sim.step()
	wp = sim.get_waypoint_hud_view()
	assert_eq(int(wp.get("current", -1)), 0, "first tick latches waypoint 0")
	assert_eq(int(wp.get("number", 0)), 1, "1-based display number")
	assert_eq(int(wp.get("name_id", -1)), 1, "marker 0's authored name id")
	var pos: Vector3 = wp.get("position", Vector3.ZERO)
	assert_almost_eq(pos.x, 100.0, 0.01, "marker 0 world X")

	# Teleport inside the 25 u radius (mission space; the player is AI index 2,
	# after the demo's two organics): the next tick advances to waypoint 1.
	sim.debug_set_entity_position(2, Vector3(95, 0, 0))
	sim.step()
	wp = sim.get_waypoint_hud_view()
	assert_eq(int(wp.get("current", -1)), 1, "proximity advance onto waypoint 1")
	sim.free()

const ANIM_FIXTURES := "res://../fixtures/anim"


class ObjectDataPlacerStub:
	extends RefCounted
	var data: NovaObjectData

	func _init(p_data: NovaObjectData) -> void:
		data = p_data

	func object_data_for(_graphic: String) -> NovaObjectData:
		return data


class GraphicDataPlacerStub:
	extends RefCounted
	var data_by_graphic: Dictionary

	func _init(p_data_by_graphic: Dictionary) -> void:
		data_by_graphic = p_data_by_graphic

	func object_data_for(graphic: String) -> NovaObjectData:
		return data_by_graphic.get(graphic) as NovaObjectData


class SkeletalDataPlacerStub:
	extends RefCounted
	var data: NovaObjectData
	var skeletal: NovaSkeletalAnim

	func _init(p_data: NovaObjectData, p_skeletal: NovaSkeletalAnim) -> void:
		data = p_data
		skeletal = p_skeletal

	func object_data_for(_graphic: String) -> NovaObjectData:
		return data

	func skeletal_anim_for(
			_item_id: int, _graphic: String) -> NovaSkeletalAnim:
		return skeletal


class PlayerOnlySkeletalPlacerStub:
	extends RefCounted
	var data: NovaObjectData
	var skeletal: NovaSkeletalAnim

	func _init(p_data: NovaObjectData, p_skeletal: NovaSkeletalAnim) -> void:
		data = p_data
		skeletal = p_skeletal

	func object_data_for(graphic: String) -> NovaObjectData:
		return data if graphic.nocasecmp_to("US01") == 0 else null

	func skeletal_anim_for(
			_item_id: int, graphic: String) -> NovaSkeletalAnim:
		return skeletal if graphic.nocasecmp_to("US01") == 0 else null

func _anim_root() -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path(ANIM_FIXTURES))
	return root


func _minimal_weapon(name: String, animadm: String) -> Dictionary:
	return {
		"name": name,
		"animadm": animadm,
		"actions": [],
		"flags": 0,
		"clipsize": 0,
		"startrounds": 0,
	}


func _weapon_arm_pitch_deg(sim: NovaSimulation) -> float:
	var overlay: Dictionary = sim.get_local_player_aim_overlay()
	var angles: PackedVector3Array = overlay.get("angles", PackedVector3Array())
	return float(angles[4].x) if angles.size() > 4 else 0.0


func _aim_verdict_sim(flags: int) -> NovaSimulation:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	sim.set_local_player_weapon({
		"name": "WPN_AIM_VERDICT",
		"actions": [
			{"name": "idle", "delaystart": 0, "delayend": 0},
			{"name": "fire", "delaystart": 0, "delayend": 0},
			{"name": "recoil", "delaystart": 0, "delayend": 0},
			{"name": "reload", "delaystart": 8, "delayend": 8},
			{"name": "scopeup", "delaystart": 0, "delayend": 0},
			{"name": "scopedown", "delaystart": 0, "delayend": 0},
		],
		"flags": flags,
		"clipsize": 30,
		"startrounds": 60,
	}, {})
	return sim


func _aimed_shot_available(sim: NovaSimulation) -> bool:
	return bool(sim.get_local_player_weapon_state().get(
			"aimed_shot_available", false))


func _present_field_for_origin(sim: NovaSimulation, kind: int, index: int,
		field: int) -> int:
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + NovaSimulation.PF_KIND]) == kind \
				and int(snapshot[base + NovaSimulation.PF_INDEX]) == index:
			return int(snapshot[base + field])
	return -1


func _present_phase_for_origin(sim: NovaSimulation, kind: int, index: int,
		channel: int) -> int:
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + NovaSimulation.PF_KIND]) == kind \
				and int(snapshot[base + NovaSimulation.PF_INDEX]) == index:
			return NovaSimulation.decode_present_part_anim_phase(
					snapshot, base, channel)
	return 0


func test_authoritative_cveh_snapshot_publishes_vehicle_motion_controls() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			NovaMissionData.KIND_ITEM, 105007,
			Vector3(2, 0, 0), Vector3.ZERO)
	assert_false(placed.is_empty())
	assert_true(md.set_entity_property_int(
			NovaMissionData.KIND_ITEM, int(placed["index"]), "team", 2))
	var item_db := _vehicle_ctrl_item_db()
	assert_not_null(item_db)
	if item_db == null:
		return

	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	sim.set_item_seat_specs([{
		"type_id": 5007,
		"seats": [{
			"type": 2,
			"position": Vector3.ZERO,
			"source_name": "ctrlx00",
		}],
	}])
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	sim.step()
	var index := int(placed["index"])
	assert_eq(_present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, index,
			NovaSimulation.PF_VEHICLE_MOTION_VALID), 1,
			"a resolved authority cveh row owns both registers at rest")
	assert_eq(_present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, index,
			NovaSimulation.PF_VEHICLE_STEERING), 0)
	assert_eq(_present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, index,
			NovaSimulation.PF_VEHICLE_SPEED), 0)
	assert_eq(_present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, index,
			NovaSimulation.PF_TEX_TEAM_VALID), 1,
			"a pool-1 sector-model row executes the retail TEX_TEAM writer")
	assert_eq(_present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, index,
			NovaSimulation.PF_TEX_TEAM), 2)
	assert_eq(_present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, index,
			NovaSimulation.PF_ZONE_CTRL_VALID), 0,
			"a non-zone sector model does not synthesize the generic-zone writers")

	assert_true(sim.local_player_toggle_mount())
	sim.set_player_input(true, false, true, false, false, false, false)
	for _tick in range(40):
		sim.step()
	var steering := _present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, index,
			NovaSimulation.PF_VEHICLE_STEERING)
	var speed := _present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, index,
			NovaSimulation.PF_VEHICLE_SPEED)
	assert_ne(steering, 0,
			"the snapshot carries the zero-extended live steer high word")
	assert_gt(speed, 0,
			"the snapshot carries the live currentSpeed magnitude")
	assert_lte(speed, 0x10000,
			"the retail speed publisher saturates at its fixed-point endpoint")
	sim.free()


func _mounted_npc_right_hand_verdict(seat_type: int) -> int:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var mount := md.add_entity(
			NovaMissionData.KIND_ITEM, 101294,
			Vector3(0, 8, 0), Vector3.ZERO)
	var npc := md.add_entity(
			NovaMissionData.KIND_ORGANIC, 105311,
			Vector3(0, 8, 0), Vector3.ZERO)
	assert_false(mount.is_empty())
	assert_false(npc.is_empty())
	assert_true(md.set_entity_property_int(
			NovaMissionData.KIND_ORGANIC, int(npc["index"]),
			"waypoint_id", 125))
	assert_true(md.set_entity_property_int(
			NovaMissionData.KIND_ORGANIC, int(npc["index"]),
			"wp_number", int(mount["bms_id"])))
	var source_names := {
		1: "sitex00",
		2: "ctrlx00",
		3: "UseGun",
		5: "drvrx00",
	}
	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	sim.set_item_seat_specs([{
		"type_id": 1294,
		"mount_config_valid": true,
		"mount_config": 6,
		"seats": [{
			"type": seat_type,
			"bone_index": 6,
			"position": Vector3.ZERO,
			"source_name": String(source_names.get(seat_type, "")),
		}],
	}])
	assert_true(sim.load_from_mission_data(md))
	sim.step()
	var verdict := _present_field_for_origin(
			sim, NovaMissionData.KIND_ORGANIC, int(npc["index"]),
			NovaSimulation.PF_RIGHT_HAND_COLLAPSED)
	sim.free()
	return verdict


func test_mounted_npc_right_hand_predicate_excludes_passengers() -> void:
	# Retail parentSlot codes: sitex=1 keeps the hand/weapon, while ctrlx=2,
	# UseGun=3, and drvrx=5 zero BN17 for non-player organics.
	for row in [
		{"seat": 1, "collapsed": 0, "name": "Passenger"},
		{"seat": 2, "collapsed": 1, "name": "Controller"},
		{"seat": 3, "collapsed": 1, "name": "Gunner"},
		{"seat": 5, "collapsed": 1, "name": "Driver"},
	]:
		assert_eq(_mounted_npc_right_hand_verdict(int(row["seat"])),
				int(row["collapsed"]), "%s seat predicate" % row["name"])


func test_aim_overlay_exports_the_retail_authored_pitch_sign() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))

	sim.set_local_player_mouse(511, false)
	sim.add_local_player_look(0.0, 100.0)
	sim.step()
	var authored_pitch := sim.get_local_player_pitch_deg()
	var arm_pitch := _weapon_arm_pitch_deg(sim)
	assert_gt(absf(authored_pitch), 0.5, "look input produced a signed pitch witness")
	assert_almost_eq(arm_pitch, authored_pitch, 0.01,
		"overlay pitch stays in authored sign for MissionObjectPlacer")
	sim.free()


func test_hud_spread_row_tracks_stance_and_settled_aim_state() -> void:
	# HUD ERROR is selected from two stance triplets. Air/water/mount overrides
	# live in the body; this public seam pins the ordinary stance order and the
	# settled-first-person +3 verdict. [orig: HUD_DrawCrosshair
	# @0x592b35..0x592b87; Player_CanFireWeapon @0x5cf780]
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	# A compact deterministic FSM is enough for the view toggle; the entity's
	# equipped ADM index still resolves the exact M4 ERROR table loaded above.
	sim.set_local_player_weapon({
		"name": "WPN_M4AUTO",
		"actions": [
			{"name": "idle", "delaystart": 0, "delayend": 0},
			{"name": "scopeup", "delaystart": 0, "delayend": 0},
			{"name": "scopedown", "delaystart": 0, "delayend": 0},
		],
		"flags": 0x1,
		"clipsize": 30,
		"startrounds": 300,
	}, {})
	sim.step()
	assert_eq(int(sim.get_local_player_weapon_state().get("hud_spread_row", -1)), 2,
			"standing selects row 2")

	assert_true(sim.request_local_player_stance(2))
	sim.step()
	assert_eq(int(sim.get_local_player_weapon_state().get("hud_spread_row", -1)), 0,
			"prone selects row 0")

	sim.set_water_z(1.0)
	sim.step()
	assert_eq(int(sim.get_local_player_weapon_state().get("hud_spread_row", -1)), 2,
			"below-water source height forces the standing row")
	sim.set_water_z(0.0)
	sim.step()
	assert_eq(int(sim.get_local_player_weapon_state().get("hud_spread_row", -1)), 0,
			"leaving water restores the authored prone row")

	assert_true(sim.request_local_player_scope_toggle())
	var aimed_row_seen := false
	for _i in range(15):
		sim.step()
		assert_false(_aimed_shot_available(sim),
				"Scoped ADS never promotes before the ease endpoint")
	for _i in range(9):
		sim.step()
		if int(sim.get_local_player_weapon_state().get("hud_spread_row", -1)) == 3:
			aimed_row_seen = true
			break
	assert_true(aimed_row_seen,
			"settled first-person aim adds the second-triplet offset")

	sim.set_local_player_camera_third_person(true)
	sim.step()
	assert_eq(int(sim.get_local_player_weapon_state().get("hud_spread_row", -1)), 0,
			"third person clears aimed-shot availability without changing stance")
	sim.free()


func test_aimed_shot_verdict_uses_both_promoted_optic_predicates() -> void:
	# Player_CanFireWeapon calls both helpers: Scoped is Flags bit 0, while the
	# misleadingly named second helper is simply Sighted bit 1 outside SWITCHFROM.
	# Both read the same post-ease promoted active bit. [orig: @0x4dcc80/@0x4dcd30]
	for case in [
		{"flags": 0x1, "expected": true, "name": "Scoped"},
		{"flags": 0x2, "expected": true, "name": "Sighted-only"},
	]:
		var sim := _aim_verdict_sim(int(case["flags"]))
		sim.step()
		assert_true(sim.request_local_player_scope_toggle())
		for _i in range(15):
			sim.step()
			assert_false(_aimed_shot_available(sim),
					"%s never promotes during the ease" % case["name"])
		# The current host order ticks the view after the body; the next body tick
		# observes the now-promoted endpoint. It may be one tick late, never early.
		sim.step()
		assert_eq(_aimed_shot_available(sim), bool(case["expected"]),
				"%s uses the correct aimed-shot predicate" % case["name"])
		if int(case["flags"]) == 0x2:
			sim.set_water_z(1.0)
			sim.set_player_input(true, false, false, false, false, false, false)
			sim.step()
			assert_true(_aimed_shot_available(sim),
					"promoted Sighted bypasses the movement/water checks")
		sim.free()


func test_ordinary_aimed_shot_rejects_water_and_movement() -> void:
	var sim := _aim_verdict_sim(0x1)
	sim.step()
	assert_true(sim.request_local_player_scope_toggle())
	for _i in range(16):
		sim.step()
	assert_true(_aimed_shot_available(sim), "settled stationary Scoped view is aimed")

	sim.set_water_z(1.0)
	sim.step()
	assert_false(_aimed_shot_available(sim),
			"Drowning/raw fixed source height rejects ordinary aimed fire")
	sim.set_water_z(0.0)
	sim.step()
	assert_true(_aimed_shot_available(sim), "leaving water restores aimed fire")

	sim.free()

	# The normal Scoped move path also requests an unscope, but its public result
	# pins Player_CanFireWeapon's MoveOrder&8 rejection end-to-end.
	sim = _aim_verdict_sim(0x1)
	sim.step()
	assert_true(sim.request_local_player_scope_toggle())
	for _i in range(16):
		sim.step()
	assert_true(_aimed_shot_available(sim))
	sim.set_player_input(true, false, false, false, false, false, false)
	sim.step()
	assert_false(_aimed_shot_available(sim),
			"MoveOrder moving rejects an ordinary aimed shot")
	sim.free()


func test_forcescoped_overrides_ordinary_gates_but_not_card_switch_reload() -> void:
	# ForceScoped overwrites the ordinary scope/movement/air/water verdict in
	# first person. The reload test sits earlier in Player_CanFireWeapon and is
	# therefore still terminal. [orig: @0x5cf7c7 and @0x5cf845..0x5cf874]
	var sim := _aim_verdict_sim(0x20000001)
	sim.step()
	assert_true(_aimed_shot_available(sim),
			"ForceScoped is aimed even without an ordinary promoted scope")
	sim.set_water_z(1.0)
	sim.set_player_input(true, false, false, false, false, false, false)
	sim.step()
	assert_true(_aimed_shot_available(sim),
			"ForceScoped preserves its movement/water override")
	sim.set_water_z(0.0)
	sim.set_player_input(false, false, false, false, false, false, false)

	# Spend one round so the public reload input gate can enter action 4.
	sim.set_local_player_weapon_input(false, true, false)
	var spent_round := false
	for _i in range(12):
		sim.step()
		var state: Dictionary = sim.get_local_player_weapon_state()
		if int(state.get("clip", 30)) == 29 and int(state.get("current", -1)) == 0:
			spent_round = true
			break
	assert_true(spent_round, "fixture reached idle with a partial magazine")
	sim.set_local_player_weapon_input(false, false, true)
	sim.step()
	sim.set_local_player_weapon_input(false, false, false)
	var reload_seen := false
	for _i in range(12):
		sim.step()
		if int(sim.get_local_player_weapon_state().get("current", -1)) == 4:
			reload_seen = true
			sim.step() # aimed verdict samples the already-current reload action
			break
	assert_true(reload_seen, "fixture entered the card-switch reload action")
	assert_false(_aimed_shot_available(sim),
			"ForceScoped cannot bypass the earlier card-switch reload rejection")
	sim.free()


func test_decoded_round_stance_uses_retail_animation_flags() -> void:
	# The decoded-round bridge consumes these 0x100/0x200 bits directly. Pin the
	# transition rows that the former hand-maintained list missed, plus
	# idle_mortar, which it incorrectly called crouched. Live IDA table reads:
	# 46=0x008, 169..171=0x18D, 172=0x28D.
	# [orig: g_animStateFlagsTable @0x8139E8]
	for row in [
		{"anim": 46, "flags": 0x008, "category": 2},
		{"anim": 169, "flags": 0x18D, "category": 1},
		{"anim": 170, "flags": 0x18D, "category": 1},
		{"anim": 171, "flags": 0x18D, "category": 1},
		{"anim": 172, "flags": 0x28D, "category": 0},
	]:
		var flags := int(NovaSimulation.infantry_anim_flags(int(row["anim"])))
		assert_eq(flags, int(row["flags"]), "retail flags for anim %d" % row["anim"])
		var category := 0 if (flags & 0x200) != 0 else (1 if (flags & 0x100) != 0 else 2)
		assert_eq(category, int(row["category"]),
				"decoded recoil category for anim %d" % row["anim"])


func test_local_fire_exports_recoil_camera_and_hud_spread() -> void:
	# Keep the world's built-in player ItemDef traits intact: the compact items.def
	# fixture intentionally lacks retail's player template 105305, while recoil's
	# source gate requires a person with an ItemDef. [orig: RoundData_SpawnRound
	# @0x4ec0d0; recoil add @0x4ec8a3]
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_eq(sim.load_ammo_table(root, "ammo.def"), OK)
	sim.set_local_player_weapon({
		"name": "WPN_M4AUTO",
		"actions": [
			{"name": "idle", "delaystart": 0, "delayend": 0},
			{"name": "fire", "delaystart": 0, "delayend": 0},
			{"name": "recoil", "delaystart": 0, "delayend": 0},
		],
		"flags": 0,
		"clipsize": 30,
		"startrounds": 300,
	}, {})
	sim.step()
	sim.set_local_player_weapon_input(false, true, false)
	for _i in range(4):
		sim.step()
		if int(sim.get_local_player_weapon_state().get("fired_serial", 0)) > 0:
			break

	var weapon_state := sim.get_local_player_weapon_state()
	var recoil_pitch := int(weapon_state.get("recoil_pitch_bam", 0))
	var weight_spread := int(weapon_state.get("weapon_weight_spread_bam", 0))
	assert_gt(recoil_pitch, 0,
			"the successful M4 round stamps the standing ammo recoil impulse")
	assert_eq(int(weapon_state.get("hud_spread_row", -1)), 2,
			"standing hip fire selects the first triplet's standing row")
	assert_eq(int(weapon_state.get("hud_spread_fp16", -1)),
			0x4000 + (recoil_pitch >> 7) + (weight_spread >> 7),
			"HUD spread preserves exact ERROR plus both live SAR terms")
	var recoil_view: Dictionary = sim.get_local_player_view()
	assert_almost_eq(float(recoil_view.get("fp_pitch_recoil_deg", 0.0)),
			float(recoil_pitch) * 2.0 * 360.0 / 4294967296.0, 0.0001,
			"the bridge exports retail's wrapped 2*recoil camera pitch")
	sim.free()


func test_weapon_channel_keeps_own_phase_and_switch_identity_per_entity() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))

	sim.set_local_player_weapon(_minimal_weapon("WPN_A", "shared.adm"), {})
	sim.step()
	var state: Dictionary = sim.get_local_player_weapon_state()
	assert_eq(String(state.get("body_anim_key", "")), "anim_idle",
		"equal state ids still export the secondary channel's independent playhead")
	assert_gt(absf(_weapon_arm_pitch_deg(sim)), 1.0,
		"the first AnimMap observed by this entity stamps the arms dip")

	for _i in range(200):
		sim.step()
	assert_lt(absf(_weapon_arm_pitch_deg(sim)), 0.001, "the first dip settled")

	sim.set_local_player_weapon(_minimal_weapon("WPN_B", "SHARED.ADM"), {})
	sim.step()
	assert_lt(absf(_weapon_arm_pitch_deg(sim)), 0.001,
		"a differently named weapon sharing the resolved AnimMap does not dip")

	sim.set_local_player_weapon(_minimal_weapon("WPN_C", "different.adm"), {})
	sim.step()
	assert_gt(absf(_weapon_arm_pitch_deg(sim)), 1.0,
		"a changed AnimMap stamps the dip")

	# Replacing the world/player keeps the equipped host state, but the new entity's
	# observed serial starts empty and must receive its own initial stamp.
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	sim.set_local_player_weapon(_minimal_weapon("WPN_C", "different.adm"), {})
	sim.step()
	assert_gt(absf(_weapon_arm_pitch_deg(sim)), 1.0,
		"a replacement local entity observes the current AnimMap as new")
	sim.free()

func test_weapon_clip_variant_ring_rotates_bake_reads_and_plays() -> void:
	# Multi-clip .adm variant rings, end to end through the public binding: clip
	# lengths arrive as per-key VARIANT arrays; the bake consumes ONE ring entry per
	# 'auto' delay field (serve-then-advance), and every play consumes + latches the
	# served variant into the state dict. A both-auto reload over a 3-ring therefore
	# eats entries 0 and 1 at bake — the FIRST reload PLAY serves variant 2, the
	# next serves 0 (the REVVY M4 "m4_1r" "m4_1r" "m4_1r2" shape).
	# [orig: Anim_InitActions reads @0x5421c5/@0x5421d8 via Anim_GetDurationTicks
	#  @0x53ee10; AnimMap_PlayAnimBySlot @0x40bda0 latches at animState+68]
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def := {
		"name": "WPN_RING", "animadm": "ring.adm",
		"actions": [
			{"name": "idle", "anim": "anim_wpn_idle", "delaystart": 0, "delayend": 0},
			{"name": "fire", "anim": "anim_wpn_fire", "delaystart": 0, "delayend": 0},
			{"name": "reload", "anim": "anim_wpn_reload", "delaystart": -1, "delayend": -1},
		],
		"flags": 0, "clipsize": 30, "startrounds": 60,
	}
	sim.set_local_player_weapon(def, {
		"anim_wpn_idle": PackedFloat32Array([0.2]),
		"anim_wpn_fire": PackedFloat32Array([0.05]),
		"anim_wpn_reload": PackedFloat32Array([0.5, 1.0, 0.25]),
	})
	sim.step()
	var state: Dictionary = sim.get_local_player_weapon_state()
	assert_eq(String(state.get("anim_key", "")), "anim_wpn_idle", "fresh slot idles")
	assert_eq(int(state.get("anim_variant", -1)), 0, "single-entry rings always serve 0")

	# Spend a round (letting the fire+recoil chain settle back to idle — the reload
	# dispatch gate refuses the edge mid-FIRE), then reload: the bake left the reload
	# ring's head at 2 (two 'auto' reads), so the FIRST reload serves variant 2.
	sim.set_local_player_weapon_input(false, true, false)
	for _i in range(6):
		sim.step()
	assert_eq(int(sim.get_local_player_weapon_state().get("clip", 0)), 29, "one round spent")
	sim.set_local_player_weapon_input(false, false, true)
	var reload_variant := -1
	var first_reload_serial := -1
	for _i in range(90):
		sim.step()
		state = sim.get_local_player_weapon_state()
		if String(state.get("anim_key", "")) == "anim_wpn_reload":
			reload_variant = int(state.get("anim_variant", -1))
			first_reload_serial = int(state.get("play_serial", 0))
			break
	assert_eq(reload_variant, 2,
		"the first reload serves variant 2 — the both-auto bake consumed entries 0+1")

	# Let the reload finish (ds 32 + de 32 ticks and the transitions), spend another
	# round, reload again: the ring wrapped, so the play serves variant 0.
	for _i in range(90):
		sim.step()
	sim.set_local_player_weapon_input(false, true, false)
	for _i in range(6):
		sim.step()
	sim.set_local_player_weapon_input(false, false, true)
	reload_variant = -1
	for _i in range(90):
		sim.step()
		state = sim.get_local_player_weapon_state()
		if String(state.get("anim_key", "")) == "anim_wpn_reload" 				and int(state.get("play_serial", 0)) != first_reload_serial:
			reload_variant = int(state.get("anim_variant", -1))
			break
	assert_eq(reload_variant, 0, "the second reload wraps the ring back to variant 0")
	sim.free()


func test_weapon_event_batch_preserves_three_undrained_ticks() -> void:
	# Game_MainLoop catch-up presents once after N fixed ticks. The sim must retain
	# each tick's clip/begin/end payload in order, including its age at the drain.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def := {
		"name": "WPN_EVENT_BATCH",
		"actions": [
			{"name": "idle", "anim": "anim_wpn_idle", "delaystart": 0, "delayend": 0},
			{"name": "fire", "anim": "anim_wpn_fire", "delaystart": 0, "delayend": 0,
				"soundset": "FIRE_BEGIN", "soundsetend": "FIRE_END"},
			{"name": "recoil", "anim": "anim_wpn_recoil", "delaystart": 0,
				"delayend": 0, "soundset": "RECOIL_BEGIN",
				"particle": "Effect_TestCas", "particleuserpoint": "bcasing"},
		],
		"flags": 0x100,
		"clipsize": 30,
		"startrounds": 60,
	}
	sim.set_local_player_weapon(def, {
		"anim_wpn_idle": 0.1,
		"anim_wpn_fire": 0.1,
		"anim_wpn_recoil": 0.1,
	})
	sim.step()
	sim.drain_local_player_weapon_events() # discard the initial idle play

	sim.set_local_player_weapon_input(true, true, false)
	sim.step()
	sim.step()
	sim.step()
	var events: Array = sim.drain_local_player_weapon_events()
	assert_eq(events.size(), 3, "FIRE, RECOIL, FIRE survive one three-tick catch-up")
	if events.size() == 3:
		assert_eq([
			String((events[0] as Dictionary).get("anim_key", "")),
			String((events[1] as Dictionary).get("anim_key", "")),
			String((events[2] as Dictionary).get("anim_key", "")),
		], ["anim_wpn_fire", "anim_wpn_recoil", "anim_wpn_fire"])
		assert_eq([
			int((events[0] as Dictionary).get("action_started", -1)),
			int((events[1] as Dictionary).get("action_started", -1)),
			int((events[2] as Dictionary).get("action_started", -1)),
		], [2, 3, 2])
		assert_eq([
			int((events[0] as Dictionary).get("action_finished", -1)),
			int((events[1] as Dictionary).get("action_finished", -1)),
			int((events[2] as Dictionary).get("action_finished", -1)),
		], [2, -1, 2])
		assert_eq([
			int((events[0] as Dictionary).get("age_ticks", -1)),
			int((events[1] as Dictionary).get("age_ticks", -1)),
			int((events[2] as Dictionary).get("age_ticks", -1)),
		], [2, 1, 0])
		assert_eq([
			String((events[0] as Dictionary).get("action_soundset", "")),
			String((events[1] as Dictionary).get("action_soundset", "")),
			String((events[2] as Dictionary).get("action_soundset", "")),
		], ["FIRE_BEGIN", "RECOIL_BEGIN", "FIRE_BEGIN"],
			"payloads are copied before a later tick or remount can overwrite them")
		assert_eq([
			String((events[0] as Dictionary).get("action_end_soundset", "")),
			String((events[1] as Dictionary).get("action_end_soundset", "")),
			String((events[2] as Dictionary).get("action_end_soundset", "")),
		], ["FIRE_END", "", "FIRE_END"])
		assert_eq(int((events[1] as Dictionary).get("action_effect", -1)), 3,
				"the recoil arbiter event survives the catch-up batch")
		assert_eq(String((events[1] as Dictionary).get("effect_particle", "")),
				"Effect_TestCas")
		assert_eq(String((events[1] as Dictionary).get("effect_particle_userpoint", "")),
				"bcasing")
	assert_true(sim.drain_local_player_weapon_events().is_empty(), "the drain is destructive")
	sim.free()


func test_weapon_event_batch_snapshots_the_scope_settle_tick() -> void:
	# Retail promotes the view before weapon actions. Across one catch-up batch,
	# an auto-fire event before 15/15 remains unsuppressed while the later event
	# at 15/15 carries the settled gate.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def := {
		"name": "WPN_SCOPE_BATCH",
		"actions": [
			{"name": "idle", "delaystart": 0, "delayend": 0},
			{"name": "fire", "delaystart": 0, "delayend": 0,
				"particle": "Effect_TestMF", "particleuserpoint": "muzzle1"},
			{"name": "recoil", "delaystart": 0, "delayend": 0},
		],
		"flags": 0x102, # Auto + Sighted
		"clipsize": 30,
		"startrounds": 60,
	}
	sim.set_local_player_weapon(def, {})
	sim.step()
	sim.drain_local_player_weapon_events()
	assert_true(sim.request_local_player_scope_toggle())
	for _i in range(12):
		sim.step()
	sim.drain_local_player_weapon_events()

	sim.set_local_player_weapon_input(true, true, false)
	sim.step() # scope 13/15, FIRE
	sim.step() # scope 14/15, RECOIL
	sim.step() # scope 15/15, FIRE
	var fire_events: Array = sim.drain_local_player_weapon_events().filter(
			func(event: Dictionary) -> bool: return int(event.get("action_started", -1)) == 2)
	assert_eq(fire_events.size(), 2)
	if fire_events.size() == 2:
		assert_false(bool((fire_events[0] as Dictionary).get("scope_settled", true)),
				"the earlier catch-up tick still shows its muzzle")
		assert_true(bool((fire_events[1] as Dictionary).get("scope_settled", false)),
				"the 15/15 tick alone suppresses its muzzle")
	sim.free()


func test_nocardswitch_controls_settled_sights_card_for_sighted_weapon() -> void:
	# Retail-derived flag vectors: M4 EOTech is Sighted and has no NoCardSwitch;
	# Remington is Sighted plus NoCardSwitch. The original's suppression predicate
	# exempts ForceScoped. Parser-to-HUD coverage lives in game_hud_test.gd.
	for case in [
		{"name": "WPN_M4AUTO_EOTECH", "flags": 0x01000902, "expected_card": true},
		{"name": "WPN_RemmingtonSG", "flags": 0x02000002, "expected_card": false},
		{"name": "WPN_SCOPED_CONTROL", "flags": 0x1, "expected_card": true},
		{"name": "WPN_FORCE_SCOPED_OVERRIDE", "flags": 0x22000002, "expected_card": true},
	]:
		var md := NovaMissionData.new()
		assert_eq(md.create_default(), OK)
		var sim := NovaSimulation.new()
		assert_true(sim.load_from_mission_data(md))
		assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
		sim.set_local_player_weapon({
			"name": String(case["name"]),
			"actions": [{"name": "idle", "delaystart": 0, "delayend": 0}],
			"flags": int(case["flags"]),
			"clipsize": 30,
			"startrounds": 60,
		}, {})
		sim.step()
		assert_true(sim.request_local_player_scope_toggle())
		for _i in range(15):
			sim.step()
		var view: Dictionary = sim.get_local_player_view()
		assert_almost_eq(float(view.get("scope_fraction", 0.0)), 1.0, 0.001,
			"the ADS ease settled before checking the card switch")
		assert_eq(bool(view.get("scope_card_active", false)), bool(case["expected_card"]),
			"Scoped/Sighted and NoCardSwitch select the card for %s" % case["name"])
		sim.free()


func test_reload_during_scope_raise_does_not_stash_an_unpromoted_scope() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	sim.set_local_player_weapon({
		"name": "WPN_SCOPE_RELOAD",
		"actions": [
			{"name": "idle", "delaystart": 0, "delayend": 0},
			{"name": "fire", "delaystart": 0, "delayend": 0},
			{"name": "recoil", "delaystart": 0, "delayend": 0},
			{"name": "reload", "delaystart": 1, "delayend": 1},
		],
		"flags": 0x2, "clipsize": 30, "startrounds": 60,
	}, {})
	sim.step()
	sim.set_local_player_weapon_input(false, true, false)
	for _i in range(6):
		sim.step()
	assert_eq(int(sim.get_local_player_weapon_state().get("clip", 0)), 29)
	assert_true(sim.request_local_player_scope_toggle())
	sim.step()
	assert_lt(float(sim.get_local_player_view().get("scope_fraction", 1.0)), 1.0)
	var before := int(sim.get_local_player_weapon_state().get("unscope_serial", 0))
	sim.set_local_player_weapon_input(false, false, true)
	for _i in range(3):
		sim.step()
	assert_eq(int(sim.get_local_player_weapon_state().get("unscope_serial", 0)), before)
	assert_true(bool(sim.get_local_player_view().get("scope_engaged", false)))
	sim.free()


func test_local_fire_spawns_the_authoritative_round_and_impact() -> void:
	# The listen-server loopback handler skips C2S 0x06 because retail local fire
	# already appends/spawns synchronously. Pin that local action seam end-to-end:
	# FSM fired -> RoundSim -> organic tag-2 effects_table row.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	# Spawn yaw 0 = mission yaw 0 = engine heading 90 BAM-deg, which faces
	# mission +y (bearing 90). The target sits 8 m along +y so the shot connects
	# only when the round bearing rides the heading frame directly — the old
	# (90 - heading) flip flew the shot along +x and only an east-side target
	# could pass (the compensating-error pair the fp_impact_probe pinned;
	# ledger D-WPN-18).
	# Use the fixture's Generic Soldier (wire id 5311 -> items.def id 105311),
	# then resolve traits through the same production seam as mission_runtime. Retail
	# returns before projectile damage when the struck entity has no ItemDef.
	assert_false(md.add_entity(NovaMissionData.KIND_ORGANIC, 5311,
			Vector3(0, 8, 0), Vector3.ZERO).is_empty())
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/def")), OK)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)
	sim.resolve_item_traits(item_db)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_eq(sim.load_ammo_table(root, "ammo.def"), OK)
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO")
	var fire_def := {
		"name": "WPN_M4AUTO",
		"actions": [
			{"name": "idle", "delaystart": 0, "delayend": 0},
			{"name": "fire", "delaystart": 0, "delayend": 0},
			{"name": "recoil", "delaystart": 0, "delayend": 0},
		],
		"flags": 0,
		"clipsize": 30,
		"startrounds": 300,
	}
	sim.set_local_player_weapon(fire_def, {})
	sim.step()
	sim.drain_local_player_weapon_events()
	sim.set_local_player_weapon_input(false, true, false)
	var impacts: Array = []
	for _i in range(4):
		sim.step()
		impacts.append_array(sim.drain_round_impacts())

	var weapon_state := sim.get_local_player_weapon_state()
	assert_eq(int(weapon_state.get("round_ring_count", 0)), 1,
			"local FIRE appends exactly one tag-2 fan-out record")
	# The fixture M4's first shot samples the pre-consume 30-round magazine, so
	# ((30 & 3) << 4) | 2 produces 0x22, not a hard-coded 0x02 and not the
	# unrelated category/rank weapon-slot combo.
	# [orig: WeaponAction_Fire @ 0x542c11; net-re §5.9.1 capture cross-witness]
	assert_eq(int(weapon_state.get("last_round_flags", 0)), 0x22)
	assert_eq(int(weapon_state.get("last_round_subtype", 0)), 12,
			"ordinary on-foot hip fire carries the retail default zoom subtype")
	assert_eq(int(weapon_state.get("last_round_slot_byte", -1)), 0,
			"the sole modeled local weapon slot has retail slot id zero")
	assert_eq(int(weapon_state.get("last_round_seq", 0)), 1)
	assert_eq(impacts.size(), 1, "one local shot reaches the target and emits one impact")
	if impacts.size() == 1:
		assert_eq(String((impacts[0] as Dictionary).get("effect", "")), "Effect_AmHitBody")
		assert_eq(String((impacts[0] as Dictionary).get("sound", "")), "IMP_BULLET_PLAYER")
		# The drained position is Godot-space (x, z_up, -y): the +y_m flight lands
		# near (0, ~eye, -8). Pins the local fire bearing = the engine heading
		# frame (D-WPN-18; RoundSim's wire-validated (cos, sin) mapping).
		var impact_pos: Vector3 = (impacts[0] as Dictionary).get("position", Vector3.ZERO)
		assert_almost_eq(impact_pos.x, 0.0, 0.75,
				"the shot flies the aim bearing, not its 90-deg mirror")
		assert_between(-impact_pos.z, 6.0, 8.5,
				"the impact lands at the north-side target range")

	# Presentation generations reset when the weapon is remounted, but the wire
	# round sequence belongs to the shooter and stays monotonic across adm/slot
	# changes. [orig: word_B7C670; net-re §5.9.1 capture 0x020b..0x0217]
	sim.set_local_player_weapon(fire_def, {})
	sim.step()
	sim.drain_local_player_weapon_events()
	sim.set_local_player_weapon_input(false, true, false)
	sim.step()
	weapon_state = sim.get_local_player_weapon_state()
	assert_eq(int(weapon_state.get("round_ring_count", 0)), 2)
	assert_eq(int(weapon_state.get("last_round_seq", 0)), 2,
			"weapon remount does not reset the shooter-lifetime sequence")
	sim.free()


func test_local_round_damages_enemy_mounted_on_rotated_emplaced_gun() -> void:
	# Exact player report: the target rendered in a rotated UseGun seat must keep
	# its authored COBJ sections under the carried body basis while its look yaw
	# remains independent, so a local-owned round through a visible section can
	# damage it.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var reference_gun := md.add_entity(
			NovaMissionData.KIND_ITEM, 101294,
			Vector3(-20, 8, 0), Vector3.ZERO)
	var reference_enemy := md.add_entity(
			NovaMissionData.KIND_ORGANIC, 105311,
			Vector3(-20, 8, 0), Vector3.ZERO)
	var rotated_gun := md.add_entity(
			NovaMissionData.KIND_ITEM, 101294,
			Vector3(0, 8, 0), Vector3(0, -90, 0))
	var rotated_enemy := md.add_entity(
			NovaMissionData.KIND_ORGANIC, 105311,
			Vector3(0, 8, 0), Vector3.ZERO)
	for pair in [
		[reference_enemy, reference_gun],
		[rotated_enemy, rotated_gun],
	]:
		assert_false((pair[0] as Dictionary).is_empty())
		assert_false((pair[1] as Dictionary).is_empty())
		assert_true(md.set_entity_property_int(
				NovaMissionData.KIND_ORGANIC,
				int((pair[0] as Dictionary)["index"]),
				"waypoint_id", 125))
		assert_true(md.set_entity_property_int(
				NovaMissionData.KIND_ORGANIC,
				int((pair[0] as Dictionary)["index"]),
				"wp_number", int((pair[1] as Dictionary)["bms_id"])))

	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	sim.set_item_seat_specs([{
		"type_id": 1294,
		"seats": [{"type": 3, "position": Vector3.ZERO, "yaw_offset": 0}],
	}])
	assert_true(sim.load_from_mission_data(md))
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	sim.resolve_item_traits(item_db)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3/CharModel.3di")), OK)
	var bad_root := NovaResourceRoot.new()
	assert_eq(bad_root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/bad")), OK)
	var skeletal := NovaSkeletalAnim.new()
	assert_true(skeletal.load_from_bad_files(
			bad_root, "BINOC.bad", {
				"anim_idle": "BINOC.bad",
				"anim_emplaced": "BINOC.bad",
			}, data.get_bone_origins(), data.get_bone_parents()))
	assert_gte(sim.resolve_collision_instances(
			item_db, SkeletalDataPlacerStub.new(data, skeletal)), 2,
			"precondition: both enemies use authored posed COBJ collision")
	var reference_card: Dictionary = sim.get_entity_debug(0)
	var rotated_card: Dictionary = sim.get_entity_debug(1)
	assert_true(bool(reference_card.get("mounted", false)))
	assert_true(bool(rotated_card.get("mounted", false)))
	assert_eq(int(rotated_card.get("mount_type", 0)), 3)
	var health_before := int(rotated_card.get("health", 0))
	assert_gt(health_before, 0)

	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/def")), OK)
	assert_eq(sim.load_ammo_table(root, "ammo.def"), OK)
	# Advance one authoritative frame so collision and the decoded presentation
	# snapshot expose the same mounted body pose.
	sim.step()
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	var reference_row_base := -1
	var rotated_row_base := -1
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + NovaSimulation.PF_KIND]) != NovaMissionData.KIND_ORGANIC:
			continue
		var mission_index := int(snapshot[base + NovaSimulation.PF_INDEX])
		if mission_index == int(reference_enemy["index"]):
			reference_row_base = base
		elif mission_index == int(rotated_enemy["index"]):
			rotated_row_base = base
	assert_gte(reference_row_base, 0,
			"the reference gunner reached the decoded presentation")
	assert_gte(rotated_row_base, 0,
			"the rotated gunner reached the decoded presentation")
	if reference_row_base < 0 or rotated_row_base < 0:
		sim.free()
		return
	assert_eq(int(snapshot[reference_row_base +
			NovaSimulation.PF_AIM_OVERLAY_VALID]), 1)
	assert_eq(int(snapshot[rotated_row_base +
			NovaSimulation.PF_AIM_OVERLAY_VALID]), 1)
	var sections: Array = sim.get_hitbox_debug().get("organics", [])
	var reference_by_section := {}
	var rotated_by_section := {}
	for value in sections:
		var row: Dictionary = value
		var handle := int(row.get("entity_handle", -1))
		var section := int(row.get("section", -1))
		if handle == 0:
			reference_by_section[section] = row
		elif handle == 1:
			rotated_by_section[section] = row
	assert_eq(reference_by_section.size(), 19)
	assert_eq(rotated_by_section.size(), 19)

	var reference_pos: Vector3 = reference_card.get("position", Vector3.ZERO)
	var rotated_pos: Vector3 = rotated_card.get("position", Vector3.ZERO)
	# PF_YAW_DEG / debug yaw is the gunner's independent look. The final body
	# field is the basis PresentAimOverlay actually applies to the rendered model
	# and the same body class build_section_matrices uses for posed collision.
	var reference_body_yaw := snapshot[reference_row_base +
			NovaSimulation.PF_AIM_BODY_YAW_DEG]
	var rotated_body_yaw := snapshot[rotated_row_base +
			NovaSimulation.PF_AIM_BODY_YAW_DEG]
	var relative_basis := MissionObjectPlacer.bms_to_godot_basis(
			Vector3(0, rotated_body_yaw, 0)) * MissionObjectPlacer.bms_to_godot_basis(
			Vector3(0, reference_body_yaw, 0)).inverse()
	var selected_section := -1
	var selected_score := -1.0
	# Hips, thighs, calves, and feet are the body/leg overlay classes. Their
	# mounted yaw is carried by the seat even when the two gunners independently
	# aim their upper bodies after the authoritative step.
	for section_value in [0, 7, 8, 11, 12, 17, 18]:
		var section := int(section_value)
		if not reference_by_section.has(section):
			continue
		var row: Dictionary = reference_by_section[section]
		var offset: Vector3 = row.get("pos", Vector3.ZERO) - reference_pos
		var radius := maxf(float(row.get("radius", 0.0)), 0.001)
		var score := Vector2(offset.x, offset.z).length() / radius
		if rotated_by_section.has(section) and score > selected_score:
			selected_score = score
			selected_section = section
	assert_gte(selected_section, 0,
			"a shared off-axis authored section exists")
	var reference_center: Vector3 = (
			reference_by_section[selected_section] as Dictionary).get(
					"pos", Vector3.ZERO)
	var expected_center := rotated_pos + relative_basis * (
			reference_center - reference_pos)
	var collision_center: Vector3 = (
			rotated_by_section[selected_section] as Dictionary).get(
					"pos", Vector3.ZERO)
	assert_lt(collision_center.distance_to(expected_center), 0.01,
			"posed collision follows the mounted entity's rendered body yaw")

	var radial := expected_center - rotated_pos
	radial.y = 0.0
	assert_gt(radial.length(), 0.01)
	var tangent := Vector3(-radial.z, 0.0, radial.x).normalized()
	assert_gte(sim.debug_spawn_round(
			expected_center - tangent,
			tangent, "AMMO_CAR15_556MM"), 0,
			"a local-owned live round starts through the rendered section")
	for _i in range(2):
		sim.step()
	var rotated_after: Dictionary = sim.get_entity_debug(1)
	assert_lt(int(rotated_after.get("health", health_before)), health_before,
			"the local round damages the rotated mounted enemy organic")
	sim.free()


func test_mounted_rendered_head_matrix_matches_collision_and_authoritative_shot() -> void:
	# Config 6 is the decisive per-config witness: the mounted body/neck stay on
	# the carrier frame while the head alone consumes aim. Build an actual
	# NovaObjectModel from the same CharModel + BINOC rig as collision, feed it
	# the packed presentation result, and compare its final head deformation to
	# COBJ section 14 before shooting through that rendered point.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var gun := md.add_entity(
			NovaMissionData.KIND_ITEM, 101294,
			Vector3(0, 8, 0), Vector3(45, 0, 0))
	var enemy := md.add_entity(
			NovaMissionData.KIND_ORGANIC, 105311,
			Vector3(0, 8, 0), Vector3.ZERO)
	assert_false(gun.is_empty())
	assert_false(enemy.is_empty())
	assert_true(md.set_entity_property_int(
			NovaMissionData.KIND_ORGANIC, int(enemy["index"]),
			"waypoint_id", 125))
	assert_true(md.set_entity_property_int(
			NovaMissionData.KIND_ORGANIC, int(enemy["index"]),
			"wp_number", int(gun["bms_id"])))

	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	sim.set_item_seat_specs([{
		"type_id": 1294,
		"mount_config_valid": true,
		"mount_config": 6,
		"seats": [{
			"type": 3,
			"bone_index": 6,
			"position": Vector3.ZERO,
			"yaw_offset": 0,
			"source_name": "UseGun",
		}],
	}])
	assert_true(sim.load_from_mission_data(md))

	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	sim.resolve_item_traits(item_db)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3/CharModel.3di")), OK)
	var bad_root := NovaResourceRoot.new()
	assert_eq(bad_root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/bad")), OK)
	var skeletal := NovaSkeletalAnim.new()
	assert_true(skeletal.load_from_bad_files(
			bad_root, "BINOC.bad", {"anim_emplaced": "BINOC.bad"},
			data.get_bone_origins(), data.get_bone_parents()),
			"mounted CharModel rig loads: %s" % skeletal.get_last_error())
	assert_gte(sim.resolve_collision_instances(
			item_db, SkeletalDataPlacerStub.new(data, skeletal)), 1,
			"the mounted enemy owns authored posed COBJ collision")
	var ammo_root := NovaResourceRoot.new()
	assert_eq(ammo_root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/def")), OK)
	assert_eq(sim.load_ammo_table(ammo_root, "ammo.def"), OK)

	# Advance one authoritative frame so the mounted seat frame, collision pose,
	# and listen-server client snapshot all describe the same clip phase.
	sim.step()
	var enemy_idx := _first_organic_ai_index(sim)
	assert_gte(enemy_idx, 0)
	var card := sim.get_entity_debug(enemy_idx)
	assert_true(bool(card.get("mounted", false)))
	assert_true(bool(card.get("mount_config_valid", false)))
	assert_eq(int(card.get("mount_config", -1)), 6)
	assert_eq(String(card.get("anim_key", "")), "anim_emplaced")
	var enemy_handle := sim.get_entity_wire_handle(enemy_idx)
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	var row_base := -1
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + NovaSimulation.PF_KIND]) == NovaMissionData.KIND_ORGANIC \
				and int(snapshot[base + NovaSimulation.PF_INDEX]) == int(enemy["index"]):
			row_base = base
			break
	assert_gte(row_base, 0, "the mounted placed enemy reached the decoded present")
	if row_base < 0:
		sim.free()
		return
	assert_eq(int(snapshot[row_base + NovaSimulation.PF_AIM_OVERLAY_VALID]), 1)
	assert_eq(int(snapshot[row_base + NovaSimulation.PF_RIGHT_HAND_COLLAPSED]), 1,
			"the mounted NPC exports retail's BN17 final-row verdict")
	var packed_body := Vector3(
			snapshot[row_base + NovaSimulation.PF_AIM_BODY_PITCH_DEG],
			snapshot[row_base + NovaSimulation.PF_AIM_BODY_YAW_DEG],
			snapshot[row_base + NovaSimulation.PF_AIM_BODY_ROLL_DEG])
	var head_offset: int = (row_base + NovaSimulation.PF_AIM_ANGLES
			+ 8 * NovaSimulation.PF_AIM_CLASS_STRIDE)
	var packed_head := Vector3(
			snapshot[head_offset], snapshot[head_offset + 1],
			snapshot[head_offset + 2])
	assert_gt(packed_head.distance_to(packed_body), 10.0,
			"the config-6 witness really drives head aim away from the mounted body")

	var model = NovaObjectModelScript.new()
	add_child_autofree(model)
	model.set_skeletal_anim(skeletal)
	model.set_object_data(data)
	assert_true(model.has_skeleton())
	var presented_position := Vector3(
			snapshot[row_base + NovaSimulation.PF_POS_X],
			snapshot[row_base + NovaSimulation.PF_POS_Y],
			snapshot[row_base + NovaSimulation.PF_POS_Z])
	assert_lt(presented_position.distance_to(
			card.get("position", Vector3.ZERO)), 0.5,
			"the selected row is the mounted placed enemy")
	model.global_position = presented_position
	model.play_body_clip_at(
			"anim_emplaced",
			int(snapshot[row_base + NovaSimulation.PF_ANIM_PHASE_TICKS]))
	PresentAimOverlay.apply(model, snapshot, row_base)
	await get_tree().process_frame
	await get_tree().process_frame

	var skeleton: Skeleton3D = model.get_skeleton()
	# Inspect the same composed pose once without the clip verdict to capture the
	# animated joint, then restore the snapshot's mounted verdict.
	model.set_right_hand_collapsed(false)
	await get_tree().process_frame
	await get_tree().process_frame
	skeleton.force_update_all_bone_transforms()
	var authored_hand_joint := (skeleton.global_transform
			* skeleton.get_bone_global_pose(16).origin)
	model.set_right_hand_collapsed(true)
	await get_tree().process_frame
	await get_tree().process_frame
	skeleton.force_update_all_bone_transforms()
	# Existing native fixture witness: CharModel COBJ 14/head authors
	# center=(3578,65,54371) in signed 16.16 collision space. The retail
	# fixed-to-render sandwich maps local collision (x,y,z) to the skeleton's
	# node frame as (y,z,x) before the live bone deformation.
	var head_center_model := Vector3(
			65.0 / 65536.0, 54371.0 / 65536.0, 3578.0 / 65536.0)
	var rendered_head_matrix := (skeleton.global_transform
			* skeleton.get_bone_global_pose(14)
			* skeleton.get_bone_global_rest(14).affine_inverse())
	var rendered_head_center: Vector3 = rendered_head_matrix * head_center_model
	var rendered_hand_matrix := (skeleton.global_transform
			* skeleton.get_bone_global_pose(16)
			* skeleton.get_bone_global_rest(16).affine_inverse())
	assert_true(rendered_hand_matrix.basis.x.is_zero_approx()
			and rendered_hand_matrix.basis.y.is_zero_approx()
			and rendered_hand_matrix.basis.z.is_zero_approx(),
			"render skinning receives retail's zero-scale BN17 basis")
	assert_true(rendered_hand_matrix.origin.is_equal_approx(authored_hand_joint),
			"render skinning collapses BN17 at the animated joint, never world origin")

	var collision_head := Vector3.INF
	var collision_hand := Vector3.INF
	for value in sim.get_hitbox_debug().get("organics", []):
		var section: Dictionary = value
		if int(section.get("entity_handle", -1)) == enemy_handle \
				and int(section.get("section", -1)) == 14:
			collision_head = section.get("pos", Vector3.INF)
		if int(section.get("entity_handle", -1)) == enemy_handle \
				and int(section.get("section", -1)) == 16:
			collision_hand = section.get("pos", Vector3.INF)
	assert_ne(collision_head, Vector3.INF,
			"authoritative collision exposes mounted head section 14")
	if collision_head != Vector3.INF:
		assert_lt(collision_head.distance_to(rendered_head_center), 0.01,
				"rendered final head bone matrix and collision section 14 are identical")
	assert_ne(collision_hand, Vector3.INF,
			"authoritative collision exposes mounted right-hand section 16")
	if collision_hand != Vector3.INF:
		assert_true(collision_hand.is_zero_approx(),
				"authoritative COBJ 16 retains retail's separate all-zero final row")

	var health_before := int(card.get("health", 0))
	var incoming := rendered_head_matrix.basis.x.normalized()
	assert_gte(sim.debug_spawn_round(
			rendered_head_center - incoming * 2.0,
			incoming, "AMMO_CAR15_556MM"), 0,
			"a local-owned round starts through the rendered mounted head")
	for _tick in range(2):
		sim.step()
	var events: Array = sim.get_round_debug().get("events", [])
	var hit_event: Dictionary = {}
	for value in events:
		var event: Dictionary = value
		if int(event.get("entity_handle", -1)) == enemy_handle \
				and String(event.get("kind_name", "")) == "organic":
			hit_event = event
	assert_false(hit_event.is_empty(),
			"the authoritative shot resolves against the rendered mounted target")
	if not hit_event.is_empty():
		assert_eq(int(hit_event.get("section", -1)), 14,
				"the primary posed-hit section remains authoritative")
		assert_false(bool(hit_event.get("fallback", true)))
	var impacts := sim.drain_round_impacts()
	assert_eq(impacts.size(), 1)
	if impacts.size() == 1:
		var impact: Dictionary = impacts[0]
		assert_lt((impact.get("direction", Vector3.ZERO) as Vector3).distance_to(
				incoming), 0.001,
				"the incoming shot direction remains authoritative for reactions")
	assert_lt(int(sim.get_entity_debug(enemy_idx).get("health", health_before)),
			health_before, "the posed head shot damages the mounted enemy")
	sim.free()


func test_weapon_event_batch_does_not_cross_lifecycle_boundaries() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def := {
		"name": "WPN_EVENT_LIFECYCLE",
		"actions": [
			{"name": "idle", "anim": "anim_wpn_idle", "delaystart": 0, "delayend": 0},
		],
	}
	var clips := {"anim_wpn_idle": 0.1}

	sim.set_local_player_weapon(def, clips)
	sim.step()
	var before_rebake: Dictionary = sim.get_local_player_weapon_state()
	# The FP model resolve explicitly rebakes the SAME weapon once its viewmodel
	# loads. Queued presentation and the live action slot survive that late bind.
	sim.rebake_local_player_weapon(def, clips)
	var after_rebake: Dictionary = sim.get_local_player_weapon_state()
	assert_eq(int(after_rebake.get("current", -1)), int(before_rebake.get("current", -2)))
	assert_eq(int(after_rebake.get("next", -1)), int(before_rebake.get("next", -2)))
	assert_eq(int(after_rebake.get("play_serial", -1)),
		int(before_rebake.get("play_serial", -2)))
	assert_false(sim.drain_local_player_weapon_events().is_empty(),
		"an explicit same-weapon rebake preserves queued presentation")
	sim.step()
	# A real mount is a new epoch even when the def name is unchanged.
	sim.set_local_player_weapon(def, clips)
	assert_true(sim.drain_local_player_weapon_events().is_empty(),
		"a same-name real mount discards the previous epoch's presentation")
	var remounted: Dictionary = sim.get_local_player_weapon_state()
	assert_eq(int(remounted.get("current", -1)), 0)
	assert_eq(int(remounted.get("next", -1)), 0)
	assert_eq(int(remounted.get("play_serial", -1)), 0)
	sim.step()
	# A different-weapon mount has the same epoch boundary.
	var def_b: Dictionary = def.duplicate(true)
	def_b["name"] = "WPN_EVENT_LIFECYCLE_B"
	sim.set_local_player_weapon(def_b, clips)
	assert_true(sim.drain_local_player_weapon_events().is_empty(),
		"a new-weapon mount discards the previous weapon's queued presentation")
	sim.step()
	sim.clear_local_player_weapon()
	assert_true(sim.drain_local_player_weapon_events().is_empty(),
		"clear discards the unmounted weapon's queued presentation")
	sim.set_local_player_weapon(def, clips)
	sim.step()
	sim.restart()
	assert_true(sim.drain_local_player_weapon_events().is_empty(),
		"restart cannot age a pre-rewind event across the logic-tick reset")
	sim.free()


func test_restart_clears_powerthrow_charge_and_input_latches() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def := {
		"name": "WPN_RESTART_POWERTHROW",
		"actions": [
			{"name": "idle", "delaystart": 0, "delayend": 0},
			{"name": "fire", "delaystart": 0, "delayend": 0},
			{"name": "recoil", "delaystart": 0, "delayend": 0},
		],
		"flags": 1 << 31,
		"clipsize": 1,
		"startrounds": 0,
	}
	sim.set_local_player_weapon(def, {})
	sim.step() # advance off tick zero so the idle sentinel cannot mask the windup
	sim.set_local_player_weapon_input(true, true, false)
	for _tick in range(5):
		sim.step()
	var wound: Dictionary = sim.get_local_player_weapon_state()
	assert_true(bool(wound.get("windup_active", false)))
	assert_gt(int(wound.get("windup_held_ticks", 0)), 0)

	sim.restart()
	var rewound: Dictionary = sim.get_local_player_weapon_state()
	assert_false(bool(rewound.get("windup_active", true)))
	assert_eq(int(rewound.get("windup_held_ticks", -1)), 0)
	var fired_before := int(rewound.get("fired_serial", 0))
	var rounds_before := int(rewound.get("round_ring_count", 0))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	sim.step() # stale held input must not recreate the windup
	assert_false(bool(sim.get_local_player_weapon_state().get("windup_active", true)))
	sim.set_local_player_weapon_input(false, false, false)
	for _tick in range(6):
		sim.step()
	var settled: Dictionary = sim.get_local_player_weapon_state()
	assert_eq(int(settled.get("fired_serial", -1)), fired_before)
	assert_eq(int(settled.get("round_ring_count", -1)), rounds_before)
	sim.free()


func test_armory_reads_and_clears_authoritative_local_loadout() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 2))
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)

	assert_eq(sim.get_local_player_class(), 8, "spawned player exposes its rifleman class")
	assert_eq(sim.get_local_player_team(), 2, "spawned player exposes the assigned red team")
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO",
		"weapon-table load exposes the entity's stamped default instead of the FP fallback")
	assert_false(sim.has_explicit_spawn_loadout(),
		"the engine's WPN_M4AUTO fallback is not an authored spawn kit")
	var fallback_loadout: Array = sim.get_local_player_loadout()
	assert_eq(fallback_loadout.size(), 1,
		"the canonical transport exposes the effective default kit")
	assert_eq(String((fallback_loadout[0] as Dictionary).get("name", "")), "WPN_M4AUTO")
	assert_true(sim.set_local_player_class(7),
		"profile class can commit without replacing the current weapon kit")
	assert_eq(sim.get_local_player_class(), 7)
	sim.set_spawn_loadout([{"name": "WPN_M4AUTO"}], true)
	assert_true(sim.has_explicit_spawn_loadout(),
		"a supplied mission/profile kit is distinguishable from the fallback")

	assert_true(sim.apply_local_player_loadout([{"name": "WPN_M4AUTO"}], 6))
	assert_eq(sim.get_local_player_class(), 6, "accepted class is authoritative on reopen")
	var accepted_loadout: Array = sim.get_local_player_loadout()
	assert_eq(accepted_loadout.size(), 1)
	assert_eq(int((accepted_loadout[0] as Dictionary).get("ammo_primary", 0)), -1,
		"unspecified canonical ammo remains the retail fallback sentinel")
	var inv: Dictionary = sim.get_local_player_inventory()
	assert_true(bool(inv.get("valid", false)), "the ACCEPT rebuilt the slot pool")
	assert_eq(String(inv.get("equipped_name", "")), "WPN_M4AUTO",
		"the ACCEPT re-selected the accepted weapon")
	assert_true(sim.apply_local_player_loadout([], 9), "the all-NONE kit is a valid apply")
	assert_eq(sim.get_local_player_class(), 9, "NONE still commits the selected class")
	assert_eq(sim.get_local_player_weapon_name(), "", "NONE clears the equipped AdmDef")
	assert_true(sim.get_local_player_loadout().is_empty(),
		"an explicit all-NONE kit remains empty instead of falling back to M4")
	sim.free()


func test_same_name_armory_accept_refills_the_live_weapon_slot() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	var weapons := NovaWeaponDatabase.new()
	assert_eq(weapons.load_from_resource_root(root, "weapon.def"), OK)
	var m4_index := weapons.find_weapon("WPN_M4AUTO")
	assert_gte(m4_index, 0)
	var m4: Dictionary = weapons.get_weapon(m4_index)
	assert_true(sim.apply_local_player_loadout([{"name": "WPN_M4AUTO"}], 8))
	sim.set_local_player_weapon(m4, {})
	sim.step()
	var full_clip := int(sim.get_local_player_weapon_state().get("clip", -1))
	sim.set_local_player_weapon_input(false, true, false)
	sim.step()
	sim.set_local_player_weapon_input(false, false, false)
	var spent_clip := int(sim.get_local_player_weapon_state().get("clip", -1))
	assert_lt(spent_clip, full_clip, "the live M4 spent a round before reopening armory")

	assert_true(sim.apply_local_player_loadout([{"name": "WPN_M4AUTO"}], 8))
	var inventory: Dictionary = sim.get_local_player_inventory()
	var equipped_combo := int(inventory.get("equipped_combo", -1))
	var rebuilt_clip := -1
	for value in inventory.get("slots", []):
		var slot: Dictionary = value
		if int(slot.get("combo", -2)) == equipped_combo:
			rebuilt_clip = int(slot.get("clip", -1))
			break
	assert_gt(rebuilt_clip, spent_clip, "ACCEPT rebuilt the same named slot at full clip")
	sim.set_local_player_weapon(m4, {})
	var mounted: Dictionary = sim.get_local_player_weapon_state()
	assert_eq(int(mounted.get("clip", -1)), rebuilt_clip,
		"same-name real mount reads the rebuilt authoritative inventory")
	assert_eq(int(mounted.get("current", -1)), 0)
	assert_eq(int(mounted.get("next", -1)), 0)
	assert_false(bool(mounted.get("windup_active", true)))
	assert_false(bool(sim.get_local_player_view().get("scope_engaged", true)))
	sim.free()


func test_loadout_weapon_category_switch_changes_equipped_weapon() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 2))
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)

	assert_true(sim.apply_local_player_loadout([
		{"name": "WPN_M4AUTO"},
		{"name": "WPN_colt45"},
	], 8))
	var weapons := NovaWeaponDatabase.new()
	assert_eq(weapons.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/weapon.def")), OK)
	var primary_index := weapons.find_weapon("WPN_M4AUTO")
	assert_gte(primary_index, 0)
	sim.set_local_player_weapon(weapons.get_weapon(primary_index), {})
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO",
		"the primary is equipped before switching")
	sim.drain_local_player_weapon_events()

	# The retail '2' binding requests category 2 (secondary). Let the outgoing
	# weapon's SWITCHFROM action complete, then observe the committed slot.
	sim.request_local_player_weapon_category(2)
	for _tick in range(120):
		sim.step()
		if sim.get_local_player_weapon_name() == "WPN_colt45":
			break
	assert_eq(sim.get_local_player_weapon_name(), "WPN_colt45",
		"switching to a secondary in the accepted loadout changes the equipped weapon")
	sim.free()


func test_weapon_switch_requested_during_draw_commits_without_a_second_press() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 2))
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([
		{"name": "WPN_M4AUTO"},
		{"name": "WPN_colt45"},
	], 8))

	var weapons := NovaWeaponDatabase.new()
	assert_eq(weapons.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/weapon.def")), OK)
	var primary_index := weapons.find_weapon("WPN_M4AUTO")
	var secondary_index := weapons.find_weapon("WPN_colt45")
	assert_gte(primary_index, 0)
	assert_gte(secondary_index, 0)
	sim.set_local_player_weapon(weapons.get_weapon(primary_index), {})

	# First switch normally, then mirror the LocalPlayerPresenter installing the new
	# weapon definition. Its first tick enters SWITCHTO (the draw animation).
	sim.request_local_player_weapon_category(2)
	for _tick in range(120):
		sim.step()
		if sim.get_local_player_weapon_name() == "WPN_colt45":
			break
	assert_eq(sim.get_local_player_weapon_name(), "WPN_colt45")
	sim.set_local_player_weapon(weapons.get_weapon(secondary_index), {})
	sim.step()
	assert_eq(int(sim.get_local_player_weapon_state().get("current", -1)), 6,
		"the newly equipped secondary is drawing through SWITCHTO")

	# One press during that draw must be remembered and committed after it settles.
	sim.request_local_player_weapon_category(3)
	for _tick in range(120):
		sim.step()
		if sim.get_local_player_weapon_name() == "WPN_M4AUTO":
			break
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO",
		"a switch requested during draw does not require a second press")
	sim.free()


func test_entities_walk_their_route() -> void:
	# Soldiers are anim-driven [orig: Entity_UpdateInfantryAI @0x4b9910]: their motion
	# comes from .bad root-motion clips resolved through a model's .adm. Without a clip
	# set they hold and stand; with one they walk the route.
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	var start: Vector3 = sim.get_entity_position(0)
	for _i in range(20):
		sim.step()
	assert_lt(start.distance_to(sim.get_entity_position(0)), 0.01,
		"no clip set -> soldiers stand still (faithful: motion comes from clips)")

	var clips := int(sim.set_infantry_anim_map(_anim_root(), "soldier.adm"))
	assert_gt(clips, 0, "fixture soldier.adm resolved root-motion clips")
	for _i in range(120):
		sim.step()
	var moved: Vector3 = sim.get_entity_position(0)
	# The walk fixture steps ~0.033u/tick toward the first route marker
	# (mission (100,0,0) -> Godot +x).
	assert_gt(start.distance_to(moved), 1.0, "entity walked away from its spawn")
	assert_gt(moved.x, start.x + 1.0, "walked toward the first route marker (+x)")
	sim.free()

func test_infantry_anim_map_failure_paths() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	assert_eq(int(sim.set_infantry_anim_map(null, "soldier.adm")), 0, "null root -> 0 clips")
	assert_eq(int(sim.set_infantry_anim_map(_anim_root(), "missing.adm")), 0, "absent .adm -> 0 clips")
	assert_eq(sim.get_infantry_clip_count(), 0, "failed load leaves no stale clip set")
	assert_gt(sim.set_infantry_anim_map(_anim_root(), "soldier.adm"), 0)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	sim.resolve_infantry_adm_ids(_anim_root(), item_db)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	assert_eq(int(sim.set_infantry_anim_map(_anim_root(), "missing.adm")), 0,
			"a retained per-entity resolver cannot replace a failed default with US01")
	assert_eq(sim.get_infantry_clip_count(), 0,
			"a failed registry rebuild remains empty after per-entity resolution")
	sim.free()


func test_restart_rebinds_baseline_player_to_own_adm() -> void:
	# The listen host's player is captured in AiSystem's baseline before the
	# MissionRuntime per-entity ADM sweep. Stop/Restart replaces the live AI rows
	# with that baseline at the same count, so a count-only late-spawn resolver
	# must explicitly repopulate the restored rows.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.has_local_player(), "listen host player exists in the restart baseline")
	assert_gt(sim.set_infantry_anim_map(_anim_root(), "soldier.adm"), 0)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	sim.resolve_infantry_adm_ids(_anim_root(), item_db)
	var player_ai_index := -1
	var local_handle := sim.get_local_player_wire_handle()
	for ai_index in range(sim.get_entity_count()):
		if int(sim.get_entity_debug(ai_index).get("wire_handle", 0)) == local_handle:
			player_ai_index = ai_index
			break
	assert_gte(player_ai_index, 0)
	assert_eq(String(sim.get_entity_debug(player_ai_index).get("adm_name", "")),
			"US01.adm", "the live host player owns its graphic ADM")
	sim.set_player_input(true, false, false, false, false, false, false)
	sim.step()
	assert_eq(sim.get_local_player_anim_key(), "anim_idle",
			"US01 lacks the requested gait and resolves through its own idle clip")

	sim.restart()
	assert_eq(String(sim.get_entity_debug(player_ai_index).get("adm_name", "")),
			"US01.adm", "restart immediately repopulates the restored baseline row")
	sim.set_player_input(true, false, false, false, false, false, false)
	sim.step()
	assert_eq(sim.get_local_player_anim_key(), "anim_idle",
			"restart repopulates US01 instead of silently retaining default soldier.adm")
	sim.free()


func test_late_spawn_player_resolves_own_adm_before_configured_usegun_pose() -> void:
	# MissionRuntime resolves per-entity ADMs once after the host player spawn. A
	# joiner's local L and host-admitted remote players spawn later; they must still
	# receive US01 rather than retaining the default E_STAND/soldier map. Otherwise
	# B50 phrase_set 4 cannot select anim_emplaced_5 and silently uses the generic
	# emplaced pose, visibly placing the body beside the gun.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_entity(
			NovaMissionData.KIND_ITEM, 101419,
			Vector3(2, 0, 0), Vector3.ZERO).is_empty())
	var object_data := NovaObjectData.new()
	assert_eq(object_data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/3dp/B50Cal/B50Cal.3di")), OK)
	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([{
			"type_id": 1419,
			"seats": ItemSeatSpecs.seat_specs_from_model(object_data),
			"mount_config_valid": true,
			"mount_config": 4,
			"primary_weapon": "WPN_EMPLCD50NA",
	}])
	assert_true(sim.load_from_mission_data(md))
	assert_gt(sim.set_infantry_anim_map(_anim_root(), "soldier.adm"), 0)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	# Reproduce the production ordering bug: the one-time sweep happens before
	# this player exists.
	sim.resolve_infantry_adm_ids(_anim_root(), item_db)
	assert_true(sim.spawn_local_player(Vector3(2, 0, 0), 0.0, 1))

	var weapon_root := NovaResourceRoot.new()
	assert_eq(weapon_root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(weapon_root, "weapon.def"), OK)
	var weapons := NovaWeaponDatabase.new()
	assert_eq(weapons.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/weapon.def")), OK)
	sim.set_local_player_weapon(
			weapons.get_weapon(weapons.find_weapon("WPN_M4AUTO")), {})
	for _tick in range(120):
		if int(sim.get_local_player_weapon_state().get("current", -1)) < 2:
			break
		sim.step()
	assert_true(sim.local_player_toggle_mount())
	sim.step()
	assert_eq(sim.get_local_player_anim_key(), "anim_emplaced_5",
			"a late-spawn player receives US01 before phrase_set 4 selects its pose")
	sim.free()


func test_load_from_editor_mission_data() -> void:
	# The editor-integration path: promote a live NovaMissionData (what the mission editor holds),
	# not a file or the synthetic demo. KIND_ORGANIC = 3.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK, "empty in-memory mission created")
	md.add_entity(3, 0, Vector3(0, 0, 0), Vector3.ZERO)
	md.add_entity(3, 0, Vector3(10, 0, 0), Vector3.ZERO)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md), "promoted the editor's live mission")
	assert_eq(sim.get_brain_count(), 2, "both organics got AI brains")
	assert_eq(sim.get_entity_kind(0), 3, "entity 0 maps back to KIND_ORGANIC")
	sim.free()

func test_item_seat_specs_mount_command_125_spawn() -> void:
	var fixture_dir := OS.get_cache_dir().path_join(
			"vehicle_idle_sim_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(fixture_dir), OK)
	var items_path := fixture_dir.path_join("items.def")
	var items_file := FileAccess.open(items_path, FileAccess.WRITE)
	assert_not_null(items_file)
	if items_file == null:
		return
	items_file.store_string("""begin "Drivable Transport Truck"
  id 101294
  type vehicle
  attrib: PlayerControl
  physics 1
  player_speed 60
  acceleration 10
  deceleration 20
  turn_rate 45
  turn_rate2 30
  sound_profile SP_Transport
end

begin "Driver"
  id 102072
  type person
end
""")
	items_file.close()
	var sndprof_path := fixture_dir.path_join("SndProf.def")
	var sndprof_file := FileAccess.open(sndprof_path, FileAccess.WRITE)
	assert_not_null(sndprof_file)
	if sndprof_file == null:
		DirAccess.remove_absolute(items_path)
		DirAccess.remove_absolute(fixture_dir)
		return
	sndprof_file.store_string("""begin "SP_Transport"
  soundloop_1 V_TRUCK_ILP .8 1.2
end
""")
	sndprof_file.close()
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(items_path), OK)

	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	var soldier := md.add_entity(NovaMissionData.KIND_ORGANIC, 102072, Vector3(11, 0, 0), Vector3.ZERO)
	assert_false(vehicle.is_empty())
	assert_false(soldier.is_empty())
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "waypoint_id", 125))
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "wp_number", int(vehicle["bms_id"])))

	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	sim.set_item_seat_specs([
		{
			"type_id": 1294,
			"seats": [
				{"type": 1, "position": Vector3(9, 0, 0), "yaw_offset": 0, "source_name": "sitex00"},
				{"type": 2, "position": Vector3(0, 1, 2), "yaw_offset": 45, "pose_index": 24, "source_name": "ctrlx24"}
			],
		}
	])
	sim.set_sound_profiles(FileAccess.get_file_as_bytes(sndprof_path))
	assert_true(sim.load_from_mission_data(md), "loaded command-125 mission with seat specs")
	sim.resolve_item_traits(item_db)
	var started: Dictionary = {}
	for effect_v in sim.drain_effects():
		var effect: Dictionary = effect_v
		if String(effect.get("kind", "")) == "vehicle_control_started":
			started = effect
			break
	assert_false(started.is_empty(),
			"command-125 controller mount emits the occupied-item lifecycle edge")
	sim.step()
	var expected_vehicle_handle := -1
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for base in range(0, snapshot.size(), stride):
		if int(snapshot[base + NovaSimulation.PF_TYPE_ID]) == 1294:
			expected_vehicle_handle = int(
					snapshot[base + NovaSimulation.PF_WIRE_HANDLE])
			break
	assert_gte(expected_vehicle_handle, 0)
	var idle: Dictionary = {}
	for emitter_v in sim.drain_sound_emitters():
		var emitter: Dictionary = emitter_v
		if int(emitter.get("lane", -1)) == 0 \
				and String(emitter.get("set", "")) == "V_TRUCK_ILP":
			idle = emitter
			break
	assert_false(idle.is_empty(),
			"an NPC control-seat occupant keeps the truck idle emitter alive without a local player")
	assert_gt(int(idle.get("source_spawn_id", 0)), 0,
			"the emitter key carries the registry-lifetime identity")
	assert_eq(int(idle.get("handle", -1)), expected_vehicle_handle)
	assert_eq(int(idle.get("source_bms_id", -1)), int(vehicle["bms_id"]))
	assert_eq(int(idle.get("lane", -1)), 0)
	assert_eq(int(idle.get("lifetime", -1)), 30)
	assert_eq(int(idle.get("emitted_tick", -1)), int(sim.get_logic_tick()),
			"catch-up transport retains the producing world tick")
	assert_eq(int(idle.get("pitch_q16", -1)), 0x10000)
	assert_eq(int(idle.get("volume_q8_8", -1)), 0xFFFF)
	assert_false(bool(idle.get("source_only", true)))
	assert_eq(int(idle.get("slot", -1)), 0)
	assert_eq(String(idle.get("set", "")), "V_TRUCK_ILP")
	assert_eq(Vector3(idle.get("pos", Vector3.INF)), Vector3(10, 0, 0))
	assert_eq(int(started.get("wire_handle", -1)), expected_vehicle_handle,
			"binding names the packed identity used by dynamic presentation")
	assert_eq(int(started.get("d", -1)), expected_vehicle_handle,
			"the generic effect payload retains the same packed identity")
	var soldier_idx := _first_organic_ai_index(sim)
	assert_true(soldier_idx >= 0, "found the soldier's AI row")
	var pos := sim.get_entity_position(soldier_idx)
	assert_true(pos.is_equal_approx(Vector3(10, 2, -1)),
		"command-125 soldier uses the IDA-priority ctrlx seat, converted to Godot axes")
	assert_almost_eq(sim.get_entity_yaw_deg(soldier_idx), 45.0, 0.01,
		"non-gunner mounted seats carry their local yaw offset")
	# The mounted anim state (100 = anim_sit_24) is asserted via the debug card below; the present
	# snapshot is the listen-server ClientState now (covered by nova_listen_server_test).
	var card: Dictionary = sim.get_entity_debug(soldier_idx)
	assert_true(bool(card["mounted"]), "debug card marks mounted occupants")
	assert_eq(int(card["mount_target_net_id"]), int(vehicle["bms_id"]))
	assert_eq(int(card["mount_seat"]), 1, "ctrlx seat was selected by original priority")
	assert_eq(int(card["mount_type"]), 2, "seat type is ctrlx/controller")
	assert_eq(int(card["mount_seat_bone"]), 0)
	assert_eq(int(card["mount_seat_pose_index"]), 24)
	assert_eq(String(card["mount_seat_source_name"]), "ctrlx24")
	assert_eq(Vector3(card["mount_seat_local"]), Vector3(0, 1, 2))
	assert_eq(int(card["mount_seat_yaw_offset"]), 45)
	var target_seats: Array = card["mount_target_seats"]
	assert_eq(target_seats.size(), 2, "debug card carries every target seat candidate")
	assert_eq(String((target_seats[0] as Dictionary)["source_name"]), "sitex00")
	assert_eq(int((target_seats[0] as Dictionary)["type"]), 1)
	assert_eq(int((target_seats[0] as Dictionary)["retail_slot"]), 0)
	assert_eq(String((target_seats[1] as Dictionary)["source_name"]), "ctrlx24")
	assert_eq(int((target_seats[1] as Dictionary)["pose_index"]), 24)
	assert_eq(int((target_seats[1] as Dictionary)["retail_slot"]), 8)
	assert_eq(int(card["anim_state"]), 100)
	assert_eq(String(card["anim_key"]), "anim_sit_24")
	sim.free()
	DirAccess.remove_absolute(items_path)
	DirAccess.remove_absolute(sndprof_path)
	DirAccess.remove_absolute(fixture_dir)



# The floating attach labels [orig: draw_vehicle_seat_and_armory_labels @0x5a3290
# selection half]: free seats in the 4.0 u radius label with exactly one nearest
# highlight; the unarmed local player sees every candidate; the armory zone flag is
# absent here so seat mode applies and no armory labels appear.
func test_attach_labels_seats() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	assert_false(vehicle.is_empty())
	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([
		{
			"type_id": 1294,
			"seats": [
				{"type": 1, "position": Vector3(0, 2, 0), "source_name": "sitex00"},
				{"type": 3, "position": Vector3(0, -1, 1), "source_name": "UseGun"},
			],
			"armory_points": [Vector3(0, 0, 1.5)],
			"primary_weapon": "WPN_EMPLCD50",
		}
	])
	assert_true(sim.load_from_mission_data(md), "loaded the labels mission")
	assert_true(sim.spawn_local_player(Vector3(12, 0, 0), 0.0, 1), "spawned the local player")
	var labels: Array = sim.get_attach_labels()
	assert_eq(labels.size(), 2, "both free seats label inside 4.0 u (armory points stay out of seat mode)")
	var nearest_count := 0
	var seat_types: Array = []
	for raw in labels:
		var l: Dictionary = raw
		seat_types.append(int(l["seat_type"]))
		if bool(l["nearest"]):
			nearest_count += 1
		assert_false(bool(l["armory"]), "no armory labels out of the zone")
		# WPN_EMPLCD50 is not in a loaded weapon table here -> the key stays absent
		# and the HUD falls to the STROVER_USEGUN default.
		assert_eq(String(l["attach_text_key"]), "")
	assert_eq(nearest_count, 1, "exactly the scan winner is highlighted")
	assert_true(seat_types.has(1) and seat_types.has(3), "sit + UseGun seats both reported")
	sim.free()


func test_attach_labels_hide_occupied_and_out_of_range() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	var soldier := md.add_entity(NovaMissionData.KIND_ORGANIC, 102072, Vector3(11, 0, 0), Vector3.ZERO)
	assert_false(vehicle.is_empty())
	assert_false(soldier.is_empty())
	# Command-125 mounts the soldier into the best seat at promote — that seat must not label.
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "waypoint_id", 125))
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "wp_number", int(vehicle["bms_id"])))
	# Same team as the local player: a live ENEMY occupant would reject the whole
	# vehicle instead [orig: Vehicle_HasEnemyOccupant @0x4359f0].
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "team", 1))
	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([
		{
			"type_id": 1294,
			"seats": [
				{"type": 2, "position": Vector3(0, 1, 1), "source_name": "ctrlx00"},
				{"type": 1, "position": Vector3(0, -2, 1), "source_name": "sitex00"},
			],
		}
	])
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3(12, 0, 0), 0.0, 1))
	var labels: Array = sim.get_attach_labels()
	assert_eq(labels.size(), 1, "the AI-occupied ctrlx seat never labels [orig: @0x5a348f]")
	assert_eq(int((labels[0] as Dictionary)["seat_type"]), 1, "the free sitex remains")
	sim.free()


func test_attach_labels_empty_out_of_range() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	assert_false(vehicle.is_empty())
	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([
		{"type_id": 1294, "seats": [{"type": 1, "position": Vector3.ZERO, "source_name": "sitex00"}]}
	])
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3(40, 0, 0), 0.0, 1), "spawned far away")
	assert_eq(sim.get_attach_labels().size(), 0,
		"outside the 4.0 u gate the nearest scan fails and no labels emit [orig: @0x5a32e2]")
	sim.free()


# Drivable items (control-seat specs) attach AI brains at promote since the vehicle
# pass, so organics no longer sit at AI index 0 — resolve the first pool-0 row.
func _first_organic_ai_index(sim: NovaSimulation) -> int:
	for i in 64:
		var d: Dictionary = sim.get_entity_debug(i)
		if d.is_empty():
			break
		if int(d.get("pool", -1)) == 0:
			return i
	return -1

func test_mounted_seat_local_matches_rotated_vehicle_userpoint() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3(0, -90, 0))
	var soldier := md.add_entity(NovaMissionData.KIND_ORGANIC, 102072, Vector3(11, 0, 0), Vector3.ZERO)
	assert_false(vehicle.is_empty())
	assert_false(soldier.is_empty())
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "waypoint_id", 125))
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "wp_number", int(vehicle["bms_id"])))

	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([
		{
			"type_id": 1294,
			"seats": [
				{
					"type": 2,
					"position": Vector3(-0.7148895, -0.1189880, 2.1048889),
					"source_name": "ctrlx10",
				}
			],
		}
	])
	assert_true(sim.load_from_mission_data(md), "loaded rotated command-125 mount")
	var soldier_idx := _first_organic_ai_index(sim)
	assert_true(soldier_idx >= 0, "found the soldier's AI row")
	var pos := sim.get_entity_position(soldier_idx)
	var expected := Vector3(10.1189880, 2.1048889, 0.7148895)
	assert_lt(pos.distance_to(expected), 0.001,
		"mounted seat local follows the same rotated side as the selected model userpoint")
	sim.free()


# The USE-ITEM toggle's weapon-busy gate at the sim binding [orig: @0x436958-0x436977]:
# a fire in flight swallows the toggle; back at idle the same toggle mounts.
func test_local_player_toggle_mount_weapon_busy_gate() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(2, 0, 0), Vector3.ZERO)
	assert_false(vehicle.is_empty())
	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([
		{
			"type_id": 1294,
			"seats": [
				{"type": 1, "position": Vector3(0, -1, 1), "yaw_offset": 0, "source_name": "sitex00"},
			],
		}
	])
	assert_true(sim.load_from_mission_data(md), "loaded the one-truck mission")
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	sim.set_local_player_weapon({
		"name": "WPN_GATE", "animadm": "gate.adm",
		"actions": [
			{"name": "idle", "anim": "anim_wpn_idle", "delaystart": 0, "delayend": 0},
			{"name": "fire", "anim": "anim_wpn_fire", "delaystart": 0, "delayend": 0},
		],
		"flags": 0, "clipsize": 30, "startrounds": 60,
	}, {
		"anim_wpn_idle": PackedFloat32Array([0.2]),
		"anim_wpn_fire": PackedFloat32Array([0.2]),
	})
	sim.step()
	# Pull the trigger: the FSM leaves idle this step; the in-flight fire swallows
	# the toggle.
	sim.set_local_player_weapon_input(false, true, false)
	sim.step()
	assert_false(sim.local_player_toggle_mount(), "a fire in flight swallows the toggle")
	# Release and settle back to idle: the same toggle now passes the gate and mounts.
	sim.set_local_player_weapon_input(false, false, false)
	for _i in range(40):
		sim.step()
	assert_true(sim.local_player_toggle_mount(), "the idle toggle mounts")
	var card: Dictionary = sim.get_world_entity_debug(int(vehicle["bms_id"]))
	var seats: Array = card.get("seats", [])
	assert_true(seats.size() == 1 and bool(seats[0]["occupied"]),
		"the scan took the truck's one sitex seat")
	sim.free()


# Local UseGun follows Player_MountWeaponSlot rather than the nonlocal direct slot
# assignment: holster the personal slot, commit the parent's embedded MountSlot, and
# restore the preserved personal slot through the same switch path on detach.
# [orig: Entity_AttachToUseGunSlot @0x546c25..0x546c3d;
# Player_MountWeaponSlot @0x4dfa40; switch commits @0x543475/@0x543539;
# Entity_DetachFromVehicle restore @0x43565f]
func test_local_first_person_usegun_parent_cull_follows_live_mount_slot() -> void:
	# Retail suppresses the parent model only after THIS parent's MountSlot is
	# EquippedSlot in first person. Pre-commit attach, third person, and detach
	# render it normally. [orig: Entity_RenderVehicleModel @0x4407d0;
	# predicate @0x4407f6..0x44084c; Render_SubmitEntity @0x440918]
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var gun := md.add_entity(NovaMissionData.KIND_ITEM, 101294,
			Vector3(2, 0, 0), Vector3.ZERO)
	assert_false(gun.is_empty())
	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	sim.set_item_seat_specs([{
		"type_id": 1294,
		"seats": [{"type": 3, "position": Vector3(0, -1, 1),
				"source_name": "UseGun"}],
		"primary_weapon": "WPN_EMPLCD50",
	}])
	assert_true(sim.load_from_mission_data(md))
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	sim.resolve_item_traits(item_db)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([{"name": "WPN_M4AUTO"}], 1))
	var weapons := NovaWeaponDatabase.new()
	assert_eq(weapons.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/weapon.def")), OK)
	var personal: Dictionary = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	var mounted: Dictionary = weapons.get_weapon(
			weapons.find_weapon("WPN_EMPLCD50"))
	sim.set_local_player_weapon(personal, {})
	sim.drain_local_player_weapon_events()
	sim.step() # seed the decoded listen-client present rows
	var gun_index := int(gun["index"])
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM, gun_index,
			NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"an unattached emplacement renders in the world pass")

	assert_true(sim.local_player_toggle_mount())
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM, gun_index,
			NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"the parent stays visible until its embedded slot commits")
	sim.step()
	var mount_event_found := false
	for raw in sim.drain_local_player_weapon_events():
		if String((raw as Dictionary).get("switch_to_weapon", "")) == "WPN_EMPLCD50":
			mount_event_found = true
	assert_true(mount_event_found)
	sim.set_local_player_weapon(mounted, {}, true)
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM, gun_index,
			NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"authored gfx1 alone cannot cull when its first-person model failed to resolve")
	sim.set_local_player_first_person_model_available(true)
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM, gun_index,
			NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 1,
			"the live parent slot with a resolved FP model suppresses the duplicate world gun")

	sim.set_local_player_camera_third_person(true)
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM, gun_index,
			NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"third person restores the parent model")
	sim.set_local_player_camera_third_person(false)
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM, gun_index,
			NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 1)
	for _tick in range(120):
		if int(sim.get_local_player_weapon_state().get("current", -1)) < 2:
			break
		sim.step()
	assert_true(sim.local_player_toggle_mount())
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM, gun_index,
			NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"detach restores the world model immediately, before the holster commit")
	sim.free()


func test_world_model_heat_glow_samples_parent_slot_and_caps_below_fp() -> void:
	# Give the emplacement a deterministic HEAT_GLOW collision track. The
	# scoped parent visual/collision frame must sample its embedded MountSlot
	# only while a live UseGun child is attached; the local FP state is the
	# comparison witness for the intentionally different endpoint.
	# [orig: attachment caller @ 0x546518;
	#  HUD_CacheWeaponSlotInfo stores @ 0x440969 / @ 0x440991]
	var object_data := NovaObjectData.new()
	assert_eq(object_data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/3dp/B50Cal/B50Cal.3di")), OK)
	assert_true(object_data.set_control_register_name(0, "HEAT_GLOW"))
	for index in range(object_data.get_part_anim_count(0) - 1, -1, -1):
		assert_true(object_data.delete_part_anim(0, index))
	var anim := object_data.add_part_anim(0, 1)
	assert_eq(anim, 0)
	assert_true(object_data.set_part_anim_channel_enabled(
			0, anim, "translation", true))
	assert_true(object_data.set_part_anim_channel_mode(
			0, anim, "translation", "x", "control_register", 0))
	assert_true(object_data.set_part_anim_channel_values(
			0, anim, "translation", "x", 0.0, 4.0, 0.0))

	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var gun := md.add_entity(NovaMissionData.KIND_ITEM, 101419,
			Vector3(2, 0, 0), Vector3.ZERO)
	assert_false(gun.is_empty())
	var gun_index := int(gun["index"])

	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	var seat_specs := ItemSeatSpecs.seat_specs_from_model(object_data)
	assert_eq(seat_specs.size(), 1)
	sim.set_item_seat_specs([{
		"type_id": 1419,
		"seats": seat_specs,
		"primary_weapon": "WPN_EMPLCD50NA",
	}])
	assert_true(sim.load_from_mission_data(md))
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	sim.resolve_item_traits(item_db)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	# Listen-host player creation rebuilds the authoritative registry. Bind the
	# collision instance to that final registry identity, as GameWorld does.
	var collision_placer := GraphicDataPlacerStub.new({
		"B50cal": object_data,
	})
	assert_eq(sim.resolve_collision_instances(
			item_db, collision_placer), 1)

	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([{"name": "WPN_M4AUTO"}], 1))
	var weapons := NovaWeaponDatabase.new()
	assert_eq(weapons.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/weapon.def")), OK)
	var personal: Dictionary = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	var mounted: Dictionary = weapons.get_weapon(
			weapons.find_weapon("WPN_EMPLCD50NA"))
	sim.set_local_player_weapon(personal, {})
	sim.drain_local_player_weapon_events()
	sim.step()

	assert_eq(_present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, gun_index,
			NovaSimulation.PF_WORLD_HEAT_GLOW_VALID), 0,
			"an unoccupied carrier is outside retail's attachment writer scope")
	var before_rows: Array = sim.get_hitbox_debug().get("entities", [])
	assert_eq(before_rows.size(), 1)
	if before_rows.size() != 1:
		sim.free()
		return
	var before: PackedVector3Array = (before_rows[0] as Dictionary).get(
			"tris", PackedVector3Array())

	assert_true(sim.local_player_toggle_mount())
	sim.step()
	var mounted_switch := false
	for raw in sim.drain_local_player_weapon_events():
		if String((raw as Dictionary).get(
				"switch_to_weapon", "")) == "WPN_EMPLCD50NA":
			mounted_switch = true
	assert_true(mounted_switch)
	sim.set_local_player_weapon(mounted, {}, true)
	assert_eq(_present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, gun_index,
			NovaSimulation.PF_WORLD_HEAT_GLOW_VALID), 1)
	assert_eq(_present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, gun_index,
			NovaSimulation.PF_WORLD_HEAT_GLOW), 0,
			"a live UseGun carrier owns the cold zero branch")

	sim.set_local_player_weapon_input(true, true, false)
	var fp_heat_glow := 0
	for _tick in range(3000):
		sim.step()
		fp_heat_glow = int(sim.get_local_player_weapon_state().get(
				"heat_glow", 0))
		if fp_heat_glow >= 0x10000:
			break
	sim.set_local_player_weapon_input(false, false, false)
	assert_eq(fp_heat_glow, 0x10000,
			"the mounted first-person consumer reaches its distinct endpoint")
	assert_eq(_present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, gun_index,
			NovaSimulation.PF_WORLD_HEAT_GLOW_VALID), 1)
	assert_eq(_present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, gun_index,
			NovaSimulation.PF_WORLD_HEAT_GLOW), 0xFFFF,
			"the same inline slot saturates the world model at 0xFFFF")

	var after_rows: Array = sim.get_hitbox_debug().get("entities", [])
	assert_eq(after_rows.size(), 1)
	if after_rows.size() != 1:
		sim.free()
		return
	var after: PackedVector3Array = (after_rows[0] as Dictionary).get(
			"tris", PackedVector3Array())
	assert_eq(after.size(), before.size())
	var moved := 0
	for index in before.size():
		if before[index].distance_to(after[index]) > 3.9:
			moved += 1
	assert_gt(moved, 0,
			"headless collision consumes the same authoritative HEAT_GLOW frame")
	sim.free()


func test_local_usegun_aim_articulates_emplaced_weapon_model() -> void:
	# B50Cal's authored PANM binds its turret and barrel to the semantic
	# EWEAP_GUNYAW/EWEAP_GUNPITCH registers. A mounted local player's live look
	# must pose those parts in authoritative model space, not only turn the camera.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_entity(
			NovaMissionData.KIND_ITEM, 101419,
			Vector3(2, 0, 0), Vector3.ZERO).is_empty())
	var object_data := NovaObjectData.new()
	assert_eq(object_data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/3dp/B50Cal/B50Cal.3di")), OK)
	var seat_specs := ItemSeatSpecs.seat_specs_from_model(object_data)
	assert_eq(seat_specs.size(), 1, "B50Cal exposes its authored Usegun seat")
	var usegun_info: Dictionary = object_data.get_user_point_info(
			int((seat_specs[0] as Dictionary).get("bone_index", 0)) - 1)
	var expected_usegun_world := MissionObjectPlacer.entity_transform(
			Vector3(2, 0, 0), Vector3.ZERO) * Vector3(
					usegun_info.get("position", Vector3.ZERO))
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([{
			"type_id": 1419,
			"seats": seat_specs,
			"primary_weapon": "WPN_EMPLCD50NA",
	}])
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(
			item_db, ObjectDataPlacerStub.new(object_data)), 1)
	# Start well off the gun's authored zero yaw. Retail's attach request snaps the
	# requester's look/body heading to the UseGun heading before establishing the
	# relationship; leaving this stale produces the visible torso twist at the grips.
	const PRE_ATTACH_YAW_DEG := 160.0
	assert_true(sim.spawn_local_player(Vector3.ZERO, PRE_ATTACH_YAW_DEG, 1))
	assert_gt(absf(wrapf(sim.get_local_player_yaw_deg(), -180.0, 180.0)),
			90.0, 'fixture starts far from the gun yaw')
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([{"name": "WPN_M4AUTO"}], 1))
	var weapons := NovaWeaponDatabase.new()
	assert_eq(weapons.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/weapon.def")), OK)
	var personal: Dictionary = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	var mounted: Dictionary = weapons.get_weapon(
			weapons.find_weapon("WPN_EMPLCD50NA"))
	sim.set_local_player_weapon(personal, {})
	sim.drain_local_player_weapon_events()
	assert_true(sim.local_player_toggle_mount())
	sim.step()
	assert_lt(sim.get_local_player_position().distance_to(expected_usegun_world),
			0.001, "mounted player origin coincides with the authored Usegun point")
	assert_lt(absf(wrapf(sim.get_local_player_yaw_deg(), -180.0, 180.0)),
			0.01, 'UseGun attach pre-snaps a mismatched local look to the gun yaw')
	for raw in sim.drain_local_player_weapon_events():
		if String((raw as Dictionary).get(
				"switch_to_weapon", "")) == "WPN_EMPLCD50NA":
			sim.set_local_player_weapon(mounted, {}, true)
	sim.debug_set_panm_time_ms(0)
	var initial_weapon_state: Dictionary = sim.get_local_player_weapon_state()
	assert_true(bool(initial_weapon_state.get(
			"emplaced_controls_valid", false)))
	var initial_yaw_control := int(initial_weapon_state.get(
			"emplaced_gun_yaw", 0))
	assert_eq(initial_yaw_control, 0,
			'the attach snap starts EWEAP_GUNYAW at its neutral phase')
	var initial_overlay: Dictionary = sim.get_local_player_aim_overlay()
	assert_true(bool(initial_overlay.get('valid', false)))
	var initial_body: Vector3 = initial_overlay.get('body', Vector3.ZERO)
	var initial_angles: PackedVector3Array = initial_overlay.get(
			'angles', PackedVector3Array())
	assert_gt(initial_angles.size(), 0)
	assert_lt(absf(wrapf(initial_body.y, -180.0, 180.0)), 0.01,
			'the mounted body neutral overlay follows the snapped gun yaw')
	var max_initial_twist_deg := 0.0
	for angle in initial_angles:
		max_initial_twist_deg = maxf(max_initial_twist_deg,
				absf(wrapf(angle.y - initial_body.y, -180.0, 180.0)))
	assert_lt(max_initial_twist_deg, 0.01,
			'no segment retains the pre-attach look as a torso twist')
	var initial_pitch_control := int(initial_weapon_state.get(
			"emplaced_gun_pitch", 0))

	var before_entities: Array = sim.get_hitbox_debug().get("entities", [])
	assert_eq(before_entities.size(), 1)
	var before: PackedVector3Array = (before_entities[0] as Dictionary).get(
			"tris", PackedVector3Array())
	assert_gt(before.size(), 0)

	sim.set_local_player_mouse(511, false)
	var yaw_before := sim.get_local_player_yaw_deg()
	sim.add_local_player_look(100.0, 0.0)
	sim.step()
	assert_lt(sim.get_local_player_position().distance_to(expected_usegun_world),
			0.001, "look yaw cannot move the player off the Usegun point")
	assert_gt(absf(sim.get_local_player_yaw_deg() - yaw_before), 0.1,
			"the mounted local player's authoritative yaw changed")
	assert_ne(int(sim.get_local_player_weapon_state().get(
			"emplaced_gun_yaw", initial_yaw_control)), initial_yaw_control,
			"look yaw reaches the retail EWEAP_GUNYAW phase")
	var yaw_entities: Array = sim.get_hitbox_debug().get("entities", [])
	var after_yaw: PackedVector3Array = (yaw_entities[0] as Dictionary).get(
			"tris", PackedVector3Array())
	var yaw_moved := 0
	for index in before.size():
		if before[index].distance_to(after_yaw[index]) > 0.001:
			yaw_moved += 1
	assert_gt(yaw_moved, 0,
			"EWEAP_GUNYAW moves the authored B50Cal turret with local look")

	var pitch_before := sim.get_local_player_pitch_deg()
	sim.add_local_player_look(0.0, 100.0)
	sim.step()
	assert_lt(sim.get_local_player_position().distance_to(expected_usegun_world),
			0.001, "look pitch cannot move the player off the Usegun point")
	assert_gt(absf(sim.get_local_player_pitch_deg() - pitch_before), 0.1,
			"the mounted local player's authoritative pitch changed")
	assert_ne(int(sim.get_local_player_weapon_state().get(
			"emplaced_gun_pitch", initial_pitch_control)),
			initial_pitch_control,
			"look pitch reaches the retail EWEAP_GUNPITCH phase")
	var pitch_entities: Array = sim.get_hitbox_debug().get("entities", [])
	var after_pitch: PackedVector3Array = (pitch_entities[0] as Dictionary).get(
			"tris", PackedVector3Array())
	var pitch_moved := 0
	for index in after_yaw.size():
		if after_yaw[index].distance_to(after_pitch[index]) > 0.001:
			pitch_moved += 1
	assert_gt(pitch_moved, 0,
			"EWEAP_GUNPITCH moves the authored B50Cal barrel with local look")
	sim.free()


func test_local_usegun_switches_viewmodel_and_borrows_parent_weapon_slot() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var gun := md.add_entity(
			NovaMissionData.KIND_ITEM, 101294, Vector3(2, 0, 0), Vector3.ZERO)
	assert_false(gun.is_empty())
	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([{
		"type_id": 1294,
		"seats": [{
			"type": 3,
			"position": Vector3(0, -1, 1),
			"source_name": "UseGun",
		}],
		# A finite clip makes parent-slot persistence observable across remounts.
		"primary_weapon": "WPN_AVENGER",
	}])
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([
		{"name": "WPN_M4AUTO"},
		{"name": "WPN_M4"},
	], 1))
	var weapons := NovaWeaponDatabase.new()
	assert_eq(weapons.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/weapon.def")), OK)
	var personal_idx := weapons.find_weapon("WPN_M4AUTO")
	var mounted_idx := weapons.find_weapon("WPN_AVENGER")
	assert_gte(personal_idx, 0)
	assert_gte(mounted_idx, 0)
	var personal_def: Dictionary = weapons.get_weapon(personal_idx)
	var mounted_def: Dictionary = weapons.get_weapon(mounted_idx)
	# Exercise the merge seam with a PowerThrow-shaped personal def while the
	# authoritative inventory still selects WPN_M4AUTO. A UseGun handoff must
	# cancel its windup before borrowing the parent's persistent slot.
	var powerthrow_personal: Dictionary = personal_def.duplicate(true)
	powerthrow_personal["flags"] = int(personal_def.get("flags", 0)) | (1 << 31)
	sim.set_local_player_weapon(powerthrow_personal, {})
	sim.drain_local_player_weapon_events()
	sim.step() # move beyond tick zero, the windup's idle sentinel
	sim.set_local_player_weapon_input(true, true, false)
	for _tick in range(5):
		sim.step()
	var wound_before_mount: Dictionary = sim.get_local_player_weapon_state()
	assert_true(bool(wound_before_mount.get("windup_active", false)))
	var personal_fired_before := int(wound_before_mount.get("fired_serial", 0))
	var personal_rounds_before := int(wound_before_mount.get("round_ring_count", 0))
	var personal_clip := int(sim.get_local_player_weapon_state().get("clip", -1))
	var inventory_clip := int(
			sim.get_local_player_inventory().get("slots", [])[0].get("clip", -1))

	assert_true(sim.local_player_toggle_mount())
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO",
			"local UseGun stages the parent slot instead of assigning it immediately")
	# The still-held PowerThrow plus a simultaneous trigger edge cannot overwrite
	# the pending mount action.
	sim.set_local_player_weapon_input(true, true, false)
	sim.step()
	sim.set_local_player_weapon_input(false, false, false)
	var mount_event: Dictionary = {}
	for raw in sim.drain_local_player_weapon_events():
		var event: Dictionary = raw
		if String(event.get("switch_to_weapon", "")) == "WPN_AVENGER":
			mount_event = event
	assert_false(mount_event.is_empty(),
			"an Emplaced pending def takes retail's same-pump -901 commit")
	assert_true(bool(mount_event.get("preserve_slot_state", false)),
			"the host must not reset the emplacement's persistent slot")
	assert_eq(sim.get_local_player_weapon_name(), "WPN_AVENGER")
	var handed_off: Dictionary = sim.get_local_player_weapon_state()
	assert_false(bool(handed_off.get("windup_active", true)),
			"UseGun input suppression cancels the outgoing PowerThrow windup")
	assert_eq(int(handed_off.get("fired_serial", -1)), personal_fired_before)
	assert_eq(int(handed_off.get("round_ring_count", -1)), personal_rounds_before,
			"releasing through the handoff cannot leak a charged personal round")
	sim.set_local_player_weapon(mounted_def, {}, true)
	var mounted_before: Dictionary = sim.get_local_player_weapon_state()
	assert_eq(int(mounted_before.get("clip", -1)),
			int(mounted_def.get("clipsize", -2)))
	var mounted_slot_before_rebake := {
		"current": int(mounted_before.get("current", -1)),
		"next": int(mounted_before.get("next", -1)),
		"phase": int(mounted_before.get("phase", -1)),
		"clip": int(mounted_before.get("clip", -1)),
	}
	sim.rebake_local_player_weapon(mounted_def, {
		"anim_wpn_idle": PackedFloat32Array([0.2]),
	}, true)
	var mounted_after_rebake: Dictionary = sim.get_local_player_weapon_state()
	for field in mounted_slot_before_rebake:
		assert_eq(int(mounted_after_rebake.get(field, -2)),
				int(mounted_slot_before_rebake[field]),
				"late mounted-def rebake preserves parent slot %s" % field)

	sim.set_local_player_weapon_input(true, true, false)
	var fired_before := int(mounted_before.get("fired_serial", 0))
	for _tick in range(120):
		sim.step()
		if int(sim.get_local_player_weapon_state().get(
				"fired_serial", 0)) > fired_before:
			break
	sim.set_local_player_weapon_input(false, false, false)
	var mounted_clip_after := int(
			sim.get_local_player_weapon_state().get("clip", -1))
	assert_eq(mounted_clip_after, int(mounted_before.get("clip", -1)) - 1,
			"local LMB consumes the parent's embedded weapon slot")
	assert_eq(int(sim.get_local_player_inventory().get(
			"slots", [])[0].get("clip", -1)), inventory_clip,
			"mounted fire cannot consume the saved personal magazine")

	for _tick in range(180):
		sim.step()
		if int(sim.get_local_player_weapon_state().get("current", -1)) < 2:
			break
	assert_true(sim.local_player_toggle_mount())
	assert_eq(sim.get_local_player_weapon_name(), "WPN_AVENGER",
			"detach also waits for the mounted slot's holster commit")
	# The entity is already detached, but its borrowed slot still owns the pending
	# UseGun SWITCHFROM. Manual requests in this frame must not replace that action.
	sim.request_local_player_weapon_category(3)
	sim.request_local_player_weapon_cycle(1)
	sim.set_local_player_weapon_input(true, true, false)
	sim.step()
	sim.set_local_player_weapon_input(false, false, false)
	var detach_event: Dictionary = {}
	for raw in sim.drain_local_player_weapon_events():
		var event: Dictionary = raw
		if String(event.get("switch_to_weapon", "")) == "WPN_M4AUTO":
			detach_event = event
	assert_false(detach_event.is_empty(),
			"outgoing Emplaced detach also commits on the next pump")
	assert_true(bool(detach_event.get("preserve_slot_state", false)))
	sim.set_local_player_weapon(personal_def, {}, true)
	assert_eq(int(sim.get_local_player_weapon_state().get("clip", -1)),
			personal_clip, "the personal slot resumes with its original magazine")

	for _tick in range(120):
		sim.step()
		if int(sim.get_local_player_weapon_state().get("current", -1)) < 2:
			break
	assert_true(sim.local_player_toggle_mount())
	var remount_event: Dictionary = {}
	for _tick in range(120):
		sim.step()
		for raw in sim.drain_local_player_weapon_events():
			var event: Dictionary = raw
			if String(event.get("switch_to_weapon", "")) == "WPN_AVENGER":
				remount_event = event
		if not remount_event.is_empty():
			break
	assert_false(remount_event.is_empty())
	sim.set_local_player_weapon(mounted_def, {}, true)
	assert_eq(int(sim.get_local_player_weapon_state().get("clip", -1)),
			mounted_clip_after,
			"the emplacement keeps its own clip state while nobody is attached")

	sim.restart()
	var restart_event: Dictionary = {}
	for raw in sim.drain_local_player_weapon_events():
		var event: Dictionary = raw
		if String(event.get("switch_to_weapon", "")) == "WPN_M4AUTO":
			restart_event = event
	assert_false(restart_event.is_empty(),
			"restart explicitly restores the saved personal presentation")
	assert_false(bool(restart_event.get("preserve_slot_state", true)),
			"restart installs a fresh personal slot epoch")
	assert_false(bool(sim.get_local_player_weapon_state().get("active", true)),
			"the mounted definition cannot pump the restored personal slot")
	sim.free()


func test_local_usegun_direct_swap_targets_latest_parent_without_switchto() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var first_gun := md.add_entity(NovaMissionData.KIND_ITEM, 101294,
			Vector3(2, 0, 0), Vector3.ZERO)
	var second_gun := md.add_entity(NovaMissionData.KIND_ITEM, 101295,
			Vector3(3, 0, 0), Vector3.ZERO)
	var third_gun := md.add_entity(NovaMissionData.KIND_ITEM, 101296,
			Vector3(3.5, 0, 0), Vector3.ZERO)
	assert_false(first_gun.is_empty())
	assert_false(second_gun.is_empty())
	assert_false(third_gun.is_empty())
	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	sim.set_item_seat_specs([
		{
			"type_id": 1294,
			"seats": [{"type": 3, "position": Vector3(0, -1, 1),
					"source_name": "UseGun"}],
			"primary_weapon": "WPN_AVENGER",
		},
		{
			"type_id": 1295,
			"seats": [{"type": 3, "position": Vector3(0, -1, 1),
					"source_name": "UseGun"}],
			"primary_weapon": "WPN_EMPLCD50",
		},
		{
			"type_id": 1296,
			"seats": [{"type": 3, "position": Vector3(0, -1, 1),
					"source_name": "UseGun"}],
			"primary_weapon": "WPN_EMPLCD50",
		},
	])
	assert_true(sim.load_from_mission_data(md))
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	sim.resolve_item_traits(item_db)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	var weapons := NovaWeaponDatabase.new()
	assert_eq(weapons.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/weapon.def")), OK)
	var personal: Dictionary = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	var first_mount: Dictionary = weapons.get_weapon(
			weapons.find_weapon("WPN_AVENGER"))
	var second_mount: Dictionary = weapons.get_weapon(
			weapons.find_weapon("WPN_EMPLCD50"))
	sim.set_local_player_weapon(personal, {})
	sim.drain_local_player_weapon_events()
	# Seed the listen-client rows and let the personal slot reach an attachable
	# state. The cull verdict is produced against this decoded presentation view.
	for _tick in range(80):
		sim.step()
		if int(sim.get_local_player_weapon_state().get("current", -1)) < 2:
			break
	var first_gun_index := int(first_gun["index"])
	var second_gun_index := int(second_gun["index"])
	var third_gun_index := int(third_gun["index"])

	assert_true(sim.local_player_toggle_mount())
	sim.step()
	var first_event: Dictionary = {}
	for raw in sim.drain_local_player_weapon_events():
		if String((raw as Dictionary).get(
				"switch_to_weapon", "")) == "WPN_AVENGER":
			first_event = raw
	assert_false(first_event.is_empty())
	sim.set_local_player_weapon(first_mount, {}, true)
	# Even a host-side resolution report cannot manufacture fpModel on a Def that
	# has none, and a different target Def must not inherit that model identity.
	sim.set_local_player_first_person_model_available(true)
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM,
			first_gun_index, NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"a live AVENGER MountSlot without gfx1 keeps its world model")
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM,
			second_gun_index, NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"mounting AVENGER cannot suppress the unrelated 50-cal parent")
	for _tick in range(80):
		sim.step()
		if int(sim.get_local_player_weapon_state().get("current", -1)) < 2:
			break

	assert_true(sim.local_player_toggle_mount(),
			"a nearby second gun is a direct mounted-seat swap")
	assert_eq(sim.get_local_player_weapon_name(), "WPN_AVENGER",
			"the old parent remains equipped until the rank commit")
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM,
			first_gun_index, NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"pre-commit swap restores the old parent world model")
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM,
			second_gun_index, NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"pre-commit target is not culled before its MountSlot is equipped")
	sim.step()
	var swap_event: Dictionary = {}
	for raw in sim.drain_local_player_weapon_events():
		if String((raw as Dictionary).get(
				"switch_to_weapon", "")) == "WPN_EMPLCD50":
			swap_event = raw
	assert_false(swap_event.is_empty(),
			"latest pending parent commits directly with no personal interlude")
	sim.set_local_player_weapon(second_mount, {}, true)
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM,
			first_gun_index, NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"the committed swap leaves the old AVENGER parent visible")
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM,
			second_gun_index, NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"the new authored model does not cull before its FP graphic resolves")
	sim.set_local_player_first_person_model_available(true)
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM,
			second_gun_index, NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 1,
			"the resolved EMPLCD50 replacement culls only its own world model")
	sim.step()
	assert_eq(int(sim.get_local_player_weapon_state().get("current", -1)), 0,
			"SWITCHRANK does not queue SWITCHTO onto the target parent slot")
	assert_true(bool(sim.get_local_player_weapon_state().get(
			"borrowed_usegun_slot", false)))

	assert_true(sim.local_player_toggle_mount(),
			"a closer third gun drives a second direct mounted-seat swap")
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM,
			second_gun_index, NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"the outgoing parent restores before the same-Def swap commits")
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM,
			third_gun_index, NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"the target parent stays visible before its slot is live")
	sim.step()
	var same_def_event: Dictionary = {}
	for raw in sim.drain_local_player_weapon_events():
		if String((raw as Dictionary).get(
				"switch_to_weapon", "")) == "WPN_EMPLCD50":
			same_def_event = raw
	assert_false(same_def_event.is_empty())
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM,
			second_gun_index, NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 0)
	assert_eq(_present_field_for_origin(sim, NovaMissionData.KIND_ITEM,
			third_gun_index, NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED), 1,
			"the same resolved Def model immediately suppresses the newly live parent")
	sim.free()


func test_death_during_usegun_draw_restores_personal_weapon() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_entity(NovaMissionData.KIND_ITEM, 101294,
			Vector3(2, 0, 0), Vector3.ZERO).is_empty())
	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([{
		"type_id": 1294,
		"seats": [{"type": 3, "position": Vector3(0, -1, 1),
				"source_name": "UseGun"}],
		"primary_weapon": "WPN_AVENGER",
	}])
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	var weapons := NovaWeaponDatabase.new()
	assert_eq(weapons.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/weapon.def")), OK)
	var mounted_def: Dictionary = weapons.get_weapon(
			weapons.find_weapon("WPN_AVENGER"))
	var personal_def: Dictionary = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	sim.set_local_player_weapon(personal_def, {})
	sim.drain_local_player_weapon_events()

	assert_true(sim.local_player_toggle_mount())
	sim.step()
	var mount_event: Dictionary = {}
	for raw in sim.drain_local_player_weapon_events():
		if String((raw as Dictionary).get(
				"switch_to_weapon", "")) == "WPN_AVENGER":
			mount_event = raw
	assert_false(mount_event.is_empty(),
			"the emplacement commits before its SWITCHTO draw")
	sim.set_local_player_weapon(mounted_def, {}, true)
	var fired_before := int(sim.get_local_player_weapon_state().get(
			"fired_serial", 0))
	sim.set_local_player_weapon_input(true, true, false)
	sim.debug_set_entity_health(0, 0)
	var restore_event: Dictionary = {}
	for _tick in range(160):
		sim.step()
		for raw in sim.drain_local_player_weapon_events():
			if String((raw as Dictionary).get(
					"switch_to_weapon", "")) == "WPN_M4AUTO":
				restore_event = raw
		if not restore_event.is_empty():
			break
	assert_false(restore_event.is_empty(),
			"forced detach during SWITCHTO eventually restores the personal slot")
	assert_eq(int(sim.get_local_player_weapon_state().get("fired_serial", 0)),
			fired_before, "a dead local gunner cannot fire the emplacement")
	sim.set_local_player_weapon(personal_def, {}, true)
	assert_false(bool(sim.get_local_player_weapon_state().get(
			"borrowed_usegun_slot", true)))
	sim.free()


func test_unarmed_offline_local_usegun_toggle_is_rejected() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_entity(NovaMissionData.KIND_ITEM, 101294,
			Vector3(2, 0, 0), Vector3.ZERO).is_empty())
	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([{
		"type_id": 1294,
		"seats": [{"type": 3, "position": Vector3(0, -1, 1),
				"source_name": "UseGun"}],
		"primary_weapon": "WPN_AVENGER",
	}])
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	sim.clear_local_player_weapon()
	assert_false(sim.local_player_toggle_mount(),
			"retail rejects offline player UseGun attach without EquippedSlot")
	sim.free()


func test_unarmed_offline_local_ordinary_seat_toggle_is_allowed() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_entity(NovaMissionData.KIND_ITEM, 101294,
			Vector3(2, 0, 0), Vector3.ZERO).is_empty())
	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([{
		"type_id": 1294,
		"seats": [{"type": 1, "position": Vector3(0, -1, 1),
				"source_name": "sitex00"}],
	}])
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	sim.clear_local_player_weapon()
	assert_true(sim.local_player_toggle_mount(),
			"the null EquippedSlot gate is UseGun-only, not a generic seat gate")
	sim.free()


func test_command_125_usegun_mount_renders_emplaced_pose() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var gun := md.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	var soldier := md.add_entity(NovaMissionData.KIND_ORGANIC, 102072, Vector3(10, 0, 0), Vector3.ZERO)
	assert_false(gun.is_empty())
	assert_false(soldier.is_empty())
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "waypoint_id", 125))
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "wp_number", int(gun["bms_id"])))

	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([
		{
			"type_id": 1294,
			"mount_config_valid": true,
			"mount_config": 3,
			"seats": [
				{"type": 3, "position": Vector3.ZERO, "yaw_offset": 0}
			],
		}
	])
	assert_true(sim.load_from_mission_data(md), "loaded command-125 UseGun mount")
	# The mounted anim state (67 = anim_emplaced) is asserted via the debug card below; the present
	# snapshot is the listen-server ClientState now (covered by nova_listen_server_test).
	var card: Dictionary = sim.get_entity_debug(0)
	assert_true(bool(card["mounted"]), "debug card marks UseGun occupant mounted")
	assert_eq(int(card["mount_type"]), 3, "seat type is UseGun/gunner")
	assert_true(bool(card["mount_config_valid"]))
	assert_eq(int(card["mount_config"]), 3)
	assert_true(bool(card["mount_target_config_valid"]))
	assert_eq(int(card["mount_target_config"]), 3)
	assert_eq(int(card["anim_state"]), 67)
	assert_eq(String(card["anim_key"]), "anim_emplaced")
	sim.free()


# (P7: the 3 no-net AI-pool present-snapshot tests were deleted — the present is now the listen-
#  server ClientState, covered by nova_listen_server_test; the editor no-net preview is retired.)



func test_foliage_mask_anchors_track_local_player_stance() -> void:
	# The hide-in-grass selection: only infantry with a stance bit set
	# ((net_stance_bits & 0x3) != 0) and no groundEntity anchor the distant
	# MODEL/depth-mask foliage tier [orig: Terrain_RenderSectorEntitiesBySide
	# @ 0x5c7dc2/0x5c7ded (MoveOrder & 0x300), groundEntity gate
	# @ 0x5c7dd5..0x5c7df7]. The local player's SELECT latches are the stance
	# writer [orig: Player_PackInputStateToEntity @ 0x4df6a7..0x4df6cd].
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	var spawn := Vector3(24.0, 0.0, -12.0)
	assert_true(sim.spawn_local_player(spawn, 0.0, 1))

	sim.step()
	assert_eq(sim.get_foliage_mask_anchor_positions().size(), 0,
		"a STANDING infantry entity never anchors the silhouette tier")

	assert_true(sim.request_local_player_stance(1))  # crouch (SELECT 169)
	sim.step()
	var crouched: PackedVector3Array = sim.get_foliage_mask_anchor_positions()
	assert_eq(crouched.size(), 1, "the crouched local player anchors the silhouette tier")
	if crouched.size() == 1:
		var player := sim.get_local_player_position()
		assert_lt(Vector2(crouched[0].x, crouched[0].z).distance_to(Vector2(player.x, player.z)), 0.1,
			"the anchor is the entity's own Godot-space ground position")

	assert_true(sim.request_local_player_stance(2))  # prone (SELECT 170)
	sim.step()
	assert_eq(sim.get_foliage_mask_anchor_positions().size(), 1,
		"prone anchors too - both MoveOrder stance bits gate the tier")

	assert_true(sim.request_local_player_stance(0))  # stand (SELECT 172)
	sim.step()
	assert_eq(sim.get_foliage_mask_anchor_positions().size(), 0,
		"standing back up empties the anchor list")
	sim.free()


func test_foliage_mask_anchors_ignore_standing_npcs() -> void:
	# Routed organics keep net_stance_bits 0 (the mirror only writes the LOCAL
	# player's SELECT latches; NPC stance never reaches the wire bits here), so
	# a demo mission full of standing walkers produces no anchors - matching
	# retail, where placed objects and standing soldiers leave MoveOrder's
	# stance bits clear [orig: the 0x5c7dc2 (flags & 0x300) reject].
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	assert_eq(sim.get_entity_count(), 2, "the demo mission has AI infantry to reject")
	for _i in range(4):
		sim.step()
	assert_eq(sim.get_foliage_mask_anchor_positions().size(), 0,
		"standing NPCs never anchor the hide-in-grass tier")
	sim.free()


func test_transport_play_flag() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	assert_false(sim.is_playing(), "starts paused")
	sim.set_playing(true)
	assert_true(sim.is_playing(), "play flag toggles")
	sim.free()

func test_bms_event_fires_through_binding() -> void:
	# The capability consolidation adds: a BMS event evaluates through the SAME binding that
	# runs the AI (the editor preview used to walk AI but never fire events). Build an
	# unconditional OutputText(77) event, tick once, and confirm the host-presentation effect
	# drains out of the shared World EffectLog.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, {"action_type": 6, "param1": 77}).is_empty())

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md), "loaded the scripted mission")
	assert_eq(sim.get_event_count(), 1, "one BMS event registered in the runtime")

	# A normal event's first processing pass is the 16th tick (the faithful quarter-list
	# round-robin cadence; see tests/mission/event_runtime_test.cpp).
	for _i in range(16):
		sim.step()
	var effects := sim.drain_effects()
	assert_eq(effects.size(), 1, "one presentation effect drained")
	assert_eq(String((effects[0] as Dictionary)["kind"]), "text", "OutputText -> text effect")
	assert_eq(int((effects[0] as Dictionary)["a"]), 77, "carries the string id")
	assert_true(sim.has_event_fired(0), "the event is marked fired")
	assert_true(sim.drain_effects().is_empty(), "drain cleared the log")
	sim.free()


# --- Read-only introspection (C8: the debug overlay's data feeds) ---

func test_logic_tick_advances_per_step_and_rewinds_on_restart() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	# The pre-mission pass already ran one tick at load, so pin DELTAS, never
	# absolutes.
	var t0: int = sim.get_logic_tick()
	sim.step()
	assert_eq(sim.get_logic_tick(), t0 + 1, "one step advances the logic tick by one")
	sim.step()
	sim.step()
	assert_eq(sim.get_logic_tick(), t0 + 3)
	sim.restart()
	assert_eq(sim.get_logic_tick(), t0, "Stop rewinds the clock to the play-start baseline")
	sim.free()


func test_variable_snapshots_are_bank_sized_and_track_writes() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	var mission: PackedInt32Array = sim.get_mission_variables_snapshot()
	var globals: PackedInt32Array = sim.get_global_variables_snapshot()
	var music: PackedInt32Array = sim.get_music_variables_snapshot()
	assert_eq(mission.size(), 512, "V0..V511")
	assert_eq(globals.size(), 256, "G0..G255")
	assert_eq(music.size(), 16, "M0..M15")

	sim.set_mission_variable(5, 42)
	sim.set_global_variable(3, -7)
	assert_eq(sim.get_mission_variables_snapshot()[5], 42, "snapshot reflects V writes")
	assert_eq(sim.get_global_variables_snapshot()[3], -7, "snapshot reflects G writes")
	assert_eq(sim.get_global_variable(3), -7, "the scalar G getter agrees")
	sim.free()


func test_fired_events_snapshot_matches_scalar() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, {"action_type": 6, "param1": 77}).is_empty())
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))

	var before: PackedByteArray = sim.get_fired_events_snapshot()
	assert_eq(before.size(), sim.get_event_count(), "one flag per event")
	assert_eq(int(before[0]), 0, "nothing fired before the first quarter pass")

	for _i in range(16):
		sim.step()
	var after: PackedByteArray = sim.get_fired_events_snapshot()
	assert_eq(int(after[0]), 1, "the fired flag sets")
	assert_eq(int(after[0]) == 1, sim.has_event_fired(0), "bulk and scalar reads agree")
	sim.free()


func test_entity_debug_card_carries_named_scalars() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	var card: Dictionary = sim.get_entity_debug(0)
	assert_false(card.is_empty(), "a live entity has a card")
	assert_eq(int(card["state"]), 16, "routed organic starts in GROUND_FOLLOWWP")
	assert_eq(String(card["state_name"]), "GROUND_FOLLOWWP", "...with its readable name")
	assert_eq(card["position"], sim.get_entity_position(0), "position matches the scalar getter")
	assert_almost_eq(float(card["yaw_deg"]), sim.get_entity_yaw_deg(0), 0.01)
	assert_eq(int(card["net_id"]), sim.get_entity_net_id(0))
	assert_eq(int(card["kind"]), sim.get_entity_kind(0))
	assert_true(bool(card["alive"]))
	assert_true(card.has("health") and card.has("ai_health"),
		"both health mirrors ride the card (they diverge under damage)")
	assert_true(bool(card["infantry"]), "demo organics route through the infantry motor")

	assert_true(sim.get_entity_debug(-1).is_empty(), "invalid index reads an empty card")
	assert_true(sim.get_entity_debug(999).is_empty())
	sim.free()


func test_debug_entity_mutations_report_missing_invalid_and_success() -> void:
	var sim := NovaSimulation.new()
	assert_eq(int(sim.debug_set_entity_health(0, 37)), ERR_INVALID_PARAMETER,
			"health reports that entity zero is absent from the empty AI pool")
	assert_eq(int(sim.debug_set_entity_position(0, Vector3.ONE)),
			ERR_INVALID_PARAMETER,
			"position reports that entity zero is absent from the empty AI pool")
	assert_eq(int(sim.debug_teleport_local_player(Vector3.ONE, 15.0, -5.0)),
			ERR_UNAVAILABLE, "teleport reports that the local player is absent")

	sim.build_demo_mission()
	var missing_index := sim.get_entity_count()
	assert_eq(int(sim.debug_set_entity_health(-1, 37)), ERR_INVALID_PARAMETER)
	assert_eq(int(sim.debug_set_entity_health(missing_index, 37)), ERR_INVALID_PARAMETER)
	assert_eq(int(sim.debug_set_entity_position(-1, Vector3.ONE)),
			ERR_INVALID_PARAMETER)
	assert_eq(int(sim.debug_set_entity_position(missing_index, Vector3.ONE)),
			ERR_INVALID_PARAMETER)
	assert_eq(int(sim.debug_teleport_local_player(Vector3.ONE, 15.0, -5.0)),
			ERR_UNAVAILABLE, "a live world without a local player is still unavailable")

	assert_eq(int(sim.debug_set_entity_health(0, 37)), OK)
	var card: Dictionary = sim.get_entity_debug(0)
	assert_eq(int(card.get("health", -1)), 37)
	assert_eq(int(card.get("ai_health", -1)), 37)

	var entity_mission_position := Vector3(6.0, 5.0, 7.0)
	assert_eq(int(sim.debug_set_entity_position(0, entity_mission_position)), OK)
	assert_lt(sim.get_entity_position(0).distance_to(Vector3(6.0, 7.0, -5.0)),
			0.001, "the successful move mutates the AI position mirror")
	var world_card: Dictionary = sim.get_world_entity_debug(int(card.get("net_id", 0)))
	var world_mission_position: Vector3 = world_card.get(
			"mission_position", Vector3.ZERO)
	assert_lt(world_mission_position.distance_to(entity_mission_position), 0.001,
			"the successful move mutates the registry position mirror")

	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var player_mission_position := Vector3(11.0, 3.0, -4.0)
	assert_eq(int(sim.debug_teleport_local_player(
			player_mission_position, 123.0, -17.0)), OK)
	assert_lt(sim.get_local_player_position().distance_to(Vector3(11.0, -4.0, -3.0)),
			0.001, "the successful teleport mutates the authoritative player position")
	assert_almost_eq(sim.get_local_player_yaw_deg(), 123.0, 0.01)
	assert_almost_eq(sim.get_local_player_pitch_deg(), -17.0, 0.01)
	sim.free()


func test_effect_state_lookup_uses_the_live_registry_not_the_ai_pool() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			NovaMissionData.KIND_BUILDING, 0, Vector3(3, 4, 5), Vector3(10, 20, 30))
	assert_false(placed.is_empty())
	var ssn := int(placed.get("bms_id", 0))
	assert_gt(ssn, 0)

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.get_entity_count(), 0,
			"a building has a registry slot but no AI-pool row")
	var state: PackedVector3Array = sim.get_entity_effect_state_for_ssn(ssn)
	assert_eq(state.size(), NovaSimulation.EFFECT_STATE_COUNT,
			"the SSN query reaches non-AI registry entities")
	if state.size() == NovaSimulation.EFFECT_STATE_COUNT:
		assert_eq(state[NovaSimulation.EFFECT_STATE_POSITION], Vector3(3, 5, -4),
				"effect position uses the canonical mission-to-Godot frame")
		assert_eq(state[NovaSimulation.EFFECT_STATE_ROTATION_DEG], Vector3(10, 20, 30),
				"effect orientation remains mission Euler degrees for the host adapter")
	assert_true(sim.get_entity_effect_state_for_ssn(0).is_empty(), "SSN zero is invalid")
	assert_true(sim.get_entity_effect_state_for_ssn(65536).is_empty(),
			"out-of-range SSNs must not wrap onto a different registry entity")
	sim.free()


func test_collision_backed_building_without_oobj_keeps_batch_visibility() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
		NovaMissionData.KIND_BUILDING, 102001, Vector3(0, 20, 0), Vector3.ZERO)
	assert_false(placed.is_empty())

	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path("res://../fixtures/def/items.def")), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(
		ProjectSettings.globalize_path("res://../fixtures/threedi/3di3/House.3di")), OK)
	assert_true(data.has_collision())
	assert_false(data.has_occlusion(), "fixture must exercise collision without OOBJ")
	var placer := ObjectDataPlacerStub.new(data)

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(item_db, placer), 1)
	sim.occlusion_init_mission()
	sim.run_occlusion_frame(Transform3D.IDENTITY, 90.0, 1.0, 0.05, 500.0, -100.0, false)
	var visibility: PackedInt64Array = sim.get_building_visibility()
	assert_eq(visibility.size(), 2, "collision-backed no-OOBJ building stays in the host batch")
	if visibility.size() == 2:
		assert_eq(int(visibility[0]), int(placed.get("bms_id", 0)))
		var packed := int(visibility[1])
		assert_eq(packed & 0xFFFFFFFF, 0xFFFFFFFF,
			"without a section map the host preserves every de-batched render part")
		assert_ne(packed & (1 << 32), 0, "the in-frustum building is visible")
	sim.free()


func test_occlusion_delta_calls_emit_changes_only() -> void:
	# The diff-based apply contract behind GameWorld's occlusion frame: the
	# first delta call after a frame emits the full verdict state, an unchanged
	# frame emits nothing, and reset_occlusion_apply_baseline() re-arms the
	# full emission (the A/B seam and host cache resets rely on it).
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
		NovaMissionData.KIND_BUILDING, 102001, Vector3(0, 20, 0), Vector3.ZERO)
	assert_false(placed.is_empty())
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path("res://../fixtures/def/items.def")), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(
		ProjectSettings.globalize_path("res://../fixtures/threedi/3di3/House.3di")), OK)
	var placer := ObjectDataPlacerStub.new(data)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(item_db, placer), 1)
	sim.occlusion_init_mission()
	sim.run_occlusion_frame(Transform3D.IDENTITY, 90.0, 1.0, 0.05, 500.0, -100.0, false)

	var first: PackedInt64Array = sim.get_building_visibility_changes()
	assert_eq(first.size(), 2, "the first delta call emits the building's state")
	assert_eq(sim.get_render_culled_changes(), PackedInt32Array([0, 0]),
			"no entities to cull in this mission")

	sim.run_occlusion_frame(Transform3D.IDENTITY, 90.0, 1.0, 0.05, 500.0, -100.0, false)
	assert_eq(sim.get_building_visibility_changes().size(), 0,
			"an unchanged frame emits no building deltas")
	assert_eq(sim.get_render_culled_changes(), PackedInt32Array([0, 0]),
			"an unchanged frame emits no culled deltas")

	sim.reset_occlusion_apply_baseline()
	assert_eq(sim.get_building_visibility_changes(), first,
			"a baseline reset re-arms the full emission")
	assert_true(bool(sim.entity_present_visible(int(placed.get("bms_id", 0)))),
			"a live placed building reads as present-visible")
	assert_true(bool(sim.entity_present_visible(424242)),
			"an unknown bms id defaults visible (never blocks a show)")
	sim.free()


func test_first_husk_kz_userpoints_feed_death_blast_traits() -> void:
	# Retail walks every exact, case-insensitive "KZ" point on the active first
	# husk and queues a radius-5 blast there. Keep the main and final models out
	# of the witness so reading either one cannot accidentally satisfy the test.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			NovaMissionData.KIND_BUILDING, 105002, Vector3.ZERO, Vector3.ZERO)
	assert_false(placed.is_empty())
	var bms_id := int(placed.get("bms_id", 0))

	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var main_data := NovaObjectData.new()
	assert_eq(main_data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3/House.3di")), OK)
	# Armry01 is a committed, loadable 3DI3 model with two user points. Relabel
	# those two 16-byte name fields in a temporary copy so the integration test
	# owns an exact multi-KZ witness without checking in another binary fixture.
	var source_path := ProjectSettings.globalize_path(
			"res://../fixtures/3dp/armry01/Armry01.3di")
	var bytes := FileAccess.get_file_as_bytes(source_path)
	for source_name in ["Armory", "Ground"]:
		var needle := String(source_name).to_ascii_buffer()
		var name_offset := -1
		for offset in range(bytes.size() - needle.size() + 1):
			var matches := true
			for byte_index in range(needle.size()):
				if bytes[offset + byte_index] != needle[byte_index]:
					matches = false
					break
			if matches:
				name_offset = offset
				break
		assert_gte(name_offset, 0, "source user-point name is present")
		if name_offset < 0:
			continue
		for byte_index in range(16):
			bytes[name_offset + byte_index] = 0
		var replacement := "KZ" if source_name == "Armory" else "kz"
		bytes[name_offset] = replacement.unicode_at(0)
		bytes[name_offset + 1] = replacement.unicode_at(1)
	var kz_fixture_path := ProjectSettings.globalize_path(
			"user://nova_simulation_kz_husk.3di")
	var fixture_file := FileAccess.open(kz_fixture_path, FileAccess.WRITE)
	assert_not_null(fixture_file)
	if fixture_file != null:
		fixture_file.store_buffer(bytes)
		fixture_file.close()
	var husk_data := NovaObjectData.new()
	assert_eq(husk_data.open_file(kz_fixture_path), OK)
	assert_eq(DirAccess.remove_absolute(kz_fixture_path), OK)

	var expected := PackedVector3Array()
	for point_index in range(husk_data.get_user_point_count()):
		var info: Dictionary = husk_data.get_user_point_info(point_index)
		if String(info.get("name", "")).nocasecmp_to("KZ") == 0:
			var model_point: Vector3 = info.get("position", Vector3.ZERO)
			# Public model space is (source y, source z, source x); the
			# destruction core consumes mission-local (forward, lateral, up).
			expected.push_back(Vector3(model_point.z, model_point.x, model_point.y))
	assert_gt(expected.size(), 1, "fixture carries a real multi-point KZ bank")

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	var placer := GraphicDataPlacerStub.new({
		"Barrel1": main_data,
		"Barrel1X": husk_data,
		# Deliberately omit Barrel1XF: KZ belongs to the first husk, while
		# the final husk is only the preferred death-piece model.
	})
	assert_eq(sim.resolve_collision_instances(item_db, placer), 1)
	var debug := sim.get_destruction_debug(bms_id)
	assert_true(bool(debug.get("husk_model_loaded", false)),
			"a successfully opened first husk supplies the retail live-model gate")
	assert_eq(int(debug.get("kz_point_count", -1)), expected.size(),
			"all first-husk KZ points reach the destruction traits")
	var actual: PackedVector3Array = debug.get("kz_points", PackedVector3Array())
	assert_eq(actual.size(), expected.size())
	for point_index in range(mini(actual.size(), expected.size())):
		assert_eq(actual[point_index], expected[point_index],
				"KZ point %d preserves retail mission-local axes" % point_index)
	sim.free()

	# Retail reads entity+52 huskModel for this walk. A final-only definition may
	# use huskFinal for pieces (and our legacy collision fallback), but it must not
	# mine that model for KZ anchors; the empty bank selects the origin fallback.
	var final_only_path := ProjectSettings.globalize_path(
			"user://nova_simulation_final_only_husk_items.def")
	var final_only_file := FileAccess.open(final_only_path, FileAccess.WRITE)
	assert_not_null(final_only_file)
	if final_only_file == null:
		return
	final_only_file.store_string(
			"begin \"Final-only KZ witness\"\n"
			+ "  id 105099\n"
			+ "  type object\n"
			+ "  graphic Barrel1\n"
			+ "  sid final_only_kz\n"
			+ "  huskfinal Barrel1XF\n"
			+ "  hp 75\n"
			+ "  kz 4.0\n"
			+ "  unit_type 6\n"
			+ "end\n")
	final_only_file.close()
	var final_only_db := NovaItemDatabase.new()
	assert_eq(final_only_db.load(final_only_path), OK)
	assert_eq(DirAccess.remove_absolute(final_only_path), OK)
	assert_true(final_only_db.get_husk(105099).is_empty())
	assert_eq(final_only_db.get_huskfinal(105099), "Barrel1XF")

	var final_only_md := NovaMissionData.new()
	assert_eq(final_only_md.create_default(), OK)
	var final_only_placed := final_only_md.add_entity(
			NovaMissionData.KIND_BUILDING, 105099, Vector3.ZERO, Vector3.ZERO)
	assert_false(final_only_placed.is_empty())
	var final_only_sim := NovaSimulation.new()
	assert_true(final_only_sim.load_from_mission_data(final_only_md))
	final_only_sim.resolve_item_traits(final_only_db)
	assert_eq(final_only_sim.resolve_collision_instances(
			final_only_db, GraphicDataPlacerStub.new({
				"Barrel1": main_data,
				"Barrel1XF": husk_data,
			})), 1)
	var final_only_debug := final_only_sim.get_destruction_debug(
			int(final_only_placed.get("bms_id", 0)))
	assert_true(bool(final_only_debug.get("husk_model_loaded", false)),
			"a successfully opened final-only husk also supplies the retail gate")
	assert_eq(int(final_only_debug.get("kz_point_count", -1)), 0,
			"huskFinal alone does not replace retail's first-stage KZ source")
	final_only_sim.free()

	# Authored names do not stand in for the live retail pointer. A placer that
	# resolves the main graphic but neither husk leaves the callback gate clear.
	var missing_sim := NovaSimulation.new()
	assert_true(missing_sim.load_from_mission_data(md))
	missing_sim.resolve_item_traits(item_db)
	assert_eq(missing_sim.resolve_collision_instances(
			item_db, GraphicDataPlacerStub.new({"Barrel1": main_data})), 1)
	var missing_debug := missing_sim.get_destruction_debug(bms_id)
	assert_true(bool(missing_debug.get("has_husk", false)),
			"items.def still records the authored husk name")
	assert_false(bool(missing_debug.get("husk_model_loaded", true)),
			"missing/corrupt husk assets leave the retail live-model gate clear")
	missing_sim.free()


func test_face_only_cfac_model_attaches_for_projectile_raycast() -> void:
	# Retail collision construction and the projectile face walker do not
	# require BVOL. Bird1 is a committed face-only witness (242 CFAC, 0 BVOL);
	# rejecting it here silently degrades authored bullet geometry to a sphere.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
		NovaMissionData.KIND_BUILDING, 102001, Vector3(0, 200, 0), Vector3.ZERO)
	assert_false(placed.is_empty())

	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path("res://../fixtures/def/items.def")), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(
		ProjectSettings.globalize_path("res://../fixtures/threedi/3di3/Bird1.3di")), OK)

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(item_db, ObjectDataPlacerStub.new(data)), 1,
		"face-only CFAC remains a real collision model")
	var entities: Array = sim.get_hitbox_debug().get("entities", [])
	assert_eq(entities.size(), 1)
	if entities.size() == 1:
		assert_eq(int((entities[0] as Dictionary).get("face_total", 0)), 242,
			"all authored Bird1 faces reach the projectile walker")
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	assert_true((sim.get_hitbox_debug().get("entities", []) as Array).is_empty(),
		"the heavier non-organic mesh view retains its local 80-unit range")
	sim.free()


func test_panm_liveness_is_scoped_to_the_active_transform_family() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
		"res://../fixtures/3dp/armry01/Armry01.3di")), OK)
	for i in range(data.get_part_anim_count(0) - 1, -1, -1):
		assert_true(data.delete_part_anim(0, i))
	var anim := data.add_part_anim(0, 0)
	assert_eq(anim, 0)
	var tracks := [
		"rotation_x", "rotation_y", "rotation_z",
		"scale_x", "scale_y", "scale_z",
		"translation",
	]
	var case := func(flags: int, live_tracks: Array) -> bool:
		assert_eq(data.set_part_animation_flags(0, anim, flags), OK)
		for track in tracks:
			assert_true(data.set_part_anim_track_field(
				0, anim, track, "control", 0))
		for track in live_tracks:
			assert_true(data.set_part_anim_track_field(
				0, anim, track, "control", 0x10))
		return data.has_live_panm_for_lod(0)

	assert_true(case.call(1 << 8, []), "spinner uses raw coefficients")
	assert_true(case.call(3 << 8, []), "view rotation type 3 is evaluated")
	assert_true(case.call(4 << 8, []), "view rotation type 4 is evaluated")
	assert_true(case.call(2 << 8, ["rotation_z"]),
		"rotation family samples rotation tracks")
	assert_false(case.call(2 << 8, ["scale_x"]),
		"rotation ignores an unrelated live scale track")
	assert_false(case.call(1, ["scale_y"]),
		"uniform scale type samples only scale_x")
	assert_true(case.call(1, ["scale_x"]))
	assert_true(case.call(2, ["scale_y"]),
		"axis scale samples all three scale tracks")
	assert_true(case.call(1 << 24, ["translation"]))
	assert_false(case.call(1 << 16, []),
		"rotation-reversed without a rotation family is inert")


func test_collision_uses_effective_lod0_and_never_first_live_lod() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
		"res://../fixtures/3dp/Pmpjk01/Pmpjk01.3di")), OK)
	var lod_count := int(data.get_summary().get("lod_count", 0))
	assert_gt(lod_count, 1, "fixture needs a second visual LOD")
	if lod_count <= 1:
		return

	# A nonempty local block wins even when inert; model-level live rows must
	# not leak through it for canonical LOD0.
	for i in range(data.get_part_anim_count(0) - 1, -1, -1):
		assert_true(data.delete_part_anim(0, i))
	var inert := data.add_part_anim(0, 0)
	assert_eq(inert, 0)
	assert_false(data.has_live_panm_for_lod(0))
	assert_eq(Array(data.get_effective_panm_targets(0)), [0],
		"inert local row suppresses the model-level fallback")

	# Make only LOD1 live. Visual de-batching may see it, but retail Generic
	# collision always uses canonical LOD0 COBJ ordinals.
	for i in range(data.get_part_anim_count(1) - 1, -1, -1):
		assert_true(data.delete_part_anim(1, i))
	var live := data.add_part_anim(1, 0)
	assert_eq(live, 0)
	assert_true(data.set_part_anim_channel_enabled(1, live, "rotation", true))
	assert_true(data.set_part_anim_channel_mode(
		1, live, "rotation", "z", "sine_wave", -1))
	assert_true(data.set_part_anim_channel_values(
		1, live, "rotation", "z", 0.0, 90.0, 1.0))
	assert_true(data.has_live_panm_for_lod(1))
	assert_eq(data.get_live_panm_lod(), 1)

	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_entity(
		NovaMissionData.KIND_BUILDING, 102001,
		Vector3.ZERO, Vector3.ZERO).is_empty())
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
		"res://../fixtures/def/items.def")), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(
		item_db, ObjectDataPlacerStub.new(data)), 1)
	sim.debug_set_panm_time_ms(0)
	var before: PackedVector3Array = (
		(sim.get_hitbox_debug().get("entities", [])[0] as Dictionary)
		.get("tris", PackedVector3Array()))
	sim.debug_set_panm_time_ms(640)
	var after: PackedVector3Array = (
		(sim.get_hitbox_debug().get("entities", [])[0] as Dictionary)
		.get("tris", PackedVector3Array()))
	assert_eq(after, before, "LOD1 PANM never transforms model-level COBJ")
	sim.free()


func test_listen_snapshot_exports_authoritative_part_anim_channels() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			NovaMissionData.KIND_ITEM, 105004, Vector3.ZERO, Vector3.ZERO)
	var ssn := int(placed.get("bms_id", 0))
	assert_gt(ssn, 0)
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, {
		"action_type": 21,
		"action_sub_type": 34,
		"param1": ssn,
		"param2": 1,
		"param3": 1,
		"param4": 65536,
	}).is_empty())
	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	sim.set_item_seat_specs([{
		"type_id": 5004,
		"seats": [{
			"type": 2,
			"position": Vector3.ZERO,
			"source_name": "ctrlx00",
		}],
	}])
	assert_true(sim.load_from_mission_data(md))
	for _tick in range(80):
		sim.step()
	assert_gt(_present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, int(placed["index"]),
			NovaSimulation.PF_ACTIVE1), 0)
	assert_eq(_present_phase_for_origin(
			sim, NovaMissionData.KIND_ITEM, int(placed["index"]), 1), 65536,
			"SP/host presentation receives the authoritative PLAYPARTANIM pose")
	sim.free()


func test_present_part_anim_phase_transport_preserves_every_dword_bit() -> void:
	# PackedFloat32 cannot carry every signed dword numerically. The snapshot
	# schema transports two exact 16-bit integers, with high16+1 reserving zero
	# for "unpublished", so wrapping PLAYPARTANIM states survive unchanged.
	var snapshot := PackedFloat32Array()
	snapshot.resize(NovaSimulation.PF_ACTIVE2 + 1)
	var phases := [
		0,
		65536,
		0x041893ab, # not exactly representable as one numeric float32
		2147483647,
		-1,
		-2147482600,
		-2147483648,
	]
	for channel in [1, 2]:
		var phase_field: int = NovaSimulation.PF_PHASE1 + (channel - 1) * 2
		var active_field: int = NovaSimulation.PF_ACTIVE1 + (channel - 1) * 2
		for phase in phases:
			snapshot[phase_field] = float(int(phase) & 0xffff)
			snapshot[active_field] = float(((int(phase) >> 16) & 0xffff) + 1)
			assert_eq(
				NovaSimulation.decode_present_part_anim_phase(
					snapshot, 0, channel),
				int(phase),
				"channel %d preserves signed dword %d" % [channel, phase])
		snapshot[active_field] = 0.0
		assert_eq(
			NovaSimulation.decode_present_part_anim_phase(
				snapshot, 0, channel),
			0,
			"zero high-word code remains the unpublished sentinel")


func test_fast_rope_suppresses_only_special1_publication() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			NovaMissionData.KIND_ITEM, 105006, Vector3.ZERO, Vector3.ZERO)
	var ssn := int(placed.get("bms_id", 0))
	assert_gt(ssn, 0)
	assert_false(md.add_event(0, 0, 0).is_empty())
	for channel in [1, 2]:
		assert_false(md.add_event_action(0, {
			"action_type": 21,
			"action_sub_type": 34,
			"param1": ssn,
			"param2": channel,
			"param3": 1,
			"param4": 65536,
		}).is_empty())

	var item_db := _fast_rope_item_db()
	assert_not_null(item_db)
	if item_db == null:
		return
	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	sim.set_item_seat_specs([{
		"type_id": 5006,
		"seats": [{
			"type": 2,
			"position": Vector3.ZERO,
			"source_name": "ctrlx00",
		}],
	}])
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	for _tick in range(80):
		sim.step()
	assert_eq(_present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, int(placed["index"]),
			NovaSimulation.PF_ACTIVE1), 0,
			"FastRope releases VEHICLE_SPECIAL1 ownership")
	assert_gt(_present_field_for_origin(
			sim, NovaMissionData.KIND_ITEM, int(placed["index"]),
			NovaSimulation.PF_ACTIVE2), 0,
			"VEHICLE_SPECIAL2 remains unconditionally published")
	assert_eq(_present_phase_for_origin(
			sim, NovaMissionData.KIND_ITEM, int(placed["index"]), 2), 65536)
	assert_false(sim.get_entity_part_anim_active(0, 1))
	assert_true(sim.get_entity_part_anim_active(0, 2))
	sim.free()


func test_listen_snapshot_attachment_follows_animated_userpoint() -> void:
	# Build a deterministic VEHICLE_SPECIAL1 track on the M1A1's real turret part,
	# then hang the synthetic ewep from a userpoint owned by that part. A rigid
	# parent-local reconstruction stays at the authored point; the authoritative
	# mounted pose carries it four metres with the live part.
	var data := _model_with_special1_in_local_slot_zero(
			"res://../fixtures/3dp/dm1a1/dm1a1.3di")
	var anchor_index := -1
	var anchor_info := {}
	for index in range(data.get_user_point_count()):
		var candidate: Dictionary = data.get_user_point_info(index)
		if String(candidate.get("name", "")).to_lower() == "ewep01":
			anchor_index = index
			anchor_info = candidate
			break
	assert_gte(anchor_index, 0, "M1A1 fixture has its authored ewep01 attachment point")
	if anchor_index < 0:
		return
	var anchor_part := int(anchor_info.get("subobject", -1))
	assert_gte(anchor_part, 0, "ewep01 is bound to a model part")
	if anchor_part < 0:
		return
	for index in range(data.get_part_anim_count(0) - 1, -1, -1):
		assert_true(data.delete_part_anim(0, index))
	var anim := data.add_part_anim(0, anchor_part)
	assert_eq(anim, 0)
	assert_true(data.set_part_anim_channel_enabled(
			0, anim, "translation", true))
	assert_true(data.set_part_anim_channel_mode(
			0, anim, "translation", "x", "control_register", 0))
	assert_true(data.set_part_anim_channel_values(
			0, anim, "translation", "x", 0.0, 4.0, 0.0))

	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			NovaMissionData.KIND_ITEM, 101291, Vector3.ZERO, Vector3.ZERO)
	var ssn := int(placed.get("bms_id", 0))
	assert_gt(ssn, 0)
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, {
		"action_type": 21,
		"action_sub_type": 34,
		"param1": ssn,
		"param2": 1,
		"param3": 1,
		"param4": 65536,
	}).is_empty())
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_trigger(1, {
		"main_type": 4,
		"sub_type": 1,
		"param1": 7,
		"param2": 1,
	}).is_empty())
	assert_false(md.add_event_action(1, {
		"action_type": 20,
		"param1": ssn,
	}).is_empty())

	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	sim.set_item_seat_specs([{
		"type_id": 1291,
		"model_data": data,
		"seats": [{
			"type": 2,
			"position": Vector3.ZERO,
			"source_name": "ctrlx00",
		}],
		"emplacement_attachments": [{
			"item_id": 101419,
			"stored_slot": 1,
			"anchor_found": true,
			"bone_index": anchor_index + 1,
			"source_name": String(anchor_info.get("name", "")),
			"local": ItemSeatSpecs.seat_local_from_user_point_position(
					anchor_info.get("position", Vector3.ZERO)),
		}],
	}])
	assert_true(sim.load_from_mission_data(md))
	# This attachment lifecycle needs the real carrier/child rows (health,
	# class, and NoNetworkCallback), not the isolated FastRope fixture used by
	# the neighboring publication tests.
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	sim.resolve_item_traits(item_db)

	# First fold materializes the synthetic row before the scripted animation has
	# reached its endpoint. Retain that baseline so the assertion cannot pass on
	# a root-only follow implementation.
	sim.step()
	var stride := sim.get_present_stride()
	var snapshot := sim.get_present_snapshot()
	var initial_position := Vector3.INF
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + NovaSimulation.PF_TYPE_ID]) == 1419:
			initial_position = Vector3(
					snapshot[base + NovaSimulation.PF_POS_X],
					snapshot[base + NovaSimulation.PF_POS_Y],
					snapshot[base + NovaSimulation.PF_POS_Z])
			break
	assert_true(initial_position.is_finite(), "synthetic ewep reached the listen client")
	for _tick in range(79):
		sim.step()
	assert_eq(_present_phase_for_origin(
			sim, NovaMissionData.KIND_ITEM, int(placed["index"]), 1), 65536,
			"carrier reached the scripted live PANM endpoint")
	snapshot = sim.get_present_snapshot()
	var final_position := Vector3.INF
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + NovaSimulation.PF_TYPE_ID]) == 1419:
			final_position = Vector3(
					snapshot[base + NovaSimulation.PF_POS_X],
					snapshot[base + NovaSimulation.PF_POS_Y],
					snapshot[base + NovaSimulation.PF_POS_Z])
			break
	assert_true(final_position.is_finite(), "animated attachment remains presented")
	if initial_position.is_finite() and final_position.is_finite():
		assert_gt(final_position.distance_to(initial_position), 3.9,
				"presented attachment follows its animated userpoint, not only the parent root")
		var expected: Vector3 = anchor_info.get("position", Vector3.ZERO) + Vector3(4, 0, 0)
		assert_lt(final_position.distance_to(expected), 0.002,
				"host snapshot uses the authoritative mounted child pose")

	# A zero-health vehicle compact legitimately retires the decoded attachment
	# subtree. Stop restores the authoritative baseline; its fresh decoded view
	# must replay the load stream because this child has no live compact of its own.
	sim.set_mission_variable(7, 1)
	var retired := false
	for _tick in range(80):
		sim.step()
		snapshot = sim.get_present_snapshot()
		retired = true
		for record in range(snapshot.size() / stride):
			if int(snapshot[record * stride + NovaSimulation.PF_TYPE_ID]) == 1419:
				retired = false
				break
		if retired:
			break
	assert_true(retired,
			"decoded zero-health carrier retires the synthetic child subtree")

	sim.restart()
	snapshot = sim.get_present_snapshot()
	var restored := false
	for record in range(snapshot.size() / stride):
		if int(snapshot[record * stride + NovaSimulation.PF_TYPE_ID]) == 1419:
			restored = true
			break
	assert_true(restored,
			"restart immediately replays restored NoNetworkCallback attachments")

	# The first live 0x0A after replay must use the re-applied authoritative
	# items.def classes. If restore had reverted the carrier callback width, this
	# fold would desynchronize and lose/scatter the following child row.
	sim.step()
	snapshot = sim.get_present_snapshot()
	var live_carrier := false
	var live_child := false
	for record in range(snapshot.size() / stride):
		var type_id := int(snapshot[record * stride + NovaSimulation.PF_TYPE_ID])
		live_carrier = live_carrier or type_id == 1291
		live_child = live_child or type_id == 1419
	assert_true(live_carrier and live_child,
			"post-restart 0x0A keeps carrier and attachment class widths aligned")
	sim.free()


func _fast_rope_collision_moved_vertices(register_name: String, channel: int) -> int:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			NovaMissionData.KIND_ITEM, 105006, Vector3.ZERO, Vector3.ZERO)
	var ssn := int(placed.get("bms_id", 0))
	assert_gt(ssn, 0)
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, {
		"action_type": 21, "action_sub_type": 34,
		"param1": ssn, "param2": channel,
		"param3": 1, "param4": 65536,
	}).is_empty())

	var item_db := _fast_rope_item_db()
	assert_not_null(item_db)
	if item_db == null:
		return 0
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/3dp/armry01/Armry01.3di")), OK)
	assert_true(data.set_control_register_name(0, register_name))
	for i in range(data.get_part_anim_count(0) - 1, -1, -1):
		assert_true(data.delete_part_anim(0, i))
	var anim := data.add_part_anim(0, 1)
	assert_eq(anim, 0)
	assert_true(data.set_part_anim_channel_enabled(
			0, anim, "translation", true))
	assert_true(data.set_part_anim_channel_mode(
			0, anim, "translation", "x", "control_register", 0))
	assert_true(data.set_part_anim_channel_values(
			0, anim, "translation", "x", 0.0, 4.0, 0.0))

	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([{
		"type_id": 5006,
		"seats": [{
			"type": 2,
			"position": Vector3.ZERO,
			"source_name": "ctrlx00",
		}],
	}])
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	var collision_placer := GraphicDataPlacerStub.new({
		"StaticCrate1": data,
	})
	assert_eq(sim.resolve_collision_instances(item_db, collision_placer), 1)
	var before_rows: Array = sim.get_hitbox_debug().get("entities", [])
	assert_eq(before_rows.size(), 1)
	if before_rows.size() != 1:
		sim.free()
		return 0
	var before: PackedVector3Array = (before_rows[0] as Dictionary).get(
			"tris", PackedVector3Array())
	for _tick in range(80):
		sim.step()
	var after_rows: Array = sim.get_hitbox_debug().get("entities", [])
	assert_eq(after_rows.size(), 1)
	if after_rows.size() != 1:
		sim.free()
		return 0
	var after: PackedVector3Array = (after_rows[0] as Dictionary).get(
			"tris", PackedVector3Array())
	assert_eq(after.size(), before.size())
	var moved := 0
	for i in before.size():
		if before[i].distance_to(after[i]) > 3.99:
			moved += 1
	sim.free()
	return moved


func test_fast_rope_collision_publishes_special2_but_not_special1() -> void:
	assert_eq(_fast_rope_collision_moved_vertices("VEHICLE_SPECIAL1", 1), 0,
			"FastRope suppresses SPECIAL1 in authoritative collision evaluation")
	assert_eq(_fast_rope_collision_moved_vertices("VEHICLE_SPECIAL2", 2), 639,
			"SPECIAL2 remains published through the same collision CTRL dictionary")


func test_animated_collision_uses_retail_section_ordinal_headlessly() -> void:
	# Armry COBJ parents are all 0; face counts are [24, 213, 1, 12].
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
		NovaMissionData.KIND_ITEM, 105004, Vector3.ZERO, Vector3.ZERO)
	var ssn := int(placed.get('bms_id', 0))
	assert_gt(ssn, 0)
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, {
		'action_type': 21, 'action_sub_type': 34,
		'param1': ssn, 'param2': 1,
		'param3': 1, 'param4': 65536,
	}).is_empty())

	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
		'res://../fixtures/def/items.def')), OK)
	var data := _model_with_special1_in_local_slot_zero(
			"res://../fixtures/3dp/armry01/Armry01.3di")
	assert_true(data.has_collision())
	for i in range(data.get_part_anim_count(0) - 1, -1, -1):
		assert_true(data.delete_part_anim(0, i))
	var anim := data.add_part_anim(0, 1)
	assert_eq(anim, 0)
	assert_true(data.set_part_anim_channel_enabled(
		0, anim, 'translation', true))
	assert_true(data.set_part_anim_channel_mode(
		0, anim, 'translation', 'x',
		'control_register', 0))
	assert_true(data.set_part_anim_channel_values(
		0, anim, 'translation', 'x',
		0.0, 4.0, 0.0))

	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([{
		'type_id': 5004,
		'seats': [{
			'type': 2,
			'position': Vector3.ZERO,
			'source_name': 'ctrlx00',
		}],
	}])
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.get_entity_count(), 1)
	assert_eq(sim.resolve_collision_instances(
		item_db, ObjectDataPlacerStub.new(data)), 1)
	var before_debug: Array = sim.get_hitbox_debug().get(
		'entities', [])
	assert_eq(before_debug.size(), 1)
	assert_eq(int((before_debug[0] as Dictionary).get(
		'face_total', 0)), 250)
	var before: PackedVector3Array = (before_debug[0] as Dictionary).get(
		'tris', PackedVector3Array())
	assert_eq(before.size(), 250 * 3)

	# No present pass/render node: collision reads authoritative AI state.
	for _tick in range(80):
		sim.step()
	assert_eq(sim.get_entity_part_anim_phase(0, 1), 65536)
	var after_debug: Array = sim.get_hitbox_debug().get(
		'entities', [])
	assert_eq(after_debug.size(), 1)
	var after: PackedVector3Array = (after_debug[0] as Dictionary).get(
		'tris', PackedVector3Array())
	assert_eq(after.size(), before.size())
	var moved := 0
	var stayed := 0
	var partial := 0
	for i in before.size():
		var distance := before[i].distance_to(after[i])
		if distance > 3.99:
			assert_almost_eq(distance, 4.0, 0.002)
			moved += 1
		elif distance < 0.002:
			stayed += 1
		else:
			partial += 1
	assert_eq(moved, 639, "only ordinal 1 moves")
	assert_eq(stayed, 111)
	assert_eq(partial, 0)
	sim.free()


func test_organic_collision_samples_current_skeletal_pose_headlessly() -> void:
	# BINOC is a committed 19-bone, three-frame BAD. Pair it with CharModel's
	# canonical 19-row model table/COBJ block so the real NovaSkeletalAnim ->
	# NovaSimulation -> CollisionWorld path can be tested without retail assets.
	var tmp := ProjectSettings.globalize_path(
			"res://.godot/person_collision_pose_test")
	assert_eq(DirAccess.make_dir_recursive_absolute(tmp), OK)
	var bad_out := FileAccess.open(tmp.path_join("BINOC.bad"), FileAccess.WRITE)
	assert_not_null(bad_out)
	if bad_out == null:
		return
	bad_out.store_buffer(FileAccess.get_file_as_bytes(
			"res://../fixtures/bad/BINOC.bad"))
	bad_out.close()
	# BINOC is a static three-frame clip. Turn BN15/head frame 1 into an
	# identity quaternion at its parser-pinned rotation offset (1324 + 16)
	# to make a deterministic moving-bone fixture while retaining its real
	# 19-bone hierarchy and every other shipped byte.
	var moving_bad := FileAccess.open(
			tmp.path_join("BINOC.bad"), FileAccess.READ_WRITE)
	assert_not_null(moving_bad)
	if moving_bad == null:
		return
	moving_bad.seek(1340)
	moving_bad.store_float(0.0)
	moving_bad.store_float(0.0)
	moving_bad.store_float(0.0)
	moving_bad.store_float(1.0)
	moving_bad.close()
	var adm_out := FileAccess.open(
			tmp.path_join("person_collision.adm"), FileAccess.WRITE)
	assert_not_null(adm_out)
	if adm_out == null:
		return
	var quote := String.chr(34)
	adm_out.store_string(
			"anim_reset %sBINOC.bad%s\n" % [quote, quote] +
			"anim_idle %sBINOC.bad%s\n" % [quote, quote] +
			"anim_idle_2 %sBINOC.bad%s\n" % [quote, quote])
	adm_out.close()

	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(tmp), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3/CharModel.3di")), OK)
	assert_true(data.has_collision())
	var skeletal := NovaSkeletalAnim.new()
	assert_true(skeletal.load_from_bad_files(
			root, "BINOC.bad", {"anim_idle": "BINOC.bad"},
			data.get_bone_origins(), data.get_bone_parents()),
			"19-bone collision rig loads: %s" % skeletal.get_last_error())
	assert_eq(skeletal.get_bone_count(), 19)
	var direct_a: Array = skeletal.eval_pose("anim_idle", 0.0)
	var direct_b: Array = skeletal.eval_pose("anim_idle", 1.0 / 30.0)
	var fixture_moved := 0
	for i in mini(direct_a.size(), direct_b.size()):
		if not (direct_a[i] as Transform3D).is_equal_approx(
				direct_b[i] as Transform3D):
			fixture_moved += 1
	assert_gt(fixture_moved, 0, "fixture must contain an animated bone")

	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_entity(
			NovaMissionData.KIND_ORGANIC, 105311,
			Vector3(10, 0, 0), Vector3.ZERO).is_empty())
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_gt(sim.set_infantry_anim_map(root, "person_collision.adm"), 0)
	assert_eq(sim.resolve_collision_instances(
			item_db, SkeletalDataPlacerStub.new(data, skeletal)), 1)

	var before: Array = sim.get_hitbox_debug().get("organics", [])
	assert_eq(before.size(), 19, "one posed sphere per CharModel COBJ/bone")
	var before_by_section := {}
	for value in before:
		var row: Dictionary = value
		assert_false(bool(row.get("fallback", true)))
		var pos := row.get("pos", Vector3.ZERO) as Vector3
		assert_lt(pos.distance_to(Vector3(10, 0, 0)), 3.0,
				"bind pose applies entity translation exactly once")
		before_by_section[int(row.get("section", -1))] = pos
	assert_true(before_by_section.has(14), "head COBJ/bone is present")

	# No presentation node or Skeleton3D is involved: advancing authoritative
	# clip_phase must move the CollisionWorld/F3 matrices directly.
	for _tick in 2:
		sim.step()
	var after: Array = sim.get_hitbox_debug().get("organics", [])
	assert_eq(after.size(), 19)
	var moved_sections := 0
	for value in after:
		var row: Dictionary = value
		var section := int(row.get("section", -1))
		if before_by_section.has(section) and (
				before_by_section[section] as Vector3).distance_to(
						row.get("pos", Vector3.ZERO) as Vector3) > 0.0001:
			moved_sections += 1
	assert_gt(moved_sections, 0,
			"current BAD pose, not bind/entity-only matrices, drives collision")
	sim.free()

	DirAccess.remove_absolute(tmp.path_join("BINOC.bad"))
	DirAccess.remove_absolute(tmp.path_join("person_collision.adm"))
	DirAccess.remove_absolute(tmp)


func test_late_spawned_player_resolves_posed_collision_on_demand() -> void:
	# Mission collision is resolved before deploy in production. A player added
	# afterward must demand the same authored COBJ + ADM source instead of
	# becoming the one-sphere fallback until the next explicit sweep.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3/CharModel.3di")), OK)
	var bad_root := NovaResourceRoot.new()
	assert_eq(bad_root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/bad")), OK)
	var skeletal := NovaSkeletalAnim.new()
	assert_true(skeletal.load_from_bad_files(
			bad_root, "BINOC.bad", {"anim_idle": "BINOC.bad"},
			data.get_bone_origins(), data.get_bone_parents()),
			"late-spawn rig loads: %s" % skeletal.get_last_error())

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(
			item_db, SkeletalDataPlacerStub.new(data, skeletal)), 0,
			"the initial pre-deploy sweep has no player to attach")
	assert_true(sim.spawn_local_player(Vector3(10, 0, 0), 0.0, 1))

	# F3 still exercises CollisionWorld's demand provider for late targets, but
	# presentation filters the local avatar before any posed/fallback row escapes.
	assert_true((sim.get_hitbox_debug().get("organics", []) as Array).is_empty(),
			"the local avatar never renders posed or fallback hitboxes")
	var local_bms_id := int(sim.get_entity_debug(
			sim.get_entity_count() - 1).get("bms_id", -1))
	assert_true(bool(sim.get_destruction_debug(local_bms_id).get(
			"has_collision_instance", false)),
			"the hidden local avatar was nevertheless attached on demand")
	sim.free()


func test_f3_hides_local_player_and_omits_distant_posed_organic() -> void:
	# The F3 person view is a nearby diagnostic. It must neither wrap the local
	# avatar in debug spheres nor spend its pose/debug budget on a target more
	# than 80 mission units away.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_entity(
			NovaMissionData.KIND_ORGANIC, 105311,
			Vector3(200, 0, 0), Vector3.ZERO).is_empty())
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3/CharModel.3di")), OK)
	var bad_root := NovaResourceRoot.new()
	assert_eq(bad_root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/bad")), OK)
	var skeletal := NovaSkeletalAnim.new()
	assert_true(skeletal.load_from_bad_files(
			bad_root, "BINOC.bad", {"anim_idle": "BINOC.bad"},
			data.get_bone_origins(), data.get_bone_parents()))

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(
			item_db, SkeletalDataPlacerStub.new(data, skeletal)), 1)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))

	var rows: Array = sim.get_hitbox_debug().get("organics", [])
	assert_true(rows.is_empty(),
			"F3 omits the 200-unit target while still excluding local handle 1")
	sim.free()


func test_f3_hides_unresolved_local_player_fallback() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	assert_true((sim.get_hitbox_debug().get("organics", []) as Array).is_empty(),
			"an unresolved local avatar never leaks through the fallback path")
	sim.free()


func test_reused_player_slot_invalidates_old_collision_attempt_identity() -> void:
	# US02 intentionally cannot resolve through this provider, so the mission
	# soldier leaves a negative collision attempt on pool-0 slot 0. After WAC
	# removes it, the local US01 player reuses that exact packed handle. The new
	# registry spawn identity must invalidate the negative cache and resolve all
	# authored person sections on demand.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_entity(
			NovaMissionData.KIND_ORGANIC, 105311,
			Vector3.ZERO, Vector3.ZERO).is_empty())
	var probe := NovaSimulation.new()
	assert_true(probe.load_from_mission_data(md))
	var old_ssn := probe.get_entity_net_id(0)
	probe.free()
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(
			0, {"action_type": 22, "param1": old_ssn}).is_empty())

	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3/CharModel.3di")), OK)
	var bad_root := NovaResourceRoot.new()
	assert_eq(bad_root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/bad")), OK)
	var skeletal := NovaSkeletalAnim.new()
	assert_true(skeletal.load_from_bad_files(
			bad_root, "BINOC.bad", {"anim_idle": "BINOC.bad"},
			data.get_bone_origins(), data.get_bone_parents()))
	var placer := PlayerOnlySkeletalPlacerStub.new(data, skeletal)

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(item_db, placer), 0,
			"US02 records one unresolved attempt on slot 0")
	for _tick in 16:
		sim.step()
	assert_true(sim.get_entity_effect_state_for_ssn(old_ssn).is_empty(),
			"the original slot occupant was removed")
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1),
			"US01 reuses the freed pool-0 slot")

	assert_true((sim.get_hitbox_debug().get("organics", []) as Array).is_empty(),
			"the newly resolved local avatar remains hidden from F3")
	var local_bms_id := int(sim.get_entity_debug(
			sim.get_entity_count() - 1).get("bms_id", -1))
	assert_true(bool(sim.get_destruction_debug(local_bms_id).get(
			"has_collision_instance", false)),
			"the old negative attempt cannot suppress the new slot identity")
	sim.free()


func test_restart_re_resolves_the_restored_collision_identity() -> void:
	# Collision caches live outside World::Snapshot. Reusing slot 0 during play
	# must not leave the restored baseline actor unbound after Stop/Restart.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			NovaMissionData.KIND_ORGANIC, 105311,
			Vector3.ZERO, Vector3.ZERO)
	assert_false(placed.is_empty())
	var bms_id := int(placed.get("bms_id", 0))
	var probe := NovaSimulation.new()
	assert_true(probe.load_from_mission_data(md))
	var old_ssn := probe.get_entity_net_id(0)
	probe.free()
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(
			0, {"action_type": 22, "param1": old_ssn}).is_empty())

	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3/CharModel.3di")), OK)
	var bad_root := NovaResourceRoot.new()
	assert_eq(bad_root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/bad")), OK)
	var skeletal := NovaSkeletalAnim.new()
	assert_true(skeletal.load_from_bad_files(
			bad_root, "BINOC.bad", {"anim_idle": "BINOC.bad"},
			data.get_bone_origins(), data.get_bone_parents()))
	var placer := SkeletalDataPlacerStub.new(data, skeletal)

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(item_db, placer), 1)
	assert_true(bool(sim.get_destruction_debug(
			bms_id).get("has_collision_instance", false)))
	for _tick in 16:
		sim.step()
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	assert_true((sim.get_hitbox_debug().get("organics", []) as Array).is_empty())
	var local_bms_id := int(sim.get_entity_debug(
			sim.get_entity_count() - 1).get("bms_id", -1))
	assert_true(bool(sim.get_destruction_debug(local_bms_id).get(
			"has_collision_instance", false)),
			"the replacement local occupant receives the cached graphic")

	sim.restart()
	var restored := sim.get_destruction_debug(bms_id)
	assert_true(bool(restored.get("has_collision_instance", false)),
			"restart rebinds the baseline before any F3 or round demand query")
	var restored_rows: Array = sim.get_hitbox_debug().get("organics", [])
	assert_eq(restored_rows.size(), 19,
			"the restored non-local actor exposes every authored section")
	for value in restored_rows:
		var row: Dictionary = value
		assert_eq(int(row.get("entity_handle", -1)), 0)
		assert_false(bool(row.get("fallback", true)))
	sim.free()


func test_f3_organic_fallbacks_match_live_filtering_bounds() -> void:
	# F3 must describe the same unresolved pool-0 actors RoundSim can hit:
	# engine-flag filtering only (dead bodies remain solid), bounded to the local
	# 80-unit view and shared 96-entity debug budget, with the local avatar hidden.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	for i in range(101):
		var pos := Vector3(200, 0, 0) if i == 0 else Vector3(i % 10, 0, i % 7)
		assert_false(md.add_entity(
				NovaMissionData.KIND_ORGANIC, 105311, pos,
				Vector3.ZERO).is_empty())

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	sim.debug_set_entity_health(1, 0)

	var rows: Array = sim.get_hitbox_debug().get("organics", [])
	assert_eq(rows.size(), 96, "fallback entities obey the F3 target cap")
	var handles := {}
	for value in rows:
		var row: Dictionary = value
		assert_true(bool(row.get("fallback", false)))
		handles[int(row.get("entity_handle", -1))] = true
	assert_false(handles.has(0), "the 200-unit actor is outside the local F3 range")
	assert_true(handles.has(1), "a zero-health corpse retains its bullet fallback")
	sim.free()


func test_time_driven_collision_advances_without_an_ai_brain() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_entity(
		NovaMissionData.KIND_BUILDING, 102001,
		Vector3.ZERO, Vector3.ZERO).is_empty())
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
		'res://../fixtures/def/items.def')), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
		'res://../fixtures/3dp/Pmpjk01/Pmpjk01.3di')), OK)
	assert_gt(data.get_part_anim_count(0), 0)
	# Remove the IR-local copy only. The parsed model-level PANM remains the
	# canonical fallback used when effective LOD0 has no local rows.
	for i in range(data.get_part_anim_count(0) - 1, -1, -1):
		assert_true(data.delete_part_anim(0, i))
	assert_eq(data.get_part_anim_count(0), 0)
	assert_true(data.has_live_panm_for_lod(0),
		'model-level PANM remains live as effective LOD0 fallback')
	var targets := data.get_effective_panm_targets(0)
	assert_gt(targets.size(), 0)
	var pose_a: Dictionary = data.evaluate_panm(0, 0, {})
	var pose_b: Dictionary = data.evaluate_panm(0, 640, {})
	var moved_target := -1
	for target_value in targets:
		var target := int(target_value)
		if pose_a.has(target) and pose_b.has(target) and not (
				pose_a[target] as Transform3D).is_equal_approx(
					pose_b[target] as Transform3D):
			moved_target = target
			break
	assert_gte(moved_target, 0,
		'effective model-level PANM advances with the shared time')

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.get_entity_count(), 0, 'static has no AiEntity controls')
	assert_eq(sim.resolve_collision_instances(
		item_db, ObjectDataPlacerStub.new(data)), 1)
	var fallback_tick := sim.get_logic_tick()
	var fallback_before: PackedVector3Array = (
		(sim.get_hitbox_debug().get('entities', [])[0] as Dictionary)
		.get('tris', PackedVector3Array()))
	for _tick in range(40):
		sim.step()
	assert_eq(sim.get_logic_tick() - fallback_tick, 40)
	var fallback_after: PackedVector3Array = (
		(sim.get_hitbox_debug().get('entities', [])[0] as Dictionary)
		.get('tris', PackedVector3Array()))
	var fallback_moved := 0
	for i in fallback_before.size():
		if fallback_before[i].distance_to(fallback_after[i]) > 0.002:
			fallback_moved += 1
	assert_gt(fallback_moved, 0,
		'direct/headless simulation uses deterministic logic_tick * 16')

	sim.debug_set_panm_time_ms(0)
	var before_debug: Array = sim.get_hitbox_debug().get('entities', [])
	assert_eq(before_debug.size(), 1)
	var before: PackedVector3Array = (before_debug[0] as Dictionary).get(
		'tris', PackedVector3Array())
	assert_gt(before.size(), 0)
	sim.debug_set_panm_time_ms(640)
	var after_debug: Array = sim.get_hitbox_debug().get('entities', [])
	assert_eq(after_debug.size(), 1)
	var after: PackedVector3Array = (after_debug[0] as Dictionary).get(
		'tris', PackedVector3Array())
	assert_eq(after.size(), before.size())
	var moved := 0
	for i in before.size():
		if before[i].distance_to(after[i]) > 0.002:
			moved += 1
	assert_gt(moved, 0,
		'free-running PANM uses retail milliseconds with zero controls')
	sim.free()


func test_entity_debug_card_keeps_its_shape_after_a_scripted_remove() -> void:
	# VaporizeSingle (action 22) despawns the registry slot while the AI entity
	# stays in the pool - the card must keep a STABLE key set with typed
	# defaults for the registry half, never a partial dictionary.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, 0, Vector3(0, 0, 0), Vector3.ZERO)
	var probe := NovaSimulation.new()
	assert_true(probe.load_from_mission_data(md))
	var ssn := probe.get_entity_net_id(0)
	probe.free()

	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, {"action_type": 22, "param1": ssn}).is_empty())
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.get_entity_effect_state_for_ssn(ssn).size(), NovaSimulation.EFFECT_STATE_COUNT,
			"the effect lookup sees the live registry slot before VaporizeSingle")
	for _i in range(16):
		sim.step()

	var card: Dictionary = sim.get_entity_debug(0)
	assert_false(card.is_empty(), "the AI entity outlives its registry slot")
	assert_true(card.has("kind") and card.has("alive") and card.has("name"),
		"the registry half keeps its keys")
	assert_eq(int(card["kind"]), -1, "...with typed defaults (kind -1)")
	assert_false(bool(card["alive"]), "...alive false")
	assert_eq(int(card["net_id"]), ssn, "the AI half still reports its scalars")
	assert_true(sim.get_entity_effect_state_for_ssn(ssn).is_empty(),
			"attached effects detach as soon as VaporizeSingle removes the registry slot")
	sim.free()


func test_ai_state_name_static_lookup() -> void:
	assert_eq(NovaSimulation.ai_state_name(16), "GROUND_FOLLOWWP")
	assert_eq(NovaSimulation.ai_state_name(13), "?", "id gaps read as unknowns")
	assert_eq(NovaSimulation.ai_state_name(23), "GROUND_DEAD")
