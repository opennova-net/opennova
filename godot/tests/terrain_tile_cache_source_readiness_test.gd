extends GutTest

const DVXI5_TRN := "res://../fixtures/godot/dvxi5/Dvxi5.trn"


func _loaded_data() -> TerrainData:
	var data := TerrainData.new()
	data.set_trn_path(ProjectSettings.globalize_path(DVXI5_TRN))
	assert_eq(data.load(), OK, "the Dvxi5 terrain fixture must load")
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
