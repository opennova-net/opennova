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
	var file := ParticleFixture.catalog("GPU particle",
			"emit_dur = 0.1;\nemit_rate = 20;\nemit_burst = 1;\nage = 2;\nalpha = 1;\nscale = 1;\ngraphic1 = gpu_contract_fallback.tga, blend;\ng1_alpha = 1;\ng1_scale = 1;",
			["GPU effect"])

	var scene := EffectScene.new()
	scene.open([file])
	for height in heights:
		var transform := Transform3D.IDENTITY
		transform.origin.y = height
		var receipt := scene.spawn(EffectSpawnRequest.make(scene.intern("GPU effect"), transform))
		assert_eq(receipt.status, EffectScene.SPAWN_STATUS_SPAWNED)
	scene.advance_in_place(0.1)
	return scene


func _live_world_scene() -> EffectScene:
	return _live_world_scene_at_heights(PackedFloat32Array([0.0]))


func _live_first_person_scene() -> EffectScene:
	var scene := _live_world_scene_at_heights(PackedFloat32Array())
	var request := EffectSpawnRequest.make(scene.intern("GPU effect"), Transform3D.IDENTITY)
	request.render_domain = EffectScene.RENDER_DOMAIN_FIRST_PERSON
	assert_eq(scene.spawn(request).status, EffectScene.SPAWN_STATUS_SPAWNED)
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
	renderer.render_now(GameWorld.current_frame_clock_ms())

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
	renderer.render_now(GameWorld.current_frame_clock_ms())
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
	renderer.render_now(GameWorld.current_frame_clock_ms())

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
	renderer.render_now(GameWorld.current_frame_clock_ms())
	assert_eq(batch.mesh.get_instance_id(), mesh_id,
			"a steady-state render rebuilds surfaces on the same ArrayMesh")
	assert_gt((batch.mesh as ArrayMesh).get_surface_count(), 0)

	renderer.hidden = true
	assert_false(batch.visible, "hiding clears the retained surfaces")
	assert_eq(batch.mesh.get_instance_id(), mesh_id,
			"hiding keeps the mesh resource for the next visible render")
	renderer.hidden = false
	renderer.render_now(GameWorld.current_frame_clock_ms())
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
			"scene,black,black,none,white,gray127,none,none")
	assert_eq(String(backend.get("view_projection_source", "")),
			"render_scene_data_corrected")
	assert_false(bool(backend.get("adds_view_projection_depth_correction", true)),
			"RenderSceneData.get_view_projection() is already depth-corrected; " +
			"correcting it again makes particles move against scene geometry.")
	assert_eq(String(backend.get("scene_color_copy_policy", "")),
			"before_each_distortion_run")
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
	renderer.render_now(GameWorld.current_frame_clock_ms())

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
	renderer.render_now(GameWorld.current_frame_clock_ms())
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
	renderer.render_now(GameWorld.current_frame_clock_ms())
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
	renderer.render_now(GameWorld.current_frame_clock_ms())
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

	first.render_now(GameWorld.current_frame_clock_ms())
	second.render_now(GameWorld.current_frame_clock_ms())
	assert_not_null(camera.compositor)
	assert_eq(camera.compositor.get_compositor_effects().size(), 6,
			"each renderer registers its two particle passes and its overlay pass once")
	var composed := camera.compositor
	first.render_now(GameWorld.current_frame_clock_ms())
	second.render_now(GameWorld.current_frame_clock_ms())
	assert_eq(camera.compositor, composed,
			"steady-state renders do not clone the composed resource")
	assert_eq(camera.compositor.get_compositor_effects().size(), 6,
			"steady-state renders do not duplicate effects")

	var first_effects := camera.compositor.get_compositor_effects().slice(0, 3)
	assert_true(first_effects[2] is SceneOverlayCompositorEffect,
			"each renderer's overlay pass follows its own particle pair")
	first.shutdown()
	first.shutdown()
	for effect in first_effects.slice(0, 2):
		assert_true(bool(effect.get_backend_report().get("shutdown", false)),
				"explicit shutdown retires each compositor effect exactly once")
	assert_true(bool((first.get_debug_draw_list_report().get("world_overlay_backend", {})
			as Dictionary).get("shutdown", false)),
			"explicit shutdown retires the overlay pass too")
	assert_eq(camera.compositor.get_compositor_effects().size(), 3,
			"shutdown removes only the departing renderer's passes")
	viewport.remove_child(first)
	first.free()
	assert_not_null(camera.compositor)
	assert_eq(camera.compositor.get_compositor_effects().size(), 3,
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
	renderer.render_now(GameWorld.current_frame_clock_ms())
	var live := renderer.get_debug_draw_list_report()
	assert_false(bool(live.get("shutdown", true)), "a fresh renderer is live")
	assert_true(bool(live.get("world_compositor_attached", false)))
	assert_eq(camera.compositor.get_compositor_effects().size(), 3)

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
	renderer.render_now(GameWorld.current_frame_clock_ms())
	var revived := renderer.get_debug_draw_list_report()
	assert_false(bool(revived.get("shutdown", true)), "ENTER_TREE clears the latch")
	for key in [
		"world_far_backend", "world_camera_backend",
		"reflection_far_backend", "reflection_camera_backend",
		"second_scene_far_backend", "second_scene_camera_backend",
		"world_overlay_backend", "reflection_overlay_backend",
		"second_scene_overlay_backend",
	]:
		assert_false(bool((revived.get(key, {}) as Dictionary).get("shutdown", true)),
				"%s is a fresh effect after re-entry" % key)
	assert_true(bool(revived.get("world_compositor_attached", false)),
			"re-entry re-attaches the world camera")
	assert_not_null(camera.compositor)
	assert_eq(camera.compositor.get_compositor_effects().size(), 3,
			"re-entry installs exactly one fresh pass set, no stale effects")
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
	particle_renderer.render_now(GameWorld.current_frame_clock_ms())
	assert_not_null(camera.compositor)
	var effects := camera.compositor.compositor_effects
	assert_eq(effects.size(), 4,
			"far particles + camera particles + the overlay tail + terminal")
	assert_eq(effects[0].effect_callback_type,
			CompositorEffect.EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT,
			"particle pass A draws before the transparent list (before water)")
	assert_eq(effects[1].effect_callback_type,
			CompositorEffect.EFFECT_CALLBACK_TYPE_POST_TRANSPARENT,
			"particle pass B follows water and camera-side alpha")
	# The retail tail (precipitation, coronas, glint, murk, glare) draws after
	# particle pass B and before the frame effects [orig:
	# Terrain_RenderSceneWithReflection @ 0x5c9690 (pass B) -> @ 0x5c96a6 ..
	# @ 0x5c9714; the bloom follows in Render_ProcessMainSceneFrame
	# @ 0x5caa97].
	assert_eq(effects[2].effect_callback_type,
			CompositorEffect.EFFECT_CALLBACK_TYPE_POST_TRANSPARENT,
			"the overlay tail follows particle pass B")
	assert_eq(effects[3].effect_callback_type,
			CompositorEffect.EFFECT_CALLBACK_TYPE_POST_TRANSPARENT,
			"FrameFX plus the display transfer remain terminal")
	assert_true(effects[0] is ParticleCompositorEffect)
	assert_true(effects[1] is ParticleCompositorEffect)
	assert_true(effects[2] is SceneOverlayCompositorEffect)
	assert_true(effects[3] is FrameFxCompositorEffect)


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
	renderer.render_now(GameWorld.current_frame_clock_ms())

	assert_not_null(reflection_camera.compositor)
	assert_eq(reflection_camera.compositor.get_compositor_effects().size(), 3,
			"reflection receives the consecutive far/camera-side pair and its overlay")
	assert_true(reflection_camera.compositor.get_compositor_effects()[2]
			is SceneOverlayCompositorEffect)
	var report := renderer.get_debug_draw_list_report()
	# The mirror's reflected scene draws only its coronas after its particle
	# passes [orig: Water_RenderReflectedWorldScene @ 0x5c85fd].
	assert_eq(String(_slot(report, "reflection_overlay_backend").get("view_kind", "")),
			"mirror")
	assert_eq(String(_slot(report, "world_overlay_backend").get("view_kind", "")),
			"scene")
	assert_true(bool(report.get("reflection_compositor_attached", false)))
	var far_backend: Dictionary = report.get("reflection_far_backend", {})
	var camera_backend: Dictionary = report.get("reflection_camera_backend", {})
	assert_eq(int(far_backend.get("callback_type", -1)), 4)
	assert_eq(int(camera_backend.get("callback_type", -1)), 4)
	assert_false(report.has("world"),
			"the cutover leaves no ambiguous legacy draw-list key")
	assert_false(report.has("world_backend"),
			"the cutover leaves no single-pass backend alias")


# --- The second scene view (the weapon Inset pass) ---
#
# The original renders the Inset aperture through the very scene routine the
# main view runs, particle passes included, so that camera needs the world's
# particles compiled for ITS eye. The device shape is HudInsetScope's: a
# sibling SubViewport sharing the main World3D, one camera, no
# WorldEnvironment of its own.

func _main_view(size := Vector2i(64, 64)) -> SubViewport:
	var viewport := SubViewport.new()
	viewport.size = size
	viewport.own_world_3d = true
	add_child_autofree(viewport)
	return viewport


func _shared_world_camera(main_viewport: SubViewport) -> Camera3D:
	var viewport := SubViewport.new()
	viewport.size = main_viewport.size
	viewport.world_3d = main_viewport.find_world_3d()
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.current = true
	viewport.add_child(camera)
	return camera


# One Bump quad, world-oriented. A Bump graphic takes the vertex-lit colour
# path: its DIFFUSE word is the light vector seen through the compiling view's
# basis. YAWANDPITCH (0x100) orients the quad in the world instead of at the
# camera, so its corners are the same for every view and the ONLY bytes that
# differ between two views' compiles are that lit colour.
func _live_lit_scene() -> EffectScene:
	return _single_quad_scene("lit", 3, "bump_scale = 1;\nflags = YAWANDPITCH;")


# One live quad at (0, 1, 0), through the same PTL loader as mounted effects.
func _single_quad_scene(name: String, blend: int, properties: String = "") -> EffectScene:
	var file := ParticleFixture.parse(_overlap_document(name, blend, properties))
	var scene := EffectScene.new()
	scene.open([file])
	_overlap_spawn(scene, name, 0.0)
	return scene



func _slot(report: Dictionary, key: String) -> Dictionary:
	return report.get(key, {}) as Dictionary


func test_second_scene_camera_receives_its_own_pair_ahead_of_the_terminal() -> void:
	var viewport := _main_view()
	var environment := WorldEnvironment.new()
	environment.environment = Environment.new()
	viewport.add_child(environment)
	var camera := Camera3D.new()
	camera.current = true
	viewport.add_child(camera)
	var frame_renderer := FrameFx.new()
	viewport.add_child(frame_renderer)
	await get_tree().process_frame
	var second_camera := _shared_world_camera(viewport)

	var renderer := ParticleRenderer.new()
	viewport.add_child(renderer)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	assert_null(renderer.get_second_scene_camera())
	assert_null(second_camera.compositor,
			"a renderer without a second scene camera leaves every other camera alone")
	var idle := renderer.get_debug_draw_list_report()
	assert_false(bool(idle.get("second_scene_compositor_attached", true)))
	assert_true(_slot(idle, "second_scene_far_side").is_empty())
	assert_true(_slot(idle, "second_scene_camera_side").is_empty())
	var main_chain := camera.compositor.compositor_effects.duplicate()
	assert_eq(main_chain.size(), 4)

	renderer.set_second_scene_camera(second_camera)
	assert_eq(renderer.get_second_scene_camera(), second_camera)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	assert_not_null(second_camera.compositor)
	if second_camera.compositor == null:
		return
	var effects := second_camera.compositor.compositor_effects
	assert_eq(effects.size(), 4,
			"far particles + camera-side particles + the overlay tail + the scenario's terminal")
	assert_true(effects[0] is ParticleCompositorEffect)
	assert_true(effects[1] is ParticleCompositorEffect)
	assert_true(effects[2] is SceneOverlayCompositorEffect,
			"the scope view runs the scene routine's overlay tail too")
	assert_true(effects[3] is FrameFxCompositorEffect,
			"FrameFX and the display transfer stay terminal in the second view too")
	assert_eq(effects[0].effect_callback_type,
			CompositorEffect.EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT,
			"this view draws the water surface, so its far side precedes the transparent list")
	assert_eq(effects[1].effect_callback_type,
			CompositorEffect.EFFECT_CALLBACK_TYPE_POST_TRANSPARENT)
	assert_eq(effects[3], main_chain[3],
			"the terminal is the one the shared scenario already ran for this camera")
	assert_false(main_chain.has(effects[0]), "the pair is this view's own, not the World pair")
	assert_false(main_chain.has(effects[1]))
	assert_false(main_chain.has(effects[2]))
	assert_eq(camera.compositor.compositor_effects, main_chain,
			"the main camera's chain is untouched by the second view")
	var report := renderer.get_debug_draw_list_report()
	assert_true(bool(report.get("second_scene_compositor_attached", false)))
	assert_true(bool(report.get("second_scene_compositor_inherited_effects", false)),
			"a camera without a compositor inherits the shared scenario's chain")
	assert_eq(int(_slot(report, "second_scene_far_backend").get("callback_type", -1)), 3)
	assert_eq(int(_slot(report, "second_scene_camera_backend").get("callback_type", -1)), 4)

	# Steady state composes once; clearing the camera retires the whole view.
	var composed := second_camera.compositor
	renderer.render_now(GameWorld.current_frame_clock_ms())
	assert_eq(second_camera.compositor, composed,
			"steady-state renders do not clone the composed resource")
	renderer.set_second_scene_camera(null)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	assert_null(second_camera.compositor,
			"clearing the camera restores its inherited (null) compositor")
	report = renderer.get_debug_draw_list_report()
	assert_false(bool(report.get("second_scene_compositor_attached", true)))
	assert_true(_slot(report, "second_scene_far_side").is_empty())
	assert_true(_slot(report, "second_scene_camera_side").is_empty())
	assert_eq(camera.compositor.compositor_effects, main_chain)
	assert_engine_error_count(0)


func test_second_scene_submission_is_compiled_for_its_own_eye() -> void:
	var viewport := _main_view()
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 10.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	# Clearly not the main eye: below the water plane, elsewhere, turned and
	# rolled, under a narrow field of view.
	var second_camera := _shared_world_camera(viewport)
	second_camera.position = Vector3(30.0, -10.0, -20.0)
	second_camera.rotation_degrees = Vector3(15.0, 120.0, 40.0)
	second_camera.fov = 5.0

	var renderer := ParticleRenderer.new()
	renderer.scene = _live_world_scene_at_heights(PackedFloat32Array([-2.0, 0.0, 3.0]))
	renderer.procedural_fallback_enabled = true
	renderer.set_water_plane(0.0, null)
	renderer.set_second_scene_camera(second_camera)
	viewport.add_child(renderer)
	renderer.render_now(GameWorld.current_frame_clock_ms())

	var report := renderer.get_debug_draw_list_report()
	var world_far := _slot(report, "world_far_side")
	var world_near := _slot(report, "world_camera_side")
	var far := _slot(report, "second_scene_far_side")
	var near := _slot(report, "second_scene_camera_side")
	assert_eq(int(world_far.get("selected_emitters", -1)), 1)
	assert_eq(int(world_near.get("selected_emitters", -1)), 2)
	assert_eq(int(far.get("selected_emitters", -1)), 2,
			"the far/camera-side bracket is this view's own: a below-water eye draws "
			+ "the above/equal emitters first")
	assert_eq(int(near.get("selected_emitters", -1)), 1)
	assert_gt(int(far.get("rendered_quad_count", 0)), 0)
	assert_gt(int(near.get("rendered_quad_count", 0)), 0)
	assert_eq(int(far.get("frame_id", -1)), int(world_far.get("frame_id", -2)),
			"every view compiles the one scene snapshot of this render")

	var backend := _slot(report, "second_scene_camera_backend")
	var world_backend := _slot(report, "world_camera_backend")
	assert_gt(int(backend.get("submitted_commands", 0)), 0,
			"the second view publishes a non-empty submission")
	# This view's pass A draws as its own render-list runs (ParticleFarPass).
	assert_gt(int(report.get("second_scene_far_render_runs", 0)), 0)
	var eye: Vector3 = backend.get("submitted_camera_position", Vector3.ZERO)
	var forward: Vector3 = backend.get("submitted_camera_forward", Vector3.ZERO)
	assert_almost_eq(eye, second_camera.global_position, Vector3.ONE * 0.0001,
			"the submission carries the second camera's eye, not the main one")
	assert_almost_eq(forward, -second_camera.global_basis.z, Vector3.ONE * 0.0001)
	assert_almost_eq(world_backend.get("submitted_camera_position", Vector3.ZERO) as Vector3,
			camera.global_position, Vector3.ONE * 0.0001)
	# Same two emitters' worth of quads would match a copied World list; the
	# billboards are built on this camera's right/up, so the bytes differ.
	assert_ne(int(far.get("vertex_checksum", 0)), int(world_near.get("vertex_checksum", 0)),
			"the same emitters compile to different quads for a different eye")
	# The renderer-level totals stay the main view's (a second view of the same
	# emitters must not double them).
	assert_eq(int(renderer.get_rendered_quad_count()),
			int(world_far.get("rendered_quad_count", 0))
			+ int(world_near.get("rendered_quad_count", 0))
			+ int(_slot(report, "first_person").get("rendered_quad_count", 0)))


func test_second_scene_backend_draws_the_particle_into_its_own_target() -> void:
	var viewport := _main_view()
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	var background := WorldEnvironment.new()
	background.environment = Environment.new()
	background.environment.background_mode = Environment.BG_COLOR
	background.environment.background_color = Color.BLACK
	viewport.add_child(background)
	# The main eye looks AWAY from the quad at (0, 1, 0); only the second eye
	# faces it, so a lit centre pixel can only be the second view's draw.
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.0, 5.0)
	camera.rotation_degrees = Vector3(0.0, 180.0, 0.0)
	camera.current = true
	viewport.add_child(camera)
	var second_camera := _shared_world_camera(viewport)
	var second_viewport := second_camera.get_viewport() as SubViewport
	second_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	second_camera.position = Vector3(0.0, 1.0, -5.0)
	second_camera.rotation_degrees = Vector3(0.0, 180.0, 0.0)

	var renderer := ParticleRenderer.new()
	renderer.scene = _single_quad_scene("impact", 0)
	renderer.texture_provider = _overlap_texture  # opaque red
	renderer.set_water_plane(-100.0, null)
	renderer.set_second_scene_camera(second_camera)
	viewport.add_child(renderer)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	for _frame in 5:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()

	var report := renderer.get_debug_draw_list_report()
	var backend := _slot(report, "second_scene_camera_backend")
	assert_gt(int(_slot(report, "second_scene_camera_side").get("rendered_quad_count", 0)), 0)
	if not bool(backend.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_true(bool(backend.get("callback_seen", false)),
			"the second view's post-transparent callback must execute")
	assert_eq(String(backend.get("status", "")), "drawn",
			String(backend.get("failure", "second scene submission failed")))
	assert_eq(int(backend.get("drawn_commands", -1)), int(backend.get("submitted_commands", 0)))
	var second_pixel := second_viewport.get_texture().get_image().get_pixel(32, 32)
	var main_pixel := viewport.get_texture().get_image().get_pixel(32, 32)
	assert_gt(second_pixel.r, 0.95,
			"the world's particle reaches the second view's framebuffer; got %s" % second_pixel)
	assert_lt(main_pixel.r, 0.05,
			"the main eye faces away, so its own target stays background; got %s" % main_pixel)
	assert_engine_error_count(0)


func test_every_view_compiles_a_snapshot_lit_for_its_own_basis() -> void:
	var viewport := _main_view()
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.0, 5.0)
	camera.rotation_degrees = Vector3(-10.0, 20.0, 0.0)
	camera.current = true
	viewport.add_child(camera)
	var mirror_camera := _shared_world_camera(viewport)
	mirror_camera.position = Vector3(4.0, 2.0, -3.0)
	mirror_camera.rotation_degrees = Vector3(25.0, 115.0, 0.0)
	var second_camera := _shared_world_camera(viewport)

	var renderer := ParticleRenderer.new()
	renderer.scene = _live_lit_scene()
	renderer.procedural_fallback_enabled = true
	renderer.set_water_plane(-100.0, mirror_camera)
	viewport.add_child(renderer)

	# The control render: no second view.
	renderer.render_now(GameWorld.current_frame_clock_ms())
	var control := renderer.get_debug_draw_list_report()
	var keys := ["world_far_side", "world_camera_side",
			"reflection_far_side", "reflection_camera_side", "first_person"]
	assert_eq(int(_slot(control, "world_camera_side").get("rendered_quad_count", 0)), 1)
	assert_eq(int(_slot(control, "reflection_camera_side").get("rendered_quad_count", 0)), 1)
	# The precondition that gives the equalities below their teeth: one
	# view-independent quad, so these two lists differ in nothing but the lit
	# colour, i.e. the mirror's relight really does rewrite the shared snapshot.
	assert_ne(int(_slot(control, "world_camera_side").get("vertex_checksum", 0)),
			int(_slot(control, "reflection_camera_side").get("vertex_checksum", 0)),
			"two bases light the same world-oriented quad differently")
	var mirror_chain := mirror_camera.compositor.compositor_effects.duplicate()
	var main_chain := camera.compositor.compositor_effects.duplicate()

	# The second view on the MAIN eye compiles after the mirror relit the shared
	# snapshot for its own basis. Byte-equal to the World list means it was relit
	# back for this basis; a missing relight leaves the mirror's light in it.
	second_camera.global_transform = camera.global_transform
	renderer.set_second_scene_camera(second_camera)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	var as_main := renderer.get_debug_draw_list_report()
	assert_eq(int(_slot(as_main, "second_scene_camera_side").get("vertex_checksum", 0)),
			int(_slot(as_main, "world_camera_side").get("vertex_checksum", 1)),
			"the second view's lit quads are relit for its own basis")
	assert_ne(int(_slot(as_main, "second_scene_camera_side").get("vertex_checksum", 0)),
			int(_slot(as_main, "reflection_camera_side").get("vertex_checksum", 0)))

	# On the MIRROR eye it must equal the mirror list instead.
	second_camera.global_transform = mirror_camera.global_transform
	renderer.render_now(GameWorld.current_frame_clock_ms())
	var as_mirror := renderer.get_debug_draw_list_report()
	assert_eq(int(_slot(as_mirror, "second_scene_camera_side").get("vertex_checksum", 0)),
			int(_slot(as_mirror, "reflection_camera_side").get("vertex_checksum", 1)))

	# Neither the World nor the mirror group draws anything different for the
	# second view's presence, wherever it looks.
	for with_second: Dictionary in [as_main, as_mirror]:
		for key: String in keys:
			for field: String in ["vertex_checksum", "rendered_quad_count",
					"draw_command_count", "selected_emitters"]:
				assert_eq(int(_slot(with_second, key).get(field, -1)),
						int(_slot(control, key).get(field, -2)),
						"%s.%s is unchanged by the second scene view" % [key, field])
	assert_eq(camera.compositor.compositor_effects, main_chain)
	assert_eq(mirror_camera.compositor.compositor_effects, mirror_chain)


func test_second_scene_view_follows_every_renderer_lifecycle_leg() -> void:
	var viewport := _main_view()
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var second_camera := _shared_world_camera(viewport)
	second_camera.position = Vector3(0.0, 0.0, 9.0)
	var renderer := ParticleRenderer.new()
	renderer.scene = _live_world_scene()
	renderer.procedural_fallback_enabled = true
	renderer.set_second_scene_camera(second_camera)
	viewport.add_child(renderer)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	assert_not_null(second_camera.compositor)
	assert_gt(int(_slot(renderer.get_debug_draw_list_report(),
			"second_scene_camera_side").get("rendered_quad_count", 0)), 0)

	# The master switch drops this view's submissions with the others.
	renderer.hidden = true
	var hidden := renderer.get_debug_draw_list_report()
	assert_true(_slot(hidden, "second_scene_camera_side").is_empty())
	assert_eq(int(_slot(hidden, "second_scene_camera_backend").get("submitted_commands", -1)), 0)
	renderer.hidden = false
	renderer.render_now(GameWorld.current_frame_clock_ms())
	assert_gt(int(_slot(renderer.get_debug_draw_list_report(),
			"second_scene_camera_backend").get("submitted_commands", 0)), 0)

	# EXIT_TREE retires and detaches the pair; re-entry renders it again.
	viewport.remove_child(renderer)
	var retired := renderer.get_debug_draw_list_report()
	for key in ["second_scene_far_backend", "second_scene_camera_backend"]:
		assert_true(bool(_slot(retired, key).get("shutdown", false)),
				"%s is retired on exit" % key)
	assert_false(bool(retired.get("second_scene_compositor_attached", true)))
	assert_null(second_camera.compositor, "the departing renderer restores the second camera")
	viewport.add_child(renderer)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	var revived := renderer.get_debug_draw_list_report()
	for key in ["second_scene_far_backend", "second_scene_camera_backend"]:
		assert_false(bool(_slot(revived, key).get("shutdown", true)),
				"%s is a fresh effect after re-entry" % key)
	assert_true(bool(revived.get("second_scene_compositor_attached", false)),
			"the retained camera re-attaches on re-entry")
	assert_eq(second_camera.compositor.get_compositor_effects().size(), 3,
			"the view's particle pair plus its overlay pass")

	# A camera freed while attached retires the view on the next render.
	second_camera.free()
	renderer.render_now(GameWorld.current_frame_clock_ms())
	var orphaned := renderer.get_debug_draw_list_report()
	assert_false(bool(orphaned.get("second_scene_compositor_attached", true)))
	assert_true(_slot(orphaned, "second_scene_camera_side").is_empty())
	assert_null(renderer.get_second_scene_camera())
	assert_engine_error_count(0)


func _overlap_texture(name: String) -> Texture2D:
	var image := Image.create(32, 32, false, Image.FORMAT_RGBA8)
	var color := Color(0.5, 0.5, 1.0, 1.0)
	if name == "impact.tga":
		color = Color.RED
	elif name == "smoke.tga":
		color = Color(0.0, 1.0, 0.0, 0.5)
	image.fill(color)
	return ImageTexture.create_from_image(image)


func _overlap_document(name: String, blend: int, properties: String = "") -> String:
	var modes := ["blend", "additive", "premult", "bump", "mod", "mod2x", "bumpadd", "distort"]
	var authored := "emit_dur = 0.1;\nemit_rate = 10;\nemit_burst = 1;\nage = 100;\nalpha = 1;\n"
	for index in range(1, 5):
		authored += "color%d = 255, 255, 255;\n" % index
	authored += "graphic1 = %s.tga, %s;\ng1_alpha = 1;\ng1_scale = 1;\n" % [name, modes[blend]]
	return ParticleFixture.definition(name, authored + properties) + ParticleFixture.effect(name, [name])



func _overlap_spawn(scene: EffectScene, name: String, depth: float) -> void:
	var pose := Transform3D.IDENTITY
	pose.origin = Vector3(0.0, 1.0, depth)
	assert_eq(scene.spawn(EffectSpawnRequest.make(scene.intern(name), pose)).status,
			EffectScene.SPAWN_STATUS_SPAWNED)
	scene.advance_in_place(0.1)


func _overlap_image(viewport: SubViewport, renderer: ParticleRenderer) -> Image:
	renderer.render_now(GameWorld.current_frame_clock_ms())
	for frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return viewport.get_texture().get_image()


func test_muzzle_distortion_preserves_particles_already_drawn_behind_it() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 128)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var background := WorldEnvironment.new()
	background.environment = Environment.new()
	background.environment.background_mode = Environment.BG_COLOR
	background.environment.background_color = Color.BLACK
	viewport.add_child(background)
	var file := ParticleFixture.parse(_overlap_document("impact", 0)
			+ _overlap_document("smoke", 0) + _overlap_document("haze", 7))
	var scene := EffectScene.new()
	scene.open([file])
	var renderer := ParticleRenderer.new()
	renderer.scene = scene
	renderer.texture_provider = _overlap_texture
	renderer.set_water_plane(-100.0, null)
	viewport.add_child(renderer)

	_overlap_spawn(scene, "impact", 0.0)
	var before := await _overlap_image(viewport, renderer)
	assert_gt(before.get_pixel(64, 64).r, 0.95, "the distant impact is visible")
	_overlap_spawn(scene, "haze", 1.0)
	var after := await _overlap_image(viewport, renderer)
	assert_almost_eq(after.get_pixel(64, 64).r, before.get_pixel(64, 64).r, 0.05,
			"neutral muzzle haze must preserve the impact behind it")

	# A color draw between two distortion runs must enter the next snapshot.
	_overlap_spawn(scene, "smoke", 2.0)
	var smoke := await _overlap_image(viewport, renderer)
	var expected := smoke.get_pixel(64, 64)
	assert_gt(expected.g, 0.5, "the intervening smoke contributes green")
	_overlap_spawn(scene, "haze", 3.0)
	var overlapping := await _overlap_image(viewport, renderer)
	var actual := overlapping.get_pixel(64, 64)
	assert_almost_eq(actual.r, expected.r, 0.05, "near haze preserves the red impact")
	assert_almost_eq(actual.g, expected.g, 0.05, "near haze preserves intervening smoke")
	var backend: Dictionary = renderer.get_debug_draw_list_report().get("world_camera_backend", {})
	assert_eq(String(backend.get("status", "")), "drawn", String(backend.get("failure", "")))
	assert_eq(int(backend.get("drawn_commands", -1)), int(backend.get("submitted_commands", 0)))
	assert_eq(int(backend.get("scene_color_copies", 0)), 2,
			"each ordered distortion run samples the preceding particle draws")


func _asymmetric_dirt_texture(_name: String) -> Texture2D:
	var image := Image.create(32, 32, false, Image.FORMAT_RGBA8)
	for y in image.get_height():
		for x in image.get_width():
			image.set_pixel(x, y, Color(1.0, 0.0, 0.0, 1.0 - float(y) / 31.0))
	return ImageTexture.create_from_image(image)


func test_dirt_splash_keeps_its_dense_base_below_its_fading_top() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 128)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var background := WorldEnvironment.new()
	background.environment = Environment.new()
	background.environment.background_mode = Environment.BG_COLOR
	background.environment.background_color = Color.BLACK
	viewport.add_child(background)
	var file := ParticleFixture.parse(_overlap_document("dirt", 0, "g1_scale = 2;"))
	var scene := EffectScene.new()
	scene.open([file])
	var renderer := ParticleRenderer.new()
	renderer.scene = scene
	renderer.texture_provider = _asymmetric_dirt_texture
	renderer.set_water_plane(-100.0, null)
	viewport.add_child(renderer)
	_overlap_spawn(scene, "dirt", 0.0)
	var image := await _overlap_image(viewport, renderer)
	var top := Vector2i(camera.unproject_position(Vector3(0.0, 1.7, 0.0)))
	var base := Vector2i(camera.unproject_position(Vector3(0.0, 0.3, 0.0)))
	# The expansion's drtspl textures put the dense base at source V=0.
	# Retail maps that edge to negative local Y, below the fading V=1 tail.
	assert_gt(image.get_pixelv(base).r, 0.8, "the dense source edge belongs at the base of the splash")
	assert_lt(image.get_pixelv(top).r, image.get_pixelv(base).r - 0.2,
			"the upper plume must fade instead of showing the texture's dense cut edge")
	assert_engine_error_count(0)


# A Bump graphic's material carries SPECULARENABLE instead of FOGENABLE
# (retail CParticleTexture_InitTextureAndChannels @ 0x5E8424 stores 0x10000 as
# its intrinsic pass word): the SPECULAR vertex colour (the modulated particle
# RGB) adds after the DOT3 stages and full scene fog leaves it untinted.
func test_bump_adds_its_specular_colour_and_does_not_fog() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 128)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var background := WorldEnvironment.new()
	background.environment = Environment.new()
	background.environment.background_mode = Environment.BG_COLOR
	background.environment.background_color = Color.BLACK
	viewport.add_child(background)
	var renderer := ParticleRenderer.new()
	renderer.scene = _live_lit_scene()
	renderer.texture_provider = _overlap_texture
	renderer.set_water_plane(-100.0, null)
	var fog_source := _live_fog_source()
	viewport.add_child(fog_source)
	renderer.environment_source = fog_source
	viewport.add_child(renderer)
	var image := await _overlap_image(viewport, renderer)
	var center := image.get_pixel(64, 64)
	var backend: Dictionary = renderer.get_debug_draw_list_report().get("world_camera_backend", {})
	assert_eq(String(backend.get("status", "")), "drawn", String(backend.get("failure", "")))
	assert_eq(String(backend.get("fog_material_targets", "")),
			"scene,black,black,none,white,gray127,none,none")
	assert_gt(center.r, 0.9, "the specular add lifts the lit bump to the white particle colour: %s" % center)
	assert_almost_eq(center.b, center.r, 0.03,
			"the fog colour (0.2, 0.3, 0.4) must not tint an unfogged bump: %s" % center)
	assert_engine_error_count(0)


func _thermal_view_source(thermal: bool) -> MissionEnvironment:
	var data := EnvFile.new()
	data.reset_to_default()
	data.fog_level = 0.0
	var environment := MissionEnvironment.new()
	environment.environment_data = data
	environment.set_thermal_view(thermal, false)
	return environment


func _thermal_pixel(blend: int, thermal: bool, background: Color) -> Color:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 128)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var clear := WorldEnvironment.new()
	clear.environment = Environment.new()
	clear.environment.background_mode = Environment.BG_COLOR
	clear.environment.background_color = background
	viewport.add_child(clear)
	var renderer := ParticleRenderer.new()
	renderer.scene = _single_quad_scene("thermal", blend)
	renderer.texture_provider = _overlap_texture
	renderer.set_water_plane(-100.0, null)
	var source := _thermal_view_source(thermal)
	viewport.add_child(source)
	renderer.environment_source = source
	viewport.add_child(renderer)
	var image := await _overlap_image(viewport, renderer)
	return image.get_pixel(64, 64)


# A thermal-view frame binds each texture's secondary material
# (retail CParticleBatch_FlushAndBindMaterial @ 0x5E42BF): Blend inverts its
# colour, MODULATE(1 - TEXTURE, 1 - DIFFUSE), and Additive darkens under
# ZERO/INVSRCCOLOR (retail CParticleTexture_InitTextureAndChannels
# @ 0x5E8584 / @ 0x5E8390).
func test_thermal_frames_bind_the_secondary_particle_materials() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	# The texel is (0.5, 0.5, 1.0): its blue complement is zero.
	var blend := await _thermal_pixel(0, false, Color.BLACK)
	var blend_thermal := await _thermal_pixel(0, true, Color.BLACK)
	assert_gt(blend.b, 0.3, "the primary Blend material shows the texel: %s" % blend)
	assert_lt(blend_thermal.b, 0.05, "the thermal Blend inverts: 1 - T.b = 0: %s" % blend_thermal)
	var grey := Color(0.5, 0.5, 0.5)
	var additive := await _thermal_pixel(1, false, grey)
	var additive_thermal := await _thermal_pixel(1, true, grey)
	assert_gt(additive.b, 0.55, "the primary Additive material brightens: %s" % additive)
	assert_lt(additive_thermal.b, 0.45,
			"the thermal Additive material darkens the grey behind it: %s" % additive_thermal)
	assert_engine_error_count(0)
