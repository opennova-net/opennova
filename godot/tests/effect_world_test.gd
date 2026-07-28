extends GutTest

# NovaEffectWorld — the runtime .ptl effect world (load + intern + spawn +
# expiry). Mirrors the witnessed chain: CEffectSystem_Init @ 0x5f6070 loads
# every mounted .ptl; CEffect_FindOrCreateMaterial @ 0x5f7310 interns effect
# names to stable 1-based handles (case-insensitive); SpawnEmitterAtPosition
# @ 0x5f6df0 spawns by handle or name.

const EffectWorldScript = preload("res://engine/world/effect_world.gd")
const MissionRuntime := preload("res://engine/world/mission_runtime.gd")

var _root_dir := ""


class OwnerPositions:
	extends RefCounted
	var state: Variant = Vector3.ZERO
	var alive := true

	func resolve(_owner_key: Variant) -> Variant:
		return state if alive else null


func before_each() -> void:
	# OS cache dir, not user:// — resource roots inside the app user-data dir are
	# rejected by NovaResourceRoot.is_valid_root (mirrors the other fixture roots).
	_root_dir = OS.get_cache_dir().path_join("opennova_effect_world_test").path_join("root_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(_root_dir)
	for fixture in ["buildup.ptl", "stock.ptl", "troytabl.ptl"]:
		var src := ProjectSettings.globalize_path("res://../fixtures/particle/%s" % fixture)
		var bytes := FileAccess.get_file_as_bytes(src)
		assert_gt(bytes.size(), 0, "fixture readable: %s" % fixture)
		var f := FileAccess.open(_root_dir.path_join(fixture), FileAccess.WRITE)
		f.store_buffer(bytes)
		f.close()


func after_each() -> void:
	if _root_dir.is_empty():
		return
	for file in DirAccess.get_files_at(_root_dir):
		DirAccess.remove_absolute(_root_dir.path_join(file))
	DirAccess.remove_absolute(_root_dir)


func _make_world() -> NovaEffectWorld:
	var world: NovaEffectWorld = add_child_autofree(EffectWorldScript.new())
	return world


func _single_group(world: NovaEffectWorld, index: int = 0) -> Dictionary:
	var groups := world.get_debug_group_report()
	assert_gt(groups.size(), index, "requested debug group exists")
	return groups[index] as Dictionary


func _single_emitter(world: NovaEffectWorld, group_index: int = 0) -> Dictionary:
	var group := _single_group(world, group_index)
	var emitters := group.get("emitters", []) as Array
	assert_eq(emitters.size(), 1, "synthetic effect has one emitter")
	return emitters[0] as Dictionary


func _make_root() -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(_root_dir), OK, "resource root mounts the fixture dir")
	return root


# One synthetic short-lived effect document (deterministic expiry, no fixture
# dependence): one burst, sub-second lifetime.
func _make_short_effect_file() -> NovaParticleFile:
	var def := NovaParticleDef.new()
	def.id = "puff dots"
	def.emit_dur = 0.1
	def.emit_rate = 50.0
	def.emit_burst = 4
	def.age = 0.2
	def.alpha = 1.0
	def.scale_value = 1.0
	var effect := NovaParticleEffect.new()
	effect.id = "puff"
	effect.pdefs = PackedStringArray(["puff dots"])
	var file := NovaParticleFile.new()
	var particles: Array = file.particles
	particles.append(def)
	file.particles = particles
	var effects: Array = file.effects
	effects.append(effect)
	file.effects = effects
	return file


func _make_renderable_effect_file() -> NovaParticleFile:
	var file := _make_short_effect_file()
	var particle := file.find_particle("puff dots")
	var graphics: Array = particle.graphics
	var layer := graphics[0] as NovaParticleGraphicLayer
	layer.present = true
	layer.texture = "bink.tga"
	layer.alpha = 1.0
	layer.scale_value = 1.0
	particle.graphics = graphics
	# Point the loose-file resolver at a committed image fixture without
	# implying that this synthetic particle file itself exists on disk.
	file.source_path = ProjectSettings.globalize_path(
			"res://../fixtures/cbin/renderable_effect_fixture.ptl")
	return file


func _warm_helper_count(world: NovaEffectWorld) -> int:
	var count := 0
	for child in world.find_children("*", "MeshInstance3D", true, false):
		if String(child.name) != "ParticleFirstPersonPacket":
			count += 1
	return count


func test_load_from_resource_root_scans_every_ptl() -> void:
	var world := _make_world()
	var count := world.load_from_resource_root(_make_root())
	assert_eq(world.file_count(), 3, "every mounted .ptl parses (buildup + stock + troytabl)")
	# buildup declares 1 effect, stock declares 7; troytabl is table-only.
	assert_eq(count, 8, "all effects across the mounts register")
	assert_gt(world.effect_count(), 0)
	assert_true(world.get_texture_provider().is_valid(), "textures route through the mounted root")


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
	# D-PTL-8 CLOSED]. stock.ptl is mounted here, so the clone must intern.
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
			func(child: Node) -> bool: return child is NovaParticleEmitter).size(), 0,
			"runtime effects remain values; no per-emitter renderer Nodes are created")
	world.advance_fixed_tick(MissionRuntime.TICK_DT)
	assert_gt(int(_single_emitter(world).alive), 0,
			"a production-tick spawn emits during that same fixed particle pass")
	# Let the one-shot emit and die through the one authoritative scene clock.
	for _i in 30:
		world.sweep(0.1)
	assert_eq(world.live_group_count(), 0, "finished finite group is swept")


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
	var file := _make_short_effect_file()
	file.find_particle("puff dots").emit_delay = 31.0
	world.load_particle_file(file)
	assert_gt(world.spawn_effect("puff", Vector3.ZERO), 0)

	world.sweep(31.0)
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
		world.sweep(0.1)
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
	assert_eq(world.spawn_effect_owned("owner", "puff", Vector3.ZERO), 0, "owned facade no-ops")
	assert_eq(world.spawn_effect_attached("attached", "puff", Transform3D.IDENTITY,
			Vector3.ZERO, Vector3.UP), 0, "attached facade no-ops")
	assert_eq(world.spawn_effect_unless_alive("slot", "puff", Vector3.ZERO), 0,
			"guarded facade no-ops")
	assert_false(world.spawn_effect_by_handle(1, Vector3.ZERO), "handle facade no-ops")
	assert_eq(world.live_group_count(), 1, "disabling does not duplicate or destroy live state")

	world.set_particles_hidden(false)
	assert_false(world.are_particles_hidden())
	assert_eq(world.active_entry_count(), 1, "re-enabling exposes the still-live entry")


func test_owned_effect_follows_entity_and_detaches_when_owner_disappears() -> void:
	var world := _make_world()
	var file := _make_short_effect_file()
	var particle := file.find_particle("puff dots")
	particle.flags = 1 << 18  # FOREVEREMIT: detachment must make it finite.
	world.load_particle_file(file)
	var positions := OwnerPositions.new()
	positions.state = Vector3(1, 2, 3)
	world.set_owner_position_provider(Callable(positions, "resolve"))
	assert_gt(world.spawn_effect_owned(17, "puff", positions.state, Vector3.UP), 0)

	assert_true(bool(_single_group(world).forever),
			"the FOREVEREMIT group begins attached and infinite")
	positions.state = Vector3(9, 8, 7)
	world.sweep(0.0)
	var moved_emitter := _single_emitter(world)
	assert_almost_eq(moved_emitter.position, positions.state, Vector3(0.001, 0.001, 0.001),
			"CEffect_UpdateEmitterTransform-style follow updates the emitter each sweep")
	assert_eq(moved_emitter.forward, Vector3.UP,
			"a position-only provider preserves the descriptor's initial up-vector fallback")

	var attached_transform := Transform3D(Basis(Vector3.UP, PI * 0.5), Vector3(6, 5, 4))
	positions.state = attached_transform
	world.sweep(0.0)
	var rotated_emitter := _single_emitter(world)
	assert_almost_eq(rotated_emitter.position, attached_transform.origin,
			Vector3(0.001, 0.001, 0.001))
	assert_almost_eq(rotated_emitter.forward, attached_transform.basis.z.normalized(),
			Vector3(0.001, 0.001, 0.001),
			"the attached entity's live forward reaches the simulator frame")

	positions.alive = false
	world.sweep(0.0)
	var detached_group := _single_group(world)
	assert_true(bool(detached_group.detached), "owner removal detaches the group")
	assert_false(bool(detached_group.forever), "detachment stops FOREVEREMIT emission")
	assert_false(bool((_single_emitter(world)).emitting),
			"the detached emitter drains without producing new particles")


func test_attached_effect_composes_the_local_userpoint_offset() -> void:
	# The per-item entity-attached emitter (the DBuggy1 exhaust shape): the model-local
	# userpoint position AND direction compose onto the live owner transform every
	# sweep, so the plume stays on the tailpipe as the vehicle moves and turns
	# [orig: Entity_SpawnBoneTrailEffect @ 0x43bef0 — one mode-2 attached emitter per
	#  masked userpoint, pos = the userpoint, forward = its direction; follow =
	#  CEffect_UpdateEmitterTransform @ 0x5f7410].
	var world := _make_world()
	var file := _make_short_effect_file()
	file.find_particle("puff dots").flags = 1 << 18  # FOREVEREMIT — the exhaust shape
	world.load_particle_file(file)
	var positions := OwnerPositions.new()
	var start := Transform3D(Basis.IDENTITY, Vector3(10, 0, 0))
	positions.state = start
	world.set_owner_position_provider(Callable(positions, "resolve"))
	var local_pos := Vector3(-0.3, 1.4, -2.6)   # the FX00 rear-pipe point shape
	var local_dir := Vector3(0, 0.643, -0.766)
	assert_gt(world.spawn_effect_attached("itemfx:1:0", "puff", start, local_pos, local_dir), 0)
	var initial_emitter := _single_emitter(world)
	assert_almost_eq(initial_emitter.position, start * local_pos, Vector3(0.001, 0.001, 0.001),
			"the spawn seeds at owner-transform * local userpoint")
	assert_almost_eq(initial_emitter.forward.normalized(), local_dir.normalized(),
			Vector3(0.001, 0.001, 0.001),
			"the spawn seeds the userpoint direction before the first sweep")
	# Drive the owner: the offset rides the basis, the direction re-orients with it.
	var moved := Transform3D(Basis(Vector3.UP, PI * 0.5), Vector3(20, 3, 5))
	positions.state = moved
	world.sweep(0.0)
	var moved_emitter := _single_emitter(world)
	assert_almost_eq(moved_emitter.position, moved * local_pos, Vector3(0.001, 0.001, 0.001),
			"the follow composes the local offset, not the owner origin")
	assert_almost_eq(moved_emitter.forward, (moved.basis * local_dir).normalized(),
			Vector3(0.001, 0.001, 0.001),
			"the userpoint direction re-orients with the owner basis")
	# Owner gone (vehicle freed) -> detach: emission stops and the group drains.
	positions.alive = false
	world.sweep(0.0)
	assert_true(bool(_single_group(world).detached),
			"owner removal detaches the attached group")


func test_replacing_an_owned_group_detaches_the_old_group_transform() -> void:
	var world := _make_world()
	var file := _make_short_effect_file()
	file.find_particle("puff dots").flags = 1 << 18
	world.load_particle_file(file)
	var positions := OwnerPositions.new()
	positions.state = Transform3D(Basis.IDENTITY, Vector3(1, 2, 3))
	world.set_owner_position_provider(Callable(positions, "resolve"))
	assert_gt(world.spawn_effect_owned(17, "puff", Vector3(1, 2, 3), Vector3.UP), 0)

	assert_gt(world.spawn_effect_owned(17, "puff", Vector3(4, 5, 6), Vector3.UP), 0)
	var groups := world.get_debug_group_report()
	assert_eq(groups.size(), 2)
	assert_true(bool((groups[0] as Dictionary).detached),
			"replacement stops and detaches the old FOREVEREMIT group")
	assert_false(bool((groups[1] as Dictionary).detached),
			"the replacement remains attached")

	positions.state = Transform3D(Basis(Vector3.UP, PI * 0.25), Vector3(9, 8, 7))
	world.sweep(0.0)
	groups = world.get_debug_group_report()
	var old_emitter := ((groups[0] as Dictionary).emitters as Array)[0] as Dictionary
	var new_emitter := ((groups[1] as Dictionary).emitters as Array)[0] as Dictionary
	assert_eq(old_emitter.position, Vector3(1, 2, 3),
			"the detached old group drains at its last position")
	assert_eq(new_emitter.position, Vector3(9, 8, 7),
			"only the replacement follows the owner")


func test_release_effect_binding_drops_generation_scoped_token_and_pose_state() -> void:
	var world := _make_world()
	var file := _make_short_effect_file()
	file.find_particle("puff dots").flags = 1 << 18
	world.load_particle_file(file)
	var owner_key := "throwable-move:1024"
	var receipt := world.spawn_effect_owned_request(
			owner_key, "puff", Vector3(1, 2, 3), Vector3.UP)
	assert_true(bool(receipt.get("spawned", false)))
	assert_true(world.has_owner_binding(owner_key),
			"a live owned spawn holds its binding identity in every table")
	assert_true(world.has_cached_owner_pose(owner_key),
			"a live owned spawn seeds a native owner pose")

	world.stop_group(int(receipt.get("group_id", 0)))
	world.release_effect_binding(owner_key)

	assert_true(world.has_no_owner_bindings(),
			"retired round keys do not accumulate admission tokens, owner tokens, reverse lookups, or poses")
	assert_true(bool(_single_group(world).detached),
			"the already-live particles remain detached to drain naturally")


func test_release_effect_binding_cleans_a_rejected_spawn_identity() -> void:
	var world := _make_world()
	# No stockeffect fallback: the unknown name is rejected after the facade has
	# allocated its generation-scoped slot and owner identities.
	world.load_particle_file(_make_short_effect_file())
	var owner_key := "throwable-move:2048"
	var receipt := world.spawn_effect_owned_request(
			owner_key, "missing-effect", Vector3.ZERO, Vector3.UP)
	assert_false(bool(receipt.get("spawned", false)))
	assert_true(world.has_owner_binding(owner_key),
			"a rejected ReplaceOwned spawn still allocates its binding identities")
	assert_false(world.has_cached_owner_pose(owner_key),
			"a rejected ReplaceOwned spawn never seeds a native owner pose")

	world.release_effect_binding(owner_key)

	assert_true(world.has_no_owner_bindings(),
			"failed throwable spawns leave no script-side binding state")


func test_authored_water_flags_bind_to_the_mission_water_plane() -> void:
	var cases := [
		{"flag": 1 << 27, "mode": 1},  # BELOWH20: kill above.
		{"flag": 1 << 28, "mode": 2},  # ABOVEH20: kill at/below.
	]
	for entry in cases:
		var world := _make_world()
		world.set_water_height(12.5)
		var file := _make_short_effect_file()
		file.find_particle("puff dots").flags = entry.flag
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
	var group: Dictionary = report[0]
	assert_eq(String(group.get("name", "")), "puff", "the group names its interned effect")
	assert_eq((group.get("emitters", []) as Array).size(), 1, "one emitter row")
	var emitter: Dictionary = (group.get("emitters", []) as Array)[0]
	assert_true(emitter.has("alive") and emitter.has("rendered")
			and emitter.has("bounds") and emitter.has("emitter_id"),
			"emitter rows carry stable value diagnostics for the box view")
	assert_false(emitter.has("node"),
			"debug reports do not leak renderer Nodes")
	var unresolved_names := world.get_unresolved_texture_names()
	assert_false(group.has("unresolved"),
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
	assert_eq(count, 8, "fixture catalog registers 8 effects")
	var spawned := world.warm_all_effects(Vector3(1, 2, 3))
	assert_eq(spawned, 8, "the warm pass spawns each cataloged effect once")
	var warm_groups := world.get_debug_group_report()
	assert_eq(warm_groups.size(), 8,
			"catalog warming must not duplicate every effect into FirstPerson")
	for group_v in warm_groups:
		var group := group_v as Dictionary
		assert_eq(int(group.get("render_domain", -1)),
				NovaEffectWorld.RENDER_DOMAIN_WORLD,
				"catalog values warm through the uncapped World packet")
	assert_gt(world.active_entry_count(), 0, "warm spawns occupy live entries")
	world.advance_fixed_tick(0.016)
	world.reset_runtime_state()
	assert_eq(world.active_entry_count(), 0, "the reset clears every warm spawn")
	assert_eq(world.warm_all_effects(Vector3.ZERO), 8,
			"a later warm (reload) spawns the catalog again")


func test_render_now_submits_a_freshly_advanced_warm_snapshot() -> void:
	var world := _make_world()
	world.load_particle_file(_make_renderable_effect_file())
	assert_gt(world.spawn_effect("puff", Vector3.ZERO), 0)
	world.advance_fixed_tick(MissionRuntime.TICK_DT)
	assert_gt(world.render_now(), 0,
			"the public warm facade synchronously submits non-empty material runs")


func test_empty_catalog_warm_cleans_pipeline_helpers() -> void:
	var world := _make_world()
	assert_eq(_warm_helper_count(world), 0)
	assert_eq(world.warm_all_effects(Vector3.ZERO), 0)
	assert_gt(_warm_helper_count(world), 0,
			"an empty catalog still exercises the deterministic shader helpers")
	await get_tree().process_frame
	assert_eq(_warm_helper_count(world), 0,
			"the empty-catalog early return does not strand helper geometry")
