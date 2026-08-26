extends GutTest


# Beauty (0x18C01) draws the given color; the isolated Q3 view (0x10401)
# draws the optional glow color; every other camera discards.
func _canary_material(beauty: Color, q3 := Color(0.0, 0.0, 0.0, 0.0)) -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = """
shader_type spatial;
render_mode unshaded, depth_draw_opaque, cull_disabled;
uniform vec3 u_beauty;
uniform vec3 u_q3;
uniform bool u_q3_present = false;

void fragment() {
	if (CAMERA_VISIBLE_LAYERS == 101377u) {
		ALBEDO = u_beauty;
	} else if (CAMERA_VISIBLE_LAYERS == 66561u && u_q3_present) {
		ALBEDO = u_q3;
	} else {
		discard;
	}
}
"""
	var material := ShaderMaterial.new()
	material.shader = shader
	material.set_shader_parameter("u_beauty", Vector3(beauty.r, beauty.g, beauty.b))
	material.set_shader_parameter("u_q3", Vector3(q3.r, q3.g, q3.b))
	material.set_shader_parameter("u_q3_present", q3.a > 0.0)
	return material


func _solid_texture(color: Color) -> ImageTexture:
	var image := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	image.set_pixel(0, 0, color)
	return ImageTexture.create_from_image(image)


func _channel_peaks(image: Image) -> Dictionary:
	var values := Vector3(-1.0, -1.0, -1.0)
	var positions := [Vector2i.ZERO, Vector2i.ZERO, Vector2i.ZERO]
	for y in image.get_height():
		for x in image.get_width():
			var pixel := image.get_pixel(x, y)
			for channel in 3:
				if pixel[channel] > values[channel]:
					values[channel] = pixel[channel]
					positions[channel] = Vector2i(x, y)
	return {"values": values, "positions": positions}


func _controlled_water_material() -> ShaderMaterial:
	var material := ShaderMaterial.new()
	material.shader = load("res://shaders/water.gdshader") as Shader
	material.render_priority = ObjectShaderCache.RENDER_RUNG_WATER
	# With a black reflection/specular/fog source, CUSTOM1.a=0 from QuadMesh,
	# and a 0.25 noise alpha, the production detail path contributes black and
	# retains exactly half of the destination: alpha_dist=0.25*1*2=0.5.
	material.set_shader_parameter("u_water_color", Vector3.ZERO)
	material.set_shader_parameter("u_fog_color", Vector3.ZERO)
	material.set_shader_parameter("u_has_reflection", false)
	material.set_shader_parameter("u_underwater_view", false)
	material.set_shader_parameter("u_noise_color",
			_solid_texture(Color(1.0, 1.0, 1.0, 0.25)))
	material.set_shader_parameter("u_noise_normal",
			_solid_texture(Color(0.5, 0.5, 1.0, 1.0)))
	return material


func _water_order_particle_scene() -> EffectScene:
	var particle := ParticleDef.new()
	particle.id = "Water order particle"
	particle.emit_dur = 0.1
	particle.emit_rate = 20.0
	particle.emit_burst = 1
	particle.age = 2.0
	particle.alpha = 1.0
	particle.scale_value = 2.0
	particle.color1 = Color.WHITE
	particle.color2 = Color.WHITE
	particle.color3 = Color.WHITE
	particle.color4 = Color.WHITE
	var graphics: Array = particle.graphics
	var layer := graphics[0] as ParticleGraphicLayer
	layer.present = true
	layer.texture = "water_order_fallback.tga"
	layer.blend_mode = 0
	layer.alpha = 0.5
	layer.scale_value = 2.0
	particle.graphics = graphics

	var effect := ParticleEffect.new()
	effect.id = "Water order effect"
	effect.pdefs = PackedStringArray([particle.id])
	var file := ParticleFile.new()
	file.particles = [particle]
	file.effects = [effect]

	var scene := EffectScene.new()
	scene.open([file])
	for row in [
			[Vector3(0.0, -0.05, 2.0), Vector3(0.0, 0.0, 1.0)],
			[Vector3(0.0, 0.05, 2.0), Vector3(0.0, 1.0, 0.0)],
	]:
		var transform := Transform3D.IDENTITY
		transform.origin = row[0]
		var receipt := scene.spawn({
			"effect_handle": scene.intern(effect.id),
			"transform": transform,
			"color_tint": row[1],
		})
		assert_eq(int(receipt.get("status", -1)),
				EffectScene.SPAWN_STATUS_SPAWNED)
	scene.advance_in_place(0.1)
	return scene


func test_world_frame_module_owns_kernel_sized_q3_and_the_terminal_effect() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 32)
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
	camera.current = true
	camera.cull_mask = 0x12345
	viewport.add_child(camera)

	# Green in beauty, red in the isolated Q3 source: the FrameFX composite
	# must add a blurred red glow over the green frame.
	var quad := MeshInstance3D.new()
	var mesh := QuadMesh.new()
	mesh.size = Vector2(8.0, 8.0)
	quad.mesh = mesh
	quad.material_override = _canary_material(Color(0.0, 1.0, 0.0),
			Color(1.0, 0.0, 0.0, 1.0))
	quad.position = Vector3(0.0, 0.0, -2.0)
	viewport.add_child(quad)

	var renderer := FrameFx.new()
	viewport.add_child(renderer)
	for _frame in 4:
		await get_tree().process_frame

	var report := renderer.get_backend_report()
	assert_eq(int(report.get("beauty_camera_mask", -1)), 101377,
			"the beauty signature admits the first-person viewmodel layer")
	assert_eq(int(report.get("q3_camera_mask", -1)), 66561)
	assert_true(bool(report.get("q3_viewport_present", false)))
	# Kernel height, beauty aspect: a 2:1 beauty view yields a 512x256 source
	# that the capture squashes into the 256-square exactly like retail's
	# StretchRect of the backbuffer-sized altbuffer.
	assert_eq(int(report.get("q3_working_height", -1)), 256)
	assert_eq(report.get("q3_viewport_size", Vector2i.ZERO), Vector2i(512, 256))
	assert_eq(int(report.get("q3_msaa_3d", -1)), Viewport.MSAA_8X)
	assert_true(bool(report.get("q3_hdr_2d", false)),
			"the Q3 source keeps gamma-domain numbers: no sRGB encode on its target")
	assert_eq(int(report.get("q3_update_mode", -1)), SubViewport.UPDATE_ALWAYS)
	assert_true(bool(report.get("terminal_compositor_installed", false)))
	assert_eq(camera.cull_mask, 101377,
			"the module selects the one supported beauty camera signature")
	assert_false(report.has("far_alpha_stage"),
			"no auxiliary far-alpha view exists: pass A rides PRE_TRANSPARENT")

	var compositor := environment.compositor
	assert_not_null(compositor)
	var effects := compositor.compositor_effects
	assert_eq(effects.size(), 1,
			"an otherwise empty world receives exactly the terminal effect")
	assert_eq(effects[0].effect_callback_type,
			CompositorEffect.EFFECT_CALLBACK_TYPE_POST_TRANSPARENT,
			"FrameFX and terminal transfer are last")

	if bool(report.get("rd_available", false)):
		RenderingServer.force_draw(true)
		RenderingServer.force_sync()
		assert_eq(String(report.get("status", "")), "drawn",
				String(report.get("failure", "FrameFX terminal failed")))
		assert_true(bool(report.get("q3_sampled", false)),
				"the terminal sampled the kernel-sized Q3 source")
		var center := viewport.get_texture().get_image().get_pixel(32, 16)
		assert_gt(center.g, 0.9, "beauty green survives the terminal transfer")
		assert_gt(center.r, 0.1,
				"the Q3 red glow is composited at half strength over beauty")
		assert_lt(center.b, 0.05)
	else:
		pending("RenderingDevice unavailable under this Godot renderer")

	viewport.remove_child(renderer)
	renderer.free()
	assert_eq(camera.cull_mask, 0x12345,
			"teardown restores the caller's camera mask")
	assert_null(environment.compositor,
			"teardown restores the inherited compositor")


func test_far_particles_water_and_camera_particles_reach_the_frame_in_retail_order() -> void:
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
	camera.position = Vector3(0.0, 0.1, 5.0)
	camera.current = true
	viewport.add_child(camera)
	camera.look_at(Vector3.ZERO, Vector3.UP)

	# An opaque red backdrop drawn by the beauty pass itself (nothing in Q3).
	var backdrop := MeshInstance3D.new()
	var backdrop_mesh := QuadMesh.new()
	backdrop_mesh.size = Vector2(20.0, 20.0)
	backdrop.mesh = backdrop_mesh
	backdrop.material_override = _canary_material(Color(1.0, 0.0, 0.0))
	backdrop.position = Vector3(0.0, 0.0, 0.0)
	viewport.add_child(backdrop)

	var water := MeshInstance3D.new()
	var water_mesh := QuadMesh.new()
	water_mesh.size = Vector2(20.0, 20.0)
	water.mesh = water_mesh
	water.material_override = _controlled_water_material()
	water.layers = Water.VISUAL_LAYER_WATER
	water.position = Vector3(0.0, 0.0, 1.0)
	water.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	viewport.add_child(water)

	var frame_renderer := FrameFx.new()
	viewport.add_child(frame_renderer)
	var particle_renderer := ParticleRenderer.new()
	particle_renderer.scene = _water_order_particle_scene()
	particle_renderer.procedural_fallback_enabled = true
	particle_renderer.set_water_plane(0.0, null)
	viewport.add_child(particle_renderer)
	particle_renderer.render_now()
	for _frame in 6:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()

	var report := particle_renderer.get_debug_draw_list_report()
	var far: Dictionary = report.get("world_far_side", {})
	var camera_side: Dictionary = report.get("world_camera_side", {})
	assert_eq(int(far.get("selected_emitters", -1)), 1)
	assert_eq(int(camera_side.get("selected_emitters", -1)), 1)
	assert_gt(int(far.get("rendered_quad_count", 0)), 0)
	assert_gt(int(camera_side.get("rendered_quad_count", 0)), 0)
	var far_backend: Dictionary = report.get("world_far_backend", {})
	var camera_backend: Dictionary = report.get("world_camera_backend", {})
	if not bool(far_backend.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_eq(String(far_backend.get("status", "")), "drawn",
			String(far_backend.get("failure", "far particle pass failed")))
	assert_eq(String(camera_backend.get("status", "")), "drawn",
			String(camera_backend.get("failure", "camera particle pass failed")))
	assert_eq(int(far_backend.get("callback_type", -1)),
			CompositorEffect.EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT,
			"pass A runs before the transparent list (and therefore before water)")
	assert_eq(int(camera_backend.get("callback_type", -1)),
			CompositorEffect.EFFECT_CALLBACK_TYPE_POST_TRANSPARENT,
			"pass B runs after the transparent list (and therefore after water)")

	var with_water_image := viewport.get_texture().get_image()
	var with_water := with_water_image.get_pixel(32, 32)
	water.visible = false
	for _frame in 3:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	var without_water_image := viewport.get_texture().get_image()
	var without_water := without_water_image.get_pixel(32, 32)
	var diagnostic := "with=%s peaks=%s without=%s peaks=%s" % [
			with_water, _channel_peaks(with_water_image),
			without_water, _channel_peaks(without_water_image)]
	# Before the terminal decode the order is red opaque backdrop -> far blue
	# source-over (PRE_TRANSPARENT) -> water x0.5 -> near green source-over
	# (POST_TRANSPARENT). Compare the same immutable particle packet with water
	# hidden: red and blue must rise, while green must remain unchanged
	# because its pass follows water. (The far-side object ALPHA strips that
	# retail draws before pass A are the documented D-RORD-7 residual.)
	assert_gt(with_water.g, 0.12,
			"the camera-side green particle must be applied after water")
	assert_gt(with_water.r, 0.002,
			"the opaque backdrop must survive both alpha stages")
	assert_gt(with_water.b, 0.002,
			"the far-side blue particle must survive water attenuation")
	assert_lt(with_water.r, with_water.g * 0.25, diagnostic)
	assert_lt(with_water.b, with_water.g * 0.25, diagnostic)
	assert_gt(without_water.r, with_water.r + 0.02,
			"water must attenuate the earlier backdrop; " + diagnostic)
	assert_gt(without_water.b, with_water.b + 0.05,
			"water must attenuate the earlier far particle; " + diagnostic)
	assert_almost_eq(without_water.g, with_water.g, 0.02,
			"water must not attenuate the later camera-side particle; " + diagnostic)
