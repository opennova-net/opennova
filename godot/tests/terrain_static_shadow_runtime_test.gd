extends GutTest

const DVXI5_TRN := "res://../fixtures/godot/dvxi5/Dvxi5.trn"
const HOUSE_3DI := "res://../fixtures/threedi/3di3/House.3di"


static func _terrain_light_epoch(light: Vector3) -> Vector3i:
	# Independent test oracle for retail's truncated (g2,g0,g1) terrain bytes.
	return Vector3i(
		int((clampf(light.z, -1.0, 1.0) + 1.0) * 127.5),
		int((clampf(light.x, -1.0, 1.0) + 1.0) * 127.5),
		int((clampf(light.y, -1.0, 1.0) + 1.0) * 127.5))


func _settle_tile_cache(terrain: Terrain) -> Dictionary:
	var diagnostics: Dictionary = {}
	for _attempt in range(512):
		terrain.render_frame()
		diagnostics = terrain.get_tile_cache_diagnostics()
		if int(diagnostics.get("pending_jobs", -1)) == 0 \
				and int(diagnostics.get("frame_requests", 0)) > 0 \
				and int(diagnostics.get("frame_ready_hits", -1)) \
						== int(diagnostics.get("frame_requests", 0)):
			return diagnostics
		await get_tree().process_frame
	assert_true(false, "the bounded terrain compiler must settle visible pages")
	return diagnostics


func test_replacing_terrain_data_cancels_old_jobs_without_borrowing_old_receiver_memory() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)
	var old_data: TerrainData = TerrainData.new()
	old_data.set_trn_path(ProjectSettings.globalize_path(DVXI5_TRN))
	assert_eq(old_data.load(), OK)
	var terrain := Terrain.new()
	viewport.add_child(terrain)
	terrain.set_terrain_data(old_data)
	terrain.build()
	terrain.set_debug_no_frustum(true)
	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()

	var object_data := ObjectData.new()
	assert_eq(object_data.open_file(ProjectSettings.globalize_path(HOUSE_3DI)), OK)
	var placer := MissionObjectPlacer.create(null, null)
	assert_true(placer.register_object_data("House", object_data))
	var caster_point := Vector3(64.0, 0.0, 64.0)
	caster_point.y = old_data.get_height_world(caster_point)
	placer.register_static_instance(901, "House", 0,
			Transform3D(Basis().scaled(Vector3(3.0, 3.0, 3.0)), caster_point), true)
	terrain.set_static_shadow_placer(placer)

	terrain.render_frame()
	var old_epoch := terrain.get_tile_cache_diagnostics()
	assert_gt(int(old_epoch.get("pending_jobs", 0)), 0,
			"the replacement must happen while old receiver jobs are outstanding")
	assert_gt(int(old_epoch.get("active_jobs", 0)), 0,
			"the replacement regression must cancel an executing receiver job, not only queued work")
	var old_weak: WeakRef = weakref(old_data)
	var replacement := TerrainData.new()
	replacement.set_trn_path(ProjectSettings.globalize_path(DVXI5_TRN))
	assert_eq(replacement.load(), OK)
	terrain.set_terrain_data(replacement)
	terrain.build()
	old_data = null
	assert_eq(int(terrain.get_tile_cache_diagnostics().get("pending_jobs", -1)), 0,
			"rebuild cancellation retires every queued/completed old receiver job")

	# Give either already-active worker time to finish and drop its old epoch.
	# The immutable snapshot owns CPT samples and the TRN route grid, so this does
	# not retain or dereference the replaced Godot TerrainData.
	for _frame in range(4):
		await get_tree().process_frame
	assert_null(old_weak.get_ref(),
			"worker snapshots retain portable receiver bytes, not the replaced resource")
	var settled := await _settle_tile_cache(terrain)
	assert_eq(int(settled.get("pending_jobs", -1)), 0)
	assert_gt(int(settled.get("ready_pages", 0)), 0)
	assert_eq(int(settled.get("upload_failures", -1)), 0)
	assert_eq(int(settled.get("shadow_raster_failures", -1)), 0,
			"only replacement-epoch shadow work may reach publication")
	terrain.set_static_shadow_placer(null)
	terrain.set_terrain_data(null)
	terrain.queue_free()
	await get_tree().process_frame
	replacement = null
	object_data = null
	placer = null
	viewport.queue_free()
	await get_tree().process_frame


func test_resolved_static_caster_changes_only_resident_page_alpha() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)
	var env_data := EnvFile.new()
	env_data.reset_to_default()
	env_data.set_curtime(900)
	var environment := MissionEnvironment.new()
	environment.name = "Env"
	environment.environment_data = env_data
	viewport.add_child(environment)
	environment.configure_mission_clock(0x0900, 60)

	var terrain_data := TerrainData.new()
	terrain_data.set_trn_path(ProjectSettings.globalize_path(DVXI5_TRN))
	assert_eq(terrain_data.load(), OK, "the Dvxi5 terrain fixture must load")

	var terrain := Terrain.new()
	viewport.add_child(terrain)
	terrain.set_environment_path(NodePath("../Env"))
	terrain.set_terrain_data(terrain_data)
	terrain.build()
	terrain.set_debug_no_frustum(true)
	# Byte-level page hashing/diff counters are capture instrumentation,
	# default-off in play; this suite asserts on them.
	terrain.set_tile_cache_capture_diagnostics(true)

	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()

	# Establish the byte-identical unshadowed CPU pages first. Attaching the placer
	# invalidates these identities and recompiles the same pages through the
	# promised static-shadow raster job. Headless Texture2DArray readback exposes
	# only its blank allocation, so authoritative comparison stays in the device
	# before upload.
	var baseline_diagnostics := await _settle_tile_cache(terrain)
	var page_array: TextureLayered = terrain.get_tile_cache_texture()
	assert_not_null(page_array)
	if page_array == null:
		return
	assert_false(bool(baseline_diagnostics["shadow_raster_available"]))
	assert_gt(int(baseline_diagnostics["resident_output_pages"]), 0)
	var baseline_hash := int(baseline_diagnostics["resident_output_hash"])

	var object_data := ObjectData.new()
	assert_eq(object_data.open_file(ProjectSettings.globalize_path(HOUSE_3DI)), OK,
		"the House fixture must provide real selected-LOD ROBJ triangles")
	var panm_index := object_data.add_part_anim(0, 0)
	assert_gte(panm_index, 0)
	assert_true(object_data.set_part_anim_channel_enabled(
			0, panm_index, "rotation", true))
	assert_true(object_data.set_part_anim_channel_mode(
			0, panm_index, "rotation", "x", "sine_wave", -1))
	assert_true(object_data.set_part_anim_channel_values(
			0, panm_index, "rotation", "x", 0.0, 90.0, 1.0))
	assert_true(object_data.has_live_panm_for_lod(0),
		"fixture pins that the retail tile projector ignores live PANM")
	var placer := MissionObjectPlacer.create(null, null)
	assert_true(placer.register_object_data("House", object_data))
	var ground_sample := Vector3(64.125, 0.0, 64.125)
	var point_ground := terrain_data.get_height_world(ground_sample)
	var bilinear_ground := terrain_data.get_height_world_bilinear(ground_sample)
	assert_almost_eq(point_ground, 40.5, 0.00001,
		"the real terrain fixture pins retail's caster-origin point sample")
	assert_gt(absf(point_ground - bilinear_ground), 0.05,
		"this fixture would catch an accidental return to bilinear caster grounding")
	var origin := Vector3(ground_sample.x, point_ground, ground_sample.z)
	placer.register_static_instance(100, "House", 0,
		Transform3D(Basis().scaled(Vector3(3.0, 3.0, 3.0)), origin), true)
	terrain.set_static_shadow_placer(placer)

	terrain.render_frame()
	var queued_shadow := terrain.get_tile_cache_diagnostics()
	assert_eq(int(queued_shadow["ready_pages"]), 0)
	assert_gt(int(queued_shadow["pending_jobs"]), 0)
	assert_eq(int(queued_shadow["frame_uploads"]), 0)
	assert_eq(int(queued_shadow["shadow_epoch_raster_jobs"]), 0,
			"static planning and rasterization must not complete inside request()")
	var diagnostics := await _settle_tile_cache(terrain)
	assert_true(bool(diagnostics["shadow_raster_available"]))
	assert_gt(int(diagnostics["shadow_raster_jobs"]), 0,
		"at least one resident page must execute the promised real-geometry raster")
	assert_eq(int(diagnostics["shadow_raster_failures"]), 0,
		"a promised real-geometry page must resolve and publish successfully")
	assert_true(bool(diagnostics["shadow_provider_snapshot_exact"]))
	assert_eq(int(diagnostics["shadow_provider_candidate_count"]), 1)
	assert_eq(int(diagnostics["shadow_provider_admitted_count"]), 1)
	assert_eq(int(diagnostics["shadow_provider_resolved_casters"]), 1)
	assert_eq(int(diagnostics["shadow_provider_epoch_plan_failures"]), 0)
	assert_eq(int(diagnostics["shadow_provider_epoch_unsupported_draw_count"]), 0,
		"retail submits the entity matrix for each selected ROBJ without PANM")
	assert_gt(int(diagnostics["frame_requests"]), 0)
	assert_eq(int(diagnostics["frame_compose_jobs"]), 0)
	assert_eq(int(diagnostics["frame_ready_hits"]),
			int(diagnostics["frame_requests"]),
		"capture-ready terrain resolves every request through an exact hit")
	assert_gt(int(diagnostics["frame_selected_ready_pages"]), 0)
	assert_gt(int(diagnostics["shadow_provider_epoch_pages_with_draws"]), 0)
	assert_gt(int(diagnostics["shadow_provider_epoch_projection_draws"]), 0)
	assert_gt(int(diagnostics["shadow_provider_epoch_triangles"]), 0)
	assert_true(bool(diagnostics["shadow_provider_epoch_has_projected_bounds"]))
	var projected_min := diagnostics["shadow_provider_epoch_projected_uv_min"] \
			as Vector2
	var projected_max := diagnostics["shadow_provider_epoch_projected_uv_max"] \
			as Vector2
	assert_lt(projected_min.x, 1.0)
	assert_lt(projected_min.y, 1.0)
	assert_gt(projected_max.x, 0.0)
	assert_gt(projected_max.y, 0.0)
	assert_gt(int(diagnostics["shadow_epoch_base_nonzero_alpha_bytes"]), 0,
		"rastered pages must start from a nonzero composed DOT3 light term")
	assert_gt(int(diagnostics["shadow_epoch_alpha_changed_bytes"]), 0,
		"the projected static silhouette must replace covered DOT3 alpha samples")
	assert_eq(int(diagnostics["shadow_epoch_rgb_changed_bytes"]), 0,
		"the page raster/composite must preserve terrain RGB byte-for-byte")
	var shadowed_hash := int(diagnostics["resident_output_hash"])
	assert_ne(shadowed_hash, baseline_hash,
		"the A-only silhouette must change the stable CPU page aggregate")

	var previous_raw_light := environment.get_light_direction()
	var initial_raw_light := previous_raw_light
	var light_epoch := _terrain_light_epoch(previous_raw_light)
	for tick_index in 3:
		environment.advance_mission_clock(1)
		var current_raw_light := environment.get_light_direction()
		assert_ne(current_raw_light, previous_raw_light,
			"mission tick %d must move the raw projection vector" % tick_index)
		assert_eq(_terrain_light_epoch(current_raw_light), light_epoch,
			"the diagnostic window must stay inside one terrain-light byte epoch")
		terrain.render_frame()
		await get_tree().process_frame
		var stable := terrain.get_tile_cache_diagnostics()
		assert_eq(int(stable["frame_compose_jobs"]), 0,
			"sub-byte light movement must reuse resident composed output")
		assert_eq(int(stable["frame_ready_hits"]), int(stable["frame_requests"]),
			"a quantized-light cache-hit frame still proves current request coverage")
		assert_gt(int(stable["frame_selected_ready_pages"]), 0,
			"cache-hit coverage must identify currently selected ready pages")
		assert_eq(int(stable["shadow_provider_frame_plan_compiles"]), 0,
			"sub-quantum light movement must reuse cached page plans outright")
		assert_eq(int(stable["shadow_provider_frame_receiver_cache_misses"]), 0,
			"unchanged terrain must not rescan page receiver samples")
		previous_raw_light = current_raw_light
	assert_eq(environment.debug_set_mission_minute_of_day(9.0 * 60.0), OK)
	assert_eq(environment.get_light_direction(), initial_raw_light,
		"restoring the exact test light keeps later output-hash checks comparable")
	terrain.render_frame()
	await get_tree().process_frame
	var restored_light := terrain.get_tile_cache_diagnostics()
	assert_eq(int(restored_light["frame_compose_jobs"]), 0,
		"returning within the same byte epoch must remain a cache hit")
	assert_eq(int(restored_light["frame_ready_hits"]),
		int(restored_light["frame_requests"]))

	terrain.set_static_terrain_shadow_enabled(false)
	var disabled := await _settle_tile_cache(terrain)
	assert_eq(int(disabled["resident_output_hash"]), baseline_hash,
		"canonical shadows_off must invalidate and restore unshadowed page alpha")
	assert_eq(int(disabled["shadow_epoch_alpha_changed_bytes"]), 0)
	terrain.set_static_terrain_shadow_enabled(true)
	var reenabled := await _settle_tile_cache(terrain)
	assert_eq(int(reenabled["resident_output_hash"]), shadowed_hash,
		"re-enabling page shadows must transactionally restore the silhouette")
	assert_gt(int(reenabled["shadow_epoch_alpha_changed_bytes"]), 0)
	assert_eq(int(reenabled["shadow_epoch_rgb_changed_bytes"]), 0)
	terrain.set_suppressed_static_shadow_bms_ids(PackedInt32Array([100]))
	var suppressed := await _settle_tile_cache(terrain)
	assert_eq(int(suppressed["resident_output_hash"]), baseline_hash,
		"BMS attribution suppression must filter the typed caster source")
	assert_eq(int(suppressed["shadow_epoch_alpha_changed_bytes"]), 0)
	assert_eq(int(suppressed["shadow_provider_epoch_triangles"]), 0)
	terrain.set_suppressed_static_shadow_bms_ids(PackedInt32Array())
	var unsuppressed := await _settle_tile_cache(terrain)
	assert_eq(int(unsuppressed["resident_output_hash"]), shadowed_hash,
		"clearing BMS suppression must restore the exact shadowed pages")
	assert_gt(int(unsuppressed["shadow_epoch_alpha_changed_bytes"]), 0)
	assert_eq(int(unsuppressed["shadow_epoch_rgb_changed_bytes"]), 0)

	assert_true(object_data.set_material_field(0, "uv_u_style", 1),
		"the fixture must expose a time/control-driven UV mutation")
	var unsupported_uv := await _settle_tile_cache(terrain)
	assert_eq(int(unsupported_uv["shadow_provider_epoch_plan_failures"]), 0,
		"one unsupported caster draw must not reject the terrain page baseline")
	assert_eq(int(unsupported_uv["ready_pages"]), int(unsuppressed["ready_pages"]),
		"unsupported caster attribution must preserve every requested resident page")
	assert_eq(int(unsupported_uv["shadow_raster_failures"]),
		int(unsuppressed["shadow_raster_failures"]),
		"an explicitly skipped unsupported draw is not a device raster failure")
	assert_gt(int(unsupported_uv["shadow_provider_epoch_unsupported_draw_count"]), 0,
		"the provider must count every page-local unsupported draw it skips")
	var unsupported_rows: Array = unsupported_uv[
			"shadow_provider_epoch_unsupported_attribution"]
	var found_dynamic_uv := false
	for row_v in unsupported_rows:
		var row := row_v as Dictionary
		if int(row.get("bms_id", 0)) == 100 \
				and (row.get("reasons", PackedStringArray()) as PackedStringArray).has(
						"dynamic_uv"):
			found_dynamic_uv = true
			break
	assert_true(found_dynamic_uv,
		"typed attribution must name the exact BMS and unsupported material state")
	assert_true(object_data.set_material_field(0, "uv_u_style", 0))
	var restored_material := await _settle_tile_cache(terrain)
	assert_eq(int(restored_material["shadow_provider_epoch_plan_failures"]), 0,
		"restoring a supported static material must make every page plan exact again")
	assert_gt(int(restored_material["shadow_epoch_alpha_changed_bytes"]), 0)
	assert_eq(int(restored_material["shadow_epoch_rgb_changed_bytes"]), 0)
	# Force a fresh semantic epoch and prove the asynchronous rebuild converges
	# to the identical canonical resident-page output.
	terrain.set_static_terrain_shadow_enabled(false)
	terrain.set_static_terrain_shadow_enabled(true)
	var fully_rebuilt_material := await _settle_tile_cache(terrain)
	assert_eq(int(fully_rebuilt_material["resident_output_pages"]),
		int(diagnostics["resident_output_pages"]))
	assert_eq(int(fully_rebuilt_material["resident_output_hash"]), shadowed_hash,
		"restoring an exact static material must recover identical full-page output")

	terrain.set_static_shadow_placer(null)
	terrain.set_terrain_data(null)
	viewport.free()
	page_array = null
	placer = null
	object_data = null
	environment = null
	env_data = null
	terrain_data = null


func test_caster_motion_recomposes_only_affected_pages_while_stale_pages_keep_serving() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)
	var terrain_data := TerrainData.new()
	terrain_data.set_trn_path(ProjectSettings.globalize_path(DVXI5_TRN))
	assert_eq(terrain_data.load(), OK)
	var terrain := Terrain.new()
	viewport.add_child(terrain)
	terrain.set_terrain_data(terrain_data)
	terrain.build()
	terrain.set_debug_no_frustum(true)
	terrain.set_tile_cache_capture_diagnostics(true)
	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()

	var object_data := ObjectData.new()
	assert_eq(object_data.open_file(ProjectSettings.globalize_path(HOUSE_3DI)), OK)
	var placer := MissionObjectPlacer.create(null, null)
	assert_true(placer.register_object_data("House", object_data))
	var still_origin := Vector3(64.0, 0.0, 64.0)
	still_origin.y = terrain_data.get_height_world(still_origin)
	placer.register_static_instance(100, "House", 0,
			Transform3D(Basis().scaled(Vector3(3.0, 3.0, 3.0)), still_origin), true)
	var mover_origin := Vector3(96.0, 0.0, 96.0)
	mover_origin.y = terrain_data.get_height_world(mover_origin)
	placer.register_static_instance(101, "House", 1,
			Transform3D(Basis().scaled(Vector3(3.0, 3.0, 3.0)), mover_origin), true)
	terrain.set_static_shadow_placer(placer)
	var settled := await _settle_tile_cache(terrain)
	var settled_ready_pages := int(settled["ready_pages"])
	var settled_hash := int(settled["resident_output_hash"])
	assert_gt(settled_ready_pages, 0)
	assert_eq(int(settled["shadow_raster_failures"]), 0)

	# Move only the second caster. The bump must localize: pages the mover
	# never touched stay exact hits, its own pages keep serving their last-
	# published payload (stale) while the replacement composes, and the
	# resident set never collapses to the fallback shader path.
	mover_origin.x += 8.0
	mover_origin.y = terrain_data.get_height_world(mover_origin)
	placer.register_static_instance(101, "House", 1,
			Transform3D(Basis().scaled(Vector3(3.0, 3.0, 3.0)), mover_origin), true)
	terrain.render_frame()
	var moved := terrain.get_tile_cache_diagnostics()
	assert_eq(int(moved["ready_pages"]), settled_ready_pages,
		"a moved caster must not collapse the resident page set")
	assert_gt(int(moved["frame_compose_jobs"]), 0,
		"the mover's own footprint pages must recompose")
	assert_lt(int(moved["frame_compose_jobs"]), int(moved["frame_requests"]),
		"pages the mover never touched must stay exact hits, not recompose")
	assert_eq(int(moved["frame_ready_hits"]) + int(moved["frame_stale_hits"]),
			int(moved["frame_requests"]),
		"every request must be served — exact or stale — during a caster move")
	assert_gt(int(moved["frame_stale_hits"]), 0,
		"the mover's re-targeted pages must keep serving their published payload")

	var resettled := await _settle_tile_cache(terrain)
	assert_eq(int(resettled["shadow_raster_failures"]), 0)
	assert_eq(int(resettled["frame_stale_hits"]), 0,
		"a settled cache serves no stale payloads")
	assert_ne(int(resettled["resident_output_hash"]), settled_hash,
		"the moved silhouette must change the stable CPU page aggregate")

	# Epoch-neutral still frames: nothing moved, so nothing may recompose and
	# the provider's source revision must hold.
	var still_revision := int(resettled["shadow_provider_source_revision"])
	for _frame in 3:
		terrain.render_frame()
		await get_tree().process_frame
		var still := terrain.get_tile_cache_diagnostics()
		assert_eq(int(still["frame_compose_jobs"]), 0,
			"an unchanged world must not recompose any page")
		assert_eq(int(still["frame_stale_hits"]), 0)
		assert_eq(int(still["frame_ready_hits"]), int(still["frame_requests"]))
		assert_eq(int(still["shadow_provider_source_revision"]), still_revision,
			"still frames must not bump the placer's shadow source revision")

	terrain.set_static_shadow_placer(null)
	terrain.set_terrain_data(null)
	viewport.free()
	placer = null
	object_data = null
	terrain_data = null


func test_retail_scrate1_constant_alpha_does_not_reject_opaque_projshad() -> void:
	var install_dir := OS.get_environment("OPENNOVA_JO_DIR").strip_edges()
	if install_dir.is_empty():
		pending("OPENNOVA_JO_DIR / retail JO PFFs are required for the Scrate1 shadow witness")
		return
	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.mount_runtime(
			install_dir, "revx02", false, "jo"), OK,
		"the installed JO runtime must mount for the asset-backed witness")
	var object_data := ObjectData.new()
	assert_eq(object_data.open_from_resource_root(
			resource_root, "Scrate1.3di", false), OK,
		"Scrate1 must resolve from the mounted retail archives")
	var material := object_data.get_material_info(0)
	assert_eq(String(material.get("shader_tag", "")), "FF_ST_OP")
	assert_eq(int(material.get("alpha_gen_style", 0)), 24,
		"Scrate1 authors retail's constant AlphaGen style")
	assert_eq(int(material.get("alpha_gen_start", -1)), 128,
		"the constant generator supplies 128/255 material alpha")
	assert_false(bool(material.get("alpha_test_enabled", true)))

	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)
	var terrain_data := TerrainData.new()
	terrain_data.set_trn_path(ProjectSettings.globalize_path(DVXI5_TRN))
	assert_eq(terrain_data.load(), OK)
	var terrain := Terrain.new()
	viewport.add_child(terrain)
	terrain.set_terrain_data(terrain_data)
	terrain.build()
	terrain.set_debug_no_frustum(true)
	# This suite asserts on the gated byte-level shadow diff counters.
	terrain.set_tile_cache_capture_diagnostics(true)
	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()

	var placer := MissionObjectPlacer.create(resource_root, null)
	assert_true(placer.register_object_data("Scrate1", object_data))
	var origin := Vector3(64.0,
		terrain_data.get_height_world(Vector3(64.0, 0.0, 64.0)), 64.0)
	placer.register_static_instance(889, "Scrate1", 0,
		Transform3D(Basis().scaled(Vector3(4.0, 4.0, 4.0)), origin), true)
	terrain.set_static_shadow_placer(placer)
	var diagnostics := await _settle_tile_cache(terrain)
	assert_gt(int(diagnostics["shadow_provider_epoch_projection_draws"]), 0,
		"the mounted Scrate1 must enter at least one resident PROJSHAD page")
	assert_eq(int(diagnostics["shadow_provider_epoch_plan_failures"]), 0)
	assert_eq(int(diagnostics["shadow_provider_epoch_unsupported_draw_count"]), 0,
		"constant AlphaGen is irrelevant to an opaque, non-alpha-tested PROJSHAD draw")
	assert_gt(int(diagnostics["shadow_provider_epoch_triangles"]), 0)
	assert_gt(int(diagnostics["shadow_epoch_alpha_changed_bytes"]), 0,
		"Scrate1 must rasterize a real opaque silhouette into page alpha")
	assert_eq(int(diagnostics["shadow_epoch_rgb_changed_bytes"]), 0)

	assert_true(object_data.set_material_field(
		0, "alpha_test_enabled", true))
	var constant_alpha_test := await _settle_tile_cache(terrain)
	assert_eq(int(constant_alpha_test[
			"shadow_provider_epoch_unsupported_draw_count"]), 0,
		"style 24 is a static start-value generator and remains exact when alpha matters")
	assert_gt(int(constant_alpha_test[
			"shadow_provider_epoch_alpha_test_triangles"]), 0)

	assert_true(object_data.set_material_field(0, "alpha_gen_style", 1))
	var time_alpha := await _settle_tile_cache(terrain)
	assert_gt(int(time_alpha["shadow_provider_epoch_unsupported_draw_count"]), 0,
		"a time-driven generator must remain fail-closed when alpha testing consumes it")
	var found_dynamic_alpha := false
	for row_v in time_alpha[
			"shadow_provider_epoch_unsupported_attribution"] as Array:
		var row := row_v as Dictionary
		if int(row.get("bms_id", 0)) == 889 \
				and (row.get("reasons", PackedStringArray()) \
						as PackedStringArray).has("dynamic_alpha"):
			found_dynamic_alpha = true
			break
	assert_true(found_dynamic_alpha,
		"unsupported attribution must preserve the exact dynamic-alpha reason")

	terrain.set_static_shadow_placer(null)
	terrain.set_terrain_data(null)
	viewport.free()
	placer = null
	object_data = null
	resource_root = null
	terrain_data = null
