extends GutTest

# NovaSimulation (the GDExtension binding): promote a synthetic BMS mission into a live
# world + AI system, tick it, and confirm the AI walks entities along their authored route.
# This is the in-Godot end of step 1 (promotion) + step 2 (locomotion).

func test_demo_mission_promotes() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	assert_true(sim.is_loaded(), "demo mission promoted")
	assert_eq(sim.get_entity_count(), 2, "two organics got AI brains")
	assert_eq(sim.get_brain_count(), 2, "two AI brains attached")
	assert_eq(sim.get_spawned_count(), 6, "one building + three markers + two organics spawned into pools")
	assert_eq(sim.get_entity_state(0), 16, "a routed organic starts in GROUND_FOLLOWWP (16)")
	sim.free()

const ANIM_FIXTURES := "res://../fixtures/anim"

func _anim_root() -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path(ANIM_FIXTURES))
	return root

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
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
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
				{"type": 1, "position": Vector3(9, 0, 0), "yaw_offset": 0, "source_name": "sitex00"},
				{"type": 2, "position": Vector3(0, 1, 2), "yaw_offset": 45, "pose_index": 24, "source_name": "ctrlx24"}
			],
		}
	])
	assert_true(sim.load_from_mission_data(md), "loaded command-125 mission with seat specs")
	var pos := sim.get_entity_position(0)
	assert_true(pos.is_equal_approx(Vector3(10, 2, -1)),
		"command-125 soldier uses the IDA-priority ctrlx seat, converted to Godot axes")
	assert_almost_eq(sim.get_entity_yaw_deg(0), 45.0, 0.01,
		"non-gunner mounted seats carry their local yaw offset")
	var snap := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	assert_eq(int(snap[NovaSimulation.PF_ANIM_STATE]), 100, "mounted ctrlx24 infantry renders anim_sit_24")
	var card: Dictionary = sim.get_entity_debug(0)
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
	assert_eq(String((target_seats[1] as Dictionary)["source_name"]), "ctrlx24")
	assert_eq(int((target_seats[1] as Dictionary)["pose_index"]), 24)
	assert_eq(int(card["anim_state"]), 100)
	assert_eq(String(card["anim_key"]), "anim_sit_24")
	assert_eq(stride, NovaSimulation.PF_STRIDE)
	sim.free()


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
	var pos := sim.get_entity_position(0)
	var expected := Vector3(10.1189880, 2.1048889, 0.7148895)
	assert_lt(pos.distance_to(expected), 0.001,
		"mounted seat local follows the same rotated side as the selected model userpoint")
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
			"emplaced_pose_variant": 3,
			"seats": [
				{"type": 3, "position": Vector3.ZERO, "yaw_offset": 0}
			],
		}
	])
	assert_true(sim.load_from_mission_data(md), "loaded command-125 UseGun mount")
	var snap := sim.get_present_snapshot()
	assert_eq(int(snap[NovaSimulation.PF_ANIM_STATE]), 67,
		"UseGun falls back to base anim_emplaced when the emplaced variant clip is unavailable")
	var card: Dictionary = sim.get_entity_debug(0)
	assert_true(bool(card["mounted"]), "debug card marks UseGun occupant mounted")
	assert_eq(int(card["mount_type"]), 3, "seat type is UseGun/gunner")
	assert_eq(int(card["mount_target_emplaced_pose_variant"]), 3)
	assert_eq(int(card["anim_state"]), 67)
	assert_eq(String(card["anim_key"]), "anim_emplaced")
	sim.free()


func test_present_snapshot_shape_and_stride() -> void:
	# ONE batched present snapshot replaces ~10 Variant-boxed scalar getter calls per entity in the
	# per-tick present loop. Its length must be count * stride, and the bound stride must match the
	# PF_STRIDE layout constant the GDScript present pass mirrors.
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	var stride: int = sim.get_present_stride()
	assert_eq(stride, NovaSimulation.PF_STRIDE, "bound stride == PF_STRIDE layout constant")
	var snap: PackedFloat32Array = sim.get_present_snapshot()
	assert_eq(snap.size(), sim.get_entity_count() * stride, "snapshot is count * stride floats")
	sim.free()

func test_present_snapshot_matches_scalar_getters() -> void:
	# The batched snapshot must carry exactly what the scalar getters report (it's the same source),
	# so the present pass and any scalar consumer agree. Checked at spawn (pre-tick).
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	var snap: PackedFloat32Array = sim.get_present_snapshot()
	var stride: int = sim.get_present_stride()
	for i in range(sim.get_entity_count()):
		var base := i * stride
		var pos: Vector3 = sim.get_entity_position(i)
		assert_almost_eq(snap[base + NovaSimulation.PF_POS_X], pos.x, 0.001, "pos.x matches")
		assert_almost_eq(snap[base + NovaSimulation.PF_POS_Y], pos.y, 0.001, "pos.y matches")
		assert_almost_eq(snap[base + NovaSimulation.PF_POS_Z], pos.z, 0.001, "pos.z matches")
		assert_almost_eq(snap[base + NovaSimulation.PF_YAW_DEG], sim.get_entity_yaw_deg(i), 0.01, "yaw_deg matches")
		assert_eq(int(snap[base + NovaSimulation.PF_BMS_ID]), sim.get_entity_bms_id(i), "bms_id matches")
		assert_eq(int(snap[base + NovaSimulation.PF_KIND]), sim.get_entity_kind(i), "kind matches")
		assert_eq(int(snap[base + NovaSimulation.PF_NET_ID]), sim.get_entity_net_id(i), "net_id matches")
		assert_eq(int(snap[base + NovaSimulation.PF_ALIVE]), 1, "spawned entity is alive")
	sim.free()


func test_present_snapshot_carries_infantry_anim_state_and_phase() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	var snap: PackedFloat32Array = sim.get_present_snapshot()
	var stride: int = sim.get_present_stride()
	assert_eq(stride, NovaSimulation.PF_STRIDE, "bound stride includes the anim fields")
	assert_lt(NovaSimulation.PF_ANIM_STATE, NovaSimulation.PF_STRIDE, "anim state is inside the record")
	assert_lt(NovaSimulation.PF_ANIM_PHASE_TICKS, NovaSimulation.PF_STRIDE, "anim phase is inside the record")
	assert_false(snap.is_empty(), "demo mission has present records")
	var base := 0
	assert_eq(int(snap[base + NovaSimulation.PF_ANIM_STATE]), 43, "demo infantry starts in IDA idle state")
	assert_gte(int(snap[base + NovaSimulation.PF_ANIM_PHASE_TICKS]), 0, "clip phase is exported as ticks")
	assert_eq(NovaSimulation.infantry_anim_key(43), "anim_idle", "state id resolves to the .adm clip key")
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
	for _i in range(16):
		sim.step()

	var card: Dictionary = sim.get_entity_debug(0)
	assert_false(card.is_empty(), "the AI entity outlives its registry slot")
	assert_true(card.has("kind") and card.has("alive") and card.has("name"),
		"the registry half keeps its keys")
	assert_eq(int(card["kind"]), -1, "...with typed defaults (kind -1)")
	assert_false(bool(card["alive"]), "...alive false")
	assert_eq(int(card["net_id"]), ssn, "the AI half still reports its scalars")
	sim.free()


func test_ai_state_name_static_lookup() -> void:
	assert_eq(NovaSimulation.ai_state_name(16), "GROUND_FOLLOWWP")
	assert_eq(NovaSimulation.ai_state_name(13), "?", "id gaps read as unknowns")
	assert_eq(NovaSimulation.ai_state_name(23), "GROUND_DEAD")
