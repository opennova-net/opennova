extends GutTest



# The synthetic Tmap terrain (fixtures/terrain/tmap) staged over the minimal
# assets it names; one root per test file (TestFs.staged_tmap), removed at the end.
const TMAP_STAGE := "foliage_tile_cache"


func after_all() -> void:
	TestFs.release_staged_tmap(TMAP_STAGE)


func _sample_height(_world_x: float, _world_z: float) -> float:
	return 10.0


func _sample_foliage(_world_x: float, _world_z: float) -> int:
	return 1


func _visible_detail_draws(dispatcher: FoliageDispatcher) -> Array:
	var rows: Array = []
	var report: Dictionary = dispatcher.get_backend_report()
	for row_value in report.get("draws", []):
		var row := row_value as Dictionary
		if bool(row.get("visible", false)) and String(row.get("tier", "")) == "detail":
			rows.append(row)
	return rows


func _settle_tile_cache_with_foliage(
		terrain: Terrain, dispatcher: FoliageDispatcher, camera: Camera3D) -> Dictionary:
	var diagnostics: Dictionary = {}
	for _attempt in range(512):
		terrain.render_frame()
		dispatcher.render_frame(camera.global_transform, GameWorld.current_frame_clock_ms())
		diagnostics = terrain.get_tile_cache_diagnostics()
		if int(diagnostics.get("pending_jobs", -1)) == 0 \
				and int(diagnostics.get("frame_requests", 0)) > 0 \
				and int(diagnostics.get("frame_ready_hits", -1)) \
						== int(diagnostics.get("frame_requests", 0)):
			return diagnostics
		await get_tree().process_frame
	assert_true(false, "the bounded terrain compiler must settle pages before foliage borrows them")
	return diagnostics


func test_runtime_detail_foliage_borrows_terrains_ready_page_binding() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)

	var data := TerrainData.new()
	data.set_trn_path(TestFs.staged_tmap(TMAP_STAGE))
	assert_eq(data.load(), OK, "the Tmap fixture terrain must load")

	var terrain := Terrain.new()
	viewport.add_child(terrain)
	terrain.set_terrain_data(data)
	terrain.build()
	terrain.set_debug_no_frustum(true)

	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()

	var dispatcher := FoliageDispatcher.new()
	viewport.add_child(dispatcher)
	var definition := TerrainFoliageDef.new()
	definition.graphic = "tile_cache_contract"
	definition.match = PackedInt32Array([1])
	dispatcher.configure_slots([definition], [BoxMesh.new()], [])
	dispatcher.height_sampler = Callable(self, "_sample_height")
	dispatcher.foliage_sampler = Callable(self, "_sample_foliage")
	dispatcher.set_terrain(terrain)

	# Retail's first terrain frame claims no page record (every record was last
	# used only one frame ago), so nothing composes and the patches draw
	# without pages. A detail patch whose page lookup finds nothing is not
	# drawn at all (retail Foliage_RenderDetailPatches skips to the next slot
	# on a null Terrain_FindSectorPatchRT @ 0x60a1e6..0x60a1e8), so detail
	# foliage stays away until the next terrain frame composes every visible
	# page before its draw.
	terrain.render_frame()
	var cold := terrain.get_tile_cache_diagnostics()
	assert_eq(int(cold.get("pending_jobs", -1)), 0)
	assert_eq(int(cold.get("frame_ready_hits", -1)), 0)
	assert_eq(int(cold.get("frame_uploads", -1)), 0)
	assert_eq(int(cold.get("frame_capacity_fallbacks", -1)),
			int(cold.get("frame_requests", 0)))
	# The first foliage pass fills retail's detail cache; the second submits its
	# resident geometry without giving terrain a frame-start publication point.
	dispatcher.render_frame(camera.global_transform, GameWorld.current_frame_clock_ms())
	dispatcher.render_frame(camera.global_transform, GameWorld.current_frame_clock_ms())
	var cold_stats := dispatcher.get_frame_stats()
	assert_gt(int(cold_stats.detail_cache_submissions), 0,
		"the cold frame's detail cells are resident and submitted")
	assert_eq(_visible_detail_draws(dispatcher).size(), 0,
		"a patch with no resident terrain page draws no detail foliage")

	var settled := await _settle_tile_cache_with_foliage(terrain, dispatcher, camera)
	assert_eq(int(settled["pending_jobs"]), 0)
	assert_eq(int(settled["frame_ready_hits"]), int(settled["frame_requests"]))

	var page_array: TextureLayered = terrain.get_tile_cache_texture()
	assert_not_null(page_array)
	var ready_draws := 0
	var fine_ready_draws := 0
	for row_value in _visible_detail_draws(dispatcher):
		var draw := row_value as Dictionary
		var material := draw.material as ShaderMaterial
		assert_not_null(material)
		if material == null:
			continue
		assert_same(material.get_shader_parameter("u_tile_cache"), page_array,
			"Terrain and detail foliage must sample one shared Texture2DArray.")
		assert_true(bool(material.get_shader_parameter("u_has_tile_cache")))
		assert_true(bool(draw.tile_cache_ready),
			"every drawn detail patch borrows a resident terrain page")
		if not bool(draw.tile_cache_ready):
			continue
		ready_draws += 1
		var layer := int(draw.tile_cache_layer)
		assert_between(layer, 0, page_array.get_layers() - 1)
		var page := draw.tile_cache_projection as Vector4
		# The lookup can answer with the flat page (span 1024) in its
		# canonical (0,0) sector: its packed coordinate masks to zero.
		assert_true(page.w == 64.0 or page.w == 128.0 or page.w == 256.0 \
				or page.w == 512.0 or page.w == 1024.0)
		if page.w == 64.0:
			fine_ready_draws += 1
		assert_almost_eq(page.z, 1.0 / page.w, 0.000001)
		var shared_point: Vector3 = (draw.mesh as Mesh).get_aabb().get_center()
		assert_between(shared_point.x, page.x, page.x + page.w)
		assert_between(shared_point.z, page.y, page.y + page.w)

	assert_gt(ready_draws, 0,
		"At least one visible detail cell must borrow a terrain-resident page/layer binding.")
	assert_gt(fine_ready_draws, 0,
		"The near-camera warmup must retain a fine page for the cross-frame LOD regression.")

	# On this pinned Tmap view, quality 0.3 keeps the same 25 near detail cells
	# while selecting 128-unit pages over them. The prior frame's overlapping
	# 64-unit pages stay resident in the 128-record cache, and retail's lookup
	# takes the first resident record in record order at the finest matching
	# granularity with no LOD or same-frame preference, so the fine pages
	# claimed first keep answering for the cells they cover.
	# (retail Terrain_FindSectorPatchRT @ 0x6042B0..0x60430B)
	terrain.set_lod_quality(0.3)
	await _settle_tile_cache_with_foliage(terrain, dispatcher, camera)
	assert_gt(int(dispatcher.get_frame_stats().detail_cells), 0,
		"The coarse regression frame must retain a live terrain detail handoff.")
	var borrowed_fine_draws := 0
	for row_value in _visible_detail_draws(dispatcher):
		var draw := row_value as Dictionary
		if bool(draw.tile_cache_ready) and (draw.tile_cache_projection as Vector4).w == 64.0:
			borrowed_fine_draws += 1
	assert_gt(borrowed_fine_draws, 0,
		"Resident fine pages from the earlier frames still answer the lookup first.")

	# Release native texture owners before RenderingServer teardown; keeping the
	# resource locals alive until process exit makes Godot report false leaks.
	dispatcher.set_terrain(null)
	dispatcher.reset()
	terrain.set_terrain_data(null)
	viewport.free()
	page_array = null
	data = null
