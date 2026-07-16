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


class NamedTextureProvider:
	extends RefCounted
	var textures: Dictionary
	var requests := PackedStringArray()

	func _init(values: Dictionary) -> void:
		textures = values

	func load_texture(name: String) -> Texture2D:
		requests.append(name)
		return textures.get(name) as Texture2D


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


func test_per_frame_flipbook_files_are_assembled_into_the_full_strip() -> void:
	# Authored BASE name + four separate archive files: the atlas must contain
	# all four complete frames in order, requested under the witnessed derived
	# names — lowercase, truncated at the first .tga, _01.tga.. suffix; there
	# is no literal-base probe. [orig: CParticleDef_ReloadGraphicFrameTextures
	# @ 0x5e4bb0 names the frames (D-PTL-14 FIXED); one atlas input per frame,
	# CParticleManager_BuildTextureAtlases @ 0x5e8db0 -> CTextureData_LoadTGA
	# @ 0x5f7b20]
	var particle := _particle("BCas.tga")
	var graphics: Array = particle.graphics
	var layer := graphics[0] as NovaParticleGraphicLayer
	layer.flip_frames = 4
	layer.flip_rate = 1
	particle.graphics = graphics
	var provider := NamedTextureProvider.new({
		"bcas_01.tga": _solid_texture(Color(1, 0, 0, 1)),
		"bcas_02.tga": _solid_texture(Color(0, 1, 0, 1)),
		"bcas_03.tga": _solid_texture(Color(0, 0, 1, 1)),
		"bcas_04.tga": _solid_texture(Color(1, 1, 0, 1)),
	})
	var emitter := NovaParticleEmitter.new()
	emitter.auto_advance = false
	emitter.set_texture_provider(Callable(provider, "load_texture"))
	emitter.def = particle
	add_child_autofree(emitter)
	emitter.play()
	emitter.advance(0.26)

	var atlas := emitter.get_debug_atlas_texture().get_image()
	assert_eq(atlas.get_width(), 34,
			"four 8px frames form a 32px strip plus the two atlas gutters")
	assert_eq(atlas.get_height(), 10)
	assert_eq(atlas.get_pixel(5, 5), Color(1, 0, 0, 1), "frame 1 stays whole")
	assert_eq(atlas.get_pixel(13, 5), Color(0, 1, 0, 1), "frame 2 is loaded")
	assert_eq(atlas.get_pixel(21, 5), Color(0, 0, 1, 1), "frame 3 is loaded")
	assert_eq(atlas.get_pixel(29, 5), Color(1, 1, 0, 1), "frame 4 is loaded")
	assert_eq(provider.requests, PackedStringArray([
		"bcas_01.tga", "bcas_02.tga", "bcas_03.tga", "bcas_04.tga",
	]), "only the witnessed derived per-frame names are requested")


func test_layers_probe_no_fallback_names_beyond_the_witnessed_derivation() -> void:
	# One-frame graphics keep the authored literal verbatim; a miss stays
	# unresolved — retail has no trailing-letter variant strip and no _01
	# probe for one-frame layers. [orig: CParticleDef_ReloadGraphicFrameTextures
	# @ 0x5e4bb0 (D-PTL-14 FIXED); the atlas exclusion gate leaves misses
	# unpacked]
	var provider := NamedTextureProvider.new({
		"CFlamet3.tga": _solid_texture(Color(0, 0, 1, 1)),
	})
	var emitter := NovaParticleEmitter.new()
	emitter.auto_advance = false
	emitter.set_texture_provider(Callable(provider, "load_texture"))
	emitter.def = _particle("CFlamet3a.tga")
	add_child_autofree(emitter)
	emitter.play()
	emitter.advance(0.26)

	assert_eq(provider.requests, PackedStringArray(["CFlamet3a.tga"]),
			"a one-frame layer requests only the authored literal")
	assert_true(Array(emitter.get_unresolved_texture_names()).has("CFlamet3a.tga"),
			"the miss is reported unresolved instead of binding a variant file")
