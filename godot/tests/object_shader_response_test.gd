extends GutTest

# Raster pins for the object wrappers' retail combiner math, drawn through the
# production .gdshader wrappers on a real RenderingDevice (pending under the
# headless dummy renderer). Every case isolates one retail rule:
# - the SELFLUM emissive saturates SelfLumColor x gain before MODULATE2X;
# - fixed-function lighting sums and saturates per vertex (Gouraud);
# - the armed reflection view draws _FFP.fx TBoringFFPClip instead of NORMAL;
# - the object sampler is a 2x anisotropic minifier.

const GLOBALS := [
	"opennova_light_block_dir", "opennova_light_block_dir_color",
	"opennova_light_block_hemi_sky", "opennova_light_block_hemi_ground",
	"opennova_light_block_gain", "opennova_thermal_view",
	"opennova_fog_enabled", "opennova_fog_color", "opennova_fog_start",
	"opennova_fog_end", "opennova_fog_type",
	"opennova_water_active", "opennova_water_height",
	"opennova_water_reflection_clip_active", "opennova_water_reflection_eye",
	"opennova_environment_cube_ready",
]
const EYE := Vector3(0.0, 0.0, 5.0)

var _saved := {}


func before_each() -> void:
	_saved.clear()
	# Restore to the project's registered defaults; reading the live value
	# back (global_shader_parameter_get) is an editor-only call.
	for name in GLOBALS:
		var setting = ProjectSettings.get_setting("shader_globals/" + name, {})
		_saved[name] = (setting as Dictionary).get("value") if setting is Dictionary else null
	_global("opennova_thermal_view", false)
	_global("opennova_fog_enabled", false)
	_global("opennova_water_active", false)
	_global("opennova_water_reflection_clip_active", false)
	_global("opennova_environment_cube_ready", false)
	_global("opennova_light_block_gain", Vector3.ONE)
	_global("opennova_light_block_dir", Vector3(0.0, 0.0, -1.0))
	_global("opennova_light_block_dir_color", Vector3.ZERO)
	_flat_hemisphere(0.5)


func after_each() -> void:
	for name in _saved:
		if _saved[name] != null:
			RenderingServer.global_shader_parameter_set(name, _saved[name])


func _global(name: String, value: Variant) -> void:
	RenderingServer.global_shader_parameter_set(name, value)


func _flat_hemisphere(level: float) -> void:
	_global("opennova_light_block_hemi_sky", Vector3.ONE * level)
	_global("opennova_light_block_hemi_ground", Vector3.ONE * level)


func _rd_available() -> bool:
	return RenderingServer.get_rendering_device() != null


func _view() -> SubViewport:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_COLOR
	environment_resource.background_color = Color.BLACK
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment_resource.glow_enabled = false
	var environment := WorldEnvironment.new()
	environment.environment = environment_resource
	viewport.add_child(environment)
	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 2.0
	camera.position = EYE
	camera.current = true
	viewport.add_child(camera)
	viewport.add_child(DisplayDecode.new())
	return viewport


func _solid(color: Color) -> ImageTexture:
	var image := Image.create(1, 1, true, Image.FORMAT_RGBA8)
	image.fill(color)
	return ImageTexture.create_from_image(image)


# A 2x2 quad in the XY plane facing the camera; normals per corner (left
# pair, right pair).
func _quad(material: ShaderMaterial, left_normal := Vector3.BACK,
		right_normal := Vector3.BACK) -> MeshInstance3D:
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3(-1, 1, 0), Vector3(1, 1, 0), Vector3(1, -1, 0), Vector3(-1, -1, 0)])
	arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array([
		left_normal, right_normal, right_normal, left_normal])
	arrays[Mesh.ARRAY_TEX_UV] = PackedVector2Array([
		Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 1)])
	arrays[Mesh.ARRAY_TEX_UV2] = arrays[Mesh.ARRAY_TEX_UV]
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2, 0, 2, 3])
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.material_override = material
	return instance


func _material(wrapper: String, diffuse: Texture2D) -> ShaderMaterial:
	var material := ShaderMaterial.new()
	material.shader = load("res://shaders/object/%s.gdshader" % wrapper) as Shader
	material.set_shader_parameter("u_diffuse", diffuse)
	material.set_shader_parameter("u_detail", _solid(Color(0.5, 0.5, 0.5, 1.0)))
	material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
	material.set_shader_parameter("u_alpha_mod", 1.0)
	return material


func _centre(viewport: SubViewport) -> Color:
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return viewport.get_texture().get_image().get_pixel(32, 32)


func _render(wrapper: String, diffuse: Texture2D, configure := Callable()) -> Color:
	var viewport := _view()
	var material := _material(wrapper, diffuse)
	if configure.is_valid():
		configure.call(material)
	viewport.add_child(_quad(material))
	return await _centre(viewport)


func test_selflum_emissive_saturates_self_lum_times_gain_before_modulate2x() -> void:
	# _FFP.fx SELFLUM: MaterialEmissive = SelfLumColor x ColorSrcGlobalGain,
	# saturated by the fixed-function lighting stage, then MODULATE2X. A
	# 0.25 SelfLumColor at gain 2 is 0.5 emissive -> white Diffuse1 x 1.0.
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	_global("opennova_light_block_gain", Vector3.ONE * 2.0)
	var pixel: Color = await _render("self_lit/opaque_double_sided", _solid(Color.WHITE),
			func(material: ShaderMaterial) -> void:
				material.set_shader_parameter("u_rgb_mod", Vector3.ONE * 0.25))
	assert_almost_eq(pixel.r, 1.0, 0.02,
			"sat(0.25 x 2) x 2 lights white Diffuse1 fully: %s" % pixel)
	_global("opennova_light_block_gain", Vector3.ONE * 4.0)
	pixel = await _render("self_lit/opaque_double_sided", _solid(Color.WHITE),
			func(material: ShaderMaterial) -> void:
				material.set_shader_parameter("u_rgb_mod", Vector3.ONE * 0.1))
	assert_almost_eq(pixel.r, 0.8, 0.02,
			"sat(0.1 x 4) x 2 = 0.8 (a gain above 1 is not clipped alone): %s" % pixel)


func test_fixed_function_diffuse_saturates_per_vertex_and_gouraud_interpolates() -> void:
	# The left corners face the light (N.L = 1), the right ones are edge-on
	# (N.L = 0). Per vertex: sat(0.1 + 4) = 1 and 0.1, interpolated to 0.55 at
	# the centre; lighting the interpolated normal instead would saturate to 1.
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	_flat_hemisphere(0.1)
	_global("opennova_light_block_dir_color", Vector3.ONE * 4.0)
	var viewport := _view()
	var material := _material("fixed/opaque_double_sided",
			_solid(Color(0.25, 0.25, 0.25, 1.0)))
	viewport.add_child(_quad(material, Vector3.BACK, Vector3.RIGHT))
	var pixel: Color = await _centre(viewport)
	assert_almost_eq(pixel.r, 0.25 * 0.55 * 2.0, 0.03,
			"Gouraud 0.55 x 0.25 x 2 at the centre, not a per-pixel 0.5: %s" % pixel)


func _arm_reflection_view() -> void:
	_global("opennova_water_active", true)
	_global("opennova_water_reflection_clip_active", true)
	_global("opennova_water_reflection_eye", EYE)
	_global("opennova_water_height", -10.0)


func test_reflection_view_lights_selflum_materials_like_any_ffp_material() -> void:
	# TBoringFFPClip has no SELFLUM block: in the armed reflection view the
	# emissive-black self-lit card is lit by the hemisphere (0.5 x 2).
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var black_selflum := func(material: ShaderMaterial) -> void:
		material.set_shader_parameter("u_rgb_mod", Vector3.ZERO)
	var normal: Color = await _render("self_lit/opaque_double_sided",
			_solid(Color.WHITE), black_selflum)
	assert_lt(normal.r, 0.02, "NORMAL: a black SelfLumColor draws black: %s" % normal)
	_arm_reflection_view()
	var clip: Color = await _render("self_lit/opaque_double_sided",
			_solid(Color.WHITE), black_selflum)
	assert_almost_eq(clip.r, 1.0, 0.02,
			"CLIP: white Diffuse1 x hemisphere 0.5 x 2: %s" % clip)


func test_reflection_view_alpha_tests_every_ffp_material_at_128() -> void:
	# AMODE_CLIP enables the alpha test at ref 0x80 for every _FFP material:
	# an opaque (untested) card whose Diffuse1.a is 0.4 vanishes only there.
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var faint := _solid(Color(1.0, 1.0, 1.0, 0.4))
	var normal: Color = await _render("fixed/opaque_double_sided", faint)
	assert_almost_eq(normal.r, 1.0, 0.02, "NORMAL ignores alpha on an opaque card: %s" % normal)
	_arm_reflection_view()
	var clip: Color = await _render("fixed/opaque_double_sided", faint)
	assert_lt(clip.r, 0.02, "CLIP discards Diffuse1.a 0.4 at ref 128: %s" % clip)


func test_reflection_view_drops_the_detail_stage() -> void:
	# The clip texture takes stage 1, so the _MT detail modulation is gone.
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var black_detail := func(material: ShaderMaterial) -> void:
		material.set_shader_parameter("u_detail", _solid(Color.BLACK))
	var normal: Color = await _render("fixed_detail/opaque_double_sided",
			_solid(Color.WHITE), black_detail)
	assert_lt(normal.r, 0.02, "NORMAL: a black detail stage darkens the card: %s" % normal)
	_arm_reflection_view()
	var clip: Color = await _render("fixed_detail/opaque_double_sided",
			_solid(Color.WHITE), black_detail)
	assert_almost_eq(clip.r, 1.0, 0.02, "CLIP: Diffuse1 alone: %s" % clip)


func test_reflection_view_fogs_alpha_blend_ffp_toward_grey() -> void:
	# TBoringFFPClip's BLEND_ALPHA pass declares FOGMODE_NORMALSET, whose
	# device fog colour is 0x7F7F7F; NORMAL fogs toward the scene colour.
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	_global("opennova_fog_enabled", true)
	_global("opennova_fog_color", Vector3.ZERO)
	_global("opennova_fog_start", 0.0)
	_global("opennova_fog_end", 1.0)
	_global("opennova_fog_type", 1)
	var normal: Color = await _render("fixed/alpha_double_sided", _solid(Color.WHITE))
	assert_lt(normal.r, 0.02, "NORMAL fogs fully to the black scene colour: %s" % normal)
	_arm_reflection_view()
	var clip: Color = await _render("fixed/alpha_double_sided", _solid(Color.WHITE))
	assert_almost_eq(clip.r, 127.0 / 255.0, 0.02, "CLIP fogs to 0x7F grey: %s" % clip)


func _mip_level_texture() -> ImageTexture:
	# 64x64 with a distinct solid colour per mip level: red, green, blue, ...
	var colours := [Color.RED, Color.GREEN, Color.BLUE, Color.WHITE]
	var data := PackedByteArray()
	var side := 64
	var level := 0
	while true:
		var colour: Color = colours[mini(level, colours.size() - 1)]
		for _texel in side * side:
			data.append(colour.r8)
			data.append(colour.g8)
			data.append(colour.b8)
			data.append(255)
		if side == 1:
			break
		side /= 2
		level += 1
	var image := Image.create_from_data(64, 64, true, Image.FORMAT_RGBA8, data)
	return ImageTexture.create_from_image(image)


func test_object_sampler_is_a_2x_anisotropic_minifier() -> void:
	# The orthographic view maps one texel to one pixel across the card; a
	# 60 degree tilt packs two texels into each pixel vertically. A 2x
	# anisotropic footprint keeps level 0 (red); an isotropic minifier would
	# pick level 1 (green) from the longer axis.
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var viewport := _view()
	var quad := _quad(_material("fixed/opaque_double_sided", _mip_level_texture()))
	quad.rotation_degrees = Vector3(60.0, 0.0, 0.0)
	viewport.add_child(quad)
	var pixel: Color = await _centre(viewport)
	assert_gt(pixel.r, 0.8, "the 2:1 footprint samples level 0: %s" % pixel)
	assert_lt(pixel.g, 0.2, "and not the isotropic level 1: %s" % pixel)


func test_object_sampler_stops_at_the_textures_last_retail_mip_level() -> void:
	# A pixel-built retail texture has no level below its 4x4 one, and D3D
	# clamps there: with the ceiling at level 2 (blue) a footprint that would
	# reach level 4 (white) still samples blue.
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var viewport := _view()
	var material := _material("fixed/opaque_double_sided", _mip_level_texture())
	material.set_shader_parameter("u_diffuse_max_lod", 2.0)
	var quad := _quad(material)
	quad.scale = Vector3.ONE / 16.0
	viewport.add_child(quad)
	var pixel: Color = await _centre(viewport)
	assert_gt(pixel.b, 0.8, "the minified card samples its last level: %s" % pixel)
	assert_lt(pixel.r + pixel.g, 0.4, "not a level below it: %s" % pixel)
