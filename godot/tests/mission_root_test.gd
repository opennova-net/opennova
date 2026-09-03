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


func test_mission_loadout_chunk_promotes_through_the_native_gate() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# The mission loadout chunk -> the sim spawn kit, end to end through the
	# engine's SP-vs-net promotion (world/player_loadout.h, S7b): a REAL mission
	# document's chunk strings stash at load and promote as ints once the weapon
	# catalog can resolve names — offline only, the witnessed gate
	# [orig: Mission_LoadBMSFile @0x40F4E0 — gate @0x40f694; the tuple parse].
	var m := MissionData.new()
	assert_eq(m.create_default(), OK)
	assert_true(m.set_weapon_loadout([
		{"name": "WPN_KNIFE", "ammo_primary": "3", "ammo_secondary": "0", "flags": "2"}]))
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(m))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	var kit: Array = sim.get_local_player_loadout()
	assert_eq(kit.size(), 1, "one mission row promotes to one kit row")
	assert_eq(String(kit[0]["name"]), "WPN_KNIFE")
	assert_eq(int(kit[0]["ammo_primary"]), 3, "the chunk string reaches the kit as an int")
	assert_eq(int(kit[0]["ammo_secondary"]), 0)
	assert_eq(int(kit[0]["flags"]), 2, "the damage class rides the flags field")


# Seat-spec EXTRACTION rules (name-prefix typing incl. embedded-token
# rejection, sitexNN pose digits + the 0..30 clamp, the yaw-zero local/yaw
# conversions, addeweap anchor resolution + the parent-root fallback) are
# native (simassets::extract_item_seat_specs) and pinned by
# tests/simassets/seat_spec_extract_test.cpp; the attach-command seat
# selection (world/vehicle_attach.h) by tests/world/seat_prediction_test.cpp.


func test_production_seat_specs_extract_target_phrase_set_config() -> void:
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth"))
	var spec := item_db.extract_seat_specs_for_item(root, 101419)
	assert_eq(spec.error, "")
	var seats := spec.get_seats()
	assert_eq(seats.size(), 1,
			"the witnessed mount target contributes exactly its Usegun seat")
	if seats.size() == 1:
		var seat: EntityCardSeat = seats[0]
		assert_eq(seat.get_source_name(), "Usegun")
		assert_eq(seat.get_bone_index(), 6,
				"the wire byte is the 1-based USRP table row, not a seat ordinal")
		assert_eq(seat.get_type(), 3)
		assert_eq(seat.get_retail_slot(), 9,
				"UseGun occupies fixed retail mountHandles slot 9")
		assert_false(seat.is_occupied(), "a def-level seat row is static data")
	assert_true(spec.is_mount_config_valid(),
			"authored phrase_set marks target config valid")
	assert_eq(spec.mount_config, 4,
			"target itemDef+0x86c phrase_set reaches the production seat spec")


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
	assert_eq(sim.debug_native_pose_stats().mounted_graphic_sources, 1,
			"duplicate/zero type ids collapse to the one resolved model source")
	# The metadata the install extracted, via the tooling card over the same
	# native extractor (ItemDatabase.extract_seat_specs_for_item).
	var spec := item_db.extract_seat_specs_for_item(root, 101419)
	assert_eq(spec.item_id, 101419,
			"the wire type maps back into the items.def id space")
	assert_eq(spec.type_id, 1419)
	assert_eq(spec.get_seats().size(), 1,
			"the late join path resolves the model's UseGun seat")
	assert_true(spec.is_mount_config_valid())
	assert_eq(spec.mount_config, 4)


# The flashbang's effects_table tag-1 "move" effect (retail ammo.def grenadefb):
# the round-bound particle the pose-follow test below watches.
const FLASHBANG_MOVE_EFFECT := "Effect_FlashBangToss"


# One synthetic in-memory particle catalog authoring that move effect as a
# FOREVEREMIT definition (the effect_world_test recipe), so a REAL EffectWorld
# interns and spawns it without a resource root and the round-bound group
# stays live until the throwable pass stops it.
func _catalog_file() -> ParticleFile:
	var def := ParticleDef.new()
	def.id = "toss dots"
	def.emit_dur = 0.1
	def.emit_rate = 50.0
	def.emit_burst = 4
	def.age = 0.2
	def.alpha = 1.0
	def.scale_value = 1.0
	def.flags = ParticleDef.FLAG_FOREVER_EMIT
	var effect := ParticleEffect.new()
	effect.id = FLASHBANG_MOVE_EFFECT
	effect.pdefs = PackedStringArray(["toss dots"])
	var file := ParticleFile.new()
	var particles: Array = file.particles
	particles.append(def)
	file.particles = particles
	var effects: Array = file.effects
	effects.append(effect)
	file.effects = effects
	return file


# The round-bound move group in the effect world's public report: by id once
# known (a released owner drops its key from the row, so a retired group is
# only reachable by id), else the first row owned by a throwable-move key.
# {} when there is none or it was swept.
func _round_move_group(fx: EffectWorld, group_id: int) -> EffectGroupReport:
	for row_v in fx.get_debug_group_report():
		var row := row_v as EffectGroupReport
		if group_id > 0:
			if int(row.id) == group_id:
				return row
		else:
			var owner: Variant = row.owner_key
			if owner is String and (owner as String).begins_with("throwable-move:"):
				return row
	return null


# Every report row still owned by a throwable-move key (the live spawn census).
func _round_move_rows(fx: EffectWorld) -> Array:
	var out: Array = []
	for row_v in fx.get_debug_group_report():
		var owner: Variant = (row_v as EffectGroupReport).owner_key
		if owner is String and (owner as String).begins_with("throwable-move:"):
			out.append(row_v)
	return out


# Build a one-organic mission + a real placer registering one real ObjectModel for it by
# (kind,index) — the in-memory mission has bms_id 0, so the present index resolves by the fallback
# key. The runtime builds its EntityIndex from options.placer's construction-time
# placed_models (each model carrying its EntityRef — the channel MissionObjectPlacer.place()
# registers; never a container scan), so the harness registers through that same channel and
# every setup below passes
# _options_with_placer(w.placer). The tests assert only Node3D position/visible on the model.
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
	model.entity_ref = EntityRef.make(MissionData.KIND_BUILDING,
			int(ref.get("index", -1)), bms_id, 102001)
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
	var rows := placer.get_static_terrain_shadow_source_diagnostics()
	assert_eq(rows.size(), 1)
	if rows.size() == 1:
		assert_eq((rows[0] as StaticTerrainShadowSourceRow).world_transform, model.transform,
				"production presentation and the shadow registry share one pose")


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
	assert_true(rt.is_playing(), "F3 Pause cannot silence a live joiner's socket")
	rt.step_once()
	assert_true(rt.is_playing(), "F3 Step cannot drop a live joiner out of the tick loop")
	rt.stop()
	assert_true(rt.is_playing(), "F3 Stop cannot rewind the world under a live peer")


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


func test_joiner_runtime_owns_fire_and_throwable_presenters() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# Joiner S2C tag-2 descriptors append to the visual RoundSim's fired queue
	# and may carry a flying throwable TrcrID. MissionRoot must own both
	# consumers on the joiner just as it does on the host. A pre-connected sim
	# pins the real production setup branch without requiring a live peer.
	var w := _make_world(Transform3D.IDENTITY)
	var joiner := Simulation.new()
	assert_true(joiner.enable_join("127.0.0.1", 9, "PresentJoiner"))
	var target := JoinTarget.new()
	target.host_ip = "127.0.0.1"
	target.port = 9
	target.player_name = "PresentJoiner"
	# A REAL bank-less MissionAudio as the fire pass's sound sink (ADR 0043
	# rule 11; its recent-fires ring is the read seam).
	var audio := MissionAudio.create(null, null)
	autofree(audio)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	var join_options := _options_with_placer(w.placer)
	join_options.simulation = joiner
	join_options.join_target = target
	assert_gt(int(rt.setup(w.mission, w.container, join_options)), 0)
	rt.setup_passes(audio, null, null, null, null)

	var fire_stats: FirePresentStats = rt.get_fire_present_stats()
	assert_not_null(fire_stats,
			"the joiner constructs the fire queue consumer")
	var throwable_stats: ThrowablePresentStats = rt.get_throwable_present_stats()
	assert_not_null(throwable_stats,
			"the joiner constructs the flying-throwable snapshot consumer")
	assert_eq(throwable_stats.live, 0)

	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
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
	# fixture ammo authors no ai_launch set, so no audio fire is expected here.
	assert_eq(int(rt.get_fire_present_stats().fires), 64)
	assert_true(audio.recent_fired_soundsets().is_empty(),
			"the fixture ammo authors no ai_launch set: the bank fired nothing")


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
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, { "action_type": 6, "param1": 42 }).is_empty())
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
	# 0.1 s of wall-clock at 62.5 Hz = floor(0.1 / 0.016) = 6 ticks.
	assert_eq(_advance_ticks(rt, 0.1), 6, "0.1 s banks 6 fixed-step ticks")
	# Sub-quantum deltas accumulate ACROSS calls instead of each firing a tick.
	assert_eq(_advance_ticks(rt, 0.008), 0, "half a quantum (plus the 0.004 remainder) fires nothing yet")
	assert_eq(_advance_ticks(rt, 0.008), 1, "the banked remainder crosses one quantum and fires once")


func test_session_frame_clamps_catchup() -> void:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	rt.play()
	# 1.0 s would be ~62 ticks; the spiral-of-death clamp caps a single frame's
	# catch-up at the native world::TickAccumulator::kMaxCatchupTicks (S14).
	assert_eq(_advance_ticks(rt, 1.0), 31, "a long stall is clamped to the catch-up cap")
	# The clamp DROPS the backlog (no banked spiral): a tiny delta afterward fires nothing.
	assert_eq(_advance_ticks(rt, 0.001), 0, "the backlog was dropped, not carried into the next frames")


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


func test_catchup_advances_round_move_effect_at_each_live_pose_and_stops_before_expiry() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# The scene-node present stays batched, but a round-bound effects_table move
	# group is part of the fixed-tick particle simulation. A four-second
	# flashbang exercises the same attached-effect path as the smoke grenade in
	# a short, production-authored lifetime.
	var w := _make_world(Transform3D.IDENTITY)
	# A REAL ItemEffectDirector and a REAL EffectWorld (ADR 0043 rule 11): the
	# runtime anchors the round through the anchor registry and spawns into
	# the effect world setup_passes binds; the world's public group report is
	# the read seam.
	var anchor_mount := ItemEffectDirector.new()
	var effect_world := EffectWorld.new()
	add_child_autofree(effect_world)
	effect_world.load_particle_file(_catalog_file())
	# The production owner-pose wiring (ItemEffectDirector.on_effect_world_started):
	# the anchor registry resolves the owned group's live pose each fixed tick.
	effect_world.set_owner_position_provider(anchor_mount.resolve_owner_transform)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	rt.setup_passes(null, effect_world, null, null, anchor_mount)
	var def_root := ResourceRoot.new()
	def_root.set_root_dir(RetailData.def_root())
	assert_eq(rt.get_sim().load_ammo_table(def_root, "ammo.def"), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	rt.get_sim().resolve_item_traits(item_db)
	assert_gte(rt.get_sim().debug_spawn_round(
			Vector3(100, 100, 100), Vector3.RIGHT, "grenadefb"), 0)
	# The per-tick observation (the GameFramePipeline leg): advance the real
	# effect world after each fixed tick and read the round-bound group from
	# the public report. A group the tick's throwable sync retired is already
	# detached BEFORE that advance (the release timing pinned below); the
	# emitter position read AFTER the advance is the owner sync's result, i.e.
	# the pose the group actually emitted from on that tick.
	var probe := {
		"advances": 0,
		"group_id": 0,
		"active_tick_numbers": [],
		"active_poses": [],
		"stops_after_advance": [],
	}
	rt.fixed_tick_completed.connect(func(_logic_tick: int) -> void:
		var group_id := int(probe["group_id"])
		if group_id > 0 and (probe["stops_after_advance"] as Array).is_empty():
			var before := _round_move_group(effect_world, group_id)
			if before == null or before.detached:
				(probe["stops_after_advance"] as Array).append(int(probe["advances"]))
		effect_world.advance_fixed_tick(Simulation.tick_dt())
		probe["advances"] = int(probe["advances"]) + 1
		var after := _round_move_group(effect_world, group_id)
		if after == null or after.detached:
			return
		probe["group_id"] = int(after.id)
		(probe["active_tick_numbers"] as Array).append(int(probe["advances"]))
		var emitters: Array = after.emitters
		(probe["active_poses"] as Array).append(
				(emitters[0] as EffectEmitterReport).position)
	)
	rt.play()

	assert_eq(_advance_ticks(rt, 0.05), 3,
			"one render frame catches up the first three round ticks")
	assert_eq(_round_move_rows(effect_world).size(), 1,
			"the attached move group exists before its first particle advance")
	assert_eq(probe["active_tick_numbers"], [1, 2, 3],
			"every birth-batch fixed tick advances the live group")
	var active_poses: Array = probe["active_poses"]
	assert_eq(active_poses.size(), 3)
	if active_poses.size() == 3:
		assert_false((active_poses[0] as Vector3).is_equal_approx(active_poses[1]),
				"the second emission sees the second simulated round pose")
		assert_false((active_poses[1] as Vector3).is_equal_approx(active_poses[2]),
				"catch-up does not emit repeatedly from the frame-start pose")

	# max_age 4 parses to 248 fixed ticks. Stop at age 240, then expire eight
	# ticks into a twelve-tick catch-up batch: advances 248..252 must observe no
	# live group, rather than emitting five stale ticks until final presentation.
	for _batch in range(7):
		assert_eq(_advance_ticks(rt, 31.0 * Simulation.tick_dt()), 31)
	assert_eq(_advance_ticks(rt, 20.0 * Simulation.tick_dt()), 20)
	assert_eq(int(probe["advances"]), 240)
	assert_eq(_advance_ticks(rt, 12.0 * Simulation.tick_dt()), 12)
	assert_eq(int(probe["advances"]), 252)
	var active_tick_numbers: Array = probe["active_tick_numbers"]
	assert_eq(active_tick_numbers.size(), 247,
			"the group advances exactly while the round is alive")
	assert_eq(int(active_tick_numbers[-1]), 247,
			"the expiry tick never advances a released round effect")
	assert_eq(probe["stops_after_advance"], [247],
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
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(md, container)
	rt.play()
	var drained: Array = []
	rt.effects_drained.connect(func(effects): drained.append_array(effects))
	# 20.5 quanta of wall-clock in ONE frame -> 20 ticks; crosses the 16th-tick quarter-pass boundary.
	assert_eq(_advance_ticks(rt, 0.328), 20, "20+ quanta of wall-clock run 20 logic ticks in one frame")
	assert_eq(drained.size(), 1, "the per-tick one-shot effect surfaced from inside the batch")
	assert_eq((drained[0] as MissionEffect).kind, "text")


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


# Drive a fresh MissionRoot with `count` frames of `step` seconds each and report total ticks +
# the entity position. Fixed iteration count (not a while-elapsed loop) keeps the fed wall-clock exact.
func _run_realtime(step: float, count: int) -> Dictionary:
	var w := _make_world(Transform3D.IDENTITY)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(w.mission, w.container, _options_with_placer(w.placer))
	rt.play()
	var ticks := 0
	for _i in range(count):
		ticks += _advance_ticks(rt, step)
	return { "ticks": ticks, "pos": rt.get_sim().get_entity_position(0) }
