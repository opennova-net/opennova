extends GutTest

const Q3_LUM_3DI := "res://../fixtures/threedi/synth/armory.3di"


# Beauty-camera canary. Q3 sources are registered only by production typed
# producers; tests do not get a second renderer or an untyped injection bus.
func _canary_material(beauty: Color) -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = """
shader_type spatial;
render_mode unshaded, depth_draw_opaque, cull_disabled;
uniform vec3 u_beauty;

void fragment() {
	if (CAMERA_VISIBLE_LAYERS == 101377u) {
		ALBEDO = u_beauty;
	} else {
		discard;
	}
}
"""
	var material := ShaderMaterial.new()
	material.shader = shader
	material.set_shader_parameter("u_beauty", Vector3(beauty.r, beauty.g, beauty.b))
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


func _max_rgb_delta(lhs: Image, rhs: Image) -> Dictionary:
	assert_eq(lhs.get_size(), rhs.get_size())
	var best: float = -1.0
	var best_position := Vector2i.ZERO
	for y in lhs.get_height():
		for x in lhs.get_width():
			var a := lhs.get_pixel(x, y)
			var b := rhs.get_pixel(x, y)
			var delta: float = abs(a.r - b.r) + abs(a.g - b.g) + \
					abs(a.b - b.b)
			if delta > best:
				best = delta
				best_position = Vector2i(x, y)
	return {"delta": best, "position": best_position}


func _lum_object_data() -> ObjectData:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(Q3_LUM_3DI)), OK)
	return data


func _hide_non_opaque_lum_surfaces(root: Node) -> void:
	if root is MeshInstance3D:
		var mesh_instance := root as MeshInstance3D
		var material := mesh_instance.get_active_material(0) as ShaderMaterial
		var shader_path := material.shader.resource_path \
				if material != null and material.shader != null else ""
		mesh_instance.visible = "/self_lit/opaque.gdshader" in shader_path
	for child in root.get_children():
		_hide_non_opaque_lum_surfaces(child)


func _q3_lum_view(use_q3: bool) -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 96)
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
	camera.cull_mask = 101377
	camera.position = Vector3(0.0, 0.0, 10.0)
	viewport.add_child(camera)
	camera.look_at(Vector3.ZERO, Vector3.UP)

	var model := ObjectModel.new()
	viewport.add_child(model)
	model.set_process(false)
	model.set_object_data(_lum_object_data())
	model.scale = Vector3.ONE * 6.0
	# The armory fixture's authored opaque LUM bulb is centered at Godot-local
	# (-0.3, 2.8, 3.4). Center and enlarge that real strip, then hide the
	# unrelated shell/material sources so its own beauty depth is the reference.
	model.position = Vector3(1.8, -16.8, -20.4)
	_hide_non_opaque_lum_surfaces(model)
	model.advance_runtime_frame(1.0 / 62.0)
	for row in model.get_surface_materials():
		var material := row as ShaderMaterial
		if material != null and material.shader != null \
				and "/self_lit/" in material.shader.resource_path:
			material.set_shader_parameter("u_diffuse", _solid_texture(Color.WHITE))
			material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
			material.set_shader_parameter("u_alpha_mod", 1.0)

	var terminal: Node3D
	if use_q3:
		terminal = FrameFx.new()
	else:
		terminal = DisplayDecode.new()
	viewport.add_child(terminal)
	if terminal is FrameFx:
		(terminal as FrameFx).advance_frame()
	return {"viewport": viewport, "model": model, "terminal": terminal}


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


func test_world_frame_module_owns_beauty_depth_q3_and_the_terminal_effect() -> void:
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

	# An unregistered beauty surface must never leak into focused Q3.
	var quad := MeshInstance3D.new()
	var mesh := QuadMesh.new()
	mesh.size = Vector2(8.0, 8.0)
	quad.mesh = mesh
	quad.material_override = _canary_material(Color(0.0, 1.0, 0.0))
	quad.position = Vector3(0.0, 0.0, -2.0)
	viewport.add_child(quad)

	var renderer := FrameFx.new()
	viewport.add_child(renderer)
	for _frame in 4:
		await get_tree().process_frame

	var report := renderer.get_backend_report()
	assert_eq(int(report.get("beauty_camera_mask", -1)), 101377,
			"the beauty signature admits the first-person viewmodel layer")
	assert_false(bool(report.get("q3_auxiliary_view", true)))
	assert_false(bool(report.get("q3_camera_mask", true)))
	assert_eq(String(report.get("q3_backend", "")),
			"typed_rendering_device_draw_list")
	assert_eq(String(report.get("q3_source_contract", "")), "Q3FrameCompiler")
	assert_eq(String(report.get("q3_target_ownership", "")),
			"terminal_compositor")
	assert_true(bool(report.get("q3_uses_resolved_beauty_depth", false)))
	assert_eq(String(report.get("q3_depth_compare", "")), "greater_or_equal")
	assert_false(bool(report.get("q3_depth_write", true)))
	assert_false(bool(report.get("q3_sun_depth_test", true)),
			"retail SunGlow is the occlusion-independent Q3 tail")
	assert_eq(int(report.get("q3_working_height", -1)), 256)
	assert_eq(int(report.get("q3_submitted_commands", -1)), 0,
			"unregistered beauty geometry is not a Q3 producer")
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
		report = renderer.get_backend_report()
		assert_eq(String(report.get("status", "")), "drawn_without_q3",
				String(report.get("failure", "FrameFX terminal failed")))
		assert_false(bool(report.get("q3_sampled", true)))
		var center := viewport.get_texture().get_image().get_pixel(32, 16)
		assert_gt(center.g, 0.65,
				"beauty green survives the terminal decode and display encode")
		assert_lt(center.r, 0.05,
				"unregistered beauty geometry cannot become a Q3 source")
		assert_lt(center.b, 0.05)
	else:
		pending("RenderingDevice unavailable under this Godot renderer")

	viewport.remove_child(renderer)
	renderer.free()
	assert_eq(camera.cull_mask, 0x12345,
			"teardown restores the caller's camera mask")
	assert_null(environment.compositor,
			"teardown restores the inherited compositor")


func test_explicit_shutdown_detaches_terminal_effect_and_is_idempotent() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(32, 32)
	viewport.own_world_3d = true
	add_child_autofree(viewport)

	var environment := WorldEnvironment.new()
	environment.environment = Environment.new()
	viewport.add_child(environment)

	var camera := Camera3D.new()
	camera.current = true
	camera.cull_mask = 0x12345
	viewport.add_child(camera)

	var renderer := FrameFx.new()
	viewport.add_child(renderer)
	assert_not_null(environment.compositor)
	assert_eq(camera.cull_mask, 101377)

	renderer.shutdown()
	var report := renderer.get_backend_report()
	assert_true(bool(report.get("shutdown", false)))
	assert_false(bool(report.get("terminal_compositor_installed", true)))
	assert_null(environment.compositor,
			"explicit shutdown restores the inherited compositor")
	assert_eq(camera.cull_mask, 0x12345,
			"explicit shutdown restores the caller's camera mask")

	# The process-exit coordinator and EXIT_TREE fallback can converge here.
	renderer.shutdown()
	renderer.advance_frame()
	assert_null(environment.compositor)
	assert_eq(camera.cull_mask, 0x12345)


func test_display_decode_reentry_recreates_released_terminal_effect() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(32, 32)
	viewport.own_world_3d = true
	add_child_autofree(viewport)

	var environment := WorldEnvironment.new()
	environment.environment = Environment.new()
	viewport.add_child(environment)

	var decoder := DisplayDecode.new()
	viewport.add_child(decoder)
	var first_compositor := environment.compositor
	assert_not_null(first_compositor)
	var first_effect := first_compositor.compositor_effects[0] \
			as FrameFxCompositorEffect
	assert_not_null(first_effect)
	assert_true(first_effect.enabled)

	viewport.remove_child(decoder)
	assert_null(environment.compositor)
	assert_false(first_effect.enabled,
			"leaving the tree disables the detached render callback")

	decoder.request_ready()
	viewport.add_child(decoder)
	var second_compositor := environment.compositor
	assert_not_null(second_compositor)
	var second_effect := second_compositor.compositor_effects[0] \
			as FrameFxCompositorEffect
	assert_not_null(second_effect)
	assert_true(second_effect.enabled)
	assert_ne(second_effect.get_instance_id(), first_effect.get_instance_id(),
			"re-entry uses a fresh effect after the prior device owner shut down")

	viewport.remove_child(decoder)
	assert_null(environment.compositor)
	assert_false(second_effect.enabled)
	decoder.free()


func test_production_water_source_reaches_the_typed_q3_draw_list() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(256, 144)
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
	camera.position = Vector3(100.3, 27.0, -33.7)
	camera.current = true
	viewport.add_child(camera)

	# Water::build is the production typed producer seam. Its live strip must
	# exist before FrameFx snapshots the registry for this frame.
	var water := Water.new()
	water.water_height = 7.0
	viewport.add_child(water)
	water.advance_frame(1.0 / 62.0)
	assert_gt((water.get_mesh_instance().mesh as ArrayMesh).get_surface_count(), 0)

	var renderer := FrameFx.new()
	viewport.add_child(renderer)
	renderer.advance_frame()
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()

	var report := renderer.get_backend_report()
	assert_gt(int(report.get("q3_submitted_commands", 0)), 0,
			"the production Water registry row compiles into Q3")
	if bool(report.get("rd_available", false)):
		assert_true(bool(report.get("q3_sampled", false)),
				"the terminal compositor samples its owned Q3 target")
		assert_gt(int(report.get("q3_drawn_commands", 0)), 0,
				String(report.get("q3_failure", "typed Q3 draw failed")))
		assert_gt(int(report.get("q3_gpu_draw_calls", 0)), 0)
	else:
		pending("RenderingDevice unavailable under this Godot renderer")
	renderer.shutdown()
	water.release_runtime_renderer_resources()


func test_production_object_q3_contributes_pixels_and_obeys_beauty_depth() -> void:
	var focused := _q3_lum_view(true)
	var beauty_only := _q3_lum_view(false)
	for _frame in 6:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()

	var renderer := focused.terminal as FrameFx
	var report := renderer.get_backend_report()
	assert_gt(int(report.get("q3_submitted_commands", 0)), 0,
			"the production ObjectModel LUM source enters typed Q3")
	if not bool(report.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_true(bool(report.get("q3_sampled", false)), "%s" % report)
	assert_gt(int(report.get("q3_drawn_commands", 0)), 0, "%s" % report)
	var focused_image: Image = focused.viewport.get_texture().get_image()
	var beauty_image: Image = beauty_only.viewport.get_texture().get_image()
	var positive_delta := _max_rgb_delta(focused_image, beauty_image)
	var center := Vector2i(64, 48)
	var focused_center := focused_image.get_pixelv(center)
	var beauty_center := beauty_image.get_pixelv(center)
	var center_delta: float = abs(focused_center.r - beauty_center.r) \
			+ abs(focused_center.g - beauty_center.g) \
			+ abs(focused_center.b - beauty_center.b)
	assert_gt(center_delta, 0.02,
			"a known model pixel receives the focused-Q3 contribution: %s" %
			positive_delta)

	# Put the same opaque beauty surface in front of both models. Their beauty
	# remains identical, but the focused renderer must now reject every model
	# fragment against the compositor-provided resolved depth.
	for view in [focused, beauty_only]:
		var occluder := MeshInstance3D.new()
		var quad := QuadMesh.new()
		quad.size = Vector2(8.0, 8.0)
		occluder.mesh = quad
		occluder.material_override = _canary_material(Color(0.12, 0.18, 0.24))
		occluder.position = Vector3(0.0, 0.3, 2.0)
		view.viewport.add_child(occluder)
	renderer.advance_frame()
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	var occluded_focused: Image = focused.viewport.get_texture().get_image()
	var occluded_beauty: Image = beauty_only.viewport.get_texture().get_image()
	var occluded_delta := _max_rgb_delta(occluded_focused, occluded_beauty)
	var occluded_focused_center := occluded_focused.get_pixelv(center)
	var occluded_beauty_center := occluded_beauty.get_pixelv(center)
	var occluded_center_delta: float = \
			abs(occluded_focused_center.r - occluded_beauty_center.r) \
			+ abs(occluded_focused_center.g - occluded_beauty_center.g) \
			+ abs(occluded_focused_center.b - occluded_beauty_center.b)
	assert_lt(occluded_center_delta, 0.01,
			"resolved beauty depth rejects the Q3 fragment behind the known pixel: %s" %
			occluded_delta)


func test_offscreen_production_q3_source_is_culled_before_vertex_packing() -> void:
	var view := _q3_lum_view(true)
	(view.model as ObjectModel).position = Vector3(100000.0, 0.0, 0.0)
	var renderer := view.terminal as FrameFx
	renderer.advance_frame()
	for _frame in 2:
		await get_tree().process_frame
	var report := renderer.get_backend_report()
	assert_gt(int(report.get("q3_registered_sources", 0)), 0)
	assert_gt(int(report.get("q3_frustum_culled_sources", 0)), 0,
			"the offscreen real ObjectModel is rejected at its source AABB")
	assert_eq(int(report.get("q3_packed_vertices", -1)), 0,
			"source rejection precedes surface array packing")
	assert_eq(int(report.get("q3_submitted_commands", -1)), 0)


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
	assert_gt(without_water.r, with_water.r + 0.015,
			"water must attenuate the earlier backdrop; " + diagnostic)
	assert_gt(without_water.b, with_water.b + 0.05,
			"water must attenuate the earlier far particle; " + diagnostic)
	assert_almost_eq(without_water.g, with_water.g, 0.02,
			"water must not attenuate the later camera-side particle; " + diagnostic)
