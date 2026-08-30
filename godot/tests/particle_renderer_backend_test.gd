extends GutTest


func _brightest_pixel(image: Image) -> Dictionary:
	var best := Color(0.0, 0.0, 0.0, 0.0)
	var position := Vector2i.ZERO
	var score := -1.0
	for y in image.get_height():
		for x in image.get_width():
			var pixel := image.get_pixel(x, y)
			var pixel_score := maxf(pixel.r, maxf(pixel.g, pixel.b))
			if pixel_score > score:
				score = pixel_score
				best = pixel
				position = Vector2i(x, y)
	return {"position": position, "color": best}


func _live_fog_source() -> MissionEnvironment:
	var data := EnvFile.new()
	data.reset_to_default()
	data.fog_type = 1
	data.fog_level = 1.0
	for keyframe in data.get_tod_keyframes():
		keyframe.set_fog_color(Color(0.2, 0.3, 0.4))
	var environment := MissionEnvironment.new()
	environment.environment_data = data
	return environment


func _live_world_scene_at_heights(heights: PackedFloat32Array) -> EffectScene:
	var particle := ParticleDef.new()
	particle.id = "GPU particle"
	particle.emit_dur = 0.1
	particle.emit_rate = 20.0
	particle.emit_burst = 1
	particle.age = 2.0
	particle.alpha = 1.0
	particle.scale_value = 1.0
	var graphics: Array = particle.graphics
	var layer := graphics[0] as ParticleGraphicLayer
	layer.present = true
	layer.texture = "gpu_contract_fallback.tga"
	layer.blend_mode = 0
	layer.alpha = 1.0
	layer.scale_value = 1.0
	particle.graphics = graphics

	var effect := ParticleEffect.new()
	effect.id = "GPU effect"
	effect.pdefs = PackedStringArray([particle.id])
	var file := ParticleFile.new()
	file.particles = [particle]
	file.effects = [effect]

	var scene := EffectScene.new()
	scene.open([file])
	for height in heights:
		var transform := Transform3D.IDENTITY
		transform.origin.y = height
		var receipt := scene.spawn({
			"effect_handle": scene.intern(effect.id),
			"transform": transform,
		})
		assert_eq(int(receipt.get("status", -1)), EffectScene.SPAWN_STATUS_SPAWNED)
	scene.advance_in_place(0.1)
	return scene


func _live_world_scene() -> EffectScene:
	return _live_world_scene_at_heights(PackedFloat32Array([0.0]))


func _live_first_person_scene() -> EffectScene:
	var scene := _live_world_scene_at_heights(PackedFloat32Array())
	var receipt := scene.spawn({
		"effect_handle": scene.intern("GPU effect"),
		"transform": Transform3D.IDENTITY,
		"render_domain": EffectScene.RENDER_DOMAIN_FIRST_PERSON,
	})
	assert_eq(int(receipt.get("status", -1)), EffectScene.SPAWN_STATUS_SPAWNED)
	scene.advance_in_place(0.1)
	return scene


func _first_person_batch(renderer: ParticleRenderer) -> MeshInstance3D:
	for descendant in renderer.find_children("*", "MeshInstance3D", true, false):
		if String(descendant.name) == "ParticleFirstPersonBatch":
			return descendant as MeshInstance3D
	return null


func test_draw_list_diagnostics_are_live_without_opt_in() -> void:
	# Diagnostics read each compiler's retained draw list in place: the first
	# report after render_now() must already be live, nothing has to be
	# switched on beforehand, and every renderer-level count derives from the
	# same lists the report exposes.
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var renderer := ParticleRenderer.new()
	renderer.scene = _live_world_scene()
	renderer.procedural_fallback_enabled = true
	viewport.add_child(renderer)
	renderer.render_now()

	var report := renderer.get_debug_draw_list_report()
	var camera_side: Dictionary = report.get("world_camera_side", {})
	var far_side: Dictionary = report.get("world_far_side", {})
	var first_person: Dictionary = report.get("first_person", {})
	assert_true(camera_side.has("compile_index"),
			"the first report after render_now() carries the live draw list")
	assert_gt(int(camera_side.get("rendered_quad_count", 0)), 0)
	assert_eq(int(renderer.get_rendered_quad_count()),
			int(far_side.get("rendered_quad_count", 0))
			+ int(camera_side.get("rendered_quad_count", 0))
			+ int(first_person.get("rendered_quad_count", 0)),
			"renderer counts and the report read the same retained lists")
	assert_eq(int(renderer.get_draw_command_count()),
			int(far_side.get("draw_command_count", 0))
			+ int(camera_side.get("draw_command_count", 0))
			+ int(first_person.get("draw_command_count", 0)))
	assert_true((report.get("reflection_far_side", {}) as Dictionary).is_empty(),
			"a slot without its camera reports idle, not a stale list")
	var bounds_rows := renderer.get_debug_emitter_bounds()
	assert_eq(bounds_rows.size(),
			int(far_side.get("selected_emitters", 0))
			+ int(camera_side.get("selected_emitters", 0))
			+ int(first_person.get("selected_emitters", 0)),
			"every selected emitter owns exactly one bounds row")

	var before_index := int(camera_side.get("compile_index", 0))
	renderer.render_now()
	var after: Dictionary = renderer.get_debug_draw_list_report().get(
			"world_camera_side", {})
	assert_eq(int(after.get("compile_index", 0)), before_index + 1,
			"each render_now() recompiles and the report follows immediately")


func test_first_person_batch_retains_one_mesh_across_renders() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var renderer := ParticleRenderer.new()
	var scene := _live_first_person_scene()
	renderer.scene = scene
	renderer.procedural_fallback_enabled = true
	viewport.add_child(renderer)
	renderer.render_now()

	var batch := _first_person_batch(renderer)
	assert_not_null(batch, "the FirstPerson tool path owns one MeshInstance3D")
	if batch == null:
		return
	var first_person: Dictionary = renderer.get_debug_draw_list_report().get(
			"first_person", {})
	assert_gt(int(first_person.get("rendered_quad_count", 0)), 0,
			"the FirstPerson emitter compiles quads")
	var mesh := batch.mesh as ArrayMesh
	assert_not_null(mesh)
	if mesh == null:
		return
	assert_gt(mesh.get_surface_count(), 0)
	assert_true(batch.visible)
	var mesh_id := mesh.get_instance_id()

	scene.advance_in_place(0.016)
	renderer.render_now()
	assert_eq(batch.mesh.get_instance_id(), mesh_id,
			"a steady-state render rebuilds surfaces on the same ArrayMesh")
	assert_gt((batch.mesh as ArrayMesh).get_surface_count(), 0)

	renderer.hidden = true
	assert_false(batch.visible, "hiding clears the retained surfaces")
	assert_eq(batch.mesh.get_instance_id(), mesh_id,
			"hiding keeps the mesh resource for the next visible render")
	renderer.hidden = false
	renderer.render_now()
	assert_true(batch.visible)
	assert_eq(batch.mesh.get_instance_id(), mesh_id)
	assert_gt((batch.mesh as ArrayMesh).get_surface_count(), 0)


func test_world_particles_use_the_uncapped_rd_compositor_contract() -> void:
	var renderer := add_child_autofree(ParticleRenderer.new()) as ParticleRenderer
	await get_tree().process_frame

	var report := renderer.get_debug_draw_list_report()
	var far_backend: Dictionary = report.get("world_far_backend", {})
	var backend: Dictionary = report.get("world_camera_backend", {})
	assert_eq(String(far_backend.get("backend", "")),
			"rendering_device_compositor")
	assert_eq(int(far_backend.get("callback_type", -1)), 3,
			"The closest public callback for the far-side subset is PRE_TRANSPARENT.")
	assert_eq(String(backend.get("backend", "")), "rendering_device_compositor")
	assert_eq(int(backend.get("callback_type", -1)), 4,
			"The camera-side emitter subset runs after transparent geometry.")
	assert_true(bool(backend.get("depth_test", false)))
	assert_false(bool(backend.get("depth_write", true)))
	assert_eq(String(backend.get("reverse_z_compare", "")), "greater_or_equal")
	assert_true(bool(backend.get("draw_list_order_preserved", false)))
	assert_true(bool(backend.get("uncapped_commands", false)))
	assert_eq(int(backend.get("draw_list_commands_dropped", -1)), 0)
	assert_true(bool(backend.get("immutable_submission_copy", false)))
	assert_false(bool(backend.get("retains_compiler_draw_list_pointer", true)))
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


func test_world_emitter_subsets_reverse_at_the_water_plane() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 10.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var renderer := ParticleRenderer.new()
	# Equality is above-inclusive in retail. Keeping one emitter exactly on the
	# plane makes this integration vector cover that boundary as well as the
	# camera-side reversal.
	renderer.scene = _live_world_scene_at_heights(
			PackedFloat32Array([-2.0, 0.0, 3.0]))
	renderer.procedural_fallback_enabled = true
	renderer.set_water_plane(0.0, null)
	viewport.add_child(renderer)
	renderer.render_now()

	var report := renderer.get_debug_draw_list_report()
	var far: Dictionary = report.get("world_far_side", {})
	var near: Dictionary = report.get("world_camera_side", {})
	assert_eq(int(far.get("selected_emitters", -1)), 1,
			"an above-water camera draws the strict-below emitter first")
	assert_eq(int(near.get("selected_emitters", -1)), 2,
			"plane equality belongs to the above-water camera-side pass")
	assert_eq(int(far.get("water_filtered_emitters", -1)), 2)
	assert_eq(int(near.get("water_filtered_emitters", -1)), 1)

	camera.position.y = -10.0
	renderer.render_now()
	report = renderer.get_debug_draw_list_report()
	far = report.get("world_far_side", {})
	near = report.get("world_camera_side", {})
	assert_eq(int(far.get("selected_emitters", -1)), 2,
			"a below-water camera draws above/equal emitters first")
	assert_eq(int(near.get("selected_emitters", -1)), 1,
			"the strict-below emitter moves to the camera-side pass")


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
	var renderer := ParticleRenderer.new()
	renderer.scene = _live_world_scene()
	renderer.procedural_fallback_enabled = true
	var fog_source := _live_fog_source()
	viewport.add_child(fog_source)
	renderer.environment_source = fog_source
	viewport.add_child(renderer)
	renderer.render_now()
	# Do not await frame_post_draw: headless compatibility renderers may never
	# emit it. Ordinary process frames still exercise the callback whenever the
	# RenderingDevice compositor is available.
	for _frame in 5:
		await get_tree().process_frame
	# D3D12 readback is asynchronous relative to process_frame. Pin the same
	# force-draw/sync barrier used by the warm test before inspecting pixels;
	# callback diagnostics alone are not proof that the resolved image is ready.
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()

	var report := renderer.get_debug_draw_list_report()
	var world: Dictionary = report.get("world_camera_side", {})
	var backend: Dictionary = report.get("world_camera_backend", {})
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
	var image := viewport.get_texture().get_image()
	var center := image.get_pixel(32, 32)
	var peak := _brightest_pixel(image)
	assert_gt(center.r, 0.05,
			"the live particle must reach the framebuffer; brightest pixel=%s at %s" % [
				peak.color, peak.position])
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
	var renderer := ParticleRenderer.new()
	viewport.add_child(renderer)
	await get_tree().process_frame

	# No scene/particles are needed: delayed emitters are exactly why the World
	# compositor must manufacture its real framebuffer-specific pipeline set.
	renderer.warm_pipelines(Vector3.ZERO)
	renderer.render_now()
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()

	var report := renderer.get_debug_draw_list_report()
	var backend: Dictionary = report.get("world_camera_backend", {})
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
	renderer.shutdown()
	assert_engine_error_count(0,
			"explicit shutdown frees warmed RenderingDevice resources exactly once")


func test_camera_compositor_coordinates_multiple_particle_renderers() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.current = true
	var first := ParticleRenderer.new()
	var second := ParticleRenderer.new()
	viewport.add_child(first)
	viewport.add_child(second)

	first.render_now()
	second.render_now()
	assert_not_null(camera.compositor)
	assert_eq(camera.compositor.get_compositor_effects().size(), 4,
			"each renderer registers its far-side and camera-side effects once")
	var composed := camera.compositor
	first.render_now()
	second.render_now()
	assert_eq(camera.compositor, composed,
			"steady-state renders do not clone the composed resource")
	assert_eq(camera.compositor.get_compositor_effects().size(), 4,
			"steady-state renders do not duplicate effects")

	var first_effects := camera.compositor.get_compositor_effects().slice(0, 2)
	first.shutdown()
	first.shutdown()
	for effect in first_effects:
		assert_true(bool(effect.get_backend_report().get("shutdown", false)),
				"explicit shutdown retires each compositor effect exactly once")
	assert_eq(camera.compositor.get_compositor_effects().size(), 2,
			"shutdown removes only the departing renderer's pair")
	viewport.remove_child(first)
	first.free()
	assert_not_null(camera.compositor)
	assert_eq(camera.compositor.get_compositor_effects().size(), 2,
			"EXIT_TREE remains idempotent after explicit shutdown")

	second.shutdown()
	second.shutdown()
	assert_null(camera.compositor,
			"the last explicit shutdown restores the inherited compositor")
	viewport.remove_child(second)
	second.free()
	assert_null(camera.compositor,
			"the last renderer restores null so WorldEnvironment inheritance resumes")


func test_exit_tree_shutdown_is_undone_by_re_entry() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0, 0, 5)
	camera.current = true
	viewport.add_child(camera)
	var renderer := ParticleRenderer.new()
	renderer.scene = _live_world_scene()
	renderer.procedural_fallback_enabled = true
	viewport.add_child(renderer)
	renderer.render_now()
	var live := renderer.get_debug_draw_list_report()
	assert_false(bool(live.get("shutdown", true)), "a fresh renderer is live")
	assert_true(bool(live.get("world_compositor_attached", false)))
	assert_eq(camera.compositor.get_compositor_effects().size(), 2)

	# EXIT_TREE retires the compositor effects for good and latches shutdown.
	viewport.remove_child(renderer)
	var retired := renderer.get_debug_draw_list_report()
	assert_true(bool(retired.get("shutdown", false)), "EXIT_TREE latches shutdown")
	assert_true(bool((retired.get("world_camera_backend", {}) as Dictionary).get(
			"shutdown", false)), "EXIT_TREE retires the camera-side effect")
	assert_false(bool(retired.get("world_compositor_attached", true)),
			"EXIT_TREE detaches from the camera compositor")
	assert_null(camera.compositor, "the departing renderer restores the camera")

	# Re-entry replaces the retired effects and clears the latch: the same
	# instance renders again, EffectWorld keeps it because it is still valid.
	viewport.add_child(renderer)
	renderer.render_now()
	var revived := renderer.get_debug_draw_list_report()
	assert_false(bool(revived.get("shutdown", true)), "ENTER_TREE clears the latch")
	for key in [
		"world_far_backend", "world_camera_backend",
		"reflection_far_backend", "reflection_camera_backend",
	]:
		assert_false(bool((revived.get(key, {}) as Dictionary).get("shutdown", true)),
				"%s is a fresh effect after re-entry" % key)
	assert_true(bool(revived.get("world_compositor_attached", false)),
			"re-entry re-attaches the world camera")
	assert_not_null(camera.compositor)
	assert_eq(camera.compositor.get_compositor_effects().size(), 2,
			"re-entry installs exactly one fresh pair, no stale effects")
	assert_gt(renderer.get_draw_command_count(), 0,
			"the retained scene publishes again through the new effects")
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	var backend: Dictionary = renderer.get_debug_draw_list_report().get(
			"world_camera_backend", {})
	if bool(backend.get("rd_available", false)):
		assert_true(bool(backend.get("callback_seen", false)),
				"the fresh effect's callback runs on the re-entered renderer")
	assert_engine_error_count(0, "the retire/re-enter round trip is clean")


func test_far_particles_lead_the_camera_chain_and_terminal_stays_last() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	add_child_autofree(viewport)
	var environment := WorldEnvironment.new()
	environment.environment = Environment.new()
	viewport.add_child(environment)
	var camera := Camera3D.new()
	camera.current = true
	viewport.add_child(camera)
	var frame_renderer := FrameFx.new()
	viewport.add_child(frame_renderer)
	await get_tree().process_frame

	var particle_renderer := ParticleRenderer.new()
	viewport.add_child(particle_renderer)
	particle_renderer.render_now()
	assert_not_null(camera.compositor)
	var effects := camera.compositor.compositor_effects
	assert_eq(effects.size(), 3,
			"far particles + camera particles + terminal are the full cutover")
	assert_eq(effects[0].effect_callback_type,
			CompositorEffect.EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT,
			"particle pass A draws before the transparent list (before water)")
	assert_eq(effects[1].effect_callback_type,
			CompositorEffect.EFFECT_CALLBACK_TYPE_POST_TRANSPARENT,
			"particle pass B follows water and camera-side alpha")
	assert_eq(effects[2].effect_callback_type,
			CompositorEffect.EFFECT_CALLBACK_TYPE_POST_TRANSPARENT,
			"FrameFX plus the display transfer remain terminal")
	assert_true(effects[0] is ParticleCompositorEffect)
	assert_true(effects[1] is ParticleCompositorEffect)
	assert_true(effects[2] is FrameFxCompositorEffect)


func test_reflection_camera_receives_two_ordered_camera_correct_submissions() -> void:
	var world_viewport := SubViewport.new()
	world_viewport.size = Vector2i(64, 64)
	world_viewport.own_world_3d = true
	add_child_autofree(world_viewport)
	var world_camera := Camera3D.new()
	world_camera.current = true
	world_viewport.add_child(world_camera)

	var reflection_viewport := SubViewport.new()
	reflection_viewport.size = Vector2i(64, 64)
	reflection_viewport.world_3d = world_viewport.world_3d
	add_child_autofree(reflection_viewport)
	var reflection_camera := Camera3D.new()
	reflection_camera.current = true
	reflection_viewport.add_child(reflection_camera)

	var renderer := ParticleRenderer.new()
	renderer.scene = _live_world_scene()
	renderer.procedural_fallback_enabled = true
	renderer.set_water_plane(0.0, reflection_camera)
	world_viewport.add_child(renderer)
	renderer.render_now()

	assert_not_null(reflection_camera.compositor)
	assert_eq(reflection_camera.compositor.get_compositor_effects().size(), 2,
			"reflection receives the consecutive far/camera-side pair")
	var report := renderer.get_debug_draw_list_report()
	assert_true(bool(report.get("reflection_compositor_attached", false)))
	var far_backend: Dictionary = report.get("reflection_far_backend", {})
	var camera_backend: Dictionary = report.get("reflection_camera_backend", {})
	assert_eq(int(far_backend.get("callback_type", -1)), 4)
	assert_eq(int(camera_backend.get("callback_type", -1)), 4)
	assert_false(report.has("world"),
			"the cutover leaves no ambiguous legacy draw-list key")
	assert_false(report.has("world_backend"),
			"the cutover leaves no single-pass backend alias")
