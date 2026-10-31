extends GutTest

# EffectWorld — the runtime .ptl effect world (load + intern + spawn +
# expiry). Mirrors the witnessed chain: CEffectSystem_Init @ 0x5f6070 loads
# every mounted .ptl; CEffectWorld_InternEffectHandle @ 0x5f7310 interns effect
# names to stable 1-based handles (case-insensitive); SpawnEmitterAtPosition
# @ 0x5f6df0 spawns by handle or name.


var _root_dir := ""


func before_each() -> void:
	# OS cache dir, not user:// — resource roots inside the app user-data dir are
	# rejected by ResourceRoot.is_valid_root (mirrors the other fixture roots).
	_root_dir = OS.get_cache_dir().path_join("opennova_effect_world_test").path_join("root_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(_root_dir)
	for fixture in ["synth_minimal_effect.ptl", "synth_multi_section.ptl", "synth_smallest.ptl", "gorehit.ptu"]:
		var src := ProjectSettings.globalize_path("res://../fixtures/particle/%s" % fixture)
		var bytes := FileAccess.get_file_as_bytes(src)
		assert_gt(bytes.size(), 0, "fixture readable: %s" % fixture)
		var f := FileAccess.open(_root_dir.path_join(fixture), FileAccess.WRITE)
		f.store_buffer(bytes)
		f.close()


func after_each() -> void:
	if _root_dir.is_empty():
		return
	TestFs.remove_dir_recursive(_root_dir)


func _make_world() -> EffectWorld:
	var world: EffectWorld = add_child_autofree(EffectWorld.new())
	return world


func _single_group(world: EffectWorld, index: int = 0) -> EffectGroupReport:
	var groups := world.get_debug_group_report()
	assert_gt(groups.size(), index, "requested debug group exists")
	return groups[index] as EffectGroupReport


func _single_emitter(world: EffectWorld, group_index: int = 0) -> EffectEmitterReport:
	var group := _single_group(world, group_index)
	var emitters := group.emitters
	assert_eq(emitters.size(), 1, "synthetic effect has one emitter")
	return emitters[0] as EffectEmitterReport


func _make_root() -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_root_dir), OK, "resource root mounts the fixture dir")
	return root


# One synthetic short-lived effect document (deterministic expiry, no fixture
# dependence): one burst, sub-second lifetime.
func _make_short_effect_file(properties: String = "") -> ParticleFile:
	return ParticleFixture.catalog(self, "puff dots",
			"emit_dur = 0.1;\nemit_rate = 50;\nemit_burst = 4;\nage = 0.2;\nalpha = 1;\nscale = 1;\n" + properties, ["puff"])


func _make_renderable_effect_file(properties: String = "") -> ParticleFile:
	var file := _make_short_effect_file(
			"graphic1 = particle_dot.tga, blend;\ng1_alpha = 1;\ng1_scale = 1;\n" + properties)
	# A loose texture locator; the synthetic PTL itself need not exist on disk.
	file.source_path = ProjectSettings.globalize_path(
			"res://../fixtures/cbin/renderable_effect_fixture.ptl")
	return file


func _warm_helper_count(world: EffectWorld) -> int:
	var count := 0
	for child in world.find_children("*", "MeshInstance3D", true, false):
		if String(child.name) != "ParticleFirstPersonBatch":
			count += 1
	return count


func test_load_from_resource_root_scans_every_ptl() -> void:
	var world := _make_world()
	var count := world.load_from_resource_root(_make_root())
	assert_eq(world.file_count(), 4,
			"every mounted .ptl parses (minimal_effect + multi_section + smallest) plus the .ptu gore set")
	# synth_minimal_effect declares 1 effect, synth_multi_section 7, gorehit.ptu 1;
	# synth_smallest is table-only.
	assert_eq(count, 9, "all effects across the mounts register")
	assert_gt(world.effect_count(), 0)
	assert_true(world.get_image_provider().is_valid(), "textures route through the mounted root")


# The mission start's effect system builds the catalog's texture atlases at its init, each
# frame's file decoded into its page and freed, a device texture made per page and none per
# frame, so the load's first draw builds no atlas again (engine renderer/particle_atlas.h
# carries the witness, D-NET-1079): the build leaves the mount's decoded textures
# empty, and a draw after it keeps the atlas it built.
func test_the_atlas_builds_once_from_decoded_frames() -> void:
	# synth_minimal_effect's graphic1 is spark.tga.
	TestFs.write_bytes(self, _root_dir.path_join("spark.tga"), TestFs.tga_bytes(Vector2i(8, 8)))
	var root := _make_root()
	var world := _make_world()
	world.load_from_resource_root(root)
	var before := int(world.get_debug_draw_list_report().get("atlas_generation", 0))
	world.build_texture_atlases()
	var report := world.get_debug_draw_list_report()
	assert_eq(int(report.get("atlas_generation", 0)), before + 1, "the init builds the atlas once")
	assert_gt(int(report.get("atlas_resolved_entry_count", 0)), 0, "spark.tga is on a page")
	assert_eq(root.get_decoded_texture_count(), 0, "no device texture is made for a frame")
	world.render_now(0)
	assert_eq(int(world.get_debug_draw_list_report().get("atlas_generation", 0)), before + 1,
			"the first draw builds no atlas again")


# The game's effect loaders present the loading screen at its stored checkpoint as they go,
# so a long load keeps the screen live through the screen's 100 ms rule: the effect system's
# load past each loose file it parses and once past the archive walk, the atlas build at its
# start, past each frame it decodes and past its walk (engine runtime/hud/loading_screen.h
# carries the witness).
# [orig: CEffectSystem_Init @0x5f6070 - LoadingScreen_PresentStoredProgress called @0x5f630b
#  (each loose .ptl), @0x5f6451 (each loose gore file), @0x5f6582 (past the archive walk);
#  CParticleManager_BuildTextureAtlases
#  @0x5e8db0 - the calls @0x5e8dfb, @0x5e8e0c, @0x5e8ef5]
func test_the_effect_loaders_present_the_stored_checkpoint_as_they_go() -> void:
	# synth_minimal_effect's graphic1 is spark.tga.
	TestFs.write_bytes(self, _root_dir.path_join("spark.tga"), TestFs.tga_bytes(Vector2i(8, 8)))
	var world := _make_world()
	var load_presents := [0]
	world.load_from_resource_root(_make_root(), func() -> void: load_presents[0] += 1)
	assert_eq(world.file_count(), 4, "the fixture root's four loose effect files parse")
	assert_eq(load_presents[0], 5, "one present past each loose file and one past the walk")
	var atlas_presents := [0]
	world.build_texture_atlases(func() -> void: atlas_presents[0] += 1)
	var entries := int(world.get_debug_draw_list_report().get("atlas_entry_count", 0))
	assert_gt(entries, 0, "spark.tga decodes into the build")
	assert_eq(atlas_presents[0], entries + 2,
			"one present at the start, one past each decoded frame and one past the walk")
	# Without a present, the loaders present nothing and load the same.
	var quiet := _make_world()
	quiet.load_from_resource_root(_make_root())
	assert_eq(quiet.file_count(), 4)


# Retail's effect loader walks every mounted archive's directory and skips an
# entry whose +12 stamp is 0, so an archived `.ptl` packed unstamped (a
# third-party packer's) never parses; every retail entry, and every entry our
# writers add, is stamped (D-VFS-13).
# [orig: CEffectSystem_Init @ 0x5f6070 — the slot walk @ 0x5f6485, the skip
#  @ 0x5f64c0]
func test_archive_walk_skips_a_zero_stamped_entry() -> void:
	var dir := _root_dir.path_join("packed")
	DirAccess.make_dir_recursive_absolute(dir)
	var fixtures := ProjectSettings.globalize_path("res://../fixtures/particle")
	assert_eq(TestPff.write(dir.path_join("localres.pff"), [
		{name = "minimal.ptl", bytes = FileAccess.get_file_as_bytes(
				fixtures.path_join("synth_minimal_effect.ptl"))},
		{name = "unstamped.ptl", timestamp = 0, bytes = FileAccess.get_file_as_bytes(
				fixtures.path_join("synth_multi_section.ptl"))},
	]), OK)
	var root := ResourceRoot.new()
	assert_eq(root.mount_runtime(dir), OK, "the packed fixture mounts")
	assert_eq(root.effect_files(), PackedStringArray(["minimal.ptl"]),
			"the walk admits the stamped entry alone")
	var world := _make_world()
	assert_eq(world.load_from_resource_root(root), 1,
			"only the stamped entry's one effect registers")
	assert_eq(world.file_count(), 1, "the zero-stamped entry is never parsed")
	assert_true(root.has_file("unstamped.ptl"), "the file itself still reads by name")
	# Release the archive before after_each deletes the fixture tree.
	world.clear_world()
	root.clear()


# Retail's catalog spans `.ptl` PLUS one gore set — `.ptu` (US) or `.ptg` (German),
# selected by the presence of `fgn2.bin`. Loading only `.ptl` silently loses every
# effect defined there: retail's blood puffs (Effect_AmHitBody / Effect_SGvBody, in
# US_BLOOD.PTU) and the whole Effect_FX50Cal* impact family. The loss is SILENT —
# an unknown name interns as an invisible `stockeffect` clone (D-PTL-8), so the
# only observable symptom is that flesh hits render nothing (D-PTL-24).
# [orig: CEffectSystem_Init @ 0x5f6070 — extension select @0x5f608b..0x5f6095,
#  archive match @0x5f64f3, shared callback CEffectWorld_ParseSectionCallback
#  @ 0x5ecb40; selector Game_LoadConfig @ 0x5514e8..0x5514fa]
func test_gore_set_effects_load_and_resolve_across_extensions() -> void:
	var root := _make_root()
	assert_eq(root.particle_extension(), ".ptu",
			"no fgn2.bin in the fixture mount selects the US gore set")
	var world := _make_world()
	world.load_from_resource_root(root)

	# The .ptu effectdef must be a REAL registration, not the stockeffect clone an
	# unknown name would hand back. A clone carries no definitions and cannot spawn.
	var handle := world.intern_effect("Effect_GoreHit")
	assert_gt(handle, 0, "the .ptu effectdef interns")
	assert_eq(world.effect_name_for_handle(handle), "Effect_GoreHit")

	# Effect_GoreHit's pdefs list mixes its own particledef with `Buildup_dots` from
	# synth_minimal_effect.ptl. The catalog registers every particledef across ALL documents before
	# resolving ANY effectdef, so the reference resolves across extensions. An effect
	# with any unresolved pdef is cleared WHOLE (D-PTL-8 all-or-nothing) and spawns
	# nothing, so a successful spawn is the proof that both halves resolved.
	assert_true(world.spawn_effect_by_handle(handle, Vector3.ZERO, Vector3.FORWARD),
			"the gore-set effect spawns; an unresolved or cloned one would not")
	assert_gt(world.live_group_count(), 0,
			"the cross-extension pdef reference resolved into a live group")


func test_intern_is_case_insensitive_and_stable() -> void:
	var world := _make_world()
	world.load_from_resource_root(_make_root())
	var handle := world.intern_effect("Buildup")
	assert_gt(handle, 0, "known effect interns to a 1-based handle")
	assert_eq(world.intern_effect("BUILDUP"), handle, "stricmp match returns the same handle")
	assert_eq(world.intern_effect("buildup"), handle, "lower-case too")
	assert_eq(world.effect_name_for_handle(handle), "Buildup")
	var other := world.intern_effect("stockeffect")
	if other > 0:
		assert_ne(other, handle, "distinct names take distinct handles")
	# Unknown names clone the stockeffect def under the requested name
	# [orig: CEffectWorld_InternEffectHandle @ 0x5f7310 — the vtable+28 clone;
	# D-PTL-8 CLOSED]. synth_multi_section.ptl (stockeffect) is mounted here, so the clone must intern.
	var cloned := world.intern_effect("no-such-effect-xyz")
	assert_gt(cloned, 0, "unknown name clones stockeffect under the requested name")
	assert_ne(cloned, handle, "the clone takes its own handle")
	assert_eq(world.effect_name_for_handle(cloned), "no-such-effect-xyz",
			"the clone registers under the REQUESTED name")
	assert_eq(world.intern_effect("NO-SUCH-EFFECT-XYZ"), cloned,
			"the cloned registration is stable and case-insensitive")


func test_intern_unknown_without_stockeffect_returns_zero() -> void:
	var world := _make_world()
	world.load_particle_file(_make_short_effect_file())  # no stockeffect mounted
	assert_eq(world.intern_effect("no-such-effect-xyz"), 0,
			"unknown name with no mounted stockeffect stays 0 (no spawn)")


func test_spawn_by_name_creates_emitters_and_sweep_expires() -> void:
	var world := _make_world()
	world.load_particle_file(_make_short_effect_file())
	var handle := world.spawn_effect("puff", Vector3(1, 2, 3))
	assert_gt(handle, 0, "spawn returns the interned handle")
	assert_eq(world.live_group_count(), 1, "one live spawn group")
	assert_almost_eq(_single_emitter(world).position, Vector3(1, 2, 3),
			Vector3(0.01, 0.01, 0.01))
	assert_eq(world.get_children().filter(
			func(child: Node) -> bool: return child.get_class() == "ParticleEmitter").size(), 0,
			"runtime effects remain values; no per-emitter renderer Nodes are created")
	world.advance_fixed_tick(Simulation.tick_dt())
	assert_gt(int(_single_emitter(world).alive), 0,
			"a production-tick spawn emits during that same fixed particle pass")
	# Let the one-shot emit and die through the one authoritative scene clock.
	for _i in 30:
		world.advance_fixed_tick(0.1)
	assert_eq(world.live_group_count(), 0, "finished finite group is swept")


# NOVISNOUPDATE freezes an emitter whose bounds lie outside the current
# camera's view (retail CParticleEmitter_AdvanceFrame @ 0x5e6588..0x5e65f3);
# the gate reads the viewport camera's Camera3D.get_frustum planes, whose
# normals point outward. An emitter in front of the camera must advance and
# one behind it must stay frozen (the 00TRa fire barrel sat frozen at age 0
# while the flip was inverted).
func test_novisnoupdate_gate_advances_on_screen_and_freezes_off_screen() -> void:
	var camera: Camera3D = add_child_autofree(Camera3D.new())
	camera.current = true
	camera.global_transform = Transform3D(Basis(), Vector3.ZERO)
	var world := _make_world()
	world.load_particle_file(ParticleFixture.catalog(self, "fire dots",
			"emit_dur = 1;\nemit_rate = 50;\nage = 2;\nalpha = 1;\nscale = 1;\nflags = FOREVEREMIT NOVISNOUPDATE;\n",
			["fire"]))
	world.spawn_effect("fire", Vector3(0, 0, -10))
	world.spawn_effect("fire", Vector3(0, 0, 10))
	for _i in 4:
		world.advance_fixed_tick(Simulation.tick_dt())
	var ahead := -1
	var behind := -1
	for group in world.get_debug_group_report():
		var emitter := (group as EffectGroupReport).emitters[0] as EffectEmitterReport
		if emitter.position.z < 0.0:
			ahead = emitter.alive
		else:
			behind = emitter.alive
	assert_gt(ahead, 0, "an emitter in front of the camera advances and emits")
	assert_eq(behind, 0, "an emitter behind the camera stays frozen")


func test_live_group_parameters_drive_rate_and_offset_without_clamping() -> void:
	var file := ParticleFixture.catalog(self, "wake dots",
			"emit_dur = 0.1;\nemit_rate = 30;\nemit_rate_adj = 30;\nemit_burst = 1;\ny_offset = 2;\nz_offset = 4;\nage = 1;\nflags = FOREVEREMIT;\n", ["wake"])
	var world := _make_world()
	world.load_particle_file(file)
	var options := EffectSpawnOptions.new()
	options.admission = EffectScene.ADMISSION_REPLACE_OWNED
	options.slot_key = "wake-slot"
	var receipt: EffectSpawnReceipt = world.spawn_effect_request(
			"wake", EffectWorld.descriptor_pose(Vector3.ZERO, Vector3.FORWARD), options)
	assert_true(receipt.spawned)

	assert_true(world.set_group_parameters(receipt.group_id, 0.75, 0.25))
	var emitter := _single_emitter(world)
	assert_almost_eq(emitter.emit_rate, 45.0, 0.0001,
			"rate = base + adjustment * (2m - 1)")
	assert_almost_eq(emitter.spawn_y_offset, 0.0, 0.0001,
			"spawn Y uses the second live control")
	assert_almost_eq(emitter.camera_pull, 0.0, 0.0001,
			"controlled trails stop reusing z_offset as camera pull")

	assert_true(world.set_group_parameters(receipt.group_id, 0.0, 1.5),
			"controls outside 0..1 are accepted")
	emitter = _single_emitter(world)
	assert_almost_eq(emitter.emit_rate, 0.0, 0.0001,
			"a negative effective rate floors at zero")
	assert_almost_eq(emitter.spawn_y_offset, 10.0, 0.0001)
	assert_false(world.set_group_parameters(receipt.group_id, NAN, 0.0))
	assert_false(world.set_group_parameters(999999, 0.5, 0.5))


func test_runtime_reset_preserves_catalog_and_discards_live_admission() -> void:
	var world := _make_world()
	world.load_particle_file(_make_short_effect_file())
	var handle := world.intern_effect("puff")
	assert_gt(world.spawn_effect_unless_alive("slot", "puff", Vector3.ZERO), 0)
	assert_eq(world.file_count(), 1)
	assert_eq(world.live_group_count(), 1)

	world.reset_runtime_state()

	assert_eq(world.file_count(), 1, "restart keeps the mounted particle catalog")
	assert_eq(world.live_group_count(), 0, "restart discards pre-rewind live groups")
	assert_eq(world.intern_effect("PUFF"), handle,
			"restart preserves the compiled catalog's interned handles")
	assert_gt(world.spawn_effect_unless_alive("slot", "puff", Vector3.ZERO), 0,
			"restart clears old admission slots for the next play session")


func test_pending_delayed_emitter_outlives_the_heuristic_window() -> void:
	var world := _make_world()
	var file := _make_short_effect_file("emit_delay = 31;")
	world.load_particle_file(file)
	assert_gt(world.spawn_effect("puff", Vector3.ZERO), 0)

	world.advance_fixed_tick(31.0)
	assert_eq(world.live_group_count(), 1,
			"a finite group remains live while its emitter still has pending delay/emission")


func test_unless_alive_owner_mapping_clears_when_group_finishes() -> void:
	var world := _make_world()
	world.load_particle_file(_make_short_effect_file())
	var owner := "action-slot-generation"
	assert_gt(world.spawn_effect_unless_alive(owner, "puff", Vector3.ZERO), 0)
	assert_gt(world.spawn_effect_unless_alive(owner, "puff", Vector3.ONE), 0)
	assert_eq(world.live_group_count(), 1,
			"the slot token suppresses a second live group without a private-map assertion")
	for _i in 30:
		world.advance_fixed_tick(0.1)
	assert_eq(world.live_group_count(), 0)
	assert_gt(world.spawn_effect_unless_alive(owner, "puff", Vector3.ZERO), 0)
	assert_eq(world.live_group_count(), 1,
			"the action slot admits a new group after its predecessor is reclaimed")


func test_spawn_by_handle_matches_name_spawn() -> void:
	var world := _make_world()
	world.load_particle_file(_make_short_effect_file())
	var handle := world.intern_effect("PUFF")
	assert_gt(handle, 0)
	assert_true(world.spawn_effect_by_handle(handle, Vector3.ZERO), "handle spawn succeeds")
	assert_eq(world.live_group_count(), 1)
	assert_false(world.spawn_effect_by_handle(99, Vector3.ZERO), "out-of-range handle refuses")


func test_particle_master_switch_suppresses_facades_and_checked_count() -> void:
	# The retail global disable makes every effect facade a no-op and its debug
	# active-entry facade return zero [orig: g_ParticlesDisabled /
	# EffectWorld_GetActiveEntryCountChecked @ 0x5f69c0].
	var world := _make_world()
	world.load_particle_file(_make_short_effect_file())
	assert_gt(world.spawn_effect("puff", Vector3.ZERO), 0)
	assert_eq(world.active_entry_count(), 1)

	world.set_particles_hidden(true)
	assert_true(world.are_particles_hidden())
	assert_eq(world.active_entry_count(), 0, "the checked facade is zero while disabled")
	assert_true(world.get_debug_group_report().is_empty(), "the debug page lists no active entries")
	assert_eq(world.spawn_effect("puff", Vector3.ZERO), 0, "named facade no-ops")
	assert_eq(world.spawn_effect_unless_alive("slot", "puff", Vector3.ZERO), 0,
			"guarded facade no-ops")
	assert_false(world.spawn_effect_by_handle(1, Vector3.ZERO), "handle facade no-ops")
	assert_eq(world.live_group_count(), 1, "disabling does not duplicate or destroy live state")

	world.set_particles_hidden(false)
	assert_false(world.are_particles_hidden())
	assert_eq(world.active_entry_count(), 1, "re-enabling exposes the still-live entry")


# A holder's per-tick re-pose: every emitter of the held group moves to the pose's
# origin, re-aimed along its forward; a detached group refuses the move
# [orig: CEffectEmitter_UpdatePositionAndParams @ 0x5f6810].
func test_move_group_re_poses_a_held_group() -> void:
	var world := _make_world()
	world.load_particle_file(_make_short_effect_file("flags = FOREVEREMIT;"))
	var options := EffectSpawnOptions.new()
	options.admission = EffectScene.ADMISSION_REPLACE_OWNED
	options.slot_key = "held"
	var receipt := world.spawn_effect_request(
			"puff", EffectWorld.descriptor_pose(Vector3(1, 2, 3), Vector3.UP), options)
	assert_true(receipt.spawned)
	var pose := Transform3D(Basis(Vector3.UP, PI * 0.5), Vector3(6, 5, 4))
	assert_true(world.move_group(receipt.group_id, pose))
	var moved := _single_emitter(world)
	assert_almost_eq(moved.position, pose.origin, Vector3(0.001, 0.001, 0.001))
	assert_almost_eq(moved.forward, pose.basis.z.normalized(), Vector3(0.001, 0.001, 0.001))
	world.stop_group(receipt.group_id)
	assert_false(world.move_group(receipt.group_id, pose), "a detached group is not moved")
	assert_false(world.move_group(0, pose))


# A slot key's release detaches what its slot holds and forgets the key's token, so
# generation-scoped keys never accumulate.
func test_release_slot_key_detaches_and_forgets_the_key() -> void:
	var world := _make_world()
	world.load_particle_file(_make_short_effect_file("flags = FOREVEREMIT;"))
	assert_gt(world.spawn_effect_unless_alive("lane:7", "puff", Vector3.ZERO), 0)
	assert_true(world.has_slot_key("lane:7"))
	world.release_slot_key("lane:7")
	assert_false(world.has_slot_key("lane:7"))
	assert_true(bool(_single_group(world).detached), "the held group drains detached")


func test_authored_water_flags_bind_to_the_mission_water_plane() -> void:
	var cases := [
		{"flag": "BELOWH20", "mode": 1},  # BELOWH20: kill above.
		{"flag": "ABOVEH20", "mode": 2},  # ABOVEH20: kill at/below.
	]
	for entry in cases:
		var world := _make_world()
		world.set_water_plane(12.5, null)
		var file := _make_short_effect_file("flags = %s;" % entry.flag)
		world.load_particle_file(file)
		world.spawn_effect("puff", Vector3.ZERO)
		var emitter := _single_emitter(world)
		assert_eq(int(emitter.kill_plane), entry.mode)
		assert_almost_eq(float(emitter.kill_plane_y), 12.5, 0.001,
				"authored water culling uses the active mission plane")


func test_clear_world_frees_live_groups() -> void:
	var world := _make_world()
	world.load_particle_file(_make_short_effect_file())
	world.spawn_effect("puff", Vector3.ZERO)
	assert_eq(world.live_group_count(), 1)
	world.clear_world()
	assert_eq(world.live_group_count(), 0)
	assert_eq(world.effect_count(), 0)


# --- Debug seams (the retail particle debug pages, mimicked; ptl-format-re.md §11) ---


func test_debug_group_report_shapes() -> void:
	var world := _make_world()
	world.load_particle_file(_make_short_effect_file())
	world.spawn_effect("puff", Vector3.ZERO)
	assert_gt(world.interned_count(), 0, "the spawn interned its handle")
	var report := world.get_debug_group_report()
	assert_eq(report.size(), world.live_group_count(), "one report row per live group")
	var group: EffectGroupReport = report[0]
	assert_eq(group.name, "puff", "the group names its interned effect")
	assert_eq(group.emitters.size(), 1, "one emitter row")
	var emitter: EffectEmitterReport = group.emitters[0]
	assert_true(emitter.alive >= 0 and emitter.rendered >= 0
			and emitter.bounds is AABB and emitter.emitter_id > 0,
			"emitter rows carry stable value diagnostics for the box view")
	assert_null(emitter.get("node"),
			"debug reports do not leak renderer Nodes")
	var unresolved_names := world.get_unresolved_texture_names()
	assert_null(group.get("unresolved"),
			"catalog-wide missing frames are not falsely attributed to this group")
	assert_true(unresolved_names is PackedStringArray,
			"unresolved texture names use the catalog-level value query")


func test_set_particles_hidden_toggles_render_visibility() -> void:
	var world := _make_world()
	world.load_particle_file(_make_short_effect_file())
	world.spawn_effect("puff", Vector3.ZERO)
	assert_false(world.are_particles_hidden(), "visible by default")
	world.set_particles_hidden(true)
	assert_true(world.are_particles_hidden())
	assert_false(world.visible, "the retail master switch hides the render output")
	world.set_particles_hidden(false)
	assert_true(world.visible)


func test_warm_all_effects_spawns_the_catalog_once_and_resets_clean() -> void:
	# The load-time warm pass behind the first-shot hitch fix: every cataloged
	# effect spawns exactly once (dedup by id), the fixed tick makes fresh
	# emitters live, and the sim-restart reset clears the warm spawns.
	var world := _make_world()
	var count := world.load_from_resource_root(_make_root())
	# 8 from the .ptl set + Effect_GoreHit from the .ptu gore set.
	assert_eq(count, 9, "fixture catalog registers 9 effects")
	var spawned := world.warm_all_effects(Vector3(1, 2, 3))
	assert_eq(spawned, 9, "the warm pass spawns each cataloged effect once")
	var warm_groups := world.get_debug_group_report()
	assert_eq(warm_groups.size(), 9,
			"catalog warming must not duplicate every effect into FirstPerson")
	for group_v in warm_groups:
		var group := group_v as EffectGroupReport
		assert_eq(group.render_domain,
				EffectWorld.RENDER_DOMAIN_WORLD,
				"catalog values warm through the uncapped World draw list")
	assert_gt(world.active_entry_count(), 0, "warm spawns occupy live entries")
	world.advance_fixed_tick(0.016)
	world.reset_runtime_state()
	assert_eq(world.active_entry_count(), 0, "the reset clears every warm spawn")
	assert_eq(world.warm_all_effects(Vector3.ZERO), 9,
			"a later warm (reload) spawns the catalog again")


func test_render_now_submits_a_freshly_advanced_warm_snapshot() -> void:
	var world := _make_world()
	world.load_particle_file(_make_renderable_effect_file())
	assert_gt(world.spawn_effect("puff", Vector3.ZERO), 0)
	world.advance_fixed_tick(Simulation.tick_dt())
	assert_gt(world.render_now(GameWorld.current_frame_clock_ms()), 0,
			"the public warm facade synchronously submits non-empty material runs")


# A load's warm ends in the reset: its last submission leaves the camera's compositor with the
# warm's spawns, so a join's waits and loading presents (whose frames run no render_now) draw
# no particle; retail's presents draw the loading screen alone (engine
# runtime/hud/loading_screen.h carries the witness, D-NET-1077).
func test_a_reset_retires_the_published_particle_submission() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var world := EffectWorld.new()
	viewport.add_child(world)
	world.load_particle_file(_make_renderable_effect_file())
	assert_gt(world.spawn_effect("puff", Vector3.ZERO), 0)
	world.advance_fixed_tick(Simulation.tick_dt())
	assert_gt(world.render_now(GameWorld.current_frame_clock_ms()), 0)
	var drawn := world.get_debug_draw_list_report()
	assert_true(bool(drawn.get("world_compositor_attached", false)))
	var published := int((drawn.get("world_camera_backend", {}) as Dictionary).get(
			"submitted_commands", 0)) + int(drawn.get("world_far_render_runs", 0))
	assert_gt(published, 0, "the render publishes the warm spawn to the camera's view")

	world.reset_runtime_state()
	var reset := world.get_debug_draw_list_report()
	assert_true((reset.get("world_camera_side", {}) as Dictionary).is_empty())
	assert_true((reset.get("world_far_side", {}) as Dictionary).is_empty())
	assert_eq(int(reset.get("world_far_render_runs", -1)), 0)
	for key in ["world_far_backend", "world_camera_backend"]:
		assert_eq(int((reset.get(key, {}) as Dictionary).get("submitted_commands", -1)), 0,
				"%s draws nothing after the reset" % key)
	# The next render publishes the reset scene: still nothing.
	world.render_now(GameWorld.current_frame_clock_ms())
	var next := world.get_debug_draw_list_report()
	assert_eq(int((next.get("world_camera_backend", {}) as Dictionary).get(
			"submitted_commands", -1)), 0)


func test_runtime_renderer_release_is_explicit_and_idempotent() -> void:
	var world := _make_world()
	await get_tree().process_frame
	world.release_runtime_renderer_resources()
	world.release_runtime_renderer_resources()
	var report := world.get_debug_draw_list_report()
	for key in [
		"world_far_backend", "world_camera_backend",
		"reflection_far_backend", "reflection_camera_backend",
		"second_scene_far_backend", "second_scene_camera_backend",
	]:
		assert_true(bool((report.get(key, {}) as Dictionary).get("shutdown", false)),
				"%s is retired before EffectWorld leaves the tree" % key)
	assert_engine_error_count(0,
			"explicit particle renderer release remains clean and idempotent")


func test_empty_catalog_warm_cleans_pipeline_helpers() -> void:
	var world := _make_world()
	assert_eq(_warm_helper_count(world), 0)
	assert_eq(world.warm_all_effects(Vector3.ZERO), 0)
	assert_gt(_warm_helper_count(world), 0,
			"an empty catalog still exercises the deterministic shader helpers")
	await get_tree().process_frame
	assert_eq(_warm_helper_count(world), 0,
			"the empty-catalog early return does not strand helper geometry")


# A descriptor spawn with no orientation is retail's zero-orientation case:
# SpawnEmitterAtPosition @ 0x5f6e52..0x5f6e5c hands the group a zero vector and
# EMITVECTOR members then emit around world +Y (SetOrientationFromDirection
# @ 0x5e5d51 leaves their axis zero). The pose seam aims that case at +Y; a
# real orientation is kept as the forward axis.
func test_descriptor_pose_without_orientation_aims_up() -> void:
	var origin := Vector3(3.0, 4.0, 5.0)
	var none := EffectWorld.descriptor_pose(origin, Vector3.ZERO)
	assert_eq(none.origin, origin, "position kept")
	assert_almost_eq(none.basis.z, Vector3.UP, Vector3.ONE * 0.0001, "zero orientation -> +Y forward")
	assert_almost_eq(none.basis.z, EffectWorld.forward_pose(origin, Vector3.UP).basis.z, Vector3.ONE * 0.0001, "same frame as an explicit +Y forward")
	var aimed := EffectWorld.descriptor_pose(origin, Vector3(0.0, -0.6, 0.8))
	assert_almost_eq(aimed.basis.z, Vector3(0.0, -0.6, 0.8), Vector3.ONE * 0.0001, "a real orientation is the forward axis")
	assert_almost_eq(aimed.basis.z.length(), 1.0, 0.0001, "normalized")


func test_preaged_effect_receives_mission_wind_before_its_first_tick() -> void:
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.current = true
	add_child_autofree(camera)
	var file := _make_renderable_effect_file("flags = GLOBALWIND;\nage = 2;")
	var normal := _make_world()
	var preaged := _make_world()
	for world in [normal, preaged]:
		world.load_particle_file(file)
		world.set_mission_wind(10, 90)
	assert_gt(normal.spawn_effect_transient("puff", Vector3.ZERO), 0)
	for tick in 8:
		normal.advance_fixed_tick(0.016)
	assert_gt(preaged.spawn_effect_transient("puff", Vector3.ZERO, Vector3.ZERO, 8), 0)
	normal.render_now(GameWorld.current_frame_clock_ms())
	preaged.render_now(GameWorld.current_frame_clock_ms())
	var expected := _single_emitter(normal)
	var actual := _single_emitter(preaged)
	assert_eq(actual.alive, expected.alive, "the catch-up population matches ordinary ticks")
	assert_true(expected.bounds_valid and actual.bounds_valid)
	assert_gt(expected.bounds.get_center().x, 0.05, "the mission wind moves the particles")
	assert_almost_eq(actual.bounds.position, expected.bounds.position, Vector3.ONE * 0.0001,
			"spawn catch-up uses mission wind even before the world's first advance")
	assert_almost_eq(actual.bounds.size, expected.bounds.size, Vector3.ONE * 0.0001)
