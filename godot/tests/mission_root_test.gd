extends GutTest

# MissionRoot is GameWorld's live mission root: it owns a real
# Simulation, present pass, and entity index and ticks them in one order.
# These focused presentation tests instantiate it over fixture nodes to prove the
# engine path without introducing a second editor gameplay runtime.



func _advance_ticks(runtime: MissionRoot, delta: float) -> int:
	var input := MissionFrameInput.new()
	input.delta_seconds = delta
	var outcome: MissionFrameOutcome = runtime.advance_session_frame(input)
	return outcome.get_ticks_run() if outcome != null else 0


static func _options_with_placer(placer: MissionObjectPlacer) -> MissionSetupOptions:
	var options := MissionSetupOptions.new()
	options.placer = placer
	return options


func test_wire_type_ids_install_the_same_late_vehicle_metadata() -> void:
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth"))
	var sim := Simulation.new()
	autofree(sim)
	assert_false(sim.install_seat_specs_for_type_ids(
			item_db, PackedInt32Array([1419])),
			"the native install has no model source before set_asset_root")
	sim.set_asset_root(root)
	assert_true(sim.install_seat_specs_for_type_ids(
			item_db, PackedInt32Array([1419, 1419, 0])),
			"the joiner prewarm installs from streamed wire type ids")
	assert_eq(sim.get_mounted_graphic_source_count(), 1,
			"duplicate/zero type ids collapse to the one resolved model source")
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var placed := mission.add_entity(MissionData.KIND_ITEM, 101419, Vector3.ZERO, Vector3.ZERO)
	assert_true(sim.load_from_mission_data(mission))
	var card := sim.entity_card_by_net_id(placed.bms_id)
	assert_not_null(card)
	assert_eq(card.get_item_id(), 1419, "the wire type reaches the live entity")
	assert_eq(card.get_seats().size(), 1, "late install supplies the live UseGun seat")



# One synthetic in-memory particle catalog authoring that move effect as a
# FOREVEREMIT definition (the effect_world_test recipe), so a REAL EffectWorld
# interns and spawns it without a resource root and the round-bound group
# stays live until the throwable pass stops it.
func _make_world(authored: Transform3D) -> Dictionary:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, 0, Vector3(10, 0, 0), Vector3.ZERO)  # KIND_ORGANIC

	var container := Node3D.new()
	add_child_autofree(container)
	var model := ObjectModel.new()
	container.add_child(model)
	model.set_process(false)
	model.transform = authored
	model.entity_ref = EntityRef.make(3, 0, 0)
	var placer := MissionObjectPlacer.new()
	placer.placed_models.append(model)
	return { "mission": md, "container": container, "model": model, "placer": placer }


func test_setup_promotes_and_counts() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	var count := int(rt.setup(w.mission, w.container, _options_with_placer(w.placer)))
	# P7: every preview is the in-process listen server, so the host player auto-spawns at bring-up —
	# the world is the one authored organic + the host player.
	assert_eq(count, 2, "one organic + the auto-spawned host player")
	assert_not_null(rt.get_sim(), "sim created")
	assert_eq(rt.entity_count(), 2)


func test_setup_wires_presented_building_transforms_to_the_shadow_registry() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var ref: EntityRef = mission.add_entity(MissionData.KIND_BUILDING, 102001,
			Vector3(10, 20, 3), Vector3(0, 25, 0))
	assert_not_null(ref)
	if ref == null:
		return
	var bms_id := ref.bms_id
	var container := Node3D.new()
	add_child_autofree(container)
	var model := ObjectModel.new()
	container.add_child(model)
	model.set_process(false)
	var placer := MissionObjectPlacer.new()
	model.entity_ref = EntityRef.make(MissionData.KIND_BUILDING,
			ref.index, bms_id, 102001)
	placer.placed_models.append(model)
	placer.register_static_instance(bms_id, "Caster", 0,
			Transform3D(Basis.IDENTITY, Vector3(-20, -20, -20)), true)
	var revision := placer.get_static_terrain_shadow_source_revision()
	var runtime := MissionRoot.new()
	add_child_autofree(runtime)
	assert_gt(int(runtime.setup(mission, container, _options_with_placer(placer))), 0)
	assert_true(runtime.tick())
	assert_gt(placer.get_static_terrain_shadow_source_revision(), revision,
			"production MissionRoot passes its placer into the EntityPresenter")
	assert_eq(placer.hide_static_instance(bms_id), model.transform,
			"production presentation and the carve share one pose")
	assert_true(placer.show_static_instance(bms_id))


func test_stats_and_manual_probe_share_one_native_profiling_owner_gate() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var board := FrameStats.new()
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.set_frame_stats(board)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	var sim := rt.get_sim()
	assert_false(sim.is_runtime_profiling_enabled(),
			"an attached but closed Stats board leaves native profiling off")

	board.set_capture_active(true)
	assert_true(sim.is_runtime_profiling_enabled(),
			"opening Stats acquires the native profiling gate")
	assert_true(rt.tick())
	var active_counters: Dictionary = sim.get_runtime_perf_counters()
	assert_true(bool(active_counters.get("runtime_profiling_enabled", false)))

	rt.set_runtime_profiling_enabled(true)
	board.set_capture_active(false)
	assert_true(sim.is_runtime_profiling_enabled(),
			"manual probe ownership survives the Stats capture release")

	board.set_capture_active(true)
	rt.set_runtime_profiling_enabled(false)
	assert_true(sim.is_runtime_profiling_enabled(),
			"Stats ownership survives the manual probe release")

	board.set_capture_active(false)
	assert_false(sim.is_runtime_profiling_enabled(),
			"the native gate closes only after both consumers release it")
	var closed_counters: Dictionary = sim.get_runtime_perf_counters()
	for key in [
		"sim_tick_us",
		"net_tick_us",
		"present_snapshot_us",
		"occlusion_build_us",
		"occlusion_probe_us",
	]:
		assert_eq(int(closed_counters.get(key, -1)), 0,
				"closing the last owner clears %s" % key)
	assert_false(bool(closed_counters.get("trace_profiling_enabled", true)))


# The world tick is the ONLY pump for the session socket, so runtime transport
# must not be able to halt a live net session: the F3 overlay ships in
# the game shell, and a paused joiner (or a stopped listen host) starves the
# uplink until the peer reaps at cs_dir0.timeout_ms = 120000
# [orig: CNapiNetwork_Init @0x4ca4a0]. A local single-player runtime remains
# controllable for F3 diagnostics and focused fixtures.
func test_transport_is_locked_out_of_a_live_net_session() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var joiner := Simulation.new()
	assert_true(joiner.enable_join("127.0.0.1", 9, "TransportLockJoiner"))
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	var lock_options := _options_with_placer(w.placer)
	lock_options.simulation = joiner
	lock_options.net_transport = "lan-join"
	assert_gt(int(rt.setup(w.mission, w.container, lock_options)), 0)

	assert_true(rt.is_transport_locked(), "a joiner runtime reports a locked transport")
	rt.play()
	assert_true(rt.is_playing(), "play still starts the session ticking")
	rt.pause()
	assert_true(rt.is_playing(), "a transport pause cannot silence a live joiner's socket")
	rt.step_once()
	assert_true(rt.is_playing(), "a transport step cannot drop a live joiner out of the tick loop")
	rt.stop()
	assert_true(rt.is_playing(), "a transport stop cannot rewind the world under a live peer")


func test_transport_still_works_for_a_local_runtime() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	assert_gt(int(rt.setup(w.mission, w.container, _options_with_placer(w.placer))), 0)
	# A directly instantiated runtime has no bound socket or peers, so it is not
	# a live net session and keeps its transport.
	assert_false(rt.is_transport_locked(),
			"a local runtime is not a live net session")
	rt.play()
	assert_true(rt.is_playing())
	rt.pause()
	assert_false(rt.is_playing(), "the local runtime transport still pauses")


func test_mission_present_stats_are_a_typed_record() -> void:
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	var stats: MissionPresentStats = rt.get_mission_present_stats()
	assert_not_null(stats)
	assert_eq(stats.plan_rebuilds, 0)
	assert_eq(stats.transform_builds, 0)
	assert_eq(stats.aim_dispatches, 0)
	assert_eq(stats.rhc_dispatches, 0)
	assert_eq(stats.part_dispatches, 0)
	assert_eq(stats.control_dispatches, 0)
	assert_eq(stats.body_dispatches, 0)


func test_wire_presenter_resets_with_runtime_stop() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	var presenter := rt.get_entity_presenter()
	assert_not_null(presenter)
	var reset_wire := Callable(presenter, "reset_wire_runtime_state")
	assert_true(rt.simulation_restarted.is_connected(reset_wire),
			"Stop clears wire handle/type caches before restored rows present again")
	rt.stop()


func test_presentation_clock_survives_setup_and_forwards_immediately() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.set_presentation_time_ms(0x1ffffffff)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	assert_eq(rt.get_sim().get_panm_time_ms(), 0xffffffff,
		"preconfigured clock is injected after mission-load reset")
	rt.set_presentation_time_ms(1234)
	assert_eq(rt.get_sim().get_panm_time_ms(), 1234,
		"zero-tick and paused frames update collision time immediately")


func test_setup_exposes_normalized_diagnostic_mission_identity() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	var identity_options := _options_with_placer(w.placer)
	identity_options.mission_file = "C:\\missions\\00TRe.bms"
	identity_options.mission_name = "Training Grounds"
	rt.setup(w.mission, w.container, identity_options)
	assert_eq(rt.get_mission_file(), "00TRe.bms",
			"diagnostics expose a portable basename, never the editor's local path")
	assert_eq(rt.get_mission_name(), "Training Grounds")


func test_tick_presents_sim_position_onto_node() -> void:
	# The node is authored far from the entity's spawn; after a tick the present pass moves it onto the
	# sim's computed position (the consolidated path the old game path never did).
	var w := _make_world(Transform3D(Basis(), Vector3(99, 99, 99)))
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	rt.step_once()
	var sim_pos: Vector3 = rt.get_sim().get_entity_position(0)
	assert_true((w.model as Node3D).position.is_equal_approx(sim_pos),
		"present moved the node onto the sim entity position")
	assert_false((w.model as Node3D).position.is_equal_approx(Vector3(99, 99, 99)),
		"node left its authored position while playing")


# (Historical transform-restore test deleted: MissionRoot.stop() still
# rewinds Simulation for teardown/fixtures, but ONED no longer owns a live
# runtime whose Stop must restore authored editor nodes.)



func test_tick_and_step_advance_and_present_like_the_game() -> void:
	# The standalone driver's tick() dispatches exactly one logic tick
	# (the engine's own dividers — WAC every 62nd tick, BMS quarter-pass every 16th — gate inside
	# the systems), so direct fixture tick() and Step must both advance + present.
	var w := _make_world(Transform3D(Basis(), Vector3(99, 99, 99)))
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	assert_true(rt.tick(), "tick() advances one logic tick")
	rt.step_once()
	var sim_pos: Vector3 = rt.get_sim().get_entity_position(0)
	assert_true((w.model as Node3D).position.is_equal_approx(sim_pos),
		"Step presents the sim state onto the node")


func test_effects_drained_signal_fires() -> void:
	# A mission runtime drains side effects each tick; the shell listens on effects_drained. Build an
	# unconditional OutputText event and confirm the signal carries it.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_gte(md.add_event(0, 0, 0), 0)
	assert_true(md.add_event_action(0, 6, 0, 42))
	var container := Node3D.new()
	add_child_autofree(container)

	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(md, container)
	# GDScript lambdas capture locals by value; mutate the array by reference (append) rather than
	# reassign, so the outer `drained` sees the signal payload.
	var drained: Array = []
	rt.effects_drained.connect(func(effects): drained.append_array(effects))
	# A normal event's first processing pass is the 16th tick (faithful quarter-list cadence).
	for _i in range(16):
		rt.step_once()
	assert_eq(drained.size(), 1, "one effect drained through the signal")
	assert_eq((drained[0] as MissionEffect).kind, "text", "OutputText -> text effect")


# --- Fixed-timestep accumulator (session_frame): the sim runs at a constant 62.5 Hz independent of
# the render/frame rate. [orig: Game_MainLoop @ 0x52b630 -> Game_ProcessMainFrame @ 0x5263f0] -----

func test_session_frame_accumulates_fixed_quanta() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	rt.play()
	# The retail bank drains 4 ms quanta and ticks on every fourth: half-tick
	# (8 ms) frames accumulate ACROSS calls, one tick every other frame.
	var ticks: Array = []
	for _i in range(4):
		ticks.append(_advance_ticks(rt, 0.008))
	assert_eq(ticks, [1, 0, 1, 0], "8 ms frames tick every other frame")


func test_session_frame_clamps_catchup() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	rt.play()
	# 1.0 s would be ~62 ticks; the retail bank clamps at 500 ms (125 quanta
	# from phase 0 = 32 ticks, world::TickAccumulator::kRetailMaxCatchupTicks).
	assert_eq(_advance_ticks(rt, 1.0), 32, "a long stall is clamped to 500 ms of bank")
	# The clamp keeps the smoothed history: the next frame fast-forwards,
	# (7 * 8000 + 16 + 4) >> 3 = 7002 units = 27 ticks from phase 125.
	assert_eq(_advance_ticks(rt, 0.001), 27, "the frame after a stall fast-forwards")


func test_session_frame_ignored_when_not_playing() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	# Not played -> paused -> banks nothing regardless of elapsed wall-clock (no burst on Play).
	assert_eq(_advance_ticks(rt, 1.0), 0, "a paused runtime banks nothing")
	rt.play()
	assert_eq(_advance_ticks(rt, 0.0), 0, "play() reset the accumulator; zero delta fires nothing")


func test_session_frame_still_presents_a_zero_tick_render_frame() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	assert_true(rt.step_once(), "seed one decoded presentation snapshot")
	# play() resets the bank, so a zero-length frame drains nothing.
	rt.play()
	(w.model as Node3D).visible = false
	assert_eq(_advance_ticks(rt, 0.0), 0, "no fixed simulation tick advances")
	assert_true((w.model as Node3D).visible,
			"the render frame still reapplies current visibility state")


func test_session_frame_presents_latest_state_once() -> void:
	# The node is authored far from spawn; after a catch-up batch the single present puts it on the
	# sim's LATEST position (decoupled render = present once per render frame, no inter-tick interpolation).
	var w := _make_world(Transform3D(Basis(), Vector3(99, 99, 99)))
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	rt.play()
	assert_gt(_advance_ticks(rt, 0.1), 0, "the batch ran at least one tick")
	var sim_pos: Vector3 = rt.get_sim().get_entity_position(0)
	assert_true((w.model as Node3D).position.is_equal_approx(sim_pos),
		"the node ends on the latest sim position after the batch")
	assert_false((w.model as Node3D).position.is_equal_approx(Vector3(99, 99, 99)),
		"the node left its authored position")


func test_catchup_exposes_each_fixed_ticks_pose_before_batched_presentation() -> void:
	var authored := Transform3D(Basis.IDENTITY, Vector3(99, 99, 99))
	var w := _make_world(authored)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	var entity_ref := EntityRef.make(3, 0, 0)
	var observed: Array = []
	rt.fixed_tick_completed.connect(func(_logic_tick: int) -> void:
		observed.append({
			"effect_pose": rt.presented_entity_effect_transform(entity_ref),
			"node_pose": (w.model as Node3D).global_transform,
		})
	)
	rt.play()
	assert_eq(_advance_ticks(rt, 0.05), 3, "one render frame catches up three fixed ticks")
	assert_eq(observed.size(), 3, "each fixed tick exposes its own attachment snapshot")
	for row_v in observed:
		var row: Dictionary = row_v
		assert_true(row.effect_pose is Transform3D)
		assert_false((row.effect_pose as Transform3D).origin.is_equal_approx(authored.origin),
				"the attachment reads current sim values, not the stale authored Node")
		assert_true((row.node_pose as Transform3D).origin.is_equal_approx(authored.origin),
				"scene Nodes still present only once after the catch-up batch")
	var final_row: Dictionary = observed[observed.size() - 1]
	var final_effect_pose: Transform3D = final_row["effect_pose"]
	assert_true((w.model as Node3D).global_position.is_equal_approx(
			final_effect_pose.origin),
			"the one final Node presentation matches the last fixed-tick value")


func test_session_frame_drains_effects_per_tick() -> void:
	# Effects must drain PER logic tick INSIDE the catch-up batch (not coalesced into one emit at the
	# end): the BMS quarter-pass one-shot still surfaces when many ticks run in a single real-time frame.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_gte(md.add_event(0, 0, 0), 0)
	assert_true(md.add_event_action(0, 6, 0, 42))
	var container := Node3D.new()
	add_child_autofree(container)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(md, container)
	rt.play()
	var drained: Array = []
	rt.effects_drained.connect(func(effects): drained.append_array(effects))
	# 328 ms in ONE frame on the reset bank: 16 + 5248 units = 82 quanta from
	# phase 0 -> 21 ticks; crosses the 16th-tick quarter-pass boundary.
	assert_eq(_advance_ticks(rt, 0.328), 21, "328 ms of wall-clock run 21 logic ticks in one frame")
	assert_eq(drained.size(), 1, "the per-tick one-shot effect surfaced from inside the batch")
	assert_eq((drained[0] as MissionEffect).kind, "text")


func test_distance_per_real_second_is_frame_rate_independent() -> void:
	# THE regression test for the reported bug. Drive two identical worlds for the SAME total wall-clock
	# (one real second), one at ~100 FPS, one at ~10 FPS. The accumulator must run the SAME number of
	# logic ticks (62 at 62.5 Hz) and leave entities at the same position. Because the per-tick motor
	# integrates a fixed displacement, equal tick count over equal wall-clock = equal distance =
	# locomotion speed decoupled from frame rate. The old "one tick per frame" path would have run 100
	# vs 10 ticks here (10x speed difference) — exactly the symptom this fixes.
	# Retail's bank low-passes the banked time, so each run settles its
	# smoothed backlog with zero-length frames before the count.
	var hi := _run_realtime(0.01, 100)  # ~100 FPS for 1.0 s
	var lo := _run_realtime(0.1, 10)    #  ~10 FPS for 1.0 s
	assert_eq(hi.ticks, lo.ticks, "same wall-clock runs the same tick count regardless of frame rate")
	assert_between(int(hi.ticks), 62, 72,
			"~62.5 Hz over one real second (plus the unsmoothed first frame's history)")
	assert_true(hi.pos.is_equal_approx(lo.pos), "the deterministic sim lands the entity at one position")


# Drive a fresh MissionRoot with `count` frames of `step` seconds each and report total ticks +
# the entity position. Fixed iteration count (not a while-elapsed loop) keeps the fed wall-clock exact.
func _run_realtime(step: float, count: int) -> Dictionary:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	rt.play()
	# One ordinary frame first (the same bank history for every run), then the
	# second of frames, then zero-length frames until the smoother has drained.
	var ticks := _advance_ticks(rt, Simulation.tick_dt())
	for _i in range(count):
		ticks += _advance_ticks(rt, step)
	for _i in range(100):
		ticks += _advance_ticks(rt, 0.0)
	return { "ticks": ticks, "pos": rt.get_sim().get_entity_position(0) }


func test_native_present_snapshot_survives_a_nested_script_snapshot_read() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var second_ref: EntityRef = w.mission.add_entity(
			MissionData.KIND_ORGANIC, 0, Vector3(20, 0, 0), Vector3.ZERO)
	var second := ObjectModel.new()
	w.container.add_child(second)
	second.entity_ref = second_ref
	w.placer.placed_models.append(second)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	assert_true(rt.step_once())
	# play() resets the bank: the zero-length frames below drain nothing.
	rt.play()
	var sim := rt.get_sim()
	var expected := second.position
	var reads := [0]
	var first: ObjectModel = w.model
	first.visibility_changed.connect(func() -> void:
		if not first.visible or reads[0] != 0:
			return
		reads[0] += 1
		assert_eq(sim.debug_set_entity_position(1, Vector3(90, 20, 3)), OK)
		var nested := sim.get_present_snapshot()
		assert_gt(nested.size(), 0, "a callback can request a fresh script snapshot")
	)
	first.visible = false
	assert_eq(_advance_ticks(rt, 0.0), 0)
	assert_eq(reads[0], 1, "the callback ran inside the placed walk")
	assert_eq(second.position, expected, "later rows still read the outer immutable frame")
	assert_eq(_advance_ticks(rt, 0.0), 0)
	assert_eq(second.position, sim.get_entity_position(1), "the next frame reads the newer state")
