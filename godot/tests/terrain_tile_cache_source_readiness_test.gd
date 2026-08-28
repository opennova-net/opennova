extends GutTest



var _terrain_root := ""


# The synthetic Tmap terrain (fixtures/terrain/tmap) staged over the minimal
# assets it names; one root per test file, removed at the end.
func _tmap_trn() -> String:
	if _terrain_root.is_empty():
		_terrain_root = TestFs.stage_terrain_root("tile_cache_readiness")
	return _terrain_root.path_join(TestFs.TMAP_TRN)


func after_all() -> void:
	if not _terrain_root.is_empty():
		TestFs.remove_dir_recursive(_terrain_root)
		_terrain_root = ""


func _loaded_data() -> TerrainData:
	var data := TerrainData.new()
	data.set_trn_path(_tmap_trn())
	assert_eq(data.load(), OK, "the Tmap terrain fixture must load")
	return data


func _build(data: TerrainData, tile_info: TerrainTileInfo = null) -> Terrain:
	var terrain: Terrain = add_child_autofree(Terrain.new())
	terrain.set_terrain_data(data)
	if tile_info != null:
		terrain.set_tile_info_override(tile_info)
	terrain.build()
	return terrain


func _one_authored_tile() -> TerrainTileInfo:
	var tile_info := TerrainTileInfo.new()
	var entry := TerrainTileEntry.new()
	entry.set_cell(0, 0)
	entry.set_tile_index(0)
	tile_info.add_entry(entry)
	return tile_info


func test_nonempty_authored_overlay_without_tilestrip_fails_page_cache_closed() -> void:
	var data := _loaded_data()
	data.set_tilestrip_tex(null)
	var terrain := _build(data, _one_authored_tile())

	var diagnostics := terrain.get_tile_cache_diagnostics()
	assert_false(bool(diagnostics["available"]),
		"the device must not publish a base-only cache when authored tiles are missing")
	assert_null(terrain.get_tile_cache_texture())
	assert_false(bool(terrain.get_terrain_material().get_shader_parameter(
		"u_has_tile_cache")))
	assert_true(bool(diagnostics["tile_overlay_required"]),
		"enabled, nonempty .til content makes its atlas a required page source")
	assert_false(bool(diagnostics["tile_overlay_available"]))


func test_declared_but_unreadable_tile_info_fails_page_cache_closed() -> void:
	var data := _loaded_data()
	data.set_tileinfo_filename("missing-authored-overlay.til")
	var terrain := _build(data)

	var diagnostics := terrain.get_tile_cache_diagnostics()
	assert_false(bool(diagnostics["available"]),
		"an unreadable declared .til must not degrade into a base-only page cache")
	assert_null(terrain.get_tile_cache_texture())
	assert_true(bool(diagnostics["tile_overlay_required"]),
		"a declared .til remains required when resolution or parsing fails")
	assert_false(bool(diagnostics["tile_overlay_available"]))


func test_tile_free_mission_keeps_base_page_cache_ready_without_tilestrip() -> void:
	var data := _loaded_data()
	data.set_tileinfo_filename("")
	data.set_tilestrip_tex(null)
	var terrain := _build(data)

	var diagnostics := terrain.get_tile_cache_diagnostics()
	assert_false(bool(diagnostics["tile_overlay_required"]),
		"a mission with no authored .til has no overlay source obligation")
	assert_false(bool(diagnostics["tile_overlay_available"]))
	assert_true(bool(diagnostics["available"]),
		"optional absence must not disable otherwise complete base page sources")
	assert_not_null(terrain.get_tile_cache_texture())
	assert_true(bool(terrain.get_terrain_material().get_shader_parameter(
		"u_has_tile_cache")))


func _settle(terrain: Terrain) -> Dictionary:
	var diagnostics: Dictionary = {}
	for _attempt in range(2048):
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


func test_unresolved_scorch_set_rejects_records_without_dropping_base_pages() -> void:
	# The fixture root carries no trscrch/qburn decal TGAs, so the permanent
	# scorch overlay is absent for the whole mission. That is an optional
	# overlay source: the base colormap/normal page cache still publishes, a
	# record the overlay cannot draw is rejected at append, and every visible
	# page keeps resolving through a ready binding. Scorch state is never a
	# reason to drop a tile binding.
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)
	var data := _loaded_data()
	data.set_tileinfo_filename("")
	data.set_tilestrip_tex(null)
	var terrain := Terrain.new()
	viewport.add_child(terrain)
	terrain.set_terrain_data(data)
	terrain.build()
	terrain.set_debug_no_frustum(true)
	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()

	var diagnostics := terrain.get_tile_cache_diagnostics()
	assert_true(bool(diagnostics["available"]),
		"an absent scorch set must not disable the base page cache")
	assert_false(bool(diagnostics["scorch_textures_ready"]),
		"the fixture resource root resolves no scorch decal textures")
	var generation := int(diagnostics["scorch_generation"])
	assert_false(terrain.append_terrain_scorch(0, 60 << 16, 60 << 16, 68 << 16, 68 << 16),
		"a record the overlay cannot draw is rejected at append")
	diagnostics = terrain.get_tile_cache_diagnostics()
	assert_eq(int(diagnostics["scorch_records"]), 0)
	assert_eq(int(diagnostics["scorch_records_rejected"]), 1)
	assert_eq(int(diagnostics["scorch_generation"]), generation,
		"a rejected record leaves the registry generation alone")
	assert_true(bool(diagnostics["available"]),
		"a rejected scorch record never drops the base page cache")

	var settled := await _settle(terrain)
	assert_gt(int(settled["frame_requests"]), 0)
	assert_eq(int(settled["frame_ready_hits"]), int(settled["frame_requests"]),
		"every visible page still resolves through a ready binding")
	assert_eq(int(settled["upload_failures"]), 0)
	assert_eq(int(settled["frame_capacity_fallbacks"]), 0)
	terrain.clear_terrain_scorches()
	terrain.render_frame()
	var cleared := terrain.get_tile_cache_diagnostics()
	assert_true(bool(cleared["available"]))
	assert_eq(int(cleared["frame_ready_hits"]), int(cleared["frame_requests"]),
		"clearing an empty registry keeps every resident page ready")
	terrain.set_terrain_data(null)
	viewport.free()
