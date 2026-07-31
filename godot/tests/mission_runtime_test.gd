extends GutTest

# MissionRuntime is GameWorld's live mission driver: it owns a real
# NovaSimulation, present pass, and entity index and ticks them in one order.
# These focused tests instantiate it directly over fixture nodes to prove the
# engine path without introducing a second editor gameplay runtime.

const MissionRuntime := preload("res://engine/world/mission_runtime.gd")
const ItemSeatSpecs := preload("res://engine/world/item_seat_specs.gd")


func test_kit_from_loadout_rows_reads_the_mission_tuple_keys() -> void:
	# The stringly-typed seam between NovaMissionData's loadout dictionaries and the sim
	# spawn kit: feed a REAL mission document (not hand-built rows) so a key drift on either
	# side breaks here instead of silently degrading the kit to all -1 defaults at promote.
	var m := NovaMissionData.new()
	assert_eq(m.create_default(), OK)
	assert_true(m.set_weapon_loadout([
		{ "name": "WPN_KNIFE", "ammo_primary": "3", "ammo_secondary": "0", "flags": "2" }]))
	var kit := MissionRuntime.kit_from_loadout_rows(m.get_weapon_loadout())
	assert_eq(kit.size(), 1, "one mission row promotes to one kit row")
	assert_eq(String(kit[0]["name"]), "WPN_KNIFE")
	assert_eq(int(kit[0]["ammo_primary"]), 3, "the chunk string reaches the kit as an int")
	assert_eq(int(kit[0]["ammo_secondary"]), 0)
	assert_eq(int(kit[0]["flags"]), 2, "the damage class rides the flags field")


func test_seat_userpoint_prefixes_match_original_seat_names() -> void:
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	assert_eq(rt._seat_type_for_user_point("sitex00"), 1, "sitex -> passenger")
	assert_eq(rt._seat_type_for_user_point("ctrlx"), 2, "ctrlx -> controller")
	assert_eq(rt._seat_type_for_user_point("UseGun01"), 3, "UseGun -> gunner")
	assert_eq(rt._seat_type_for_user_point("drvrx"), 5, "drvrx -> driver")
	assert_eq(rt._seat_type_for_user_point("ground"), 0, "non-seat userpoints are ignored")
	assert_eq(ItemSeatSpecs.seat_type_for_user_point("fooUseGun"), 0,
			"retail classifies a prefix, not an embedded seat token")
	assert_eq(ItemSeatSpecs.seat_type_for_user_point("xctrlx"), 0,
			"embedded controller text is not a model seat row")


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


func test_shared_seat_rules_predict_original_command_rules() -> void:
	var seats := [
		{ "type": 1, "position": Vector3(5, 0, 0), "source_name": "sitex00" },
		{ "type": 2, "position": Vector3(1, 0, 0), "source_name": "ctrlx00" },
		{ "type": 5, "position": Vector3(2, 0, 0), "source_name": "drvrx00" },
	]
	var passenger := ItemSeatSpecs.predict_best_seat(seats, 123)
	assert_eq(int(passenger["seat_index"]), 0, "command 123 is passenger-only")
	assert_eq(String(passenger["seat"]["source_name"]), "sitex00")

	var non_controller := ItemSeatSpecs.predict_best_seat(seats, 124)
	assert_eq(int(non_controller["seat_index"]), 2, "command 124 skips ctrlx and takes driver before passenger")
	assert_eq(String(non_controller["seat"]["source_name"]), "drvrx00")

	var any := ItemSeatSpecs.predict_best_seat(seats, 125)
	assert_eq(int(any["seat_index"]), 1, "command 125 can select ctrlx by original priority")
	assert_eq(String(any["seat"]["source_name"]), "ctrlx00")
	assert_eq(String(any["candidates"][1]["status"]), "selected")


func test_production_seat_specs_extract_target_phrase_set_config() -> void:
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/3dp/B50Cal"))
	var spec := ItemSeatSpecs.seat_specs_for_item(
			root, item_db, 101419, 101419)
	assert_eq(String(spec.get("error", "")), "")
	var seats: Array = spec.get("seats", []) as Array
	assert_eq(seats.size(), 1,
			"the witnessed B50Cal target contributes exactly its Usegun seat")
	if seats.size() == 1:
		assert_eq(String((seats[0] as Dictionary).get("source_name", "")), "Usegun")
		assert_eq(int((seats[0] as Dictionary).get("bone_index", 0)), 6,
				"the wire byte is the 1-based USRP table row, not a seat ordinal")
		assert_eq(int((seats[0] as Dictionary).get("type", 0)), 3)
		assert_eq(int((seats[0] as Dictionary).get("retail_slot", -1)), 9,
				"UseGun occupies fixed retail mountHandles slot 9")
	assert_true(bool(spec.get("mount_config_valid", false)),
			"authored phrase_set marks target config valid")
	assert_eq(int(spec.get("mount_config", -1)), 4,
			"target itemDef+0x86c phrase_set reaches the production seat spec")


func test_emplacement_specs_resolve_userpoints_and_keep_missing_anchor_fallback() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/3dp/B50Cal/B50Cal.3di")), OK)
	var rows := [
		{"kind": 0, "key": "addeweap", "userpoint": "Usegun", "item_id": 710101},
		{"kind": 1, "key": "addeweapG", "userpoint": "missing", "item_id": 710102},
	]
	var resolved := ItemSeatSpecs.emplacement_specs_from_model(
			data, rows, true)
	assert_eq(resolved.size(), 2, "a missing retail anchor still spawns at parent root")
	assert_true(resolved[0]["anchor_found"])
	assert_eq(resolved[0]["bone_index"], 6,
			"attachment bone is the 1-based source USRP row")
	assert_eq(resolved[0]["subobject"],
			int(data.get_user_point_info(5)["subobject"]))
	assert_eq(resolved[0]["local"],
			ItemSeatSpecs.seat_local_from_user_point_position(
					data.get_user_point_info(5)["position"]))
	assert_false(resolved[1]["anchor_found"])
	assert_eq(resolved[1]["bone_index"], 0)
	assert_eq(resolved[1]["local"], Vector3.ZERO,
			"retail's unresolved-anchor fallback copies the parent root")
	assert_eq(resolved[1]["key"], "addeweapG",
			"variant metadata survives model resolution")


# An animatable placed entity: play_part_anim marks it for the registry, set_part_phase + Node3D
# transform/visible let the present pass drive it.
class FakeModel:
	extends Node3D
	var phases: Array = []
	func play_part_anim(_channel: int, _play_type: int, _time_s: float) -> void:
		pass
	func set_part_phase(channel: int, phase: int) -> void:
		phases.append([channel, phase])


class FireAudioStub:
	extends RefCounted
	var calls: Array = []

	func fire_soundset(set_name: String, world_pos: Vector3,
			source_bms_id: int = 0) -> bool:
		calls.append({
			"set": set_name,
			"pos": world_pos,
			"source_bms_id": source_bms_id,
		})
		return true

	func slot_soundset(_set_name: String, _world_pos: Vector3,
			_exclusive_key: String = "") -> bool:
		return true


class CatchupEffectAnchorMount:
	extends RefCounted
	var anchors: Dictionary = {}

	func register_effect_anchor(owner_key: Variant, resolver: Callable) -> void:
		anchors[owner_key] = resolver

	func unregister_effect_anchor(owner_key: Variant) -> void:
		anchors.erase(owner_key)


class CatchupEffectWorld:
	extends RefCounted
	var anchor_mount: CatchupEffectAnchorMount
	var owner_key: Variant
	var group_live := false
	var spawn_count := 0
	var fixed_advance_count := 0
	var active_tick_numbers: Array[int] = []
	var active_poses: Array[Transform3D] = []
	var stops_after_advance: Array[int] = []

	func _init(mount: CatchupEffectAnchorMount) -> void:
		anchor_mount = mount

	func spawn_effect_owned_request(key: Variant, _name: String,
			_position: Vector3, _orientation: Vector3) -> Dictionary:
		owner_key = key
		group_live = true
		spawn_count += 1
		return {"spawned": true, "effect_handle": 1, "group_id": 91}

	func stop_group(group_id: int) -> void:
		assert(group_id == 91)
		group_live = false
		stops_after_advance.append(fixed_advance_count)

	func advance_fixed_tick(_delta: float) -> void:
		fixed_advance_count += 1
		if not group_live:
			return
		var resolver: Variant = anchor_mount.anchors.get(owner_key)
		if not (resolver is Callable) or not (resolver as Callable).is_valid():
			return
		var pose: Variant = (resolver as Callable).call()
		if pose is Transform3D:
			active_tick_numbers.append(fixed_advance_count)
			active_poses.append(pose)


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
	var count := int(rt.setup(w.mission, w.container))
	# P7: every preview is the in-process listen server, so the host player auto-spawns at bring-up —
	# the world is the one authored organic + the host player.
	assert_eq(count, 2, "one organic + the auto-spawned host player")
	assert_not_null(rt.get_sim(), "sim created")
	assert_eq(rt.entity_count(), 2)


func test_stats_and_manual_probe_share_one_native_profiling_owner_gate() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var board := FrameStatsBoard.new()
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.set_frame_stats_board(board)
	rt.setup(w.mission, w.container)
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
	var joiner := NovaSimulation.new()
	assert_true(joiner.enable_join("127.0.0.1", 9, "TransportLockJoiner"))
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	assert_gt(int(rt.setup(w.mission, w.container, {
		"simulation": joiner,
		"net_transport": "lan-join",
	})), 0)

	assert_true(rt.is_transport_locked(), "a joiner runtime reports a locked transport")
	rt.play()
	assert_true(rt.is_playing(), "play still starts the session ticking")
	rt.pause()
	assert_true(rt.is_playing(), "F3 Pause cannot silence a live joiner's socket")
	rt.step_once()
	assert_true(rt.is_playing(), "F3 Step cannot drop a live joiner out of the tick loop")
	rt.stop()
	assert_true(rt.is_playing(), "F3 Stop cannot rewind the world under a live peer")


func test_transport_still_works_for_a_local_runtime() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	assert_gt(int(rt.setup(w.mission, w.container)), 0)
	# A directly instantiated runtime has no bound socket or peers, so it is not
	# a live net session and keeps its transport.
	assert_false(rt.is_transport_locked(),
			"a local runtime is not a live net session")
	rt.play()
	assert_true(rt.is_playing())
	rt.pause()
	assert_false(rt.is_playing(), "the local runtime transport still pauses")


func test_joiner_runtime_owns_fire_and_throwable_presenters() -> void:
	# Joiner S2C tag-2 descriptors append to the visual RoundSim's fired queue
	# and may carry a flying throwable TrcrID. MissionRuntime must own both
	# consumers on the joiner just as it does on the host. A pre-connected sim
	# pins the real production setup branch without requiring a live peer.
	var w := _make_world(Transform3D.IDENTITY)
	var joiner := NovaSimulation.new()
	assert_true(joiner.enable_join("127.0.0.1", 9, "PresentJoiner"))
	var target := JoinTarget.new()
	target.host_ip = "127.0.0.1"
	target.port = 9
	target.player_name = "PresentJoiner"
	var audio := FireAudioStub.new()
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	assert_gt(int(rt.setup(w.mission, w.container, {
		"simulation": joiner,
		"join_target": target,
		"fire_audio": func(): return audio,
	})), 0)

	var fire_stats := rt.get_fire_present_stats()
	assert_true(fire_stats.has("fires"),
			"the joiner constructs the fire queue consumer")
	var throwable_stats := rt.get_throwable_present_stats()
	assert_not_null(throwable_stats,
			"the joiner constructs the flying-throwable snapshot consumer")
	assert_eq(throwable_stats.live, 0)

	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/def")), OK)
	assert_eq(rt.get_sim().load_ammo_table(root, "ammo.def"), OK)
	for _frame in range(64):
		assert_gte(rt.get_sim().debug_spawn_round(
				Vector3(0, 2, 0), Vector3.FORWARD,
				"AMMO_CAR15_556MM"), 0)
		assert_true(rt.tick())
		assert_true(rt.get_sim().drain_fire_presentation_events().is_empty(),
				"the runtime drained the complete fire queue this frame")

	assert_eq(audio.calls.size(), 64,
			"each remote-style round reaches the joiner's fire presenter once")
	assert_eq(int(rt.get_fire_present_stats()["fires"]), 64)


func test_mission_present_stats_are_a_typed_record() -> void:
	var rt := MissionRuntime.new()
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
	assert_eq(stats.muzzle_queries, 0)


func test_wire_presenter_resets_with_runtime_stop() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	var placer := RefCounted.new()
	rt.setup(w.mission, w.container, {"placer": placer})
	var wire_present := rt.get_wire_presenter()
	assert_not_null(wire_present)
	var reset_wire := Callable(wire_present, "reset_runtime_state")
	assert_true(rt.simulation_restarted.is_connected(reset_wire),
			"Stop clears wire handle/type caches before restored rows present again")
	rt.stop()


func test_presentation_clock_survives_setup_and_forwards_immediately() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.set_presentation_time_ms(0x1ffffffff)
	rt.setup(w.mission, w.container)
	assert_eq(rt.get_sim().get_panm_time_ms(), 0xffffffff,
		"preconfigured clock is injected after mission-load reset")
	rt.set_presentation_time_ms(1234)
	assert_eq(rt.get_sim().get_panm_time_ms(), 1234,
		"zero-tick and paused frames update collision time immediately")


func test_setup_exposes_normalized_diagnostic_mission_identity() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, {
		"debug_mission_file": "C:\\missions\\00TRe.bms",
		"debug_mission_name": "Training Grounds",
	})
	assert_eq(rt.get_mission_file(), "00TRe.bms",
			"diagnostics expose a portable basename, never the editor's local path")
	assert_eq(rt.get_mission_name(), "Training Grounds")


func test_tick_presents_sim_position_onto_node() -> void:
	# The node is authored far from the entity's spawn; after a tick the present pass moves it onto the
	# sim's computed position (the consolidated path the old game path never did).
	var w := _make_world(Transform3D(Basis(), Vector3(99, 99, 99)))
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container)
	rt.step_once()
	var sim_pos: Vector3 = rt.get_sim().get_entity_position(0)
	assert_true((w.model as Node3D).position.is_equal_approx(sim_pos),
		"present moved the node onto the sim entity position")
	assert_false((w.model as Node3D).position.is_equal_approx(Vector3(99, 99, 99)),
		"node left its authored position while playing")


# (Historical transform-restore test deleted: MissionRuntime.stop() still
# rewinds NovaSimulation for teardown/fixtures, but ONED no longer owns a live
# runtime whose Stop must restore authored editor nodes.)



func test_tick_and_step_advance_and_present_like_the_game() -> void:
	# The standalone driver's tick() dispatches exactly one logic tick
	# (the engine's own dividers — WAC every 62nd tick, BMS quarter-pass every 16th — gate inside
	# the systems), so direct fixture tick() and Step must both advance + present.
	var w := _make_world(Transform3D(Basis(), Vector3(99, 99, 99)))
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container)
	assert_true(rt.tick(), "tick() advances one logic tick")
	rt.step_once()
	var sim_pos: Vector3 = rt.get_sim().get_entity_position(0)
	assert_true((w.model as Node3D).position.is_equal_approx(sim_pos),
		"Step presents the sim state onto the node")


func test_effects_drained_signal_fires() -> void:
	# A mission runtime drains side effects each tick; the shell listens on effects_drained. Build an
	# unconditional OutputText event and confirm the signal carries it.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, { "action_type": 6, "param1": 42 }).is_empty())
	var container := Node3D.new()
	add_child_autofree(container)

	var rt := MissionRuntime.new()
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
	assert_eq(String((drained[0] as Dictionary)["kind"]), "text", "OutputText -> text effect")


# --- Fixed-timestep accumulator (tick_realtime): the sim runs at a constant 62.5 Hz independent of
# the render/frame rate. [orig: Game_MainLoop @ 0x52b630 -> Game_ProcessMainFrame @ 0x5263f0] -----

func test_tick_realtime_accumulates_fixed_quanta() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container)
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
	rt.setup(w.mission, w.container)
	rt.play()
	# 1.0 s would be ~62 ticks; the spiral-of-death clamp caps a single frame's catch-up.
	assert_eq(rt.tick_realtime(1.0), MissionRuntime.MAX_CATCHUP_TICKS, "a long stall is clamped to the catch-up cap")
	# The clamp DROPS the backlog (no banked spiral): a tiny delta afterward fires nothing.
	assert_eq(rt.tick_realtime(0.001), 0, "the backlog was dropped, not carried into the next frames")


func test_tick_realtime_ignored_when_not_playing() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container)
	# Not played -> paused -> banks nothing regardless of elapsed wall-clock (no burst on Play).
	assert_eq(rt.tick_realtime(1.0), 0, "a paused runtime banks nothing")
	rt.play()
	assert_eq(rt.tick_realtime(0.0), 0, "play() reset the accumulator; zero delta fires nothing")


func test_tick_realtime_still_presents_a_zero_tick_render_frame() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container)
	rt.play()
	assert_eq(rt.tick_realtime(MissionRuntime.TICK_DT), 1,
			"seed one decoded presentation snapshot")
	(w.model as Node3D).visible = false
	assert_eq(rt.tick_realtime(0.0), 0, "no fixed simulation tick advances")
	assert_true((w.model as Node3D).visible,
			"the render frame still reapplies current visibility state")


func test_tick_realtime_presents_latest_state_once() -> void:
	# The node is authored far from spawn; after a catch-up batch the single present puts it on the
	# sim's LATEST position (decoupled render = present once per render frame, no inter-tick interpolation).
	var w := _make_world(Transform3D(Basis(), Vector3(99, 99, 99)))
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container)
	rt.play()
	assert_gt(rt.tick_realtime(0.1), 0, "the batch ran at least one tick")
	var sim_pos: Vector3 = rt.get_sim().get_entity_position(0)
	assert_true((w.model as Node3D).position.is_equal_approx(sim_pos),
		"the node ends on the latest sim position after the batch")
	assert_false((w.model as Node3D).position.is_equal_approx(Vector3(99, 99, 99)),
		"the node left its authored position")


func test_catchup_exposes_each_fixed_ticks_pose_before_batched_presentation() -> void:
	var authored := Transform3D(Basis.IDENTITY, Vector3(99, 99, 99))
	var w := _make_world(authored)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container)
	var entity_ref := {"kind": 3, "index": 0, "bms_id": 0}
	var observed: Array = []
	rt.fixed_tick_completed.connect(func(_logic_tick: int) -> void:
		observed.append({
			"effect_pose": rt.presented_entity_effect_transform(entity_ref),
			"node_pose": (w.model as Node3D).global_transform,
		})
	)
	rt.play()
	assert_eq(rt.tick_realtime(0.05), 3, "one render frame catches up three fixed ticks")
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


func test_catchup_advances_round_move_effect_at_each_live_pose_and_stops_before_expiry() -> void:
	# The scene-node present stays batched, but a round-bound effects_table move
	# group is part of the fixed-tick particle simulation. A four-second
	# flashbang exercises the same attached-effect path as the smoke grenade in
	# a short, production-authored lifetime.
	var w := _make_world(Transform3D.IDENTITY)
	var anchor_mount := CatchupEffectAnchorMount.new()
	var effect_world := CatchupEffectWorld.new(anchor_mount)
	var rt := MissionRuntime.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, {
		"fire_fx": func() -> Variant: return effect_world,
		"game_world": anchor_mount,
	})
	var def_root := NovaResourceRoot.new()
	def_root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/def"))
	assert_eq(rt.get_sim().load_ammo_table(def_root, "ammo.def"), OK)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	rt.get_sim().resolve_item_traits(item_db)
	assert_gte(rt.get_sim().debug_spawn_round(
			Vector3(100, 100, 100), Vector3.RIGHT, "grenadefb"), 0)
	rt.fixed_tick_completed.connect(func(_logic_tick: int) -> void:
		effect_world.advance_fixed_tick(MissionRuntime.TICK_DT)
	)
	rt.play()

	assert_eq(rt.tick_realtime(0.05), 3,
			"one render frame catches up the first three round ticks")
	assert_eq(effect_world.spawn_count, 1,
			"the attached move group exists before its first particle advance")
	assert_eq(effect_world.active_tick_numbers, [1, 2, 3],
			"every birth-batch fixed tick advances the live group")
	assert_eq(effect_world.active_poses.size(), 3)
	if effect_world.active_poses.size() == 3:
		assert_false(effect_world.active_poses[0].origin.is_equal_approx(
				effect_world.active_poses[1].origin),
				"the second emission sees the second simulated round pose")
		assert_false(effect_world.active_poses[1].origin.is_equal_approx(
				effect_world.active_poses[2].origin),
				"catch-up does not emit repeatedly from the frame-start pose")

	# max_age 4 parses to 248 fixed ticks. Stop at age 240, then expire eight
	# ticks into a twelve-tick catch-up batch: advances 248..252 must observe no
	# live group, rather than emitting five stale ticks until final presentation.
	for _batch in range(7):
		assert_eq(rt.tick_realtime(31.0 * MissionRuntime.TICK_DT), 31)
	assert_eq(rt.tick_realtime(20.0 * MissionRuntime.TICK_DT), 20)
	assert_eq(effect_world.fixed_advance_count, 240)
	assert_eq(rt.tick_realtime(12.0 * MissionRuntime.TICK_DT), 12)
	assert_eq(effect_world.fixed_advance_count, 252)
	assert_eq(effect_world.active_tick_numbers.size(), 247,
			"the group advances exactly while the round is alive")
	assert_eq(effect_world.active_tick_numbers[-1], 247,
			"the expiry tick never advances a released round effect")
	assert_eq(effect_world.stops_after_advance, [247],
			"release happens before fixed advance 248, inside the catch-up batch")


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
	rt.setup(md, container)
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
	rt.setup(w.mission, w.container)
	rt.play()
	var ticks := 0
	for _i in range(count):
		ticks += rt.tick_realtime(step)
	return { "ticks": ticks, "pos": rt.get_sim().get_entity_position(0) }
