extends GutTest

# MissionPresentation is GameWorld's live mission driver: it owns a real
# Simulation, present pass, and entity index and ticks them in one order.
# These focused presentation tests instantiate it over fixture nodes to prove the
# engine path without introducing a second editor gameplay runtime.

const MissionPresentation := preload("res://game/world/mission_presentation.gd")
const ItemSeatSpecs := preload("res://game/world/item_seat_specs.gd")


func _advance_ticks(runtime: MissionPresentation, delta: float) -> int:
	var input := MissionFrameInput.new()
	input.delta_seconds = delta
	var outcome: MissionFrameOutcome = runtime.advance_session_frame(input)
	return outcome.get_ticks_run() if outcome != null else 0


func test_mission_loadout_chunk_promotes_through_the_native_gate() -> void:
	# The mission loadout chunk -> the sim spawn kit, end to end through the
	# engine's SP-vs-net promotion (world/player_loadout.h, S7b): a REAL mission
	# document's chunk strings stash at load and promote as ints once the weapon
	# catalog can resolve names — offline only, the witnessed gate
	# [orig: Mission_LoadBMSFile @0x40F4E0 — gate @0x40f694; the tuple parse].
	var m := MissionData.new()
	assert_eq(m.create_default(), OK)
	assert_true(m.set_weapon_loadout([
		{ "name": "WPN_KNIFE", "ammo_primary": "3", "ammo_secondary": "0", "flags": "2" }]))
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(m))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	var kit: Array = sim.get_local_player_loadout()
	assert_eq(kit.size(), 1, "one mission row promotes to one kit row")
	assert_eq(String(kit[0]["name"]), "WPN_KNIFE")
	assert_eq(int(kit[0]["ammo_primary"]), 3, "the chunk string reaches the kit as an int")
	assert_eq(int(kit[0]["ammo_secondary"]), 0)
	assert_eq(int(kit[0]["flags"]), 2, "the damage class rides the flags field")
	sim.free()


# Seat-spec EXTRACTION rules (name-prefix typing incl. embedded-token
# rejection, sitexNN pose digits + the 0..30 clamp, the yaw-zero local/yaw
# conversions, addeweap anchor resolution + the parent-root fallback) are
# native (simassets::extract_item_seat_specs) and pinned by
# tests/simassets/seat_spec_extract_test.cpp; ItemSeatSpecs keeps only the
# MCP command-walk mirror exercised below.


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
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/3dp/B50Cal"))
	var spec := item_db.extract_seat_specs_for_item(root, 101419)
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


func test_wire_type_ids_install_the_same_late_vehicle_metadata() -> void:
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/3dp/B50Cal"))
	var sim := Simulation.new()
	autofree(sim)
	assert_false(sim.install_seat_specs_for_type_ids(
			item_db, PackedInt32Array([1419])),
			"the native install has no model source before set_asset_root")
	sim.set_asset_root(root)
	assert_true(sim.install_seat_specs_for_type_ids(
			item_db, PackedInt32Array([1419, 1419, 0])),
			"the joiner prewarm installs from streamed wire type ids")
	assert_eq(int(sim.debug_native_pose_stats()["mounted_graphic_sources"]), 1,
			"duplicate/zero type ids collapse to the one resolved model source")
	# The metadata the install extracted, via the tooling card over the same
	# native extractor (ItemDatabase.extract_seat_specs_for_item).
	var spec := item_db.extract_seat_specs_for_item(root, 101419)
	assert_eq(int(spec.get("item_id", 0)), 101419,
			"the wire type maps back into the items.def id space")
	assert_eq(int(spec.get("type_id", 0)), 1419)
	assert_eq((spec.get("seats", []) as Array).size(), 1,
			"the late join path resolves the model's UseGun seat")
	assert_true(bool(spec.get("mount_config_valid", false)))
	assert_eq(int(spec.get("mount_config", -1)), 4)


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


# The typed owner-anchor registry (ItemEffectDirector) with the registrations
# captured for the pose-follow assertions below.
class CatchupEffectAnchorMount:
	extends ItemEffectDirector
	var anchors: Dictionary = {}

	func register_effect_anchor(owner_key: Variant, resolver: Callable) -> void:
		anchors[owner_key] = resolver
		super.register_effect_anchor(owner_key, resolver)

	func unregister_effect_anchor(owner_key: Variant) -> void:
		anchors.erase(owner_key)
		super.unregister_effect_anchor(owner_key)


class CatchupEffectWorld:
	extends EffectWorld
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
			_position: Vector3, _orientation: Vector3 = Vector3.ZERO) -> Dictionary:
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


# Build a one-organic mission + a real placer registering one real ObjectModel for it by
# (kind,index) — the in-memory mission has bms_id 0, so the present index resolves by the fallback
# key. The runtime builds its EntityIndex from options.placer's construction-time
# placed_entity_records ({model, ref} — the channel MissionObjectPlacer.place() records; never a
# container scan), so the harness registers through that same channel and every setup below passes
# {"placer": w.placer}. The tests assert only Node3D position/visible on the model.
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
	var ref := { "kind": 3, "index": 0, "bms_id": 0, "group": -1, "team": -1, "position": Vector3.ZERO }
	model.set_meta("entity_ref", ref)
	var placer := MissionObjectPlacer.new()
	placer.placed_entity_records.append({ "model": model, "ref": ref })
	return { "mission": md, "container": container, "model": model, "placer": placer }


func test_setup_promotes_and_counts() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	var count := int(rt.setup(w.mission, w.container, {"placer": w.placer}))
	# P7: every preview is the in-process listen server, so the host player auto-spawns at bring-up —
	# the world is the one authored organic + the host player.
	assert_eq(count, 2, "one organic + the auto-spawned host player")
	assert_not_null(rt.get_sim(), "sim created")
	assert_eq(rt.entity_count(), 2)


func test_setup_wires_presented_building_transforms_to_the_shadow_registry() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var ref: Dictionary = mission.add_entity(MissionData.KIND_BUILDING, 102001,
			Vector3(10, 20, 3), Vector3(0, 25, 0))
	assert_false(ref.is_empty())
	if ref.is_empty():
		return
	var bms_id := int(ref.get("bms_id", 0))
	var container := Node3D.new()
	add_child_autofree(container)
	var model := ObjectModel.new()
	container.add_child(model)
	model.set_process(false)
	var placer := MissionObjectPlacer.new()
	placer.placed_entity_records.append({ "model": model, "ref": ref })
	placer.register_static_instance(bms_id, "Caster", 0,
			Transform3D(Basis.IDENTITY, Vector3(-20, -20, -20)), true)
	var revision := placer.get_static_terrain_shadow_source_revision()
	var runtime := MissionPresentation.new()
	add_child_autofree(runtime)
	assert_gt(int(runtime.setup(mission, container, { "placer": placer })), 0)
	assert_true(runtime.tick())
	assert_gt(placer.get_static_terrain_shadow_source_revision(), revision,
			"production MissionPresentation passes its placer into PresentApplier")
	var rows := placer.get_static_terrain_shadow_source_diagnostics()
	assert_eq(rows.size(), 1)
	if rows.size() == 1:
		assert_eq((rows[0] as Dictionary).get("world_transform"), model.transform,
				"production presentation and the shadow registry share one pose")


func test_stats_and_manual_probe_share_one_native_profiling_owner_gate() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var board := FrameStatsBoard.new()
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.set_frame_stats_board(board)
	rt.setup(w.mission, w.container, {"placer": w.placer})
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
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	assert_gt(int(rt.setup(w.mission, w.container, {
		"simulation": joiner,
		"net_transport": "lan-join",
		"placer": w.placer,
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
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	assert_gt(int(rt.setup(w.mission, w.container, {"placer": w.placer})), 0)
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
	# and may carry a flying throwable TrcrID. MissionPresentation must own both
	# consumers on the joiner just as it does on the host. A pre-connected sim
	# pins the real production setup branch without requiring a live peer.
	var w := _make_world(Transform3D.IDENTITY)
	var joiner := Simulation.new()
	assert_true(joiner.enable_join("127.0.0.1", 9, "PresentJoiner"))
	var target := JoinTarget.new()
	target.host_ip = "127.0.0.1"
	target.port = 9
	target.player_name = "PresentJoiner"
	var audio := FireAudioStub.new()
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	assert_gt(int(rt.setup(w.mission, w.container, {
		"simulation": joiner,
		"join_target": target,
		"fire_audio": func(): return audio,
		"placer": w.placer,
	})), 0)

	var fire_stats := rt.get_fire_present_stats()
	assert_not_null(fire_stats,
			"the joiner constructs the fire queue consumer")
	var throwable_stats := rt.get_throwable_present_stats()
	assert_not_null(throwable_stats,
			"the joiner constructs the flying-throwable snapshot consumer")
	assert_eq(throwable_stats.live, 0)

	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/def")), OK)
	assert_eq(rt.get_sim().load_ammo_table(root, "ammo.def"), OK)
	for _frame in range(64):
		assert_gte(rt.get_sim().debug_spawn_round(
				Vector3(0, 2, 0), Vector3.FORWARD,
				"AMMO_CAR15_556MM"), 0)
		assert_eq(_advance_ticks(rt, Simulation.tick_dt()), 1,
				"the live joiner advances through the typed session frame")
		assert_true(rt.get_sim().drain_fire_presentation_events().is_empty(),
				"the runtime drained the complete fire queue this frame")

	# Each remote-style round reaches the joiner's fire presenter once. The
	# SOUND leg is the sim's now (world/fire_sound.h; the fire_sound ctest pins
	# the gate, and fire_present_pass_test pins the drain-to-audio play) — the
	# fixture ammo authors no ai_launch set, so no audio call is expected here.
	assert_eq(int(rt.get_fire_present_stats().fires), 64)


func test_mission_present_stats_are_a_typed_record() -> void:
	var rt := MissionPresentation.new()
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
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, {"placer": w.placer})
	var wire_present := rt.get_wire_presenter()
	assert_not_null(wire_present)
	var reset_wire := Callable(wire_present, "reset_runtime_state")
	assert_true(rt.simulation_restarted.is_connected(reset_wire),
			"Stop clears wire handle/type caches before restored rows present again")
	rt.stop()


func test_presentation_clock_survives_setup_and_forwards_immediately() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.set_presentation_time_ms(0x1ffffffff)
	rt.setup(w.mission, w.container, {"placer": w.placer})
	assert_eq(rt.get_sim().get_panm_time_ms(), 0xffffffff,
		"preconfigured clock is injected after mission-load reset")
	rt.set_presentation_time_ms(1234)
	assert_eq(rt.get_sim().get_panm_time_ms(), 1234,
		"zero-tick and paused frames update collision time immediately")


func test_setup_exposes_normalized_diagnostic_mission_identity() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, {
		"debug_mission_file": "C:\\missions\\00TRe.bms",
		"debug_mission_name": "Training Grounds",
		"placer": w.placer,
	})
	assert_eq(rt.get_mission_file(), "00TRe.bms",
			"diagnostics expose a portable basename, never the editor's local path")
	assert_eq(rt.get_mission_name(), "Training Grounds")


func test_tick_presents_sim_position_onto_node() -> void:
	# The node is authored far from the entity's spawn; after a tick the present pass moves it onto the
	# sim's computed position (the consolidated path the old game path never did).
	var w := _make_world(Transform3D(Basis(), Vector3(99, 99, 99)))
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, {"placer": w.placer})
	rt.step_once()
	var sim_pos: Vector3 = rt.get_sim().get_entity_position(0)
	assert_true((w.model as Node3D).position.is_equal_approx(sim_pos),
		"present moved the node onto the sim entity position")
	assert_false((w.model as Node3D).position.is_equal_approx(Vector3(99, 99, 99)),
		"node left its authored position while playing")


# (Historical transform-restore test deleted: MissionPresentation.stop() still
# rewinds Simulation for teardown/fixtures, but ONED no longer owns a live
# runtime whose Stop must restore authored editor nodes.)



func test_tick_and_step_advance_and_present_like_the_game() -> void:
	# The standalone driver's tick() dispatches exactly one logic tick
	# (the engine's own dividers — WAC every 62nd tick, BMS quarter-pass every 16th — gate inside
	# the systems), so direct fixture tick() and Step must both advance + present.
	var w := _make_world(Transform3D(Basis(), Vector3(99, 99, 99)))
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, {"placer": w.placer})
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
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, { "action_type": 6, "param1": 42 }).is_empty())
	var container := Node3D.new()
	add_child_autofree(container)

	var rt := MissionPresentation.new()
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


# --- Fixed-timestep accumulator (session_frame): the sim runs at a constant 62.5 Hz independent of
# the render/frame rate. [orig: Game_MainLoop @ 0x52b630 -> Game_ProcessMainFrame @ 0x5263f0] -----

func test_session_frame_accumulates_fixed_quanta() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, {"placer": w.placer})
	rt.play()
	# 0.1 s of wall-clock at 62.5 Hz = floor(0.1 / 0.016) = 6 ticks.
	assert_eq(_advance_ticks(rt, 0.1), 6, "0.1 s banks 6 fixed-step ticks")
	# Sub-quantum deltas accumulate ACROSS calls instead of each firing a tick.
	assert_eq(_advance_ticks(rt, 0.008), 0, "half a quantum (plus the 0.004 remainder) fires nothing yet")
	assert_eq(_advance_ticks(rt, 0.008), 1, "the banked remainder crosses one quantum and fires once")


func test_session_frame_clamps_catchup() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, {"placer": w.placer})
	rt.play()
	# 1.0 s would be ~62 ticks; the spiral-of-death clamp caps a single frame's
	# catch-up at the native world::TickAccumulator::kMaxCatchupTicks (S14).
	assert_eq(_advance_ticks(rt, 1.0), 31, "a long stall is clamped to the catch-up cap")
	# The clamp DROPS the backlog (no banked spiral): a tiny delta afterward fires nothing.
	assert_eq(_advance_ticks(rt, 0.001), 0, "the backlog was dropped, not carried into the next frames")


func test_session_frame_ignored_when_not_playing() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, {"placer": w.placer})
	# Not played -> paused -> banks nothing regardless of elapsed wall-clock (no burst on Play).
	assert_eq(_advance_ticks(rt, 1.0), 0, "a paused runtime banks nothing")
	rt.play()
	assert_eq(_advance_ticks(rt, 0.0), 0, "play() reset the accumulator; zero delta fires nothing")


func test_session_frame_still_presents_a_zero_tick_render_frame() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, {"placer": w.placer})
	rt.play()
	assert_eq(_advance_ticks(rt, Simulation.tick_dt()), 1,
			"seed one decoded presentation snapshot")
	(w.model as Node3D).visible = false
	assert_eq(_advance_ticks(rt, 0.0), 0, "no fixed simulation tick advances")
	assert_true((w.model as Node3D).visible,
			"the render frame still reapplies current visibility state")


func test_session_frame_presents_latest_state_once() -> void:
	# The node is authored far from spawn; after a catch-up batch the single present puts it on the
	# sim's LATEST position (decoupled render = present once per render frame, no inter-tick interpolation).
	var w := _make_world(Transform3D(Basis(), Vector3(99, 99, 99)))
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, {"placer": w.placer})
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
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, {"placer": w.placer})
	var entity_ref := {"kind": 3, "index": 0, "bms_id": 0}
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


func test_catchup_advances_round_move_effect_at_each_live_pose_and_stops_before_expiry() -> void:
	# The scene-node present stays batched, but a round-bound effects_table move
	# group is part of the fixed-tick particle simulation. A four-second
	# flashbang exercises the same attached-effect path as the smoke grenade in
	# a short, production-authored lifetime.
	var w := _make_world(Transform3D.IDENTITY)
	var anchor_mount := CatchupEffectAnchorMount.new()
	var effect_world := CatchupEffectWorld.new(anchor_mount)
	add_child_autofree(effect_world)
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, {
		"fire_fx": func() -> Variant: return effect_world,
		"effect_anchors": anchor_mount,
		"placer": w.placer,
	})
	var def_root := ResourceRoot.new()
	def_root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/def"))
	assert_eq(rt.get_sim().load_ammo_table(def_root, "ammo.def"), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	rt.get_sim().resolve_item_traits(item_db)
	assert_gte(rt.get_sim().debug_spawn_round(
			Vector3(100, 100, 100), Vector3.RIGHT, "grenadefb"), 0)
	rt.fixed_tick_completed.connect(func(_logic_tick: int) -> void:
		effect_world.advance_fixed_tick(Simulation.tick_dt())
	)
	rt.play()

	assert_eq(_advance_ticks(rt, 0.05), 3,
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
		assert_eq(_advance_ticks(rt, 31.0 * Simulation.tick_dt()), 31)
	assert_eq(_advance_ticks(rt, 20.0 * Simulation.tick_dt()), 20)
	assert_eq(effect_world.fixed_advance_count, 240)
	assert_eq(_advance_ticks(rt, 12.0 * Simulation.tick_dt()), 12)
	assert_eq(effect_world.fixed_advance_count, 252)
	assert_eq(effect_world.active_tick_numbers.size(), 247,
			"the group advances exactly while the round is alive")
	assert_eq(effect_world.active_tick_numbers[-1], 247,
			"the expiry tick never advances a released round effect")
	assert_eq(effect_world.stops_after_advance, [247],
			"release happens before fixed advance 248, inside the catch-up batch")


func test_session_frame_drains_effects_per_tick() -> void:
	# Effects must drain PER logic tick INSIDE the catch-up batch (not coalesced into one emit at the
	# end): the BMS quarter-pass one-shot still surfaces when many ticks run in a single real-time frame.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, { "action_type": 6, "param1": 42 }).is_empty())
	var container := Node3D.new()
	add_child_autofree(container)
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.setup(md, container)
	rt.play()
	var drained: Array = []
	rt.effects_drained.connect(func(effects): drained.append_array(effects))
	# 20.5 quanta of wall-clock in ONE frame -> 20 ticks; crosses the 16th-tick quarter-pass boundary.
	assert_eq(_advance_ticks(rt, 0.328), 20, "20+ quanta of wall-clock run 20 logic ticks in one frame")
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


# Drive a fresh MissionPresentation with `count` frames of `step` seconds each and report total ticks +
# the entity position. Fixed iteration count (not a while-elapsed loop) keeps the fed wall-clock exact.
func _run_realtime(step: float, count: int) -> Dictionary:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, {"placer": w.placer})
	rt.play()
	var ticks := 0
	for _i in range(count):
		ticks += _advance_ticks(rt, step)
	return { "ticks": ticks, "pos": rt.get_sim().get_entity_position(0) }
