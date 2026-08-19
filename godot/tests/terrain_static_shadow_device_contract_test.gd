extends GutTest


func _source(path: String) -> String:
	var file := FileAccess.open(path, FileAccess.READ)
	assert_not_null(file, "Production source must be readable: %s" % path)
	return file.get_as_text() if file != null else ""


func test_device_snapshots_shadow_state_before_queuing_portable_work() -> void:
	var header := _source("res://src/terrain/nova_terrain_tile_cache_device.h")
	var source := _source("res://src/terrain/nova_terrain_tile_cache_device.cpp")
	assert_true(header.contains("class TerrainStaticShadowPageRasterizer"),
		"The device needs a typed page compiler, not a framebuffer-opacity hook.")
	assert_true(header.contains("TerrainStaticShadowCompilationSnapshot"))
	assert_true(header.contains(
			"std::shared_ptr<const TerrainStaticShadowCompilationSnapshot>"),
		"Only an immutable portable snapshot may cross the worker boundary.")
	var refresh := source.find("TerrainTileCacheDevice::_refresh_shadow_snapshot()")
	var snapshot := source.find(
			"static_shadow_rasterizer_->compilation_snapshot()", refresh)
	assert_gt(refresh, -1)
	assert_gt(snapshot, refresh)
	assert_false(source.contains("observed_shadow_revision_"),
		"A planner state change is per-page identity, never a global cache drop:"
		+ " only pages an actually-changed caster or light quantum touches may"
		+ " re-target.")
	assert_true(header.contains("plan_page("),
		"The producer seam must expose the memoized per-page plan to request().")
	var plan_page := source.find(
			"static_shadow_rasterizer_->plan_page(request.page)")
	var mix_stamp := source.find("mix_terrain_static_shadow_content_stamp(")
	var lookup := source.find("cache_.request(request)")
	assert_gt(plan_page, snapshot,
		"Page identity must come from the live planner's per-page stamp.")
	assert_gt(mix_stamp, plan_page,
		"The per-page plan stamp is what mixes into page identity.")
	var invalidate_page := source.find("TerrainTileCacheDevice::_invalidate_page(")
	var allocate_texture := source.find(
			"bool TerrainTileCacheDevice::_allocate_texture()", invalidate_page)
	assert_gt(invalidate_page, -1)
	assert_gt(allocate_texture, invalidate_page)
	var invalidate_body := source.substr(
			invalidate_page, allocate_texture - invalidate_page)
	assert_true(invalidate_body.contains("cache_.invalidate(p_page)"))
	assert_true(invalidate_body.contains("ready_page_keys_[layer]"))
	assert_true(invalidate_body.contains("ready_generations_[layer] = 0"),
		"The device readiness count must drop with the page-local cache identity.")
	assert_true(source.contains(
			"ready_page_keys_[job.target.layer] = job.target.page"),
		"A published layer must retain enough identity for page-local invalidation.")
	assert_gt(lookup, mix_stamp,
		"Shadow state must participate in identity before cache lookup/queueing.")


func test_worker_snapshot_owns_the_complete_receiver_height_field() -> void:
	var header := _source("res://src/terrain/nova_terrain_tile_cache_device.h")
	var rasterizer := _source(
			"res://src/terrain/nova_terrain_static_shadow_rasterizer.cpp")
	assert_true(header.contains("TerrainStaticShadowReceiverStorage"),
			"The immutable worker snapshot needs one owner for every receiver pointer.")
	assert_true(header.contains("std::vector<uint16_t> heightmap"),
			"The receiver owner must keep the copied CPT height samples alive.")
	assert_true(header.contains("std::array<int, 16 * 16> sector_grid"),
			"The receiver owner must also copy the TRN routing grid used by workers.")
	var refresh := rasterizer.find("void refresh_receiver_terrain()")
	var snapshot := rasterizer.find("TerrainStaticShadowRasterizer::compilation_snapshot()")
	assert_gt(refresh, -1)
	assert_gt(snapshot, refresh)
	var refresh_body := rasterizer.substr(refresh, snapshot - refresh)
	assert_true(refresh_body.contains("receiver_storage->sector_grid.data()"),
			"The planner's receiver view must point into snapshot-owned routing storage.")
	assert_false(refresh_body.contains(
			"field.layout.sector_grid = &terrain_data->get_trn().sector_grid"),
			"No worker-bound planner may retain a pointer into a replaceable TerrainData.")


func test_workers_finish_exact_pages_before_generation_checked_upload() -> void:
	var source := _source("res://src/terrain/nova_terrain_tile_cache_device.cpp")
	var worker := source.find("void worker_loop()")
	var compose := source.find("compose_terrain_tile_page(", worker)
	var plan := source.find("shadow_planner.plan(", compose)
	var begin_alpha := source.find("begin_terrain_static_shadow_alpha_page(", plan)
	var raster := source.find("shadow_planner.rasterize(", begin_alpha)
	var apply_alpha := source.find("apply_terrain_static_shadow_alpha_page(", raster)
	var drain := source.find("TerrainTileCacheDevice::_drain_completed()")
	var generation_gate := source.find("cache_.can_publish(job)", drain)
	var make_image := source.find("image_from_rgba8(completion.pixels)", drain)
	var upload := source.find("texture_->update_layer(image, job.target.layer)", drain)
	var publish := source.find("cache_.publish(job)", upload)
	assert_gt(compose, worker)
	assert_gt(plan, compose,
		"Workers must re-plan from their private snapshot before rasterizing;"
		+ " request() computes only the memoized identity stamp.")
	assert_gt(begin_alpha, compose,
		"The raster must start from the composed DOT3 alpha, never from a blank opacity mask.")
	assert_gt(raster, begin_alpha,
		"Resolved projected coverage must mutate the typed alpha page.")
	assert_gt(apply_alpha, raster,
		"The final raster bytes must pass the A-only identity/dimension gate.")
	assert_gt(generation_gate, drain)
	assert_gt(make_image, generation_gate,
		"A stale completion must be rejected before its layer is mutated.")
	assert_gt(upload, make_image,
		"The validated RGB-invariant page must be the page uploaded and published.")
	assert_gt(publish, upload)
	assert_true(source.contains("kWorkerCount = 2"))
	assert_true(source.contains("kUploadBudgetPerFrame = 2"))
	assert_true(source.contains("take_completion_if("),
		"Cold layers (no published payload on screen) drain outside the"
		+ " refresh budget so first-fill completes in a few frames.")
	assert_true(source.contains(
			"ready_generations_[candidate.job.target.layer] == 0"),
		"Only refresh uploads — layers already serving a published payload —"
		+ " count against the per-frame budget.")
	assert_true(source.contains("completion.shadow_alpha_changed_bytes"),
		"Worker diagnostics must report exact CPU alpha replacements.")
	assert_true(source.contains("completion.shadow_rgb_changed_bytes"),
		"Worker diagnostics must independently prove the page RGB invariant.")


func test_device_coalesces_stale_work_and_prioritizes_current_frame_demand() -> void:
	var source := _source("res://src/terrain/nova_terrain_tile_cache_device.cpp")
	var async_state := source.find("struct TerrainTileCacheDevice::AsyncState")
	var device_constructor := source.find(
			"TerrainTileCacheDevice::TerrainTileCacheDevice()", async_state)
	assert_gt(async_state, -1)
	assert_gt(device_constructor, async_state)
	var body := source.substr(async_state, device_constructor - async_state)
	assert_true(body.contains("TerrainTileCompositionDemandQueue demand_queue"),
			"The worker payload queue must consume the portable demand-order policy.")
	assert_true(body.contains("TerrainTileCompositionDemandQueue completion_queue"),
			"Completed payloads must use the same current-frame-first policy before upload.")
	assert_true(body.contains("demand_queue.enqueue("),
			"Enqueue must coalesce an older generation for the same cache layer.")
	assert_true(body.contains("completion_queue.remove_older_generations("),
			"A new layer generation must purge an already-completed superseded payload.")
	assert_true(body.contains("removed_sequences"),
			"Coalesced schedule entries must remove their matching queued payloads.")
	assert_true(body.contains("demand_queue.take_next()"),
			"Workers must select newest-frame demand instead of dequeuing strict FIFO.")
	assert_true(body.contains("completion_queue.take_next()"),
			"The upload gate must select current-frame completions instead of strict FIFO.")
	assert_true(source.contains("diagnostic_frame_id_, shadow_snapshot"),
			"Every queued page must carry the render frame that demanded it.")


func test_terrain_owns_and_frames_the_concrete_shadow_rasterizer() -> void:
	var header := _source("res://src/terrain/nova_terrain.h")
	var source := _source("res://src/terrain/nova_terrain.cpp")
	assert_true(header.contains("TerrainStaticShadowRasterizer static_shadow_rasterizer"),
		"Terrain must own the producer whose non-owning address the cache device retains.")
	assert_true(source.contains(
			"p_placer.is_valid() ? &static_shadow_rasterizer : nullptr"),
		"The device must expose the producer only while a mission source is attached.")
	var light_sample := source.find("page_light_direction = cached_env_node->get_light_direction()")
	var frame_raster := source.find("static_shadow_rasterizer.begin_frame(page_light_direction)")
	var frame_cache := source.find("tile_cache_device.begin_frame(draw_list.frame_id)")
	assert_gt(frame_raster, light_sample,
		"The producer must consume the same direct environment tuple as the page composer.")
	assert_gt(frame_cache, frame_raster,
		"The caster/light snapshot must be final before any page plan is requested.")


func test_game_world_attaches_and_detaches_the_mission_shadow_source() -> void:
	var source := _source("res://game/world/game_world.gd")
	var placed := source.find("_mission_stats = _placer.place(mission, self, options)")
	var attached := source.find("_terrain.set_static_shadow_placer(_placer)")
	assert_gt(attached, placed,
		"Only successfully placed ObjectData/transform sources may enter the page collector.")
	var unload := source.find("func unload() -> void:")
	var detached := source.find("_terrain.set_static_shadow_placer(null)", unload)
	var release := source.find("_placer = null", unload)
	assert_gt(detached, unload)
	assert_gt(release, detached,
		"Terrain must release its Ref before GameWorld drops the mission placer.")


func test_page_shadow_alpha_preserves_sky_and_fog_without_a_black_overlay() -> void:
	var shared := _source("res://shaders/terrain_lighting.gdshaderinc")
	var runtime := _source("res://shaders/terrain.gdshader")
	var device := _source("res://src/terrain/nova_terrain.cpp")
	assert_true(shared.contains("cm.a * u_sun_light + u_sky_ambient"),
		"A zeroed page light term must remove only direct sun while retaining sky ambient.")
	var surface := runtime.find("terrain_surface_color_from_colormap(")
	var fog := runtime.find("result = apply_terrain_fog(")
	assert_gt(fog, surface,
		"Fog must still composite after the shadowed terrain light result.")
	assert_false(device.contains("sun_shadow_catcher.gdshader"),
		"The page-alpha result replaces the legacy final-RGB black shadow overlay.")


func test_capture_variants_control_page_shadows_without_a_static_shadow_map() -> void:
	var terrain_header := _source("res://src/terrain/nova_terrain.h")
	var terrain_source := _source("res://src/terrain/nova_terrain.cpp")
	var device_header := _source(
		"res://src/terrain/nova_terrain_tile_cache_device.h")
	var world := _source("res://game/world/game_world.gd")
	var session := _source(
		"res://game/world/shadow_attribution_capture_session.gd")
	assert_true(terrain_header.contains("set_static_terrain_shadow_enabled("))
	assert_true(terrain_header.contains(
			"set_suppressed_static_shadow_bms_ids("))
	assert_true(device_header.contains("invalidate_static_shadow_pages()"),
		"Provider control changes need an explicit ready-binding invalidation seam.")
	assert_true(terrain_source.count(
			"tile_cache_device.invalidate_static_shadow_pages();") >= 3,
		"Attach, enable, and suppression changes must retire stale page bindings.")
	assert_false(world.contains("NovaStaticSunShadow"),
		"The page provider makes the old static DirectionalLight shadow map obsolete.")
	assert_true(session.contains(
			"_terrain.set_static_terrain_shadow_enabled("),
		"Canonical shadows_off must disable page-composed silhouettes too.")
	assert_true(session.contains(
			"_terrain.set_suppressed_static_shadow_bms_ids("),
		"Attribution suppression must filter the provider's typed BMS sources.")
	assert_false(session.contains('get_node_or_null(\n\t\t\t"NovaStaticSunShadow")'),
		"Capture state must not depend on the retired static shadow-map node.")


func test_device_frame_diagnostics_are_bounded_and_capacity_is_explicit() -> void:
	var header := _source("res://src/terrain/nova_terrain_tile_cache_device.h")
	var source := _source("res://src/terrain/nova_terrain_tile_cache_device.cpp")
	for counter in [
			"frame_requests_",
			"frame_ready_hits_",
			"frame_stale_hits_",
			"frame_selected_ready_pages_",
		"frame_compose_jobs_",
		"frame_compose_us_",
		"frame_uploads_",
		"frame_capacity_fallbacks_",
		]:
		assert_true(header.contains(counter),
			"The device must own bounded current-frame counter %s." % counter)
		assert_true(source.contains('diagnostics["%s"]' % counter.trim_suffix("_")),
			"The public terrain diagnostics must expose %s." % counter)
	var begin_frame := source.find("TerrainTileCacheDevice::begin_frame(")
	var same_frame_gate := source.find(
			"diagnostic_frame_id_ != p_frame_id", begin_frame)
	var cache_begin := source.find("cache_.begin_frame(p_frame_id)", begin_frame)
	assert_gt(same_frame_gate, begin_frame,
		"Diagnostic counters reset only when the logical frame changes.")
	assert_gt(cache_begin, same_frame_gate)
	var null_decision := source.find("if (!decision.has_value())")
	var valid_lod_gate := source.find(
			"request.page.page_lod_level) == 0")
	var capacity_increment := source.find("++frame_capacity_fallbacks_", null_decision)
	var job_check := source.find("if (!decision->job.has_value())", null_decision)
	assert_gt(valid_lod_gate, -1)
	assert_lt(valid_lod_gate, null_decision,
		"Invalid page levels fail before the capacity-only null-decision counter.")
	assert_gt(capacity_increment, null_decision)
	assert_lt(capacity_increment, job_check,
		"Only a valid cache request with no available slot is a capacity fallback.")
	var request_increment := source.find("++frame_requests_", valid_lod_gate)
	assert_gt(request_increment, valid_lod_gate,
		"Only a validated page request belongs in the coverage denominator.")
	assert_lt(request_increment, null_decision,
		"Every portable-cache decision must be covered by a frame request.")
	var ready_hit_increment := source.find("++frame_ready_hits_", job_check)
	var ready_hit_return := source.find("return decision->binding", job_check)
	assert_gt(ready_hit_increment, job_check,
		"A resident ready decision must be counted separately from composition.")
	assert_lt(ready_hit_increment, ready_hit_return)
	var cached_ready_selection := source.find(
			"_record_frame_selected_ready(decision->binding)", job_check)
	assert_gt(cached_ready_selection, job_check,
		"A ready cache hit must contribute current-frame selection evidence.")
	assert_lt(cached_ready_selection, ready_hit_return)
	var drain := source.find("TerrainTileCacheDevice::_drain_completed()")
	var generation_gate := source.find("cache_.can_publish(job)", drain)
	var upload := source.find("texture_->update_layer(image, job.target.layer)", drain)
	var publish := source.find("cache_.publish(job)", upload)
	assert_gt(generation_gate, drain)
	assert_gt(upload, generation_gate)
	assert_gt(publish, upload)
	assert_true(source.contains("frame_compose_us_ += completion.compose_us"),
		"Worker CPU time is accounted when a bounded completion is drained.")
	var clear := source.find("void TerrainTileCacheDevice::clear()")
	var rasterizer_setter := source.find(
			"void TerrainTileCacheDevice::set_static_shadow_rasterizer(", clear)
	var clear_body := source.substr(clear, rasterizer_setter - clear)
	assert_true(clear_body.contains("diagnostic_frame_active_ = false"))
	assert_true(clear_body.contains("frame_requests_ = 0"))
	assert_true(clear_body.contains("frame_ready_hits_ = 0"))
	assert_true(clear_body.contains("frame_stale_hits_ = 0"))
	assert_true(clear_body.contains("frame_selected_ready_pages_ = 0"))
	assert_true(clear_body.contains("frame_compose_jobs_ = 0"))
	assert_true(clear_body.contains("frame_compose_us_ = 0"))
	assert_true(clear_body.contains("frame_uploads_ = 0"))
	assert_true(clear_body.contains("frame_capacity_fallbacks_ = 0"))
	assert_true(clear_body.contains(
			"frame_shadow_alpha_changed_bytes_ = 0"))
	assert_true(clear_body.contains("frame_shadow_rgb_changed_bytes_ = 0"))
	assert_true(clear_body.contains("frame_output_pages_ = 0"))
	assert_true(clear_body.contains("frame_output_hash_ = 0"))
	var begin_body := source.substr(begin_frame, cache_begin - begin_frame)
	assert_true(begin_body.contains("frame_requests_ = 0"))
	assert_true(begin_body.contains("frame_ready_hits_ = 0"))
	assert_true(begin_body.contains("frame_stale_hits_ = 0"))
	assert_true(begin_body.contains("frame_selected_ready_pages_ = 0"))
	assert_true(begin_body.contains("frame_selected_ready_layers_.fill(false)"))
	assert_true(source.contains("for (uint8_t value : completion.pixels.pixels)"),
		"A stable CPU output hash must cover every final composed page byte.")


func test_provider_uses_page_local_point_receiver_state_and_complete_raster_stamp() -> void:
	# The policy half moved into engine/runtime/terrain (planner + geometry);
	# the adapter keeps only marshalling, so each pin points at the file that
	# owns the behavior now.
	var adapter := _source(
		"res://src/terrain/nova_terrain_static_shadow_rasterizer.cpp")
	var planner := _source(
		"res://../engine/runtime/terrain/terrain_static_shadow_planner.cpp")
	var geometry := _source(
		"res://../engine/runtime/terrain/terrain_static_shadow_geometry.cpp")
	assert_true(planner.contains("page_receiver_minimum("),
		"The broad phase must use the requested page's receiver minimum.")
	assert_true(adapter.contains("get_height_world(source.world_transform.origin)"),
		"Retail grounds each caster with the point sampler, not bilinear interpolation.")
	assert_false(adapter.contains(
		"get_height_world_bilinear(source.world_transform.origin)"))
	for token in [
			"resolved.render_object_offset",
			"resolved.material_index",
			"resolved.uvs",
			"material.blend",
			"material.alpha_scale",
			"pyramid->mips",
			"pyramid->storage",
			"material.team_alpha_frames",
	]:
		assert_true(geometry.contains(token),
			"Raster-affecting input must participate in geometry content: %s" % token)
	assert_true(planner.contains("hash_value(revision, record.team)"),
		"Team selects TEX_TEAM alpha frames, so it must invalidate resident pages.")
	assert_true(geometry.contains('strutil::iequals(anim_control_name, "TEX_TEAM")'),
		"Only the witnessed discrete TEX_TEAM flipbook can select caster-local frames.")
	assert_false(geometry.contains("UnsupportedLivePanmLod"),
		"The retail tile pass submits entity transforms and does not evaluate PANM.")


func test_provider_attributes_partial_or_malformed_selected_robj_geometry() -> void:
	var planner := _source(
		"res://../engine/runtime/terrain/terrain_static_shadow_planner.cpp")
	var geometry := _source(
		"res://../engine/runtime/terrain/terrain_static_shadow_geometry.cpp")
	var portable := _source(
		"res://../engine/runtime/terrain/terrain_static_shadow.h")
	assert_true(portable.contains("is_valid_empty()"),
		"An authored zero-strip hierarchy ROBJ must remain an exact no-op.")
	assert_true(portable.contains(
			"authored_surface_count == valid_surface_count"),
		"Any partial decoded surface population must be observably inexact.")
	assert_true(geometry.contains(
			"terrain_static_shadow_strip_indices_are_valid("),
		"The resolver must inspect authored indices before the curated soup drops them.")
	assert_true(geometry.contains("expand_authored_strip_bounds("),
		"Malformed-index attribution still needs conservative authored page bounds.")
	for reason in [
		"incomplete_geometry",
		"malformed_indices",
		"missing_required_uvs",
	]:
		assert_true(geometry.contains('names.push_back("%s")' % reason),
			"Canonical diagnostics must attribute %s." % reason)
	assert_true(planner.contains("admitted && !record.geometry->bounds_exact"),
		"Unbound malformed geometry must fail planning globally, never publish as exact.")
