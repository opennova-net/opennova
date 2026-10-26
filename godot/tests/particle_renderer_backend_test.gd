extends GutTest


func _live_world_scene_at_heights(heights: PackedFloat32Array) -> EffectScene:
	var file := ParticleFixture.catalog(self, "GPU particle",
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
	# The eye looks at the emitters: one out of its view never submits (retail
	# CParticleEmitter_TrySubmitForRender @ 0x5e7540).
	camera.look_at(Vector3.ZERO, Vector3.UP)
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
	camera.look_at(Vector3.ZERO, Vector3.UP)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	report = renderer.get_debug_draw_list_report()
	far = report.get("world_far_side", {})
	near = report.get("world_camera_side", {})
	assert_eq(int(far.get("selected_emitters", -1)), 2,
			"a below-water camera draws above/equal emitters first")
	assert_eq(int(near.get("selected_emitters", -1)), 1,
			"the strict-below emitter moves to the camera-side pass")


# The render submit reads the view's own eye: an emitter behind the camera never joins
# the render list, so it is neither partitioned, depth-sorted nor built, and turning the
# eye toward it submits it (retail CParticleEmitter_TrySubmitForRender @ 0x5e7540 tests
# the emitter's bounds against the view before it registers).
func test_an_emitter_out_of_the_view_never_submits() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var renderer := ParticleRenderer.new()
	# One emitter ahead of the eye (looking down -Z), one 15 m behind it.
	var scene := _live_world_scene_at_heights(PackedFloat32Array())
	for z in [0.0, 20.0]:
		var transform := Transform3D.IDENTITY
		transform.origin.z = z
		assert_eq(scene.spawn(EffectSpawnRequest.make(scene.intern("GPU effect"),
				transform)).status, EffectScene.SPAWN_STATUS_SPAWNED)
	scene.advance_in_place(0.1)
	renderer.scene = scene
	renderer.procedural_fallback_enabled = true
	viewport.add_child(renderer)
	renderer.render_now(GameWorld.current_frame_clock_ms())

	var near: Dictionary = renderer.get_debug_draw_list_report().get("world_camera_side", {})
	assert_eq(int(near.get("selected_emitters", -1)), 1, "the emitter ahead submits")
	assert_eq(int(near.get("frustum_culled_emitters", -1)), 1,
			"the emitter behind the eye never submits")
	assert_gt(int(near.get("rendered_quad_count", 0)), 0)

	camera.look_at(Vector3(0.0, 0.0, 20.0), Vector3.UP)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	near = renderer.get_debug_draw_list_report().get("world_camera_side", {})
	assert_eq(int(near.get("selected_emitters", -1)), 1, "turned around, the other submits")
	assert_eq(int(near.get("frustum_culled_emitters", -1)), 1)


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
	# Terrain_RenderWorldScene @ 0x5c9690 (pass B) -> @ 0x5c96a6 ..
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
	# The mirror's reflected scene draws its tracer ribbons, then its coronas,
	# after its particle passes [orig: Water_RenderReflectedWorldScene @ 0x5c85f6,
	# @ 0x5c85fd].
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
	var file := ParticleFixture.parse(self, _overlap_document(name, blend, properties))
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
	# Each eye looks at the emitters: one out of a view never submits there (retail
	# CParticleEmitter_TrySubmitForRender @ 0x5e7540).
	camera.look_at(Vector3.ZERO, Vector3.UP)
	# Clearly not the main eye: below the water plane, elsewhere, turned and
	# rolled, under a narrower field of view.
	var second_camera := _shared_world_camera(viewport)
	second_camera.position = Vector3(30.0, -10.0, -20.0)
	second_camera.look_at(Vector3(0.0, 0.5, 0.0), Vector3.UP)
	second_camera.rotate_object_local(Vector3.FORWARD, deg_to_rad(40.0))
	second_camera.fov = 30.0

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


func _overlap_source_image(name: String) -> Image:
	var image := Image.create(32, 32, false, Image.FORMAT_RGBA8)
	var color := Color(0.5, 0.5, 1.0, 1.0)
	if name == "impact.tga":
		color = Color.RED
	image.fill(color)
	return image


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


# The distortion row's content gate counts every class-7 child of a visible
# group, ahead of the render submit's live-count and view tests (retail
# EffectWorld_HasDistortionParticles @ 0x5F6640 tests neither): a haze behind
# the eye compiles no quad, yet the row still runs.
func test_an_out_of_view_haze_still_runs_the_distortion_row() -> void:
	var viewport := _main_view()
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var scene := EffectScene.new()
	scene.open([ParticleFixture.parse(self, _overlap_document("haze", 7))])
	var renderer := ParticleRenderer.new()
	renderer.scene = scene
	renderer.procedural_fallback_enabled = true
	renderer.set_water_plane(-100.0, null)
	viewport.add_child(renderer)
	_overlap_spawn(scene, "haze", 20.0)
	renderer.render_now(GameWorld.current_frame_clock_ms())

	var report := renderer.get_debug_draw_list_report()
	var distortion := _slot(report, "distortion")
	assert_eq(int(distortion.get("selected_emitters", -1)), 0, "the haze behind the eye never submits")
	assert_eq(int(distortion.get("frustum_culled_emitters", -1)), 1)
	assert_true(bool(report.get("distortion_present", false)),
			"the row runs for any class-7 emitter, in view or not")


# Each water mirror's reflected scene culls by the mirror's own eye: retail
# Render_MainScene rebuilds g_MainViewport from the mirrored camera block over
# the RTT's square rect, at the pass's horizontal fov, ahead of
# Water_RenderReflectedWorldScene, which builds none (D-NET-NEW-L3-36 follow-up
# 3). So an emitter only the main eye sees never submits to the mirror's passes.
func test_each_mirror_culls_by_its_own_eye_over_a_square() -> void:
	# A mirror's reflected scene builds no viewport of its own: its passes submit against the
	# mirror eye Render_MainScene rebuilt last (the mirrored block, the RTT's square rect, the
	# pass's horizontal fov; engine renderer/particle_frame.h ParticleViewInput::frustum).
	var viewport := _main_view()
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var mirror_camera := _shared_world_camera(viewport)
	mirror_camera.position = Vector3(0.0, 0.0, 5.0)
	mirror_camera.rotation_degrees = Vector3(0.0, 180.0, 0.0)
	var renderer := ParticleRenderer.new()
	renderer.scene = _live_world_scene()
	renderer.procedural_fallback_enabled = true
	renderer.set_water_plane(-100.0, mirror_camera)
	viewport.add_child(renderer)
	renderer.render_now(GameWorld.current_frame_clock_ms())

	var report := renderer.get_debug_draw_list_report()
	assert_eq(int(_slot(report, "world_camera_side").get("selected_emitters", -1)), 1)
	assert_eq(int(_slot(report, "reflection_camera_side").get("selected_emitters", -1)), 0,
			"the mirror's eye faces away: its pass submits nothing the main eye sees")
	assert_eq(int(_slot(report, "reflection_camera_side").get("frustum_culled_emitters", -1)), 1)

	# The square: a wide mirror frame whose own field is short vertically still
	# submits an emitter inside its horizontal fov's square.
	(mirror_camera.get_viewport() as SubViewport).size = Vector2i(128, 32)
	mirror_camera.keep_aspect = Camera3D.KEEP_WIDTH
	mirror_camera.fov = 60.0
	mirror_camera.rotation_degrees = Vector3.ZERO
	mirror_camera.position = Vector3(0.0, -2.5, 5.0)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	report = renderer.get_debug_draw_list_report()
	assert_eq(int(_slot(report, "reflection_camera_side").get("selected_emitters", -1)), 1,
			"26.6 degrees up: outside the frame's 8.2-degree half height, inside the 30-degree square")

	# The mirror's passes run inside its reflected scene, which a frame renders only while
	# its water pass runs (retail Render_TerrainScene's g_WaterActive test ahead of
	# Water_ReflectionPrerender): a mirror the water left unrendered compiles none.
	(mirror_camera.get_viewport() as SubViewport).render_target_update_mode = \
			SubViewport.UPDATE_DISABLED
	renderer.render_now(GameWorld.current_frame_clock_ms())
	report = renderer.get_debug_draw_list_report()
	assert_true(_slot(report, "reflection_camera_side").is_empty(),
			"an unrendered mirror compiles no pass")
	assert_true(_slot(report, "reflection_far_side").is_empty())
	assert_eq(int(_slot(report, "world_camera_side").get("selected_emitters", -1)), 1)


func test_the_inset_mirror_compiles_its_own_pair_while_it_renders() -> void:
	# The weapon Inset pass's own prerender runs the reflected scene again, its particle
	# pair for its own eye (engine environment/water_mirror.h inset_mirror_source,
	# renderer/particle_frame.h compiles_mirror_particles), only on a frame whose Inset
	# pass renders that mirror.
	var viewport := _main_view()
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var inset_mirror := _shared_world_camera(viewport)
	(inset_mirror.get_viewport() as SubViewport).size = Vector2i(64, 64)
	inset_mirror.keep_aspect = Camera3D.KEEP_WIDTH
	inset_mirror.fov = 9.5
	inset_mirror.position = Vector3(0.0, 0.0, 5.0)
	var renderer := ParticleRenderer.new()
	renderer.scene = _live_world_scene()
	renderer.procedural_fallback_enabled = true
	renderer.set_water_plane(-100.0, null)
	renderer.set_inset_mirror_camera(inset_mirror)
	viewport.add_child(renderer)
	renderer.render_now(GameWorld.current_frame_clock_ms())

	var report := renderer.get_debug_draw_list_report()
	assert_eq(renderer.get_inset_mirror_camera(), inset_mirror)
	assert_true(bool(report.get("inset_mirror_compositor_attached", false)))
	# Its overlay pass follows its pair and leads with its own tracer ribbons, then
	# the mirror's tail (engine renderer/scene_overlay.h kInsetMirrorOverlayOrder).
	assert_not_null(inset_mirror.compositor)
	var effects: Array = []
	if inset_mirror.compositor != null:
		effects = inset_mirror.compositor.get_compositor_effects()
	assert_eq(effects.size(), 3, "the Inset mirror's pair, then its overlay pass")
	if effects.size() == 3:
		assert_true(effects[2] is SceneOverlayCompositorEffect)
	assert_eq(String(_slot(report, "inset_mirror_overlay_backend").get("view_kind", "")),
			"inset_mirror")
	assert_eq(int(_slot(report, "inset_mirror_camera_side").get("selected_emitters", -1)), 1,
			"the Inset mirror's own eye sees the emitter")
	assert_true(_slot(report, "reflection_camera_side").is_empty(), "no main mirror this frame")

	# Its narrow square culls by its own eye.
	inset_mirror.position = Vector3(3.0, 0.0, 5.0)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	report = renderer.get_debug_draw_list_report()
	assert_eq(int(_slot(report, "inset_mirror_camera_side").get("selected_emitters", -1)), 0)
	assert_eq(int(_slot(report, "inset_mirror_camera_side").get("frustum_culled_emitters", -1)), 1)

	# The Inset pass left its mirror unrendered: no pair.
	(inset_mirror.get_viewport() as SubViewport).render_target_update_mode = \
			SubViewport.UPDATE_DISABLED
	renderer.render_now(GameWorld.current_frame_clock_ms())
	report = renderer.get_debug_draw_list_report()
	assert_true(_slot(report, "inset_mirror_camera_side").is_empty())
	assert_true(_slot(report, "inset_mirror_far_side").is_empty())
	assert_eq(int(_slot(report, "world_camera_side").get("selected_emitters", -1)), 1)

	renderer.set_inset_mirror_camera(null)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	report = renderer.get_debug_draw_list_report()
	assert_false(bool(report.get("inset_mirror_compositor_attached", true)),
			"no Inset mirror: its pair detaches")


# Every atlas page carries the levels retail builds it with: 0x180000, so at
# most three box-filtered levels, and the materials bound the stage to the last
# of them, sampled bilinear on the nearest level (D-RMAT-23; retail
# CParticleTexture_InitTextureAndChannels @ 0x5E82B2..0x5E82EB,
# GTexture_CreateFromPixelData_0 @ 0x6877F2..0x687801).
func test_atlas_pages_carry_their_three_point_mip_levels() -> void:
	for blend in [0, 3]:
		var viewport := SubViewport.new()
		viewport.size = Vector2i(64, 64)
		viewport.own_world_3d = true
		add_child_autofree(viewport)
		var camera := Camera3D.new()
		camera.position = Vector3(0.0, 1.0, 5.0)
		camera.current = true
		viewport.add_child(camera)
		var renderer := ParticleRenderer.new()
		renderer.scene = _single_quad_scene("mips%d" % blend, blend)
		renderer.image_provider = _overlap_source_image
		viewport.add_child(renderer)
		renderer.render_now(GameWorld.current_frame_clock_ms())
		var pages: Array = renderer.get_debug_draw_list_report().get("atlas_pages", [])
		assert_eq(pages.size(), 1, "one page for the one graphic (blend %d)" % blend)
		for value in pages:
			var page: Dictionary = value
			assert_eq(int(page.get("side", 0)), 1024 if blend == 0 else 256,
					"the page side by the graphic's type")
			assert_eq(int(page.get("levels", 0)), 3, "three retail levels (blend %d)" % blend)
			assert_eq(float(page.get("max_lod", -1.0)), 2.0,
					"the stage stops at level 2 (blend %d)" % blend)
			assert_true(bool(page.get("texture_mipmaps", false)),
					"the page texture carries a chain (blend %d)" % blend)


# The pages follow the session's two game.cfg words (D-RMAT-24): DXT5 at a
# texcompression_level of 1 or less (the fresh profile's 1), halved once at a
# particle_density of 1 or less, three levels and the stage's level-2 bound
# either way (retail CParticleTexture_InitTextureAndChannels @ 0x5E82B2..0x5E82CC).
func test_atlas_pages_follow_the_session_compression_and_density() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var renderer := ParticleRenderer.new()
	renderer.scene = _single_quad_scene("pagewords", 0)
	renderer.image_provider = _overlap_source_image
	viewport.add_child(renderer)
	var cases := [
		# [texcompression, density, flags, dxt5, device side]
		[GameWorld.texcompression_level_fresh_profile(), GameWorld.particle_density_fresh_profile(),
				0x180200, true, 1024],
		[2, 2, 0x180000, false, 1024],
		[2, 1, 0x190000, false, 512],
		[0, 0, 0x190200, true, 512],
	]
	for words in cases:
		renderer.set_session_render_settings(words[0], words[1])
		renderer.render_now(GameWorld.current_frame_clock_ms())
		var report := renderer.get_debug_draw_list_report()
		assert_eq(int(report.get("atlas_page_flags", 0)), words[2], "the page flags for %s" % [words])
		var pages: Array = report.get("atlas_pages", [])
		assert_eq(pages.size(), 1)
		for value in pages:
			var page: Dictionary = value
			assert_eq(bool(page.get("dxt5", false)), words[3], "the page format for %s" % [words])
			assert_eq(int(page.get("side", 0)), 1024, "the packed side stays the type's")
			assert_eq(int(page.get("device_side", 0)), words[4], "the device side for %s" % [words])
			assert_eq(int(page.get("levels", 0)), 3, "three levels for %s" % [words])
			assert_eq(float(page.get("max_lod", -1.0)), 2.0, "level 2 bounds the stage")
			assert_true(bool(page.get("texture_mipmaps", false)), "the page texture is mipmapped")


func _solid_image(color: Color) -> Image:
	var image := Image.create(32, 32, false, Image.FORMAT_RGBA8)
	image.fill(color)
	return image


func _red_image(_name: String) -> Image:
	return _solid_image(Color.RED)


func _green_image(_name: String) -> Image:
	return _solid_image(Color.GREEN)


# The page textures pass A's render-list runs bind (ParticleFarPass: the
# renderer's visible ParticleFarRun children, one material per run).
func _far_run_textures(renderer: ParticleRenderer) -> Array:
	var textures := []
	for node in renderer.find_children("ParticleFarRun*", "MeshInstance3D", true, false):
		var run := node as MeshInstance3D
		if not run.visible or run.mesh == null or run.mesh.get_surface_count() == 0:
			continue
		var material := run.mesh.surface_get_material(0) as ShaderMaterial
		textures.append(material.get_shader_parameter("albedo_tex"))
	return textures


# A catalog rebuild (here a new image provider) builds new atlas pages, and
# pass A's runs draw with them: the far-pass materials are made again with the
# pages, never kept from the catalog they were first made for.
func test_far_pass_materials_follow_a_catalog_rebuild() -> void:
	var viewport := _main_view()
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 10.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	# The eye looks at the quad: an emitter out of its view never submits, so it
	# would run no pass at all (retail CParticleEmitter_TrySubmitForRender
	# @ 0x5e7540 tests the bounds against the view before it registers).
	camera.look_at(Vector3(0.0, 1.0, 0.0), Vector3.UP)
	var renderer := ParticleRenderer.new()
	renderer.scene = _single_quad_scene("rebuilt", 0)
	renderer.image_provider = _red_image
	# The quad at y 1 sits below the plane and the eye above it: pass A. The
	# uncompressed page words keep the page readable.
	renderer.set_water_plane(5.0, null)
	renderer.set_session_render_settings(2, 2)
	viewport.add_child(renderer)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	assert_gt(int(renderer.get_debug_draw_list_report().get("world_far_render_runs", 0)), 0)
	var before := _far_run_textures(renderer)
	assert_eq(before.size(), 1, "one pass-A run")

	renderer.image_provider = _green_image
	renderer.render_now(GameWorld.current_frame_clock_ms())
	var after := _far_run_textures(renderer)
	assert_eq(after.size(), 1, "one pass-A run after the rebuild")
	if before.size() != 1 or after.size() != 1:
		return
	assert_ne(after[0], before[0], "pass A binds the rebuilt page, not the old one")
	# The rebuilt page holds the new graphic where the atlas placed it.
	var entry := {}
	for value in renderer.get_debug_draw_list_report().get("atlas_entries", []):
		if bool((value as Dictionary).get("resolved", false)):
			entry = value
	assert_false(entry.is_empty(), "the graphic resolved")
	var image := (after[0] as Texture2D).get_image()
	assert_not_null(image)
	if entry.is_empty() or image == null:
		return
	var rect: Rect2i = entry.get("pixel_rect", Rect2i())
	var centre := image.get_pixel(rect.position.x + rect.size.x / 2,
			rect.position.y + rect.size.y / 2)
	assert_gt(centre.g, 0.9, "pass A samples the green graphic")
	assert_lt(centre.r, 0.1, "not the red one the first catalog held")
	assert_engine_error_count(0)


# Every view builds one particle in its scene pass's stride (D-RMAT-24): the
# main scene's 1, 2 or 4 at particle_density 2, 1 or 0, twice that while the
# main view renders as the NVG scene (retail Render_ProcessMainSceneFrame
# @ 0x5CA8B8..0x5CA8DE, NVG_RenderSceneToTarget @ 0x5D088F..0x5D08B5,
# CParticleEmitter_BuildBillboardQuads @ 0x5E6DA2..0x5E6E06).
func test_views_build_particles_in_the_scene_pass_stride() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var authored := "emit_dur = 0.1;\nemit_rate = 10;\nemit_burst = 8;\nage = 100;\nalpha = 1;\n"
	for index in range(1, 5):
		authored += "color%d = 255, 255, 255;\n" % index
	authored += "graphic1 = stride.tga, blend;\ng1_alpha = 1;\ng1_scale = 1;\n"
	var file := ParticleFixture.parse(self,
			ParticleFixture.definition("stride", authored) + ParticleFixture.effect("stride", ["stride"]))
	var scene := EffectScene.new()
	scene.open([file])
	_overlap_spawn(scene, "stride", 0.0)
	var renderer := ParticleRenderer.new()
	renderer.scene = scene
	renderer.image_provider = _overlap_source_image
	viewport.add_child(renderer)
	var built := {}
	for words in [[2, false], [1, false], [0, false], [2, true], [0, true]]:
		renderer.set_session_render_settings(2, words[0])
		renderer.set_main_view_nvg_scene(words[1])
		renderer.render_now(GameWorld.current_frame_clock_ms())
		var report := renderer.get_debug_draw_list_report()
		var quads := 0
		var skipped := 0
		for key in ["world_far_side", "world_camera_side"]:
			quads += int(_slot(report, key).get("rendered_quad_count", 0))
			skipped += int(_slot(report, key).get("lod_skipped_particles", 0))
		built[words] = quads
		if words == [2, false]:
			assert_eq(skipped, 0, "density 2 in the main scene skips nothing")
	# The emitter's serials run 0..n-1, so a stride of d builds ceil(n / d).
	var n: int = built[[2, false]]
	assert_gte(n, 8, "the burst's particles all build at density 2")
	assert_eq(built[[1, false]], ceili(n / 2.0), "density 1: one in two")
	assert_eq(built[[0, false]], ceili(n / 4.0), "density 0: one in four")
	assert_eq(built[[2, true]], ceili(n / 2.0), "the NVG scene at density 2: one in two")
	assert_eq(built[[0, true]], ceili(n / 8.0), "the NVG scene at density 0: one in eight")
