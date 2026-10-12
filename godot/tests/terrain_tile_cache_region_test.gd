extends GutTest

# An edit of the placed tiles (an editor's tile tools, ADR 0046 S25) recomposes only the cache pages its changed
# records' squares show: Terrain's tile-info change runs TerrainTileCacheDevice::set_tile_entries in place of the
# rebuild, its pages the bytes a terrain built with the new placement composes; tiles over a terrain built with
# none rebuild its sources (the tile set was never loaded).

# The synthetic Tmap terrain (fixtures/terrain/tmap) staged over the minimal assets it names.
const TMAP_STAGE := "tile_cache_region"


func after_all() -> void:
	TestFs.release_staged_tmap(TMAP_STAGE)


## The staged tile set (mnml_t.tga) made one the tile pass draws: 128 texels square, four 64-texel cells of their
## own colours (a set under 64 texels tall draws no tile, D-TIL-8), so a placed tile changes the pages under it.
func _stage_tile_set(trn: String) -> void:
	var colours := [Color(1, 0, 0), Color(0, 1, 0), Color(0, 0, 1), Color(1, 1, 0)]
	var bytes := TestFs.tga_bytes(Vector2i(128, 128))
	for y in 128:
		for x in 128:
			var colour: Color = colours[(y / 64) * 2 + x / 64]
			var at := 18 + (y * 128 + x) * 4
			bytes[at] = colour.b8
			bytes[at + 1] = colour.g8
			bytes[at + 2] = colour.r8
			bytes[at + 3] = 255
	TestFs.write_bytes(self, trn.get_base_dir().path_join("mnml_t.tga"), bytes)


func _loaded_data() -> TerrainData:
	var data := TerrainData.new()
	var trn := TestFs.staged_tmap(TMAP_STAGE)
	_stage_tile_set(trn)
	data.set_trn_path(trn)
	assert_eq(data.load(), OK, "the Tmap terrain fixture must load")
	return data


func _tile(cell_x: int, cell_z: int, index: int) -> TerrainTileEntry:
	var entry := TerrainTileEntry.new()
	entry.set_cell(cell_x, cell_z)
	entry.set_tile_index(index)
	return entry


# A terrain over `tile_info` in a viewport whose camera looks over the fixture's sectors, its page hashes kept.
func _viewed_terrain(tile_info: TerrainTileInfo) -> Terrain:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)
	var terrain := Terrain.new()
	viewport.add_child(terrain)
	terrain.set_terrain_data(_loaded_data())
	terrain.set_tile_info_override(tile_info)
	terrain.set_tile_cache_capture_diagnostics(true)
	terrain.build()
	terrain.set_debug_no_frustum(true)
	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()
	return terrain


func test_an_added_tile_recomposes_only_its_pages() -> void:
	var tiles := TerrainTileInfo.new()
	tiles.add_entry(_tile(2, 2, 0))
	var terrain := _viewed_terrain(tiles)
	var settled := await TestFs.settle_tile_cache(self, terrain)
	assert_true(bool(settled["tile_overlay_available"]), "the tile set is a page source")
	var revision := int(settled["source_revision"])
	var resident := int(settled["ready_pages"])
	var before_hash := int(settled["resident_output_hash"])
	assert_gt(resident, 1)

	tiles.add_entry(_tile(3, 2, 1))
	var updated := terrain.get_tile_cache_diagnostics()
	assert_eq(int(updated["source_revision"]), revision, "an edit of the entries keeps the sources: no rebuild")
	assert_eq(int(updated["tile_entry_updates"]), 1)
	var retired := int(updated["tile_entry_pages_retired"])
	assert_gt(retired, 0, "the pages the new tile's square overlaps recompose")
	assert_lt(retired, resident, "the pages it does not overlap keep their bytes")

	settled = await TestFs.settle_tile_cache(self, terrain)
	assert_eq(int(settled["frame_ready_hits"]), int(settled["frame_requests"]),
		"every visible page resolves again")
	assert_ne(int(settled["resident_output_hash"]), before_hash, "the new tile's pages show it")
	# The pages hold the bytes a terrain built with both tiles composes.
	var both := TerrainTileInfo.new()
	both.add_entry(_tile(2, 2, 0))
	both.add_entry(_tile(3, 2, 1))
	var fresh := await TestFs.settle_tile_cache(self, _viewed_terrain(both))
	assert_eq(int(settled["ready_pages"]), int(fresh["ready_pages"]))
	assert_eq(int(settled["resident_output_hash"]), int(fresh["resident_output_hash"]),
		"an in-place edit composes what a build of the new placement does")


func test_a_tile_removed_from_the_middle_recomposes_as_a_build_without_it() -> void:
	var tiles := TerrainTileInfo.new()
	tiles.add_entry(_tile(2, 2, 0))
	tiles.add_entry(_tile(3, 2, 1))
	tiles.add_entry(_tile(2, 3, 2))
	var terrain := _viewed_terrain(tiles)
	var settled := await TestFs.settle_tile_cache(self, terrain)
	var revision := int(settled["source_revision"])
	var before_hash := int(settled["resident_output_hash"])

	# The middle record removed: the records after it move up one, every square they move past recomposes.
	tiles.remove_entry(1)
	var updated := terrain.get_tile_cache_diagnostics()
	assert_eq(int(updated["source_revision"]), revision, "a removal keeps the sources: no rebuild")
	assert_gt(int(updated["tile_entry_pages_retired"]), 0)
	settled = await TestFs.settle_tile_cache(self, terrain)
	assert_ne(int(settled["resident_output_hash"]), before_hash, "the removed tile's pages no longer show it")
	var kept := TerrainTileInfo.new()
	kept.add_entry(_tile(2, 2, 0))
	kept.add_entry(_tile(2, 3, 2))
	var fresh := await TestFs.settle_tile_cache(self, _viewed_terrain(kept))
	assert_eq(int(settled["resident_output_hash"]), int(fresh["resident_output_hash"]),
		"the pages a build of the remaining tiles composes")


func test_a_new_override_of_the_same_tiles_retires_nothing() -> void:
	var tiles := TerrainTileInfo.new()
	tiles.add_entry(_tile(2, 2, 0))
	var terrain := _viewed_terrain(tiles)
	var settled := await TestFs.settle_tile_cache(self, terrain)
	var copy := TerrainTileInfo.new()
	copy.add_entry(_tile(2, 2, 0))
	terrain.set_tile_info_override(copy)
	var updated := terrain.get_tile_cache_diagnostics()
	assert_eq(int(updated["source_revision"]), int(settled["source_revision"]))
	assert_eq(int(updated["tile_entry_pages_retired"]), 0, "the same placement changes no page")


func test_tiles_over_a_terrain_built_with_none_rebuild_its_sources() -> void:
	var data := _loaded_data()
	data.set_tileinfo_filename("")
	var terrain: Terrain = add_child_autofree(Terrain.new())
	terrain.set_terrain_data(data)
	terrain.build()
	var built := terrain.get_tile_cache_diagnostics()
	assert_true(bool(built["available"]))
	assert_false(bool(built["tile_overlay_available"]), "a terrain with no tiles loads no tile set")
	var tiles := TerrainTileInfo.new()
	tiles.add_entry(_tile(2, 2, 0))
	terrain.set_tile_info_override(tiles)
	var rebuilt := terrain.get_tile_cache_diagnostics()
	assert_ne(int(rebuilt["source_revision"]), int(built["source_revision"]),
		"the first tiles need the tile set: the sources are built again")
	assert_true(bool(rebuilt["tile_overlay_available"]))
	assert_true(bool(rebuilt["available"]))
