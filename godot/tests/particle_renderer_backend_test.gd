extends GutTest


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
	assert_eq(int(backend.get("push_constant_bytes", 0)), 96)
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
