extends GutTest

const HOUSE_3DI := "res://../fixtures/threedi/synth/house.3di"
# house with a live LOD0 sine rotation row; the same plus material 0's UV
# generator set to style 1; house with material 0 alpha-tested and
# time-scrolled at one texture per second (style 16, rate 1). Minted once
# from the retired edit surface (fixtures/README.md).
const SYN_HOUSE_SINE := "res://../fixtures/threedi/synth/house_lod0_sine_rotx.3di"
const SYN_HOUSE_SINE_UV1 := "res://../fixtures/threedi/synth/house_lod0_sine_rotx_uv1.3di"
const SYN_HOUSE_UVSCROLL := "res://../fixtures/threedi/synth/house_mtrl0_uvscroll16_alphatest.3di"


# The synthetic Tmap terrain (fixtures/terrain/tmap) staged over the minimal
# assets it names; one root per test file (TestFs.staged_tmap), removed at the end.
const TMAP_STAGE := "static_shadow"


func after_all() -> void:
	TestFs.release_staged_tmap(TMAP_STAGE)


static func _terrain_light_epoch(raw_tuple: Vector3) -> Vector3i:
	# Independent test oracle for retail's truncated (g2,g0,g1) terrain bytes,
	# packed from the RAW Environment_GetLightDirectionFloat tuple (never the
	# Godot-axes vector get_light_direction() serves).
	return Vector3i(
		int((clampf(raw_tuple.z, -1.0, 1.0) + 1.0) * 127.5),
		int((clampf(raw_tuple.x, -1.0, 1.0) + 1.0) * 127.5),
		int((clampf(raw_tuple.y, -1.0, 1.0) + 1.0) * 127.5))


func test_replacing_terrain_data_cancels_old_jobs_without_borrowing_old_receiver_memory() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)
	var old_data: TerrainData = TerrainData.new()
	old_data.set_trn_path(TestFs.staged_tmap(TMAP_STAGE))
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
	assert_true(placer.register_object_data("house", object_data))
	var caster_point := Vector3(64.0, 0.0, 64.0)
	caster_point.y = old_data.get_height_world(caster_point)
	placer.register_static_instance(901, "house", 0,
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
	replacement.set_trn_path(TestFs.staged_tmap(TMAP_STAGE))
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
	var settled := await TestFs.settle_tile_cache(self, terrain)
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
	terrain_data.set_trn_path(TestFs.staged_tmap(TMAP_STAGE))
	assert_eq(terrain_data.load(), OK, "the Tmap terrain fixture must load")

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
	var baseline_diagnostics := await TestFs.settle_tile_cache(self, terrain)
	var page_array: TextureLayered = terrain.get_tile_cache_texture()
	assert_not_null(page_array)
	if page_array == null:
		return
	assert_false(bool(baseline_diagnostics["shadow_raster_available"]))
	assert_gt(int(baseline_diagnostics["resident_output_pages"]), 0)
	var baseline_hash := int(baseline_diagnostics["resident_output_hash"])

	var object_data := ObjectData.new()
	assert_eq(object_data.open_file(ProjectSettings.globalize_path(SYN_HOUSE_SINE)), OK,
		"the house fixture must provide real selected-LOD ROBJ triangles")
	assert_true(object_data.has_live_panm_for_lod(0),
		"fixture pins that the retail tile projector ignores live PANM")
	var placer := MissionObjectPlacer.create(null, null)
	assert_true(placer.register_object_data("house", object_data))
	var ground_sample := Vector3(64.125, 0.0, 64.125)
	var point_ground := terrain_data.get_height_world(ground_sample)
	var bilinear_ground := terrain_data.get_height_world_bilinear(ground_sample)
	# The synthetic hill's checker cell at atlas (576, 576): the sloped plane
	# (36 + 8 + 4) minus the checker's 0.5 (minted by
	# tests/fixtures/minimal_terrain_gen.cpp, which prints this pin every run).
	assert_almost_eq(point_ground, 47.5, 0.00001,
		"the synthetic terrain fixture pins retail's caster-origin point sample")
	assert_gt(absf(point_ground - bilinear_ground), 0.05,
		"this fixture would catch an accidental return to bilinear caster grounding")
	var origin := Vector3(ground_sample.x, point_ground, ground_sample.z)
	placer.register_static_instance(100, "house", 0,
		Transform3D(Basis().scaled(Vector3(3.0, 3.0, 3.0)), origin), true)
	terrain.set_static_shadow_placer(placer)

	terrain.render_frame()
	var queued_shadow := terrain.get_tile_cache_diagnostics()
	assert_eq(int(queued_shadow["ready_pages"]), 0)
	assert_gt(int(queued_shadow["pending_jobs"]), 0)
	assert_eq(int(queued_shadow["frame_uploads"]), 0)
	assert_eq(int(queued_shadow["shadow_epoch_raster_jobs"]), 0,
			"static planning and rasterization must not complete inside request()")
	var diagnostics := await TestFs.settle_tile_cache(self, terrain)
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

	var previous_raw_light := environment.get_light_direction_render_tuple()
	var initial_raw_light := previous_raw_light
	var light_epoch := _terrain_light_epoch(previous_raw_light)
	for tick_index in 3:
		environment.advance_mission_clock(1)
		var current_raw_light := environment.get_light_direction_render_tuple()
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
	assert_eq(environment.get_light_direction_render_tuple(), initial_raw_light,
		"restoring the exact test light keeps later output-hash checks comparable")
	terrain.render_frame()
	await get_tree().process_frame
	var restored_light := terrain.get_tile_cache_diagnostics()
	assert_eq(int(restored_light["frame_compose_jobs"]), 0,
		"returning within the same byte epoch must remain a cache hit")
	assert_eq(int(restored_light["frame_ready_hits"]),
		int(restored_light["frame_requests"]))

	terrain.set_static_terrain_shadow_enabled(false)
	var disabled := await TestFs.settle_tile_cache(self, terrain)
	assert_eq(int(disabled["resident_output_hash"]), baseline_hash,
		"canonical shadows_off must invalidate and restore unshadowed page alpha")
	assert_eq(int(disabled["shadow_epoch_alpha_changed_bytes"]), 0)
	terrain.set_static_terrain_shadow_enabled(true)
	var reenabled := await TestFs.settle_tile_cache(self, terrain)
	assert_eq(int(reenabled["resident_output_hash"]), shadowed_hash,
		"re-enabling page shadows must transactionally restore the silhouette")
	assert_gt(int(reenabled["shadow_epoch_alpha_changed_bytes"]), 0)
	assert_eq(int(reenabled["shadow_epoch_rgb_changed_bytes"]), 0)
	terrain.set_suppressed_static_shadow_bms_ids(PackedInt32Array([100]))
	var suppressed := await TestFs.settle_tile_cache(self, terrain)
	assert_eq(int(suppressed["resident_output_hash"]), baseline_hash,
		"BMS attribution suppression must filter the typed caster source")
	assert_eq(int(suppressed["shadow_epoch_alpha_changed_bytes"]), 0)
	assert_eq(int(suppressed["shadow_provider_epoch_triangles"]), 0)
	terrain.set_suppressed_static_shadow_bms_ids(PackedInt32Array())
	var unsuppressed := await TestFs.settle_tile_cache(self, terrain)
	assert_eq(int(unsuppressed["resident_output_hash"]), shadowed_hash,
		"clearing BMS suppression must restore the exact shadowed pages")
	assert_gt(int(unsuppressed["shadow_epoch_alpha_changed_bytes"]), 0)
	assert_eq(int(unsuppressed["shadow_epoch_rgb_changed_bytes"]), 0)

	# Material 0's UV generator switched to style 1 (time/control-driven): the
	# same house rows with that one authored change, reloaded into the
	# registered ObjectData so the projector sees a document change.
	# (Material 0's uv_u_style 1 — the time/control-driven UV mutation — is
	# the minimal_3di_gen ctest's pin on house_lod0_sine_rotx_uv1.)
	assert_eq(object_data.open_file(ProjectSettings.globalize_path(SYN_HOUSE_SINE_UV1)), OK)
	var animated_uv := await TestFs.settle_tile_cache(self, terrain)
	assert_eq(int(animated_uv["shadow_provider_epoch_plan_failures"]), 0,
		"the shared runtime evaluator must keep dynamic projected-shadow UV exact")
	assert_eq(int(animated_uv["ready_pages"]), int(unsuppressed["ready_pages"]),
		"dynamic UV evaluation must preserve every requested resident page")
	assert_eq(int(animated_uv["shadow_raster_failures"]),
		int(unsuppressed["shadow_raster_failures"]),
		"dynamic UV evaluation must not create a device raster failure")
	assert_eq(int(animated_uv["shadow_provider_epoch_unsupported_draw_count"]), 0,
		"dynamic UV is a supported projected-shadow input, not skipped attribution")
	assert_gt(int(animated_uv["shadow_provider_epoch_triangles"]), 0,
		"the dynamically transformed material must still submit its silhouettes")
	assert_eq(object_data.open_file(ProjectSettings.globalize_path(SYN_HOUSE_SINE)), OK)
	var restored_material := await TestFs.settle_tile_cache(self, terrain)
	assert_eq(int(restored_material["shadow_provider_epoch_plan_failures"]), 0,
		"restoring a supported static material must make every page plan exact again")
	assert_gt(int(restored_material["shadow_epoch_alpha_changed_bytes"]), 0)
	assert_eq(int(restored_material["shadow_epoch_rgb_changed_bytes"]), 0)

	# Force a fresh semantic epoch and prove the asynchronous rebuild converges
	# to the identical canonical resident-page output.
	terrain.set_static_terrain_shadow_enabled(false)
	terrain.set_static_terrain_shadow_enabled(true)
	var fully_rebuilt_material := await TestFs.settle_tile_cache(self, terrain)
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
	terrain_data.set_trn_path(TestFs.staged_tmap(TMAP_STAGE))
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
	assert_true(placer.register_object_data("house", object_data))
	var still_origin := Vector3(64.0, 0.0, 64.0)
	still_origin.y = terrain_data.get_height_world(still_origin)
	placer.register_static_instance(100, "house", 0,
			Transform3D(Basis().scaled(Vector3(3.0, 3.0, 3.0)), still_origin), true)
	var mover_origin := Vector3(96.0, 0.0, 96.0)
	mover_origin.y = terrain_data.get_height_world(mover_origin)
	placer.register_static_instance(101, "house", 1,
			Transform3D(Basis().scaled(Vector3(3.0, 3.0, 3.0)), mover_origin), true)
	terrain.set_static_shadow_placer(placer)
	var settled := await TestFs.settle_tile_cache(self, terrain)
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
	placer.register_static_instance(101, "house", 1,
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

	var resettled := await TestFs.settle_tile_cache(self, terrain)
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


func test_animated_caster_material_keeps_one_worker_snapshot_across_still_frames() -> void:
	# Material animation is sampled at the tick of the frame that requests a
	# page (retail evaluates tile-model materials inside the tile render), so
	# a continuously scrolling caster material must not republish the shared
	# worker snapshot every frame: the provider's epoch counters, which reset
	# only when a new snapshot is published, hold across still frames whose
	# millisecond clock keeps advancing, and no resident page recomposes.
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)
	var terrain_data := TerrainData.new()
	terrain_data.set_trn_path(TestFs.staged_tmap(TMAP_STAGE))
	assert_eq(terrain_data.load(), OK)
	var terrain := Terrain.new()
	viewport.add_child(terrain)
	terrain.set_terrain_data(terrain_data)
	terrain.build()
	terrain.set_debug_no_frustum(true)
	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()

	# Retail's time-scroll UV mode (style 16) at one texture per second on an
	# alpha-sampled material: the evaluated UV translation moves every 1/256 s.
	# (Material 0's alpha test + uv_u_style 16 at one texture per second is the
	# minimal_3di_gen ctest's pin on house_mtrl0_uvscroll16_alphatest.)
	var object_data := ObjectData.new()
	assert_eq(object_data.open_file(ProjectSettings.globalize_path(SYN_HOUSE_UVSCROLL)), OK)
	var placer := MissionObjectPlacer.create(null, null)
	assert_true(placer.register_object_data("house", object_data))
	var origin := Vector3(64.0, 0.0, 64.0)
	origin.y = terrain_data.get_height_world(origin)
	placer.register_static_instance(100, "house", 0,
			Transform3D(Basis().scaled(Vector3(3.0, 3.0, 3.0)), origin), true)
	terrain.set_static_shadow_placer(placer)
	var clock_ms := 1000
	terrain.set_light_context(null, clock_ms)
	var settled := await TestFs.settle_tile_cache(self, terrain)
	assert_eq(int(settled["shadow_raster_failures"]), 0)
	assert_eq(int(settled["shadow_provider_epoch_plan_failures"]), 0)
	var epoch_plans := int(settled["shadow_provider_epoch_plan_count"])
	assert_gt(epoch_plans, 0,
		"resident pages must have planned through the shared worker snapshot")

	for _frame in 3:
		clock_ms += 16
		terrain.set_light_context(null, clock_ms)
		terrain.render_frame()
		await get_tree().process_frame
		var still := terrain.get_tile_cache_diagnostics()
		assert_eq(int(still["frame_compose_jobs"]), 0,
			"an advancing material clock must not recompose resident pages")
		assert_eq(int(still["frame_ready_hits"]), int(still["frame_requests"]))
		assert_eq(int(still["shadow_provider_epoch_plan_count"]), epoch_plans,
			"an advancing material clock must not republish the worker snapshot")
		assert_eq(int(still["shadow_provider_frame_plan_compiles"]), 0,
			"an advancing material clock must reuse cached page plans outright")

	terrain.set_static_shadow_placer(null)
	terrain.set_terrain_data(null)
	viewport.free()
	placer = null
	object_data = null
	terrain_data = null


func test_retail_scrate1_constant_alpha_does_not_reject_opaque_projshad() -> void:
	var resource_root := RetailData.mount_install_with("Scrate1.3di")
	if resource_root == null:
		pending("OPENNOVA_JO_DIR / retail JO PFFs serving Scrate1.3di are required for the shadow witness")
		return
	var object_data := ObjectData.new()
	assert_eq(object_data.open_from_resource_root(
			resource_root, "Scrate1.3di", false), OK,
		"Scrate1 must resolve from the mounted retail archives")
	# Scrate1 authors retail's constant AlphaGen style (24, start 128) on an
	# opaque FF_ST_OP material with no alpha test — the asset-gated
	# threedi_retail_material_facts ctest pins the MTRL row; the evaluated
	# constant alpha is the runtime's own read.
	assert_almost_eq(float(data_alpha_mod(object_data)), 128.0 / 255.0, 0.0001,
		"the constant generator supplies 128/255 material alpha")

	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)
	var terrain_data := TerrainData.new()
	terrain_data.set_trn_path(TestFs.staged_tmap(TMAP_STAGE))
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
	var diagnostics := await TestFs.settle_tile_cache(self, terrain)
	assert_gt(int(diagnostics["shadow_provider_epoch_projection_draws"]), 0,
		"the mounted Scrate1 must enter at least one resident PROJSHAD page")
	assert_eq(int(diagnostics["shadow_provider_epoch_plan_failures"]), 0)
	assert_eq(int(diagnostics["shadow_provider_epoch_unsupported_draw_count"]), 0,
		"constant AlphaGen is irrelevant to an opaque, non-alpha-tested PROJSHAD draw")
	assert_gt(int(diagnostics["shadow_provider_epoch_triangles"]), 0)
	assert_gt(int(diagnostics["shadow_epoch_alpha_changed_bytes"]), 0,
		"Scrate1 must rasterize a real opaque silhouette into page alpha")
	assert_eq(int(diagnostics["shadow_epoch_rgb_changed_bytes"]), 0)

	terrain.set_static_shadow_placer(null)
	terrain.set_terrain_data(null)
	viewport.free()
	placer = null
	object_data = null
	resource_root = null
	terrain_data = null


# Material 0's evaluated constant alpha at t = 0 with no controls (the
# runtime evaluator the shadow projector shares).
static func data_alpha_mod(object_data: ObjectData) -> float:
	return float(object_data.eval_material_runtime(0, 0, {}).get("alpha_mod", -1.0))
