extends GutTest

const Q3_LUM_3DI := "res://../fixtures/threedi/synth/armory.3di"
const PLACER_ITEMS_DEF := "res://../fixtures/def/items.def"
const PLACER_DEF_ROOT := "res://../fixtures/def"


# Beauty-camera canary. Q3 sources are registered only by production typed
# producers; tests do not get a second renderer or an untyped injection bus.
func _canary_material(beauty: Color) -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = """
shader_type spatial;
render_mode unshaded, depth_draw_opaque, cull_disabled;
uniform vec3 u_beauty;

void fragment() {
	if (CAMERA_VISIBLE_LAYERS == 14126081u) {
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


# The share of the beauty footprint (non-black beauty pixels) that carries a
# non-black Q3 pixel at the same position.
func _q3_footprint_presence(beauty: Image, q3: Image) -> Dictionary:
	var footprint := 0
	var present := 0
	for y in beauty.get_height():
		for x in beauty.get_width():
			var beauty_pixel := beauty.get_pixel(x, y)
			if beauty_pixel.r + beauty_pixel.g + beauty_pixel.b <= 0.05:
				continue
			footprint += 1
			var q3_pixel := q3.get_pixel(x, y)
			if q3_pixel.r + q3_pixel.g + q3_pixel.b > 0.05:
				present += 1
	return {"footprint": footprint, "present": present,
			"fraction": float(present) / float(maxi(footprint, 1))}


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
	camera.cull_mask = 14126081
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
	# The QuadMesh stand-in carries no strip depth replica (CUSTOM0.x), so it
	# keeps its projected depth.
	material.set_shader_parameter("u_scene_depth_range", Vector2.ZERO)
	return material


func _water_order_particle_scene() -> EffectScene:
	# Colors are authored per definition. The removed spawn-time tint is not
	# part of the retail descriptor, so it cannot distinguish these passes.
	var text := ""
	for index in 2:
		var name := "Water order %d" % index
		var color := "0, 0, 255" if index == 0 else "0, 255, 0"
		var properties := "emit_dur = 0.1;\nemit_rate = 20;\nemit_burst = 1;\nage = 2;\nalpha = 1;\nscale = 2;\n"
		for corner in range(1, 5):
			properties += "color%d = %s;\n" % [corner, color]
		properties += "graphic1 = water_order_fallback.tga, blend;\ng1_alpha = 0.5;\ng1_scale = 2;"
		text += ParticleFixture.definition(name, properties) + ParticleFixture.effect(name, [name])
	var file := ParticleFixture.parse(self, text)

	var scene := EffectScene.new()
	scene.open([file])
	for index in 2:
		var transform := Transform3D.IDENTITY
		transform.origin = Vector3(0.0, -0.05 if index == 0 else 0.05, 2.0)
		var request := EffectSpawnRequest.make(scene.intern("Water order %d" % index), transform)
		assert_eq(scene.spawn(request).status, EffectScene.SPAWN_STATUS_SPAWNED)
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
	assert_eq(int(report.get("beauty_camera_mask", -1)), 14126081,
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
	assert_true(bool(report.get("q3_sun_depth_test", false)),
			"the bloom pass z-tests the sun glow through the far-band viewport")
	var far_band: Vector2 = report.get("q3_far_band", Vector2.ZERO)
	assert_almost_eq(far_band.x, 0.98, 0.000001,
			"Render_SetViewportFarDepth MinZ")
	assert_almost_eq(far_band.y, 0.99996948, 0.000001,
			"Render_SetViewportFarDepth MaxZ")
	assert_eq(int(report.get("q3_working_height", -1)), 256)
	assert_eq(int(report.get("q3_submitted_commands", -1)), 0,
			"unregistered beauty geometry is not a Q3 producer")
	assert_true(bool(report.get("terminal_compositor_installed", false)))
	assert_eq(camera.cull_mask, 14126081,
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
	assert_eq(camera.cull_mask, 14126081)

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


# A flat gamma-domain grey behind a DisplayDecode, at the given 3D MSAA.
func _decoded_grey_view(msaa: Viewport.MSAA) -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(32, 32)
	viewport.own_world_3d = true
	viewport.msaa_3d = msaa
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)

	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_COLOR
	environment_resource.background_color = Color.BLACK
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	var environment := WorldEnvironment.new()
	environment.environment = environment_resource
	viewport.add_child(environment)

	var camera := Camera3D.new()
	camera.current = true
	viewport.add_child(camera)

	var quad := MeshInstance3D.new()
	var mesh := QuadMesh.new()
	mesh.size = Vector2(8.0, 8.0)
	quad.mesh = mesh
	var shader := Shader.new()
	shader.code = """
shader_type spatial;
render_mode unshaded, cull_disabled;
void fragment() {
	ALBEDO = vec3(0.5);
}
"""
	var material := ShaderMaterial.new()
	material.shader = shader
	quad.material_override = material
	quad.position = Vector3(0.0, 0.0, -2.0)
	viewport.add_child(quad)

	viewport.add_child(DisplayDecode.new())
	var effect := environment.compositor.compositor_effects[0] \
			as FrameFxCompositorEffect
	return {"viewport": viewport, "effect": effect}


# The menu avatar preview renders its SubViewport at 4x MSAA. There the
# renderer's resolved scene depth is a sampled copy with no depth-attachment
# usage, so the decode-only terminal (no Q3 source) must never bind it as a
# framebuffer attachment: binding it failed the whole target chain every frame
# with a RenderingDevice error and skipped the decode, double-encoding the
# preview. The MSAA view decodes exactly like a single-sample one.
func test_display_decode_decodes_a_multisampled_view() -> void:
	var single := _decoded_grey_view(Viewport.MSAA_DISABLED)
	var multi := _decoded_grey_view(Viewport.MSAA_4X)
	for _frame in 4:
		await get_tree().process_frame
	var multi_effect := multi["effect"] as FrameFxCompositorEffect
	var report := multi_effect.get_backend_report()
	if not bool(report.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_eq(String(report.get("status", "")), "drawn_without_q3",
			String(report.get("failure", "the MSAA terminal failed")))
	assert_gt(int(report.get("rendered_frames", 0)), 0)
	var single_center := (single["viewport"] as SubViewport).get_texture() \
			.get_image().get_pixel(16, 16)
	var multi_center := (multi["viewport"] as SubViewport).get_texture() \
			.get_image().get_pixel(16, 16)
	assert_almost_eq(multi_center.g, single_center.g, 0.02,
			"the MSAA preview gets the same one terminal decode")


func test_framefx_reentry_recreates_released_terminal_effect() -> void:
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
	var first_compositor := environment.compositor
	assert_not_null(first_compositor)
	var first_effect := first_compositor.compositor_effects[0] \
			as FrameFxCompositorEffect
	assert_not_null(first_effect)
	assert_true(first_effect.enabled)
	assert_eq(camera.cull_mask, 14126081)

	viewport.remove_child(renderer)
	assert_null(environment.compositor,
			"leaving the tree restores the inherited compositor")
	assert_false(first_effect.enabled,
			"leaving the tree disables the detached render callback")
	assert_eq(camera.cull_mask, 0x12345)
	assert_true(bool(renderer.get_backend_report().get("shutdown", false)))

	# A plain re-add (no request_ready from the caller) must revive the node.
	viewport.add_child(renderer)
	var second_compositor := environment.compositor
	assert_not_null(second_compositor,
			"re-entry reinstalls the terminal compositor")
	var second_effect := second_compositor.compositor_effects[0] \
			as FrameFxCompositorEffect
	assert_not_null(second_effect)
	assert_true(second_effect.enabled)
	assert_ne(second_effect.get_instance_id(), first_effect.get_instance_id(),
			"re-entry uses a fresh effect after the prior device owner shut down")
	assert_false(bool(renderer.get_backend_report().get("shutdown", true)),
			"re-entry clears the shutdown latch")
	assert_eq(camera.cull_mask, 14126081,
			"re-entry re-applies the beauty camera signature")

	viewport.remove_child(renderer)
	assert_null(environment.compositor)
	assert_false(second_effect.enabled)
	assert_eq(camera.cull_mask, 0x12345)
	renderer.free()


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
	# Water publishes the strip arrays it uploads, so the focused Q3 cache
	# re-packs exactly that one entry per rebuilt frame from CPU memory and
	# never reads the surface back through the server.
	for _rebuild in 2:
		water.advance_frame(1.0 / 62.0)
		renderer.advance_frame()
		var rebuilt := renderer.get_backend_report()
		assert_eq(int(rebuilt.get("q3_repacked_entries", -1)), 1,
				"a rebuilt strip re-packs exactly its own entry")
		assert_eq(int(rebuilt.get("q3_readbacks_this_frame", -1)), 0,
				"the published strip is never read back through the server")
		assert_gt(int(rebuilt.get("q3_packed_vertices", 0)), 0)
	renderer.advance_frame()
	var stable := renderer.get_backend_report()
	assert_eq(int(stable.get("q3_repacked_entries", -1)), 0,
			"an unchanged strip is served from the cache")
	assert_eq(int(stable.get("q3_readbacks_this_frame", -1)), 0)
	assert_eq(int(stable.get("q3_packed_vertices", -1)), 0)
	assert_gt(int(stable.get("q3_submitted_commands", 0)), 0)
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
	# The opaque LUM bulb re-tests its OWN beauty depth in Q3. Count how much
	# of its beauty footprint survives into the Q3 target: an unpulled copy
	# loses a random ulp-level subset of its fragments (speckle), so the pin
	# is a footprint fraction, never one centre pixel.
	var beauty_image: Image = beauty_only.viewport.get_texture().get_image()
	var q3_image: Image = renderer.get_q3_target_image()
	assert_not_null(q3_image, "the terminal effect exposes its Q3 target")
	if q3_image == null:
		return
	assert_eq(q3_image.get_size(), beauty_image.get_size())
	var footprint := _q3_footprint_presence(beauty_image, q3_image)
	assert_gt(int(footprint.footprint), 50,
			"the bulb covers a measurable beauty footprint")
	assert_gt(float(footprint.fraction), 0.9,
			"the LUM copy passes its own beauty depth across its footprint: %s" %
			footprint)
	# The bulb's Q3 copy is the SELFLUM block of a white Diffuse1 under the
	# noon default gain (1, 1, 1): min(1, 1 x 1 x 2) = white. FrameFX then
	# composites the blurred capture at the witnessed SRCALPHA/ONE alpha 0.5,
	# so over a black beauty pixel the composite adds at most 0.5 per channel
	# (1.5 summed) and, for a bulb this size, at least the old per-channel
	# 0.1 pin (0.3 summed) somewhere inside the kernel's reach.
	var focused_image: Image = focused.viewport.get_texture().get_image()
	var composite_delta := _max_rgb_delta(focused_image, beauty_image)
	assert_gt(float(composite_delta.delta), 0.3,
			"the half-strength composite adds the bulb's glow over beauty: %s" %
			composite_delta)
	assert_lt(float(composite_delta.delta), 1.5 + 3.0 / 255.0,
			"the composite never exceeds alpha 0.5 of a saturated white blur: %s" %
			composite_delta)

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
	assert_lt(float(occluded_delta.delta), 0.01,
			"resolved beauty depth rejects every Q3 fragment behind the occluder: %s" %
			occluded_delta)
	var occluded_q3: Image = renderer.get_q3_target_image()
	assert_not_null(occluded_q3)
	if occluded_q3 != null:
		var occluded_presence := _q3_footprint_presence(beauty_image, occluded_q3)
		assert_lt(float(occluded_presence.fraction), 0.05,
				"the occluded bulb's former footprint is empty in the Q3 target: %s" %
				occluded_presence)


func test_stable_q3_scene_packs_once_and_never_reads_the_server_back() -> void:
	# The view compiles twice: the READY compile read each LUM surface once
	# into the retained cache, the second compile of the unchanged scene must
	# have been served entirely from it.
	var view := _q3_lum_view(true)
	var renderer := view.terminal as FrameFx
	var first := renderer.get_backend_report()
	assert_gt(int(first.get("q3_submitted_commands", 0)), 0)
	assert_gt(int(first.get("q3_cached_entries", 0)), 0,
			"first sight packed each surface into the cache")
	assert_gt(int(first.get("q3_cached_vertex_bytes", 0)), 0)
	assert_eq(int(first.get("q3_readbacks_this_frame", -1)), 0,
			"a stable frame reads nothing back through the server")
	assert_eq(int(first.get("q3_packed_vertices", -1)), 0,
			"a stable frame re-packs nothing")
	assert_eq(int(first.get("q3_repacked_entries", -1)), 0)
	assert_eq(String(first.get("q3_geometry_submission", "")),
			"cached_per_source_surface_streams")

	renderer.advance_frame()
	var second := renderer.get_backend_report()
	assert_eq(int(second.get("q3_submitted_commands", -1)),
			int(first.get("q3_submitted_commands", 0)))
	assert_eq(int(second.get("q3_readbacks_this_frame", -1)), 0)
	assert_eq(int(second.get("q3_packed_vertices", -1)), 0)
	assert_eq(int(second.get("q3_repacked_entries", -1)), 0)
	assert_eq(int(second.get("q3_cached_entries", -1)),
			int(first.get("q3_cached_entries", 0)))

	# An invalidated source (a rebuilt mesh, carved MultiMesh rows) re-packs
	# its surfaces once at its next sight, from the mesh's retained arrays
	# (RetainedArrayMesh: no server readback), then settles again.
	var bulb := _first_visible_mesh(view.model)
	assert_not_null(bulb)
	if bulb == null:
		return
	FrameFx.invalidate_q3_source(bulb)
	renderer.advance_frame()
	var third := renderer.get_backend_report()
	assert_eq(int(third.get("q3_readbacks_this_frame", -1)), 0,
			"invalidation re-reads the retained arrays, never the server")
	assert_gt(int(third.get("q3_packed_vertices", 0)), 0,
			"invalidation re-packs the source once")
	renderer.advance_frame()
	var fourth := renderer.get_backend_report()
	assert_eq(int(fourth.get("q3_readbacks_this_frame", -1)), 0)
	assert_eq(int(fourth.get("q3_packed_vertices", -1)), 0)


func test_pruned_q3_source_frees_its_device_buffer_on_an_empty_frame() -> void:
	# Evictions are handed to the render side through the published frame.
	# Once the last glow source is gone that frame has no commands; its
	# evicted entries must still be consumed (their RD vertex buffers freed)
	# instead of waiting for the next non-empty frame.
	var view := _q3_lum_view(true)
	var renderer := view.terminal as FrameFx
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	var drawn := renderer.get_backend_report()
	if not bool(drawn.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_gt(int(drawn.get("q3_drawn_commands", 0)), 0,
			String(drawn.get("q3_failure", "Q3 setup draw failed")))
	assert_gt(int(drawn.get("q3_device_buffers", 0)), 0,
			"the drawn LUM surfaces hold device buffers: %s" % drawn)

	var model := view.model as Node
	(view.viewport as Node).remove_child(model)
	model.free()
	renderer.advance_frame()
	var pruned := renderer.get_backend_report()
	assert_eq(int(pruned.get("q3_submitted_commands", -1)), 0,
			"the pruned source leaves an empty frame")
	assert_eq(int(pruned.get("q3_cached_entries", -1)), 0,
			"the cache evicted the pruned source's entries")
	for _frame in 3:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	var consumed := renderer.get_backend_report()
	assert_eq(int(consumed.get("q3_device_buffers", -1)), 0,
			"the empty frame's render frees the evicted device buffers: %s" %
			consumed)
	renderer.shutdown()


func test_static_row_rewrite_rereads_instance_rows_without_a_readback() -> void:
	# A static RLOD switch or destruction carve rewrites MultiMesh rows only;
	# the population's mesh never changes. The placer therefore invalidates
	# the population's INSTANCES: the cache re-reads its rows once and keeps
	# the packed surfaces, so no server readback and no re-pack follow.
	var viewport := SubViewport.new()
	viewport.size = Vector2i(96, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.current = true
	viewport.add_child(camera)

	# The production LUM material and mesh: the armory fixture's opaque bulb,
	# harvested from a template ObjectModel the way the placer harvests.
	var template := ObjectModel.new()
	viewport.add_child(template)
	template.set_process(false)
	template.set_object_data(_lum_object_data())
	_hide_non_opaque_lum_surfaces(template)
	var bulb := _first_visible_mesh(template)
	assert_not_null(bulb, "the armory fixture carries an opaque LUM bulb")
	if bulb == null:
		return
	var bulb_mesh: Mesh = bulb.mesh
	var bulb_material: Material = bulb.get_active_material(0)
	viewport.remove_child(template)
	template.free()

	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var record := mission.add_entity(
			MissionData.KIND_BUILDING, 105004, Vector3.ZERO, Vector3.ZERO)
	assert_not_null(record)
	# A second bulb in the same 512-unit bin keeps the population live after
	# the carve below: the dense populations hide an emptied level, and a
	# hidden population is (rightly) never compiled or re-read.
	assert_not_null(mission.add_entity(
			MissionData.KIND_BUILDING, 105004, Vector3(2.0, 0.0, 0.0),
			Vector3.ZERO))
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(PLACER_ITEMS_DEF)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path(PLACER_DEF_ROOT))
	var placer := MissionObjectPlacer.create(root, item_db)
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", ObjectData.new(), [{
				"mesh": bulb_mesh, "material": bulb_material,
				"offset": Transform3D.IDENTITY, "submesh": 0,
			}]))
	var parent := Node3D.new()
	viewport.add_child(parent)
	var stats := placer.place(mission, parent)
	assert_eq(stats.batched, 2,
			"the bulb population is one static batch over two rows: %s" % stats.to_json_value())
	var population := parent.get_node_or_null(
			"MissionObjects/StaticPopulations/Batch_StaticCrate1_0") as MultiMeshInstance3D
	assert_not_null(population, "the population is emitted")
	if population == null:
		return
	# Look at the bulb from a few units away so both the population's source
	# bounds and its one instance pass the frustum test under every renderer
	# (headless Godot keeps no MultiMesh row data and reads rows as identity).
	var center: Vector3 = population.global_transform * bulb_mesh.get_aabb().get_center()
	camera.position = center + Vector3(0.0, 0.0, 8.0)
	camera.look_at(center, Vector3.UP)

	# The READY compile is the population's first sight.
	var renderer := FrameFx.new()
	viewport.add_child(renderer)
	var first := renderer.get_backend_report()
	assert_gt(int(first.get("q3_submitted_commands", 0)), 0,
			"the static population is a typed Q3 source: %s" % first)
	assert_eq(int(first.get("q3_instance_row_reads_this_frame", -1)), 1,
			"first sight reads the population's rows once")
	assert_eq(int(first.get("q3_readbacks_this_frame", -1)), 0,
			"first sight packs the model's retained arrays, no server readback")
	renderer.advance_frame()
	var stable := renderer.get_backend_report()
	assert_eq(int(stable.get("q3_readbacks_this_frame", -1)), 0)
	assert_eq(int(stable.get("q3_packed_vertices", -1)), 0)
	assert_eq(int(stable.get("q3_instance_row_reads_this_frame", -1)), 0,
			"a stable frame re-reads no rows")
	var cached_entries := int(stable.get("q3_cached_entries", 0))
	assert_gt(cached_entries, 0)

	# The destruction carve rewrites the population's rows through the
	# production path (the RLOD switch shares _write_static_instance_slots).
	var bms_id := record.bms_id
	assert_true(placer.hide_static_instance(bms_id) is Transform3D)
	renderer.advance_frame()
	var carved := renderer.get_backend_report()
	assert_eq(int(carved.get("q3_instance_row_reads_this_frame", -1)), 1,
			"the rewritten rows are re-read once")
	assert_eq(int(carved.get("q3_readbacks_this_frame", -1)), 0,
			"a row rewrite never reads the population's mesh back: %s" % carved)
	assert_eq(int(carved.get("q3_packed_vertices", -1)), 0,
			"a row rewrite never re-packs the population's surfaces")
	assert_eq(int(carved.get("q3_repacked_entries", -1)), 0)
	assert_eq(int(carved.get("q3_cached_entries", -1)), cached_entries,
			"the packed surfaces stay cached across the rewrite")
	renderer.advance_frame()
	var settled := renderer.get_backend_report()
	assert_eq(int(settled.get("q3_instance_row_reads_this_frame", -1)), 0)
	assert_eq(int(settled.get("q3_readbacks_this_frame", -1)), 0)
	assert_eq(int(settled.get("q3_packed_vertices", -1)), 0)
	renderer.shutdown()


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


func test_stable_q3_frame_touches_no_records_beyond_the_frustum_test() -> void:
	# Every registered source is a persistent record: a stable frame walks
	# the live records, probes their transforms and refreshes nothing; a moved
	# or hidden source is picked up through its record alone, never a walk
	# that re-reads every source.
	var view := _q3_lum_view(true)
	var renderer := view.terminal as FrameFx
	renderer.advance_frame()
	var stable := renderer.get_backend_report()
	assert_gt(int(stable.get("q3_records", 0)), 0,
			"the LUM model's sources are live records: %s" % stable)
	assert_eq(int(stable.get("q3_records_touched_this_frame", -1)), 0,
			"a stable frame refreshes no record: %s" % stable)
	assert_eq(int(stable.get("q3_material_reads_this_frame", -1)), 0,
			"a stable frame reads no material block")
	var stable_commands := int(stable.get("q3_submitted_commands", 0))
	assert_gt(stable_commands, 0)

	var bulb := _first_visible_mesh(view.model)
	assert_not_null(bulb)
	if bulb == null:
		return
	# A move refreshes the bounds of the moved model's visible sources only;
	# its hidden LOD/material sources and every other record stay untouched.
	(view.model as ObjectModel).position += Vector3(0.5, 0.0, 0.0)
	renderer.advance_frame()
	var moved := renderer.get_backend_report()
	var moved_touched := int(moved.get("q3_records_touched_this_frame", 0))
	assert_gt(moved_touched, 0, "the move is picked up: %s" % moved)
	assert_lte(moved_touched, int(moved.get("q3_submitted_commands", 0)) +
			int(moved.get("q3_frustum_culled_sources", 0)),
			"only the visible sources refresh: %s" % moved)
	assert_eq(int(moved.get("q3_submitted_commands", -1)), stable_commands)
	renderer.advance_frame()
	var settled := renderer.get_backend_report()
	assert_eq(int(settled.get("q3_records_touched_this_frame", -1)), 0,
			"the moved model settles: %s" % settled)

	# A visibility change reaches its record through the node's signal.
	bulb.visible = false
	renderer.advance_frame()
	var hidden := renderer.get_backend_report()
	assert_eq(int(hidden.get("q3_records_touched_this_frame", -1)), 1,
			"the hidden source alone is refreshed: %s" % hidden)
	assert_lt(int(hidden.get("q3_submitted_commands", 0)), stable_commands,
			"and its copy leaves the draw list")
	bulb.visible = true
	renderer.advance_frame()
	var shown := renderer.get_backend_report()
	assert_eq(int(shown.get("q3_records_touched_this_frame", -1)), 1,
			"the shown source alone is refreshed: %s" % shown)
	assert_eq(int(shown.get("q3_submitted_commands", -1)), stable_commands,
			"and its copy is back")
	assert_eq(int(shown.get("q3_readbacks_this_frame", -1)), 0,
			"visibility never re-reads geometry")


func test_object_material_parameter_write_reaches_q3_through_its_invalidation() -> void:
	# An object surface's material block (u_diffuse, u_rgb_mod, ...) is read
	# once into its record; a stable frame reads no material. ObjectModel's
	# runtime writes name the material (FrameFx.invalidate_q3_object_material),
	# and the surfaces on it re-read the block exactly at their next sight.
	var view := _q3_lum_view(true)
	var renderer := view.terminal as FrameFx
	var bulb := _first_visible_mesh(view.model)
	assert_not_null(bulb)
	if bulb == null:
		return
	var camera := (view.viewport as SubViewport).get_camera_3d()
	var pixel := Vector2i(camera.unproject_position(
			bulb.global_transform * bulb.get_aabb().get_center()))
	var bright_image: Image = await _render_q3_frame(renderer)
	var stable := renderer.get_backend_report()
	assert_eq(int(stable.get("q3_material_reads_this_frame", -1)), 0,
			"a stable frame reads no material block: %s" % stable)

	var material := bulb.get_active_material(0) as ShaderMaterial
	assert_not_null(material)
	if material == null:
		return
	material.set_shader_parameter("u_rgb_mod", Vector3(0.25, 0.25, 0.25))
	renderer.advance_frame()
	var unnamed := renderer.get_backend_report()
	assert_eq(int(unnamed.get("q3_material_reads_this_frame", -1)), 0,
			"a write that names no material is not re-read per frame")
	FrameFx.invalidate_q3_object_material(material)
	var dim_image: Image = await _render_q3_frame(renderer)
	var named := renderer.get_backend_report()
	assert_gte(int(named.get("q3_material_reads_this_frame", 0)), 1,
			"the named material is re-read at its next sight: %s" % named)
	assert_eq(int(named.get("q3_records_touched_this_frame", -1)), 0,
			"a parameter write touches no scene state")
	assert_eq(int(named.get("q3_readbacks_this_frame", -1)), 0,
			"and re-reads no geometry")
	renderer.advance_frame()
	var settled := renderer.get_backend_report()
	assert_eq(int(settled.get("q3_material_reads_this_frame", -1)), 0)
	if not bool(settled.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_not_null(bright_image)
	assert_not_null(dim_image)
	if bright_image == null or dim_image == null:
		return
	# The SELFLUM copy is Diffuse1 x u_rgb_mod x min(gain, 1) x 2: the
	# re-read block dims the white bulb's copy from saturated to 0.5 grey.
	var bright_peak := _q3_peak_near(bright_image, pixel, 3)
	var dim_peak := _q3_peak_near(dim_image, pixel, 3)
	assert_gt(bright_peak, 2.5,
			"the white bulb's copy saturates at %s: %f" % [pixel, bright_peak])
	assert_between(dim_peak, 1.2, 1.8,
			"the re-read u_rgb_mod 0.25 dims the copy to 0.5 grey: %f" % dim_peak)
	renderer.shutdown()


func test_cleared_water_strip_leaves_the_q3_draw_list() -> void:
	# The water strip is a per-frame producer: its record follows every
	# publication, and a frame whose march yields no strip clears the surface
	# AND names the source, so the last published strip is never drawn again.
	var viewport := SubViewport.new()
	viewport.size = Vector2i(256, 144)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(100.3, 27.0, -33.7)
	camera.current = true
	viewport.add_child(camera)
	var water := Water.new()
	water.water_height = 7.0
	viewport.add_child(water)
	water.advance_frame(1.0 / 62.0)
	assert_gt((water.get_mesh_instance().mesh as ArrayMesh).get_surface_count(), 0)
	var renderer := FrameFx.new()
	viewport.add_child(renderer)
	renderer.advance_frame()
	var drawn := renderer.get_backend_report()
	assert_gt(int(drawn.get("q3_submitted_commands", 0)), 0,
			"the strip compiles into Q3: %s" % drawn)

	# The g_WaterActive gate (every tracked visible terrain sector above the
	# water height) clears the beauty strip, but the bloom pass's nightvision
	# redraw is not gated on it (retail FrameFX_RenderGlowSource @ 0x582a59..
	# 0x582a5d calls render_water_surface(0, 1) unconditionally): its strip
	# stays in the Q3 draw list.
	water.set_visible_terrain_bounds(true, 100.0, 200.0)
	water.advance_frame(1.0 / 62.0)
	assert_eq((water.get_mesh_instance().mesh as ArrayMesh).get_surface_count(), 0,
			"the inactive water pass clears the beauty strip")
	renderer.advance_frame()
	var ungated := renderer.get_backend_report()
	assert_gt(int(ungated.get("q3_submitted_commands", 0)), 0,
			"the nightvision redraw ignores g_WaterActive: %s" % ungated)

	# Below the plane the redraw has no side: the view-0 call requires the
	# camera strictly above the water (retail render_water_surface @ 0x5c3304).
	# Its strip clears and names the source, so the last publication is never
	# drawn again.
	camera.position = Vector3(100.3, 5.0, -33.7)
	water.advance_frame(1.0 / 62.0)
	renderer.advance_frame()
	var cleared := renderer.get_backend_report()
	assert_eq(int(cleared.get("q3_submitted_commands", -1)), 0,
			"the cleared strip is not drawn from its last publication: %s" % cleared)
	assert_eq(int(cleared.get("q3_readbacks_this_frame", -1)), 0)

	# The camera comes back above: the rebuilt strip is published and drawn
	# again, from memory.
	camera.position = Vector3(100.3, 27.0, -33.7)
	water.set_visible_terrain_bounds(false, 0.0, 0.0)
	water.advance_frame(1.0 / 62.0)
	renderer.advance_frame()
	var restored := renderer.get_backend_report()
	assert_gt(int(restored.get("q3_submitted_commands", 0)), 0,
			"the strip is back: %s" % restored)
	assert_eq(int(restored.get("q3_readbacks_this_frame", -1)), 0,
			"the published strip is never read back through the server")
	renderer.shutdown()
	water.release_runtime_renderer_resources()


class ExitTreeWaterReleaser extends Node3D:
	# The shell's exit-tree path: GameWorld releases the water's renderer
	# resources (the strip MeshInstance3D is freed) while the shell subtree is
	# leaving the tree. A node freed there never emits tree_exited.
	var water: Water = null

	func _exit_tree() -> void:
		if water != null:
			water.release_runtime_renderer_resources()


func test_q3_source_freed_inside_an_ancestor_exit_tree_leaves_the_records() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(256, 144)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(100.3, 27.0, -33.7)
	camera.current = true
	viewport.add_child(camera)
	var holder := ExitTreeWaterReleaser.new()
	viewport.add_child(holder)
	var water := Water.new()
	water.water_height = 7.0
	holder.add_child(water)
	holder.water = water
	water.advance_frame(1.0 / 62.0)
	var renderer := FrameFx.new()
	viewport.add_child(renderer)
	renderer.advance_frame()
	var drawn := renderer.get_backend_report()
	var live_records := int(drawn.get("q3_records", 0))
	assert_gt(int(drawn.get("q3_submitted_commands", 0)), 0,
			"the strip compiles into Q3: %s" % drawn)
	assert_gt(live_records, 0)

	# The holder leaves the tree; its _exit_tree frees the registered strip
	# node after the strip's own exit-tree notification and before any
	# tree_exited signal could reach the registry.
	viewport.remove_child(holder)
	assert_null(water.get_mesh_instance(),
			"the strip node is freed inside the ancestor's exit-tree handler")
	renderer.advance_frame()
	var after := renderer.get_backend_report()
	assert_eq(int(after.get("q3_records", -1)), live_records - 1,
			"the freed strip's record is reaped, never walked: %s" % after)
	assert_eq(int(after.get("q3_submitted_commands", -1)), 0,
			"nothing draws from the freed source")
	holder.free()
	renderer.shutdown()


func test_q3_viewport_resize_retires_invalidated_uniform_sets_cleanly() -> void:
	var view := _q3_lum_view(true)
	var renderer := view.terminal as FrameFx
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	var report := renderer.get_backend_report()
	if not bool(report.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_gt(int(report.get("q3_drawn_commands", 0)), 0,
			String(report.get("q3_failure", "Q3 setup draw failed")))

	# Resizing replaces the compositor-owned beauty snapshot. Godot invalidates
	# uniform sets that sampled the prior texture; cleanup must query their typed
	# validity instead of sending those stale RIDs through free_rid again.
	(view.viewport as SubViewport).size = Vector2i(160, 90)
	renderer.advance_frame()
	for _frame in 3:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	assert_engine_error_count(0,
			"Q3 resize cleanup skips uniform sets Godot already invalidated")
	renderer.shutdown()


# A 64x64 beauty view over an opaque red backdrop (drawn by the beauty pass
# itself, nothing in Q3), the camera 5 u in front of it.
func _particle_order_view() -> SubViewport:
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

	var backdrop := MeshInstance3D.new()
	var backdrop_mesh := QuadMesh.new()
	backdrop_mesh.size = Vector2(20.0, 20.0)
	backdrop.mesh = backdrop_mesh
	backdrop.material_override = _canary_material(Color(1.0, 0.0, 0.0))
	backdrop.position = Vector3(0.0, 0.0, 0.0)
	viewport.add_child(backdrop)
	return viewport


# The two water-order particles (far-side blue, camera-side green) over the
# water plane at height 0, rendered once.
func _particle_order_renderer(viewport: SubViewport) -> ParticleRenderer:
	var frame_renderer := FrameFx.new()
	viewport.add_child(frame_renderer)
	var particle_renderer := ParticleRenderer.new()
	particle_renderer.scene = _water_order_particle_scene()
	particle_renderer.procedural_fallback_enabled = true
	particle_renderer.set_water_plane(0.0, null)
	viewport.add_child(particle_renderer)
	particle_renderer.render_now(GameWorld.current_frame_clock_ms())
	return particle_renderer


func _particle_far_runs(particle_renderer: ParticleRenderer) -> Array[MeshInstance3D]:
	var runs: Array[MeshInstance3D] = []
	for child in particle_renderer.get_children():
		if child is MeshInstance3D and (child as MeshInstance3D).visible \
				and String(child.name).begins_with("ParticleFarRun"):
			runs.append(child as MeshInstance3D)
	return runs


func test_far_particles_water_and_camera_particles_reach_the_frame_in_retail_order() -> void:
	var viewport := _particle_order_view()
	var water := MeshInstance3D.new()
	var water_mesh := QuadMesh.new()
	water_mesh.size = Vector2(20.0, 20.0)
	water.mesh = water_mesh
	water.material_override = _controlled_water_material()
	water.layers = Water.VISUAL_LAYER_WATER
	water.position = Vector3(0.0, 0.0, 1.0)
	water.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	viewport.add_child(water)

	var particle_renderer := _particle_order_renderer(viewport)
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
	# Pass A draws inside the transparent list (retail
	# Terrain_RenderWorldScene @ 0x5c95b5, before the water pass
	# @ 0x5c95dc): render-list runs at the particle far-side rung.
	assert_gt(int(report.get("world_far_render_runs", 0)), 0,
			"pass A draws as render-list runs")
	var runs := _particle_far_runs(particle_renderer)
	assert_gt(runs.size(), 0, "the far runs are visible instances")
	for run in runs:
		var material := run.mesh.surface_get_material(0) as ShaderMaterial
		assert_eq(material.render_priority, ObjectShaderCache.RENDER_RUNG_PARTICLE_FAR_SIDE,
				"a far run sorts at the particle far-side rung")
	assert_lt(ObjectShaderCache.RENDER_RUNG_PARTICLE_FAR_SIDE, ObjectShaderCache.RENDER_RUNG_WATER)
	var camera_backend: Dictionary = report.get("world_camera_backend", {})
	if not bool(camera_backend.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_eq(String(camera_backend.get("status", "")), "drawn",
			String(camera_backend.get("failure", "camera particle pass failed")))
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
	# source-over (the particle far-side rung) -> water x0.5 (the water rung)
	# -> near green source-over (POST_TRANSPARENT). Compare the same immutable
	# particle packet with water hidden: red and blue must rise, while green
	# must remain unchanged because its pass follows water.
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


func test_far_side_alpha_objects_draw_before_particle_pass_a() -> void:
	# Retail draws the far-side object ALPHA strips and tracers before pass A
	# (Terrain_RenderWorldScene @ 0x5c95ac..0x5c95b5), so a
	# half-transparent far-side card lies UNDER the far blue particle. Over the
	# red backdrop a magenta card at alpha 0.5 then a blue particle at alpha a
	# leave blue - red = 1.5a - 0.5 (0.25 at the particle's 0.5 peak); the
	# particle drawn first leaves blue - red = a - 0.5, never positive.
	var viewport := _particle_order_view()
	var card_shader := Shader.new()
	card_shader.code = """
shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled, fog_disabled;

void fragment() {
	ALBEDO = vec3(1.0, 0.0, 1.0);
	ALPHA = 0.5;
}
"""
	var card_material := ShaderMaterial.new()
	card_material.shader = card_shader
	card_material.render_priority = ObjectShaderCache.RENDER_RUNG_ALPHA_FAR_SIDE
	var card := MeshInstance3D.new()
	var card_mesh := QuadMesh.new()
	card_mesh.size = Vector2(20.0, 20.0)
	card.mesh = card_mesh
	card.material_override = card_material
	# Nearer than the particles: the rung, not the depth, orders the two.
	card.position = Vector3(0.0, 0.0, 3.0)
	viewport.add_child(card)

	var particle_renderer := _particle_order_renderer(viewport)
	var far_runs := int(particle_renderer.get_debug_draw_list_report().get(
			"world_far_render_runs", 0))
	assert_gt(far_runs, 0, "pass A drew render-list runs")
	for _frame in 6:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	# The camera-side green particle composites on top of both and scales the
	# difference by its own 1 - alpha; take the best row through the centre.
	var image := viewport.get_texture().get_image()
	var best := -1.0
	var best_pixel := Color.BLACK
	for y in range(24, 44):
		var pixel := image.get_pixel(32, y)
		if pixel.b - pixel.r > best:
			best = pixel.b - pixel.r
			best_pixel = pixel
	assert_gt(best, 0.05,
			"the far particle lands on top of the far-side alpha card: %s" % best_pixel)


const Q3_ADDITIVE_LUM_3DI := "res://../fixtures/threedi/synth/mount.3di"
# The same heat slab authored FF_ST_AB_LUM (minimal_3di_gen mount_mtrl2_ab_lum).
const Q3_ALPHA_LUM_3DI := "res://../fixtures/threedi/synth/mount_mtrl2_ab_lum.3di"
# The per-vertex skinned person wearing FF_ST_AD_LUM (person_mtrl0_ad_lum).
const Q3_SKINNED_LUM_3DI := "res://../fixtures/threedi/synth/person_mtrl0_ad_lum.3di"


func _keep_only_shader_surfaces(root: Node, shader_fragment: String) -> void:
	if root is MeshInstance3D:
		var mesh_instance := root as MeshInstance3D
		var material := mesh_instance.get_active_material(0) as ShaderMaterial
		var shader_path := material.shader.resource_path 				if material != null and material.shader != null else ""
		mesh_instance.visible = shader_fragment in shader_path
	for child in root.get_children():
		_keep_only_shader_surfaces(child, shader_fragment)


func _first_visible_mesh(root: Node) -> MeshInstance3D:
	if root is MeshInstance3D and (root as MeshInstance3D).visible:
		return root as MeshInstance3D
	for child in root.get_children():
		var found := _first_visible_mesh(child)
		if found != null:
			return found
	return null


func _q3_peak_near(image: Image, center: Vector2i, radius: int) -> float:
	var best := 0.0
	for y in range(center.y - radius, center.y + radius + 1):
		for x in range(center.x - radius, center.x + radius + 1):
			if x < 0 or y < 0 or x >= image.get_width() or y >= image.get_height():
				continue
			var pixel := image.get_pixel(x, y)
			best = maxf(best, pixel.r + pixel.g + pixel.b)
	return best


func _render_q3_frame(renderer: FrameFx) -> Image:
	renderer.advance_frame()
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return renderer.get_q3_target_image()


func test_water_nv_redraw_keeps_additive_lum_copies_weighted_by_its_alpha() -> void:
	# The NV water redraw follows the object copies in the fixed FrameFX
	# bracket with src ONE / dst SRC_ALPHA, so an additive LUM card in front
	# of murky water keeps dst x (noiseA x diffuseA x 2) of its Q3 copy: a
	# larger NV alpha retains MORE of the card. An inverted write (1 - a)
	# would erase it as the alpha grows.
	var viewport := SubViewport.new()
	viewport.size = Vector2i(192, 144)
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

	var water := Water.new()
	water.water_height = 7.0
	viewport.add_child(water)
	water.advance_frame(1.0 / 62.0)
	var water_material := water.get_water_material()
	water_material.set_shader_parameter("u_water_color", Vector3.ZERO)
	water_material.set_shader_parameter("u_has_reflection", false)
	water_material.set_shader_parameter("u_noise_normal",
			_solid_texture(Color(0.5, 0.5, 1.0, 1.0)))
	water_material.set_shader_parameter("u_noise_color",
			_solid_texture(Color(1.0, 1.0, 1.0, 1.0)))

	# The mount fixture's authored FF_ST_AD_LUM heat slab is the production
	# additive LUM producer; every other surface stays out of the frame.
	var model := ObjectModel.new()
	viewport.add_child(model)
	model.set_process(false)
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(Q3_ADDITIVE_LUM_3DI)), OK)
	model.set_object_data(data)
	model.scale = Vector3.ONE * 10.0
	_keep_only_shader_surfaces(model, "/self_lit/additive")
	model.advance_runtime_frame(1.0 / 62.0)
	for row in model.get_surface_materials():
		var material := row as ShaderMaterial
		if material != null and material.shader != null 				and "/self_lit/" in material.shader.resource_path:
			material.set_shader_parameter("u_diffuse", _solid_texture(Color.WHITE))
			material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
			material.set_shader_parameter("u_alpha_mod", 1.0)
	var slab := _first_visible_mesh(model)
	assert_not_null(slab, "the mount fixture carries an additive LUM surface")
	if slab == null:
		return
	# Centre the slab 10 u ahead of and 5 u below the eye: in front of the
	# 7 u water plane, over water pixels, nothing else between.
	var target := Vector3(100.3, 22.0, -43.7)
	var slab_center: Vector3 = slab.global_transform * slab.get_aabb().get_center()
	model.position += target - slab_center
	var pixel := Vector2i(camera.unproject_position(target))

	var renderer := FrameFx.new()
	viewport.add_child(renderer)
	var full_alpha_image: Image = await _render_q3_frame(renderer)
	var report := renderer.get_backend_report()
	if not bool(report.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_gte(int(report.get("q3_drawn_commands", 0)), 2,
			"the additive LUM slab and the NV water both reach the Q3 draw list: %s" % report)
	assert_not_null(full_alpha_image, "the terminal effect exposes its Q3 target")
	if full_alpha_image == null:
		return
	var full_peak := _q3_peak_near(full_alpha_image, pixel, 2)

	water_material.set_shader_parameter("u_noise_color",
			_solid_texture(Color(1.0, 1.0, 1.0, 0.25)))
	var quarter_alpha_image: Image = await _render_q3_frame(renderer)
	var quarter_peak := _q3_peak_near(quarter_alpha_image, pixel, 2)

	water.visible = false
	var alone_image: Image = await _render_q3_frame(renderer)
	var alone_peak := _q3_peak_near(alone_image, pixel, 2)
	var diagnostic := "pixel=%s full=%f quarter=%f alone=%f" % [
			pixel, full_peak, quarter_peak, alone_peak]

	assert_gt(alone_peak, 0.2, "the additive LUM copy reaches the Q3 target; " + diagnostic)
	assert_lt(quarter_peak, alone_peak - 0.02,
			"the murky NV redraw attenuates the earlier copy; " + diagnostic)
	assert_gt(full_peak, quarter_peak + 0.02,
			"a larger NV alpha retains MORE of the copy (dst x a, not dst x (1 - a)); "
			+ diagnostic)
	assert_gt(full_peak, 0.02 * alone_peak,
			"the redraw weights the copy instead of erasing it; " + diagnostic)
	# The redraw marches the nightvision rows (retail render_water_surface(0, 1)
	# @ 0x582a59): the flat 0.1 base caps the row alpha at 0.1 x 229.5 / 255,
	# so even a full noise alpha keeps under a fifth of the copy (the beauty
	# rows' 1 - murk base would keep most of it).
	assert_lt(full_peak, 0.2 * alone_peak,
			"the nightvision rows' 0.1 base bounds the retained copy; " + diagnostic)
	renderer.shutdown()
	water.release_runtime_renderer_resources()


func _additive_lum_slab_view(background: Color,
		fixture: String = Q3_ADDITIVE_LUM_3DI,
		shader_fragment: String = "/self_lit/additive") -> Dictionary:
	# The mount fixture's FF_ST_AD_LUM heat slab (or the named fixture's first
	# surface on the named self_lit wrapper), dimmed to u_rgb_mod 0.25, centred
	# 10 u ahead of and 5 u below the eye over the given background; every
	# other surface stays out of the frame.
	var viewport := SubViewport.new()
	viewport.size = Vector2i(192, 144)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)

	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_COLOR
	environment_resource.background_color = background
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment_resource.glow_enabled = false
	var environment := WorldEnvironment.new()
	environment.environment = environment_resource
	viewport.add_child(environment)

	var camera := Camera3D.new()
	camera.position = Vector3(100.3, 27.0, -33.7)
	camera.current = true
	viewport.add_child(camera)

	var model := ObjectModel.new()
	viewport.add_child(model)
	model.set_process(false)
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(fixture)), OK)
	model.set_object_data(data)
	model.scale = Vector3.ONE * 10.0
	_keep_only_shader_surfaces(model, shader_fragment)
	model.advance_runtime_frame(1.0 / 62.0)
	for row in model.get_surface_materials():
		var material := row as ShaderMaterial
		if material != null and material.shader != null \
				and "/self_lit/" in material.shader.resource_path:
			material.set_shader_parameter("u_diffuse", _solid_texture(Color.WHITE))
			material.set_shader_parameter("u_rgb_mod", Vector3(0.25, 0.25, 0.25))
			material.set_shader_parameter("u_alpha_mod", 1.0)
	var slab := _first_visible_mesh(model)
	assert_not_null(slab, "the fixture carries a LUM surface on %s" % shader_fragment)
	var pixel := Vector2i.ZERO
	if slab != null:
		var target := Vector3(100.3, 22.0, -43.7)
		var slab_center: Vector3 = slab.global_transform * slab.get_aabb().get_center()
		model.position += target - slab_center
		pixel = Vector2i(camera.unproject_position(target))
	var renderer := FrameFx.new()
	viewport.add_child(renderer)
	return {"viewport": viewport, "renderer": renderer, "pixel": pixel,
			"slab": slab, "environment": environment_resource, "camera": camera}


func test_alpha_blend_lum_strip_contributes_nothing_to_q3() -> void:
	# _FFP.fx's SELFLUM block sets MaterialDiffuse = float4(ColorSrcZero, 0),
	# so the LUM NORMAL copy carries alpha 0 into the Q3 target; under the
	# AlphaBlend strip's SRCALPHA/INVSRCALPHA that leaves the black-cleared
	# target untouched. The beauty pass still shows the strip (its wrapper
	# writes texture alpha): the copy reaches the draw list and paints
	# nothing.
	var view := _additive_lum_slab_view(Color(0.2, 0.4, 0.6),
			Q3_ALPHA_LUM_3DI, "/self_lit/alpha")
	if view.slab == null:
		return
	var renderer := view.renderer as FrameFx
	var q3_image: Image = await _render_q3_frame(renderer)
	var report := renderer.get_backend_report()
	if not bool(report.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_gt(int(report.get("q3_drawn_commands", 0)), 0,
			"the AlphaBlend LUM slab reaches the Q3 draw list: %s" % report)
	assert_not_null(q3_image, "the terminal effect exposes its Q3 target")
	if q3_image == null:
		return
	var pixel: Vector2i = view.pixel
	var beauty: Image = (view.viewport as SubViewport).get_texture().get_image()
	var beauty_pixel := beauty.get_pixel(pixel.x, pixel.y)
	assert_gt(absf(beauty_pixel.r - 0.2) + absf(beauty_pixel.g - 0.4)
			+ absf(beauty_pixel.b - 0.6), 0.1,
			"the beauty pass draws the AlphaBlend LUM strip over the background: %s"
			% beauty_pixel)
	var copy := q3_image.get_pixel(pixel.x, pixel.y)
	assert_lt(copy.r + copy.g + copy.b, 3.0 / 255.0,
			"the alpha-0 SELFLUM copy leaves the Q3 target black at %s: %s" % [pixel, copy])
	assert_lt(_q3_peak_near(q3_image, pixel, 2), 3.0 / 255.0,
			"nothing of the strip lands near %s in the Q3 target" % pixel)
	renderer.shutdown()


func test_skinned_lum_model_never_reaches_q3() -> void:
	# Retail's bone path (collect_render_batches_for_entity) appends only the
	# opaque list and the two alpha queues; the Q3 copy is the rigid object
	# path's alone. A per-vertex skinned model wearing a LUM material is
	# therefore never a Q3 producer (renderer::q3_object_source_admitted),
	# however glow-capable its material word is.
	var view := _additive_lum_slab_view(Color.BLACK, Q3_SKINNED_LUM_3DI,
			"/self_lit/additive")
	if view.slab == null:
		return
	var renderer := view.renderer as FrameFx
	var q3_image: Image = await _render_q3_frame(renderer)
	var report := renderer.get_backend_report()
	if not bool(report.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_eq(int(report.get("q3_drawn_commands", 0)), 0,
			"a skinned LUM model publishes no Q3 source: %s" % report)
	assert_not_null(q3_image, "the terminal effect exposes its Q3 target")
	if q3_image == null:
		return
	var pixel: Vector2i = view.pixel
	var beauty: Image = (view.viewport as SubViewport).get_texture().get_image()
	var beauty_pixel := beauty.get_pixel(pixel.x, pixel.y)
	assert_gt(beauty_pixel.r + beauty_pixel.g + beauty_pixel.b, 0.1,
			"the beauty pass draws the skinned LUM model: %s" % beauty_pixel)
	assert_lt(_q3_peak_near(q3_image, pixel, 2), 3.0 / 255.0,
			"the Q3 target stays black where the skinned model draws")
	renderer.shutdown()


func test_lum_q3_copy_is_the_selflum_block_not_the_beauty_pixel() -> void:
	# The LUM GLOW slot is a copy of the NORMAL block re-shaded into the
	# black-cleared Q3 target (_FFP.fx LUM GLOW copy), never the finished
	# beauty pixel. Over a coloured background an additive LUM card's beauty
	# pixel is background + card; its Q3 copy must be the SELFLUM value
	# alone: white Diffuse1 x u_rgb_mod 0.25 x min(gain, 1) x 2 = 0.5 grey per
	# additive face, with no background in it and no dependence on it.
	var over_blue := _additive_lum_slab_view(Color(0.2, 0.4, 0.6))
	if over_blue.slab == null:
		return
	var blue_renderer := over_blue.renderer as FrameFx
	var blue_image: Image = await _render_q3_frame(blue_renderer)
	var report := blue_renderer.get_backend_report()
	if not bool(report.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_gt(int(report.get("q3_drawn_commands", 0)), 0,
			"the additive LUM slab reaches the Q3 draw list: %s" % report)
	assert_not_null(blue_image, "the terminal effect exposes its Q3 target")
	if blue_image == null:
		return
	var pixel: Vector2i = over_blue.pixel
	var copy := blue_image.get_pixel(pixel.x, pixel.y)
	assert_gt(copy.r, 0.45,
			"the slab's SELFLUM copy reaches the Q3 target at %s: %s" % [pixel, copy])
	assert_lt(absf(copy.b - copy.r), 0.03,
			"the copy is the grey SELFLUM value; a beauty copy carries the blue "
			+ "background (+0.4 blue over red): %s" % copy)
	assert_lt(absf(copy.g - copy.r), 0.03, "%s" % copy)
	# The same copy over black: the Q3 pixel must not move with the beauty
	# background at all.
	var over_black := _additive_lum_slab_view(Color.BLACK)
	var black_renderer := over_black.renderer as FrameFx
	var black_image: Image = await _render_q3_frame(black_renderer)
	assert_not_null(black_image)
	if black_image == null:
		return
	var black_copy := black_image.get_pixel(pixel.x, pixel.y)
	assert_lt(absf(black_copy.r - copy.r) + absf(black_copy.g - copy.g)
			+ absf(black_copy.b - copy.b), 3.0 / 255.0,
			"the SELFLUM copy is independent of the beauty background: %s vs %s" %
			[copy, black_copy])
	blue_renderer.shutdown()
	black_renderer.shutdown()


## A 1024x1024 texture whose level 0 is red and every smaller level white.
func _red_level0_texture() -> ImageTexture:
	var data := PackedByteArray()
	var side := 1024
	var colour := Color.RED
	while side >= 1:
		var level := Image.create(side, side, false, Image.FORMAT_RGBA8)
		level.fill(colour)
		data.append_array(level.get_data())
		colour = Color.WHITE
		side /= 2
	return ImageTexture.create_from_image(
			Image.create_from_data(1024, 1024, true, Image.FORMAT_RGBA8, data))


func test_lum_q3_copy_stops_at_the_stage_textures_last_retail_mip_level() -> void:
	# The Q3 copy samples Diffuse1 like the beauty wrapper: the 2x anisotropic
	# footprint clamped at the stage's last retail mip level
	# (u_diffuse_max_lod; GTexture_CreateFromPixelData_0, retail). The bulb
	# minifies a 1024 texture far past level 0: unbounded it reads the white
	# small levels, with a ceiling of 0 it keeps level 0's red.
	var view := _q3_lum_view(true)
	var renderer := view.terminal as FrameFx
	var bulb := _first_visible_mesh(view.model)
	assert_not_null(bulb)
	if bulb == null:
		return
	var camera := (view.viewport as SubViewport).get_camera_3d()
	var pixel := Vector2i(camera.unproject_position(
			bulb.global_transform * bulb.get_aabb().get_center()))
	var material := bulb.get_active_material(0) as ShaderMaterial
	material.set_shader_parameter("u_diffuse", _red_level0_texture())
	material.set_shader_parameter("u_diffuse_max_lod", 1000.0)
	FrameFx.invalidate_q3_object_material(material)
	var unbounded: Image = await _render_q3_frame(renderer)
	if not bool(renderer.get_backend_report().get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	material.set_shader_parameter("u_diffuse_max_lod", 0.0)
	FrameFx.invalidate_q3_object_material(material)
	var ceiling: Image = await _render_q3_frame(renderer)
	assert_not_null(unbounded)
	assert_not_null(ceiling)
	if unbounded == null or ceiling == null:
		return
	var free_pixel := unbounded.get_pixelv(pixel)
	var clamped_pixel := ceiling.get_pixelv(pixel)
	assert_gt(free_pixel.g, 0.5, "unbounded, the minified copy reads a white level: %s" % free_pixel)
	assert_gt(clamped_pixel.r, 0.5, "the ceiling keeps level 0's red: %s" % clamped_pixel)
	assert_lt(clamped_pixel.g, 0.2, "and never a smaller white level: %s" % clamped_pixel)
	renderer.shutdown()


const VIEWMODEL_RIG_ROOT := "res://../fixtures/anim"
const VIEWMODEL_PROJECTION_GLOBAL := "opennova_viewmodel_projection"
# Where the posed bulb's centre lands under the WORLD projection: right of
# and below the view centre of the 192 x 144 view, 3 u ahead of the eye, so
# at a 1.61 focal ratio the gun's own footprint clears its copy's.
const VIEWMODEL_BULB_PIXEL := Vector2(146.0, 100.0)
const VIEWMODEL_BULB_DEPTH := 3.0


# The viewmodel projection feed (x = the renderfov focal ratio, y = the FP
# near plane, z = far) is process-wide, like the rig's teardown every leg
# that writes it puts the project default back.
func _restore_viewmodel_projection() -> void:
	var shipped: Dictionary = ProjectSettings.get_setting(
			"shader_globals/" + VIEWMODEL_PROJECTION_GLOBAL, {})
	RenderingServer.global_shader_parameter_set(VIEWMODEL_PROJECTION_GLOBAL,
			shipped.get("value", Vector4(1.0, 0.05, 4000.0, 1.0)))


# The state the viewmodel rig stamps on every FP part's geometry: the
# viewmodel layer and the renderfov/depth-band fold.
func _stamp_viewmodel_surfaces(root: Node) -> void:
	if root is GeometryInstance3D:
		var geometry := root as GeometryInstance3D
		geometry.layers = Water.VISUAL_LAYER_VIEWMODEL
		geometry.set_instance_shader_parameter("u_viewmodel_pass", true)
	for child in root.get_children():
		_stamp_viewmodel_surfaces(child)


# The first-person gun as the rig presents it: a rigid model on a one-bone
# rig (the fake-skinned FP gun: every rigid strip rides the rig's bone), its
# geometry on the viewmodel layer through the renderfov fold, the bone posed
# off its rest. Only the armory's opaque LUM bulb stays drawn: a white
# SELFLUM strip whose beauty depth is the gun's band.
func _viewmodel_bulb_view(use_q3: bool) -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(192, 144)
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

	# The world pass: an 80 degree HORIZONTAL fov at the scene near plane.
	var camera := Camera3D.new()
	camera.current = true
	camera.keep_aspect = Camera3D.KEEP_WIDTH
	camera.fov = 80.0
	camera.near = 0.2
	camera.far = 100.0
	camera.cull_mask = FrameFx.kBeautyCameraMask
	viewport.add_child(camera)

	var rig_root := ResourceRoot.new()
	assert_eq(rig_root.set_root_dir(ProjectSettings.globalize_path(VIEWMODEL_RIG_ROOT)), OK)
	var skeletal := SkeletalAnim.new()
	assert_true(skeletal.load_from_resource_root(rig_root, "soldier.adm"),
			"soldier.adm loads: %s" % skeletal.get_last_error())
	var model := ObjectModel.new()
	viewport.add_child(model)
	model.set_process(false)
	model.set_object_data(_lum_object_data())
	model.set_skeletal_anim(skeletal)
	_hide_non_opaque_lum_surfaces(model)
	model.advance_runtime_frame(1.0 / 62.0)
	for row in model.get_surface_materials():
		var material := row as ShaderMaterial
		if material != null and material.shader != null \
				and "/self_lit/" in material.shader.resource_path:
			material.set_shader_parameter("u_diffuse", _solid_texture(Color.WHITE))
			material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
			material.set_shader_parameter("u_alpha_mod", 1.0)
	_stamp_viewmodel_surfaces(model)

	var skeleton := model.get_skeleton()
	var bulb := _first_visible_mesh(model)
	assert_not_null(skeleton, "the rig builds the skeleton the rigid strips ride")
	assert_not_null(bulb, "the armory carries its opaque LUM bulb")
	if skeleton != null and bulb != null:
		assert_eq(skeleton.get_bone_count(), 1, "a one-bone rig: every strip rides bone 0")
		assert_eq(bulb.get_parent(), skeleton, "the rigid bulb is bound to the rig")
		var rest := skeleton.get_bone_rest(0)
		skeleton.set_bone_pose_rotation(0,
				Quaternion(Vector3(0.0, 0.0, 1.0), 0.6) * rest.basis.get_rotation_quaternion())
		skeleton.set_bone_pose_position(0, rest.origin + Vector3(0.25, -0.15, 0.1))
		# Where the skin draws the bulb (node x global pose x bind), moved so
		# its centre projects onto VIEWMODEL_BULB_PIXEL through the world pass.
		var part := bulb.global_transform * skeleton.get_bone_global_pose(0) \
				* skeleton.get_bone_global_rest(0).affine_inverse()
		var bulb_center: Vector3 = part * bulb.get_aabb().get_center()
		model.position += camera.project_position(VIEWMODEL_BULB_PIXEL,
				VIEWMODEL_BULB_DEPTH) - bulb_center

	var terminal: Node3D
	if use_q3:
		terminal = FrameFx.new()
	else:
		terminal = DisplayDecode.new()
	viewport.add_child(terminal)
	if terminal is FrameFx:
		(terminal as FrameFx).advance_frame()
	return {"viewport": viewport, "terminal": terminal, "bulb": bulb}


# Non-black pixels: their count and their centroid in pixel coordinates.
func _lit_pixels(image: Image) -> Dictionary:
	var count := 0
	var sum := Vector2.ZERO
	for y in image.get_height():
		for x in image.get_width():
			var pixel := image.get_pixel(x, y)
			if pixel.r + pixel.g + pixel.b > 0.05:
				count += 1
				sum += Vector2(x + 0.5, y + 0.5)
	return {"count": count, "centroid": sum / float(maxi(count, 1))}


# Q3 texels outside the beauty footprint (its non-black pixels) grown by
# `grow` pixels, so an edge pixel the two rasterizations split differently
# never counts.
func _q3_outside_footprint(beauty: Image, q3: Image, grow: int) -> int:
	var outside := 0
	for y in q3.get_height():
		for x in q3.get_width():
			var pixel := q3.get_pixel(x, y)
			if pixel.r + pixel.g + pixel.b <= 0.05:
				continue
			var covered := false
			for dy in range(-grow, grow + 1):
				for dx in range(-grow, grow + 1):
					var bx := x + dx
					var by := y + dy
					if bx < 0 or by < 0 or bx >= beauty.get_width() \
							or by >= beauty.get_height():
						continue
					var gun := beauty.get_pixel(bx, by)
					covered = covered or gun.r + gun.g + gun.b > 0.05
			if not covered:
				outside += 1
	return outside


func test_viewmodel_lum_copy_draws_under_the_world_projection() -> void:
	# Retail's first-person pass submits the gun without the 0x100 glow
	# suppression, so the collector queues its rigid LUM strips to Q3, and
	# FrameFX flushes them after the scene under the WORLD projection against
	# the beauty depth (runtime/renderer/q3_frame.h q3_object_source_admitted).
	# With a renderfov of 55 against the world's 80 the gun draws 1.61x
	# farther from the view centre than its copy, which glows outside the
	# gun's footprint at the world-projected position; with equal fovs the
	# copy covers its own strip and the gun's band depth rejects all of it.
	var focal_ratio := tan(deg_to_rad(80.0) * 0.5) / tan(deg_to_rad(55.0) * 0.5)
	RenderingServer.global_shader_parameter_set(VIEWMODEL_PROJECTION_GLOBAL,
			Vector4(focal_ratio, 0.05, 100.0, 1.0))
	var focused := _viewmodel_bulb_view(true)
	var beauty_only := _viewmodel_bulb_view(false)
	var renderer := focused.terminal as FrameFx
	if focused.bulb == null or beauty_only.bulb == null:
		renderer.shutdown()
		_restore_viewmodel_projection()
		return
	var q3_image: Image = await _render_q3_frame(renderer)
	var report := renderer.get_backend_report()
	if not bool(report.get("rd_available", false)):
		renderer.shutdown()
		_restore_viewmodel_projection()
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_gt(int(report.get("q3_submitted_commands", 0)), 0,
			"the viewmodel's rigid LUM strip is a Q3 source: %s" % report)
	assert_not_null(q3_image, "the terminal effect exposes its Q3 target")
	if q3_image == null:
		renderer.shutdown()
		_restore_viewmodel_projection()
		return
	var beauty: Image = (beauty_only.viewport as SubViewport).get_texture().get_image()
	var gun := _lit_pixels(beauty)
	var copy := _lit_pixels(q3_image)
	var view_centre := Vector2(beauty.get_size()) * 0.5
	var world_projected: Vector2 = view_centre + \
			(gun.centroid - view_centre) / focal_ratio
	var diagnostic := "gun=%s copy=%s world-projected=%s" % [gun, copy, world_projected]
	assert_gt(int(gun.count), 20, "the gun's bulb covers a measurable footprint; " + diagnostic)
	assert_gt(int(copy.count), 20, "the copy reaches the Q3 target; " + diagnostic)
	assert_gte(_q3_outside_footprint(beauty, q3_image, 1), int(copy.count) - 2,
			"the copy glows outside the gun's footprint; " + diagnostic)
	assert_lt((copy.centroid as Vector2).distance_to(world_projected), 1.5,
			"the copy sits at the posed bulb's world-projected position; " + diagnostic)
	assert_lt((copy.centroid as Vector2).distance_to(VIEWMODEL_BULB_PIXEL), 1.5,
			"the world projection of the posed bone's part matrix; " + diagnostic)

	# Equal fovs: the gun now draws where its copy does, and its band depth
	# rejects the copy over its own strip.
	RenderingServer.global_shader_parameter_set(VIEWMODEL_PROJECTION_GLOBAL,
			Vector4(1.0, 0.05, 100.0, 1.0))
	var equal_q3: Image = await _render_q3_frame(renderer)
	var equal_beauty: Image = (beauty_only.viewport as SubViewport).get_texture().get_image()
	var equal_gun := _lit_pixels(equal_beauty)
	var equal_copy := _lit_pixels(equal_q3)
	diagnostic = "gun=%s copy=%s" % [equal_gun, equal_copy]
	assert_lt((equal_gun.centroid as Vector2).distance_to(VIEWMODEL_BULB_PIXEL), 1.5,
			"with equal fovs the gun draws at its world-projected position; " + diagnostic)
	assert_eq(_q3_outside_footprint(equal_beauty, equal_q3, 1), 0,
			"no copy texel leaves the gun's footprint; " + diagnostic)
	# At most an edge pixel the skinned beauty and the rigid copy rasterize
	# apart may survive (none on the reference machine).
	assert_lte(int(equal_copy.count), 2,
			"the gun's band depth rejects its copy; " + diagnostic)
	renderer.shutdown()
	_restore_viewmodel_projection()
