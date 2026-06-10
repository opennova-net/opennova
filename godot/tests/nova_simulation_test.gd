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
