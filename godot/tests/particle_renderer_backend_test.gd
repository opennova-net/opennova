extends GutTest


class FogSource:
	extends Node

	func get_env_generation() -> int:
		return 1

	func get_fog_color() -> Vector3:
		return Vector3(0.2, 0.3, 0.4)

	func get_fog_start() -> float:
		return 0.0

	func get_fog_level() -> float:
		return 1.0

	func get_fog_type() -> int:
		return 1


func _live_world_scene() -> NovaEffectScene:
	var particle := NovaParticleDef.new()
	particle.id = "GPU particle"
	particle.emit_dur = 0.1
	particle.emit_rate = 20.0
	particle.emit_burst = 1
	particle.age = 2.0
	particle.alpha = 1.0
	particle.scale_value = 1.0
	var graphics: Array = particle.graphics
	var layer := graphics[0] as NovaParticleGraphicLayer
	layer.present = true
	layer.texture = "gpu_contract_fallback.tga"
	layer.blend_mode = 0
	layer.alpha = 1.0
	layer.scale_value = 1.0
	particle.graphics = graphics

	var effect := NovaParticleEffect.new()
	effect.id = "GPU effect"
	effect.pdefs = PackedStringArray([particle.id])
	var file := NovaParticleFile.new()
	file.particles = [particle]
	file.effects = [effect]

	var scene := NovaEffectScene.new()
	scene.open([file])
	var receipt := scene.spawn({
		"effect_handle": scene.intern(effect.id),
		"transform": Transform3D.IDENTITY,
	})
	assert_eq(int(receipt.get("status", -1)), NovaEffectScene.SPAWN_STATUS_SPAWNED)
	scene.advance_in_place(0.1)
	return scene


func test_world_particles_use_the_uncapped_rd_compositor_contract() -> void:
	var renderer := add_child_autofree(NovaParticleRenderer.new()) as NovaParticleRenderer
	await get_tree().process_frame

	var report := renderer.get_debug_packet_report()
	var backend: Dictionary = report.get("world_backend", {})
	assert_eq(String(backend.get("backend", "")), "rendering_device_compositor")
	assert_eq(int(backend.get("callback_type", -1)), 4,
			"World particles must draw after transparent scene geometry.")
	assert_true(bool(backend.get("depth_test", false)))
	assert_false(bool(backend.get("depth_write", true)))
	assert_eq(String(backend.get("reverse_z_compare", "")), "greater_or_equal")
	assert_true(bool(backend.get("packet_order_preserved", false)))
	assert_true(bool(backend.get("uncapped_commands", false)))
	assert_eq(int(backend.get("packet_commands_dropped", -1)), 0)
	assert_true(bool(backend.get("immutable_submission_copy", false)))
	assert_false(bool(backend.get("retains_compiler_packet_pointer", true)))
	assert_eq(int(backend.get("push_constant_bytes", 0)), 128)
	assert_eq(String(backend.get("fog_source", "")),
			"immutable_opennova_environment_snapshot")
	assert_eq(String(backend.get("fog_distance_policy", "")),
			"type0_eye_depth_else_radial")
	assert_eq(String(backend.get("fog_material_targets", "")),
			"scene,black,black,scene,white,gray127,black,scene")
	assert_eq(String(backend.get("view_projection_source", "")),
			"render_scene_data_corrected")
	assert_false(bool(backend.get("adds_view_projection_depth_correction", true)),
			"RenderSceneData.get_view_projection() is already depth-corrected; " +
			"correcting it again makes particles move against scene geometry.")
	assert_eq(String(backend.get("scene_color_copy_policy", "")),
			"once_per_distorting_view")
	assert_eq(String(backend.get("scene_color_snapshot_backend", "")),
			"fullscreen_sampled_blit",
			"Resolved scene color is sampleable but is not a transfer source.")
	assert_eq(String(backend.get("scene_color_source_requirement", "")),
			"sampling_only")
	assert_false(bool(backend.get("scene_color_requires_copy_from", true)),
			"Distortion must not call texture_copy on Godot's resolved color buffer.")
	assert_eq(String(backend.get("scene_color_scratch_ownership", "")),
			"effect_owned_rd_texture")
	assert_eq(String(backend.get("scene_color_scratch_allocation", "")),
			"lazy_on_first_distort")
	assert_eq(String(backend.get("scene_color_scratch_retention", "")),
			"until_target_rebuild")
	assert_eq(String(backend.get("scene_color_fallback_binding", "")),
			"shared_1x1_texture")
	assert_eq(String(report.get("first_person_backend", "")),
			"array_mesh_fallback_tool_only")
	assert_false(bool(report.get("world_mesh_instance", true)))
	for descendant in renderer.find_children("*", "MeshInstance3D", true, false):
		assert_ne(String(descendant.name), "ParticleWorldPacket",
				"World particles must not regress to command-local ArrayMesh surfaces.")


func test_world_backend_executes_a_live_gpu_submission() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0, 0, 5)
	camera.current = true
	viewport.add_child(camera)
	var renderer := NovaParticleRenderer.new()
	renderer.scene = _live_world_scene()
	renderer.procedural_fallback_enabled = true
	var fog_source := FogSource.new()
	viewport.add_child(fog_source)
	renderer.environment_source = fog_source
	viewport.add_child(renderer)
	renderer.render_now()
	# Do not await frame_post_draw: headless compatibility renderers may never
	# emit it. Ordinary process frames still exercise the callback whenever the
	# RenderingDevice compositor is available.
	for _frame in 5:
		await get_tree().process_frame

	var report := renderer.get_debug_packet_report()
	var world: Dictionary = report.get("world", {})
	var backend: Dictionary = report.get("world_backend", {})
	assert_gt(int(world.get("rendered_quad_count", 0)), 0,
			"the fixture must compile visible world geometry")
	assert_gt(int(world.get("draw_command_count", 0)), 0,
			"the fixture must publish at least one material run")
	if not bool(backend.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_true(bool(backend.get("callback_seen", false)),
			"the post-transparent compositor callback must execute")
	assert_eq(String(backend.get("status", "")), "drawn",
			String(backend.get("failure", "live submission failed")))
	assert_gt(int(backend.get("submitted_commands", 0)), 0)
	assert_eq(int(backend.get("drawn_commands", -1)),
			int(backend.get("submitted_commands", 0)))
	assert_gt(int(backend.get("gpu_draw_calls", 0)), 0)
	assert_eq(int(backend.get("drawn_frame_id", -1)),
			int(backend.get("submitted_frame_id", 0)))
	var center := viewport.get_texture().get_image().get_pixel(32, 32)
	assert_gt(center.r, 0.05, "the live particle must reach the framebuffer")
	assert_gt(center.g, center.r + 0.02,
			"full linear fog must tint the white fallback toward the supplied green")
	assert_gt(center.b, center.g + 0.02,
			"full linear fog must tint the white fallback toward the supplied blue")


func test_pipeline_warm_is_serviced_by_the_real_rd_compositor() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0, 0, 5)
	camera.current = true
	viewport.add_child(camera)
	var renderer := NovaParticleRenderer.new()
	viewport.add_child(renderer)
	await get_tree().process_frame

	# No scene/particles are needed: delayed emitters are exactly why the World
	# compositor must manufacture its real framebuffer-specific pipeline set.
	renderer.warm_pipelines(Vector3.ZERO)
	renderer.render_now()
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()

	var report := renderer.get_debug_packet_report()
	var backend: Dictionary = report.get("world_backend", {})
	if not bool(backend.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_true(bool(backend.get("callback_seen", false)),
			"the forced draw/sync must service the callback before reset can cancel it")
	assert_eq(int(backend.get("pipeline_warm_requests", 0)), 1)
	assert_eq(int(backend.get("pipeline_warm_requests_serviced", 0)), 1,
			String(backend.get("failure", "RD pipeline warm was not serviced")))
	assert_eq(int(backend.get("warmed_pipeline_modes", 0)), 8,
			"all retail blend modes must exist on the actual World framebuffer")
	assert_gt(int(backend.get("warmed_framebuffer_formats", 0)), 0)
	assert_true(bool(backend.get("scene_snapshot_pipeline_warmed", false)),
			"Distort's resolved-scene copy pipeline must be warmed too")


func test_camera_compositor_coordinates_multiple_particle_renderers() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.current = true
	var first := NovaParticleRenderer.new()
	var second := NovaParticleRenderer.new()
	viewport.add_child(first)
	viewport.add_child(second)

	first.render_now()
	second.render_now()
	assert_not_null(camera.compositor)
	assert_eq(camera.compositor.get_compositor_effects().size(), 2,
			"each renderer is registered exactly once on the shared camera")
	var composed := camera.compositor
	first.render_now()
	second.render_now()
	assert_eq(camera.compositor, composed,
			"steady-state renders do not clone the composed resource")
	assert_eq(camera.compositor.get_compositor_effects().size(), 2,
			"steady-state renders do not duplicate effects")

	viewport.remove_child(first)
	first.free()
	assert_not_null(camera.compositor)
	assert_eq(camera.compositor.get_compositor_effects().size(), 1,
			"non-LIFO teardown removes only the departing renderer")

	viewport.remove_child(second)
	second.free()
	assert_null(camera.compositor,
			"the last renderer restores null so WorldEnvironment inheritance resumes")
