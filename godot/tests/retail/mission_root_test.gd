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
		PackedStringArray(["WPN_KNIFE", "3", "0", "2"])]))
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
# native (mission::extract_item_seat_specs) and pinned by
# tests/mission/seat_spec_extract_test.cpp; the attach-command seat
# selection (world/vehicle_attach.h) by tests/world/seat_prediction_test.cpp.



const FLASHBANG_MOVE_EFFECT := "Effect_FlashBangToss"


# One synthetic in-memory particle catalog authoring that move effect as a
# FOREVEREMIT definition (the effect_world_test recipe), so a REAL EffectWorld
# interns and spawns it without a resource root and the round-bound group
# stays live until the throwable pass stops it.
func _catalog_file() -> ParticleFile:
	return ParticleFixture.catalog(self, "toss dots",
			"emit_dur = 0.1;\nemit_rate = 50;\nemit_burst = 4;\nage = 0.2;\nalpha = 1;\nscale = 1;\nflags = FOREVEREMIT;\n", [FLASHBANG_MOVE_EFFECT])


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
	# The per-tick observation (the GameWorld leg): advance the real
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

	# max_age 4 parses to 248 fixed ticks. Step to age 240, then expire eight
	# ticks into a twelve-tick catch-up batch: advances 248..252 must observe no
	# live group, rather than emitting five stale ticks until final presentation.
	for _step in range(237):
		assert_true(rt.step_once())
	assert_eq(int(probe["advances"]), 240)
	# play() resets the bank: one 192 ms frame drains 48 quanta = 12 ticks.
	rt.play()
	assert_eq(_advance_ticks(rt, 12.0 * Simulation.tick_dt()), 12)
	assert_eq(int(probe["advances"]), 252)
	var active_tick_numbers: Array = probe["active_tick_numbers"]
	assert_eq(active_tick_numbers.size(), 247,
			"the group advances exactly while the round is alive")
	assert_eq(int(active_tick_numbers[-1]), 247,
			"the expiry tick never advances a released round effect")
	assert_eq(probe["stops_after_advance"], [247],
			"release happens before fixed advance 248, inside the catch-up batch")
