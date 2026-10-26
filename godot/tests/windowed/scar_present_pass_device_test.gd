extends GutTest

# The methods of tests/scar_present_pass_test.gd that need a RenderingDevice, split out of
# it so the windowed run selects them (G-100).

const SHADER_SCORCH := "res://shaders/scar_quad.gdshader"
const SHADER_HOLE := "res://shaders/scar_quad_hole.gdshader"
# The pass fog globals the raster legs drive (restored to the project defaults).
const FOG_GLOBALS := ["opennova_fog_enabled", "opennova_fog_color", "opennova_fog_start",
		"opennova_fog_end", "opennova_fog_type"]

var _temp_dirs: Array[String] = []


func after_each() -> void:
	ShaderGlobals.restore_defaults(FOG_GLOBALS)
	for dir_path in _temp_dirs:
		TestFs.remove_dir_recursive(dir_path)
	_temp_dirs.clear()


# A 64x64 own-world view over a flat clear, its camera 5 units off the quad.
func _raster_view(clear: Color) -> SubViewport:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var background := WorldEnvironment.new()
	background.environment = Environment.new()
	background.environment.background_mode = Environment.BG_COLOR
	background.environment.background_color = clear
	viewport.add_child(background)
	return viewport


# The centre pixel of one drawer-state quad (4x4, facing the camera) textured
# with a flat texel.
func _render_quad_centre(viewport: SubViewport, shader_path: String, texel: Color) -> Color:
	var image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	image.fill(texel)
	var material := ShaderMaterial.new()
	material.shader = load(shader_path)
	material.set_shader_parameter("albedo_tex", ImageTexture.create_from_image(image))
	var quad := QuadMesh.new()
	quad.size = Vector2(4.0, 4.0)
	var instance := MeshInstance3D.new()
	instance.mesh = quad
	instance.material_override = material
	viewport.add_child(instance)
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	var pixel := viewport.get_texture().get_image().get_pixel(32, 32)
	instance.queue_free()
	await get_tree().process_frame
	return pixel


# Both drawer states carry FOGENABLE (the 0x20000 bit of 0x120651 and
# 0x460651) and the drawer selects the scene fog colour
# [orig: Scar_DrawBatches @0x5CCD33 -> CD3DDevice_SetFogAndBlendMode(dev, 0)]:
# a scar at full pass fog renders the fog colour, not its own texel.
func test_scar_quads_fog_toward_the_scene_fog_colour() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var viewport := _raster_view(Color.BLACK)
	# Full linear fog at 5 units: visibility (end - d) / (end - start) = 0.
	RenderingServer.global_shader_parameter_set("opennova_fog_enabled", true)
	RenderingServer.global_shader_parameter_set("opennova_fog_color", Vector3(1.0, 0.0, 0.0))
	RenderingServer.global_shader_parameter_set("opennova_fog_start", 0.0)
	RenderingServer.global_shader_parameter_set("opennova_fog_end", 1.0)
	RenderingServer.global_shader_parameter_set("opennova_fog_type", 1)
	for shader_path in [SHADER_SCORCH, SHADER_HOLE]:
		var pixel: Color = await _render_quad_centre(
				viewport, shader_path, Color(0.5, 0.5, 0.5, 1.0))
		assert_gt(pixel.r, 0.9, "%s fogs to the red scene fog colour: %s" % [shader_path, pixel])
		assert_lt(pixel.g, 0.1, "%s: no texel grey survives full fog: %s" % [shader_path, pixel])


# The two drawer states by pixels, black texels over a white clear. The scorch
# TGAs are black RGB under an alpha falloff, so the mark IS the
# SRCALPHA/INVSRCALPHA blend (0x120651: blend nibble 1, no ALPHATESTENABLE, so
# the drawer's SetAlphaTestRef(128) latch is inert); the bhole state
# (0x460651) adds the live GREATER/128 alpha test. An alpha scissor (the
# 2026-08-21 first cut) drew every scorch as an opaque black blob.
func test_scar_quads_blend_and_only_the_hole_state_alpha_tests() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	RenderingServer.global_shader_parameter_set("opennova_fog_enabled", false)
	var viewport := _raster_view(Color.WHITE)
	for shader_path in [SHADER_SCORCH, SHADER_HOLE]:
		var dense: Color = await _render_quad_centre(
				viewport, shader_path, Color(0.0, 0.0, 0.0, 0.75))
		assert_between(dense.r, 0.05, 0.95,
				"%s blends a 0.75-alpha texel over the clear: %s" % [shader_path, dense])
	var faint_scorch: Color = await _render_quad_centre(
			viewport, SHADER_SCORCH, Color(0.0, 0.0, 0.0, 0.25))
	assert_between(faint_scorch.r, 0.05, 0.98,
			"scorch has no alpha test: a 0.25-alpha texel still blends: %s" % faint_scorch)
	var faint_hole: Color = await _render_quad_centre(
			viewport, SHADER_HOLE, Color(0.0, 0.0, 0.0, 0.25))
	assert_gt(faint_hole.r, 0.99,
			"bhole's GREATER/128 test discards a 0.25-alpha texel: %s" % faint_hole)
