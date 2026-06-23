extends GutTest

# MissionRuntime is the shared driver both the game and the editor go through: it owns a real
# NovaSimulation + the present pass + the entity index, ticks them in one order, and restores authored
# transforms on Stop. This drives the REAL sim (not a fake) over fake placed nodes to prove the whole
# path: setup builds the index, tick presents sim state onto the nodes, Stop rewinds + restores.

const MissionRuntime := preload("res://engine/world/mission_runtime.gd")
const MissionSeatDiagnostics := preload("res://engine/world/mission_seat_diagnostics.gd")


func test_seat_userpoint_prefixes_match_original_seat_names() -> void:
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	assert_eq(rt._seat_type_for_user_point("sitex00"), 1, "sitex -> passenger")
	assert_eq(rt._seat_type_for_user_point("ctrlx"), 2, "ctrlx -> controller")
	assert_eq(rt._seat_type_for_user_point("UseGun01"), 3, "UseGun -> gunner")
	assert_eq(rt._seat_type_for_user_point("drvrx"), 5, "drvrx -> driver")
	assert_eq(rt._seat_type_for_user_point("ground"), 0, "non-seat userpoints are ignored")


func test_numbered_seat_userpoints_select_sit_pose_index() -> void:
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	assert_eq(rt._seat_pose_index_for_user_point("sitex00"), 0)
	assert_eq(rt._seat_pose_index_for_user_point("ctrlx03"), 3)
	assert_eq(rt._seat_pose_index_for_user_point("drvrx24"), 24)
	assert_eq(rt._seat_pose_index_for_user_point("UseGun01"), 0,
		"UseGun uses emplaced variants from the target, not sit_N userpoint digits")
	assert_eq(rt._seat_pose_index_for_user_point("sitex99"), 30,
		"sit_N is clamped to the known sit_0..sit_30 table")
	assert_eq(rt._seat_pose_index_for_user_point("ground"), 0)


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


func test_shared_seat_diagnostics_predict_original_command_rules() -> void:
	var seats := [
		{ "type": 1, "position": Vector3(5, 0, 0), "source_name": "sitex00" },
		{ "type": 2, "position": Vector3(1, 0, 0), "source_name": "ctrlx00" },
		{ "type": 5, "position": Vector3(2, 0, 0), "source_name": "drvrx00" },
	]
	var passenger := MissionSeatDiagnostics.predict_best_seat(seats, 123)
	assert_eq(int(passenger["seat_index"]), 0, "command 123 is passenger-only")
	assert_eq(String(passenger["seat"]["source_name"]), "sitex00")

	var non_controller := MissionSeatDiagnostics.predict_best_seat(seats, 124)
	assert_eq(int(non_controller["seat_index"]), 2, "command 124 skips ctrlx and takes driver before passenger")
	assert_eq(String(non_controller["seat"]["source_name"]), "drvrx00")

	var any := MissionSeatDiagnostics.predict_best_seat(seats, 125)
	assert_eq(int(any["seat_index"]), 1, "command 125 can select ctrlx by original priority")
	assert_eq(String(any["seat"]["source_name"]), "ctrlx00")
	assert_eq(String(any["candidates"][1]["status"]), "selected")


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


# --- Fixed-timestep accumulator (tick_realtime): the sim runs at a constant 62.5 Hz independent of
# the render/frame rate. [orig: Game_MainLoop @ 0x52b630 -> Game_ProcessMainFrame @ 0x5263f0] -----

func test_tick_realtime_accumulates_fixed_quanta() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, { "tick_mode": NovaSimulation.TICK_DIVIDED })
	rt.play()
	# 0.1 s of wall-clock at 62.5 Hz = floor(0.1 / 0.016) = 6 ticks.
	assert_eq(rt.tick_realtime(0.1), 6, "0.1 s banks 6 fixed-step ticks")
	# Sub-quantum deltas accumulate ACROSS calls instead of each firing a tick.
	assert_eq(rt.tick_realtime(0.008), 0, "half a quantum (plus the 0.004 remainder) fires nothing yet")
	assert_eq(rt.tick_realtime(0.008), 1, "the banked remainder crosses one quantum and fires once")


func test_tick_realtime_clamps_catchup() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, { "tick_mode": NovaSimulation.TICK_DIVIDED })
	rt.play()
	# 1.0 s would be ~62 ticks; the spiral-of-death clamp caps a single frame's catch-up.
	assert_eq(rt.tick_realtime(1.0), MissionRuntime.MAX_CATCHUP_TICKS, "a long stall is clamped to the catch-up cap")
	# The clamp DROPS the backlog (no banked spiral): a tiny delta afterward fires nothing.
	assert_eq(rt.tick_realtime(0.001), 0, "the backlog was dropped, not carried into the next frames")


func test_tick_realtime_ignored_when_not_playing() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, { "tick_mode": NovaSimulation.TICK_DIVIDED })
	# Not played -> paused -> banks nothing regardless of elapsed wall-clock (no burst on Play).
	assert_eq(rt.tick_realtime(1.0), 0, "a paused runtime banks nothing")
	rt.play()
	assert_eq(rt.tick_realtime(0.0), 0, "play() reset the accumulator; zero delta fires nothing")


func test_tick_realtime_presents_latest_state_once() -> void:
	# The node is authored far from spawn; after a catch-up batch the single present puts it on the
	# sim's LATEST position (decoupled render = present once per host frame, no inter-tick interpolation).
	var w := _make_world(Transform3D(Basis(), Vector3(99, 99, 99)))
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, { "tick_mode": NovaSimulation.TICK_DIVIDED })
	rt.play()
	assert_gt(rt.tick_realtime(0.1), 0, "the batch ran at least one tick")
	var sim_pos: Vector3 = rt.get_sim().get_entity_position(0)
	assert_true((w.model as Node3D).position.is_equal_approx(sim_pos),
		"the node ends on the latest sim position after the batch")
	assert_false((w.model as Node3D).position.is_equal_approx(Vector3(99, 99, 99)),
		"the node left its authored position")


func test_tick_realtime_drains_effects_per_tick() -> void:
	# Effects must drain PER logic tick INSIDE the catch-up batch (not coalesced into one emit at the
	# end): the BMS quarter-pass one-shot still surfaces when many ticks run in a single real-time frame.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, { "action_type": 6, "param1": 42 }).is_empty())
	var container := Node3D.new()
	add_child_autofree(container)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(md, container, { "tick_mode": NovaSimulation.TICK_DIVIDED })
	rt.play()
	var drained: Array = []
	rt.effects_drained.connect(func(effects): drained.append_array(effects))
	# 20.5 quanta of wall-clock in ONE frame -> 20 ticks; crosses the 16th-tick quarter-pass boundary.
	assert_eq(rt.tick_realtime(0.328), 20, "20+ quanta of wall-clock run 20 logic ticks in one frame")
	assert_eq(drained.size(), 1, "the per-tick one-shot effect surfaced from inside the batch")
	assert_eq(String((drained[0] as Dictionary)["kind"]), "text")


func test_distance_per_real_second_is_frame_rate_independent() -> void:
	# THE regression test for the reported bug. Drive two identical worlds for the SAME total wall-clock
	# (one real second), one at ~100 FPS, one at ~10 FPS. The accumulator must run the SAME number of
	# logic ticks (62 at 62.5 Hz) and leave entities at the same position. Because the per-tick motor
	# integrates a fixed displacement, equal tick count over equal wall-clock = equal distance =
	# locomotion speed decoupled from frame rate. The old "one tick per frame" path would have run 100
	# vs 10 ticks here (10x speed difference) — exactly the symptom this fixes.
	var hi := _run_realtime(0.01, 100)  # ~100 FPS for 1.0 s
	var lo := _run_realtime(0.1, 10)    #  ~10 FPS for 1.0 s
	assert_eq(hi.ticks, lo.ticks, "same wall-clock runs the same tick count regardless of frame rate")
	assert_eq(hi.ticks, 62, "~62.5 Hz over one real second")
	assert_true(hi.pos.is_equal_approx(lo.pos), "the deterministic sim lands the entity at one position")


# Drive a fresh MissionRuntime with `count` frames of `step` seconds each and report total ticks +
# the entity position. Fixed iteration count (not a while-elapsed loop) keeps the fed wall-clock exact.
func _run_realtime(step: float, count: int) -> Dictionary:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, { "tick_mode": NovaSimulation.TICK_DIVIDED })
	rt.play()
	var ticks := 0
	for _i in range(count):
		ticks += rt.tick_realtime(step)
	return { "ticks": ticks, "pos": rt.get_sim().get_entity_position(0) }
