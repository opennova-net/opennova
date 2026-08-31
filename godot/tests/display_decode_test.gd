extends GutTest

## The terminal display decode (ADR 0043): every engine shader writes
## gamma-domain values into the scene target (color.gdshaderinc) and one
## DisplayDecodeEffect per 3D view applies the display transfer last. The
## retired FrameFX/Q3 bloom bracket has no seam here any more — Environment
## glow is the canonical bloom; these pins cover the decode contract, the
## install/teardown lifecycle, and the MultiMesh particle presenter that used
## to ride the compositor.


# Gamma-domain canary: an unshaded hand shader writes the raw gamma value the
# scene contract expects; the decode + Godot's sRGB output encode must return
# the same byte to the presented image.
func _canary_material(beauty: Color) -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = """
shader_type spatial;
render_mode unshaded, depth_draw_opaque, cull_disabled, fog_disabled;
uniform vec3 u_beauty;

void fragment() {
	ALBEDO = u_beauty;
}
"""
	var material := ShaderMaterial.new()
	material.shader = shader
	material.set_shader_parameter("u_beauty", Vector3(beauty.r, beauty.g, beauty.b))
	return material


func _decode_view() -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 32)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)

	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_COLOR
	environment_resource.background_color = Color.BLACK
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment_resource.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var environment := WorldEnvironment.new()
	environment.environment = environment_resource
	viewport.add_child(environment)

	var camera := Camera3D.new()
	camera.current = true
	camera.position = Vector3(0.0, 0.0, 4.0)
	viewport.add_child(camera)

	var quad := MeshInstance3D.new()
	quad.mesh = QuadMesh.new()
	(quad.mesh as QuadMesh).size = Vector2(8.0, 8.0)
	quad.material_override = _canary_material(Color(0.0, 0.8, 0.0))
	viewport.add_child(quad)

	return {"viewport": viewport, "environment": environment, "camera": camera}


func test_display_decode_installs_terminal_effect_and_decodes() -> void:
	var view := _decode_view()
	var environment := view.environment as WorldEnvironment
	var viewport := view.viewport as SubViewport

	var decoder := DisplayDecode.new()
	viewport.add_child(decoder)

	var compositor := environment.compositor
	assert_not_null(compositor,
			"the decode installs on the nearest WorldEnvironment")
	var effects := compositor.compositor_effects
	assert_eq(effects.size(), 1,
			"an otherwise empty world receives exactly the terminal effect")
	assert_eq(effects[0].effect_callback_type,
			CompositorEffect.EFFECT_CALLBACK_TYPE_POST_TRANSPARENT,
			"the terminal transfer is last")

	var report := decoder.get_backend_report()
	assert_eq(String(report.get("backend", "")),
			"rendering_device_display_decode")
	assert_true(bool(report.get("terminal_compositor_installed", false)))

	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	report = decoder.get_backend_report()
	if bool(report.get("rd_available", false)):
		assert_eq(String(report.get("status", "")), "drawn",
				String(report.get("failure", "display decode failed")))
		assert_eq(String(report.get("framebuffer_blend_domain", "")), "gamma")
		var center := viewport.get_texture().get_image().get_pixel(32, 16)
		assert_gt(center.g, 0.65,
				"the gamma-domain beauty green survives decode + display encode")
		assert_lt(center.r, 0.05)
		assert_lt(center.b, 0.05)
	else:
		pending("RenderingDevice unavailable under this Godot renderer")

	viewport.remove_child(decoder)
	decoder.free()
	assert_null(environment.compositor,
			"teardown restores the inherited compositor")


func test_explicit_shutdown_detaches_terminal_effect_and_is_idempotent() -> void:
	var view := _decode_view()
	var environment := view.environment as WorldEnvironment
	var viewport := view.viewport as SubViewport

	var decoder := DisplayDecode.new()
	viewport.add_child(decoder)
	assert_not_null(environment.compositor)

	decoder.shutdown()
	var report := decoder.get_backend_report()
	assert_true(bool(report.get("shutdown", false)))
	assert_false(bool(report.get("terminal_compositor_installed", true)))
	assert_null(environment.compositor,
			"explicit shutdown restores the inherited compositor")

	# The process-exit coordinator and EXIT_TREE fallback can converge here.
	decoder.shutdown()
	assert_null(environment.compositor)
	viewport.remove_child(decoder)
	decoder.free()


func test_display_decode_reentry_recreates_released_terminal_effect() -> void:
	var view := _decode_view()
	var environment := view.environment as WorldEnvironment
	var viewport := view.viewport as SubViewport

	var decoder := DisplayDecode.new()
	viewport.add_child(decoder)
	var first_compositor := environment.compositor
	assert_not_null(first_compositor)
	var first_effect := first_compositor.compositor_effects[0] \
			as DisplayDecodeEffect
	assert_not_null(first_effect)
	assert_true(first_effect.enabled)

	viewport.remove_child(decoder)
	assert_null(environment.compositor,
			"leaving the tree restores the inherited compositor")
	assert_false(first_effect.enabled,
			"leaving the tree disables the detached render callback")
	assert_true(bool(decoder.get_backend_report().get("shutdown", false)))

	# A plain re-add (no request_ready from the caller) must revive the node.
	viewport.add_child(decoder)
	var second_compositor := environment.compositor
	assert_not_null(second_compositor,
			"re-entry reinstalls the terminal compositor")
	var second_effect := second_compositor.compositor_effects[0] \
			as DisplayDecodeEffect
	assert_not_null(second_effect)
	assert_true(second_effect.enabled)
	assert_ne(second_effect.get_instance_id(), first_effect.get_instance_id(),
			"re-entry uses a fresh effect after the prior device owner shut down")
	assert_false(bool(decoder.get_backend_report().get("shutdown", true)),
			"re-entry clears the shutdown latch")

	viewport.remove_child(decoder)
	assert_null(environment.compositor)
	assert_false(second_effect.enabled)
	decoder.free()


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


func test_particles_present_as_scene_multimesh_batches() -> void:
	# ADR 0043: particles are transparent scene meshes. Godot's transparent
	# pipeline orders them against water and everything else per instance —
	# the retired compositor PRE/POST water split (D-RORD-7) has no seam to
	# pin any more; this pins the presenter contract instead.
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.1, 5.0)
	camera.current = true
	viewport.add_child(camera)
	camera.look_at(Vector3.ZERO, Vector3.UP)

	var particle_renderer := ParticleRenderer.new()
	particle_renderer.scene = _water_order_particle_scene()
	particle_renderer.procedural_fallback_enabled = true
	viewport.add_child(particle_renderer)
	particle_renderer.scene.advance_in_place(0.2)
	particle_renderer.render_now()

	var report := particle_renderer.get_debug_draw_list_report()
	assert_eq(String(report.get("presenter", "")), "multimesh_scene")
	assert_gt(int(report.get("rendered_quad_count", 0)), 0,
			"an emitting effect presents live quads")
	assert_gt(int(report.get("draw_command_count", 0)), 0)
	assert_gt(int(report.get("pool_used", 0)), 0)
	var batches := 0
	for child in particle_renderer.get_children():
		var mmi := child as MultiMeshInstance3D
		if mmi == null or not mmi.visible:
			continue
		batches += 1
		assert_eq(mmi.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF)
		assert_not_null(mmi.multimesh)
		assert_gt(mmi.multimesh.visible_instance_count, 0)
		assert_not_null(mmi.material_override as StandardMaterial3D,
				"presentation uses canonical scene materials")
	assert_eq(batches, int(report.get("pool_used", 0)),
			"every used pool entry is a visible scene batch")

	particle_renderer.set_hidden(true)
	assert_eq(int(particle_renderer.get_rendered_quad_count()), 0,
			"hiding drops the presented pool")
	for child in particle_renderer.get_children():
		var mmi := child as MultiMeshInstance3D
		if mmi != null:
			assert_false(mmi.visible)
