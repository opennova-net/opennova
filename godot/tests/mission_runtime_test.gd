extends GutTest

# MissionRuntime is the shared driver both the game and the editor go through: it owns a real
# NovaSimulation + the present pass + the entity index, ticks them in one order, and restores authored
# transforms on Stop. This drives the REAL sim (not a fake) over fake placed nodes to prove the whole
# path: setup builds the index, tick presents sim state onto the nodes, Stop rewinds + restores.

const MissionRuntime := preload("res://engine/world/mission_runtime.gd")


func test_seat_userpoint_prefixes_match_original_seat_names() -> void:
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	assert_eq(rt._seat_type_for_user_point("sitex00"), 1, "sitex -> passenger")
	assert_eq(rt._seat_type_for_user_point("ctrlx"), 2, "ctrlx -> controller")
	assert_eq(rt._seat_type_for_user_point("UseGun01"), 3, "UseGun -> gunner")
	assert_eq(rt._seat_type_for_user_point("drvrx"), 5, "drvrx -> driver")
	assert_eq(rt._seat_type_for_user_point("ground"), 0, "non-seat userpoints are ignored")


func test_seat_userpoints_are_converted_through_vehicle_yaw_zero_basis() -> void:
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	assert_true(rt._seat_local_from_user_point_position(Vector3(1, 2, 3)).is_equal_approx(Vector3(-1, 3, 2)),
		"seat offsets use the same yaw-zero model-forward correction as object placement")
	assert_eq(rt._seat_yaw_offset_from_user_point_rotation(Vector3.BACK), 0,
		"model forward maps to vehicle yaw")
	assert_eq(rt._seat_yaw_offset_from_user_point_rotation(Vector3.RIGHT), -90,
		"model right maps to a left-facing seat offset")
	assert_eq(rt._seat_yaw_offset_from_user_point_rotation(Vector3.LEFT), 90,
		"model left maps to a right-facing seat offset")


# An animatable placed entity: play_part_anim marks it for the registry, set_part_phase + Node3D
# transform/visible let the present pass drive it.
class FakeModel:
	extends Node3D
	var phases: Array = []
	func play_part_anim(_channel: int, _play_type: int, _time_s: float) -> void:
		pass
	func set_part_phase(channel: int, phase: int) -> void:
		phases.append([channel, phase])


# Build a one-organic mission + a container holding one fake node tagged to match it by (kind,index)
# (the in-memory mission has bms_id 0, so the present index resolves by the fallback key).
func _make_world(authored: Transform3D) -> Dictionary:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, 0, Vector3(10, 0, 0), Vector3.ZERO)  # KIND_ORGANIC

	var container := Node3D.new()
	add_child_autofree(container)
	var model := FakeModel.new()
	model.transform = authored
	model.set_meta("entity_ref", { "kind": 3, "index": 0, "bms_id": 0, "group": -1 })
	container.add_child(model)
	return { "mission": md, "container": container, "model": model }


func test_setup_promotes_and_counts() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	var count := int(rt.setup(w.mission, w.container, { "tick_mode": NovaSimulation.TICK_EVERY_PROCESS }))
	assert_eq(count, 1, "one organic promoted to an AI entity")
	assert_not_null(rt.get_sim(), "sim created")
	assert_eq(rt.entity_count(), 1)


func test_tick_presents_sim_position_onto_node() -> void:
	# The node is authored far from the entity's spawn; after a tick the present pass moves it onto the
	# sim's computed position (the consolidated path the old game path never did).
	var w := _make_world(Transform3D(Basis(), Vector3(99, 99, 99)))
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, { "tick_mode": NovaSimulation.TICK_EVERY_PROCESS })
	rt.step_once()
	var sim_pos: Vector3 = rt.get_sim().get_entity_position(0)
	assert_true((w.model as Node3D).position.is_equal_approx(sim_pos),
		"present moved the node onto the sim entity position")
	assert_false((w.model as Node3D).position.is_equal_approx(Vector3(99, 99, 99)),
		"node left its authored position while playing")


func test_stop_restores_authored_transform() -> void:
	var authored := Transform3D(Basis(), Vector3(99, 99, 99))
	var w := _make_world(authored)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, { "tick_mode": NovaSimulation.TICK_EVERY_PROCESS })
	rt.step_once()  # moves the node off its authored transform
	rt.stop()
	assert_true((w.model as Node3D).transform.is_equal_approx(authored),
		"Stop restored the authored node transform")


func test_divided_mode_ticks_and_steps_like_the_game() -> void:
	# DIVIDED is the game's mode: one logic tick per host frame (the engine's own dividers — WAC
	# every 62nd tick, BMS quarter-pass every 16th — gate inside the systems). The editor preview
	# now runs this mode too, so tick() and Step must both advance + present under it.
	var w := _make_world(Transform3D(Basis(), Vector3(99, 99, 99)))
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, { "tick_mode": NovaSimulation.TICK_DIVIDED })
	assert_true(rt.tick(), "a DIVIDED tick advances one logic tick")
	rt.step_once()
	var sim_pos: Vector3 = rt.get_sim().get_entity_position(0)
	assert_true((w.model as Node3D).position.is_equal_approx(sim_pos),
		"Step under DIVIDED presents the sim state onto the node")


func test_effects_drained_signal_fires() -> void:
	# A mission runtime drains side effects each tick; the host listens on effects_drained. Build an
	# unconditional OutputText event and confirm the signal carries it.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, { "action_type": 6, "param1": 42 }).is_empty())
	var container := Node3D.new()
	add_child_autofree(container)

	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(md, container, { "tick_mode": NovaSimulation.TICK_EVERY_PROCESS })
	# GDScript lambdas capture locals by value; mutate the array by reference (append) rather than
	# reassign, so the outer `drained` sees the signal payload.
	var drained: Array = []
	rt.effects_drained.connect(func(effects): drained.append_array(effects))
	# A normal event's first processing pass is the 16th tick (faithful quarter-list cadence).
	for _i in range(16):
		rt.step_once()
	assert_eq(drained.size(), 1, "one effect drained through the signal")
	assert_eq(String((drained[0] as Dictionary)["kind"]), "text", "OutputText -> text effect")
