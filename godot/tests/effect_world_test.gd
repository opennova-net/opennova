extends GutTest

# NovaEffectWorld — the runtime .ptl effect world (load + intern + spawn +
# expiry). Mirrors the witnessed chain: CEffectSystem_Init @ 0x5f6070 loads
# every mounted .ptl; CEffect_FindOrCreateMaterial @ 0x5f7310 interns effect
# names to stable 1-based handles (case-insensitive); SpawnEmitterAtPosition
# @ 0x5f6df0 spawns by handle or name.

const EffectWorldScript = preload("res://engine/world/effect_world.gd")

var _root_dir := ""


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
	var emitter: NovaParticleEmitter = null
	for child in world.get_children():
		if child is NovaParticleEmitter:
			emitter = child
			break
	assert_not_null(emitter, "spawn adds a NovaParticleEmitter child")
	assert_almost_eq(emitter.global_position, Vector3(1, 2, 3), Vector3(0.01, 0.01, 0.01))
	# Let the one-shot emit and die, then sweep past the group window.
	for i in 30:
		emitter.advance(0.1)
	assert_eq(emitter.get_alive_count(), 0, "finite one-shot finishes")
	world.sweep(2.0)
	assert_eq(world.live_group_count(), 0, "finished finite group is swept")


func test_spawn_by_handle_matches_name_spawn() -> void:
	var world := _make_world()
	world.load_particle_file(_make_short_effect_file())
	var handle := world.intern_effect("PUFF")
	assert_gt(handle, 0)
	assert_true(world.spawn_effect_by_handle(handle, Vector3.ZERO), "handle spawn succeeds")
	assert_eq(world.live_group_count(), 1)
	assert_false(world.spawn_effect_by_handle(99, Vector3.ZERO), "out-of-range handle refuses")


func test_clear_world_frees_live_groups() -> void:
	var world := _make_world()
	world.load_particle_file(_make_short_effect_file())
	world.spawn_effect("puff", Vector3.ZERO)
	assert_eq(world.live_group_count(), 1)
	world.clear_world()
	assert_eq(world.live_group_count(), 0)
	assert_eq(world.effect_count(), 0)
