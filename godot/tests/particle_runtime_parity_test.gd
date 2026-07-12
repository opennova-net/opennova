extends GutTest

# Focused regressions for Godot-side particle state that is not exercised by
# the portable emitter tests: spawn snapshots, runtime texture visibility,
# curve rebaking, and texture-atlas identity.


class TextureProvider:
	extends RefCounted
	var texture: Texture2D

	func _init(value: Texture2D) -> void:
		texture = value

	func load_texture(_name: String) -> Texture2D:
		return texture


func _solid_texture(color: Color) -> ImageTexture:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(color)
	return ImageTexture.create_from_image(image)


func _particle(texture_name: String = "") -> NovaParticleDef:
	var particle := NovaParticleDef.new()
	particle.id = "RuntimeParity"
	particle.emit_dur = 1.0
	particle.emit_rate = 10.0
	particle.emit_burst = 1
	particle.age = 5.0
	particle.alpha = 1.0
	particle.scale_value = 2.0
	particle.color1 = Color(0.25, 0.5, 0.75, 1)
	particle.color2 = Color(0.25, 0.5, 0.75, 1)
	particle.color3 = Color(0.25, 0.5, 0.75, 1)
	particle.color4 = Color(0.25, 0.5, 0.75, 1)
	var graphics: Array = particle.graphics
	var layer := graphics[0] as NovaParticleGraphicLayer
	layer.present = true
	layer.index = 1
	layer.texture = texture_name
	layer.alpha = 1.0
	layer.scale_value = 2.0
	particle.graphics = graphics
	return particle


func _emitter(particle: NovaParticleDef, procedural: bool = true) -> NovaParticleEmitter:
	var emitter := NovaParticleEmitter.new()
	emitter.auto_advance = false
	emitter.procedural_fallback_enabled = procedural
	emitter.def = particle
	add_child_autofree(emitter)
	emitter.play()
	emitter.advance(0.26)
	return emitter


func test_live_particles_keep_their_spawn_captured_color() -> void:
	var particle := _particle()
	var emitter := _emitter(particle)
	assert_gt(emitter.get_alive_count(), 0)
	var spawned := emitter.get_debug_first_color()
	assert_almost_eq(spawned.r, 0.25, 0.01)
	assert_almost_eq(spawned.g, 0.5, 0.01)
	assert_almost_eq(spawned.b, 0.75, 0.01)

	# Definition edits affect future particles, not records already spawned.
	particle.color1 = Color(0, 1, 0, 1)
	particle.color2 = Color(0, 1, 0, 1)
	particle.color3 = Color(0, 1, 0, 1)
	particle.color4 = Color(0, 1, 0, 1)
	emitter.advance(0.0)
	var rendered := emitter.get_debug_first_color()
	assert_almost_eq(rendered.r, 0.25, 0.01,
			"the live particle retains its spawn snapshot")
	assert_almost_eq(rendered.g, 0.5, 0.01,
			"the mutable definition is not re-read while rendering")
	assert_almost_eq(rendered.b, 0.75, 0.01)


func test_blank_runtime_graphic_is_invisible_but_preview_fallback_is_explicit() -> void:
	var emitter := _emitter(_particle(), false)
	assert_gt(emitter.get_alive_count(), 0, "a blank graphic does not suppress simulation")
	assert_eq(emitter.get_rendered_instance_count(), 0,
			"retail-authored blank graphics remain invisible at runtime")

	emitter.procedural_fallback_enabled = true
	assert_gt(emitter.get_rendered_instance_count(), 0,
			"editor diagnostics may explicitly render the soft-disc fallback")


func test_set_tables_rebakes_curve_references_before_future_spawns() -> void:
	var particle := _particle()
	particle.alpha_func.name = "zero_alpha"
	particle.alpha_func.present = true
	var emitter := _emitter(particle)
	assert_gt(emitter.get_debug_first_color().a, 0.9,
			"an unresolved curve leaves alpha unchanged")

	var table := NovaParticleTable.new()
	table.id = "zero_alpha"
	var zeroes := PackedByteArray()
	zeroes.resize(256)
	table.data = zeroes
	emitter.tables = [table]
	emitter.advance(0.26)
	assert_lt(emitter.get_debug_first_color().a, 0.01,
			"late table assignment rebuilds native LUTs instead of keeping stale curves")


func test_same_sized_provider_texture_rebuilds_atlas_pixels() -> void:
	var particle := _particle("same_name.tga")
	var red_provider := TextureProvider.new(_solid_texture(Color(1, 0, 0, 1)))
	var emitter := NovaParticleEmitter.new()
	emitter.auto_advance = false
	emitter.set_texture_provider(Callable(red_provider, "load_texture"))
	emitter.def = particle
	add_child_autofree(emitter)
	emitter.play()
	emitter.advance(0.26)
	var first := emitter.get_debug_atlas_texture().get_image()
	assert_gt(first.get_pixel(1, 1).r, 0.9)

	var green_provider := TextureProvider.new(_solid_texture(Color(0, 1, 0, 1)))
	emitter.set_texture_provider(Callable(green_provider, "load_texture"))
	var rebuilt := emitter.get_debug_atlas_texture().get_image()
	assert_gt(rebuilt.get_pixel(1, 1).g, 0.9,
			"texture identity invalidates an equal-dimension atlas cache entry")
	assert_lt(rebuilt.get_pixel(1, 1).r, 0.1)
