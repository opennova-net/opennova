extends GutTest

# A painted region of the colour map or the detail blend map (an editor's paint stroke, ADR 0046 S25) updates the
# terrain in place: the tile cache carries the region through the colour quadrants (their DXT1 levels at the default
# texcompression_level 1) and recomposes every page that samples it, wherever the frame drew it, to the bytes a
# terrain built over the painted map composes; the blend texture is normalized again over the region, in place.

# The synthetic Tmap terrain (fixtures/terrain/tmap) staged over the minimal assets it names, with a colour map and a
# detail blend map of the game's side (1024 texels) written over the staged ones.
const TMAP_STAGE := "colormap_region"
const WRAP_STAGE := "colormap_region_wrap"

var _wrap_root := ""


func before_all() -> void:
	_stage_maps(TestFs.staged_tmap(TMAP_STAGE).get_base_dir())


func after_all() -> void:
	TestFs.release_staged_tmap(TMAP_STAGE)
	if not _wrap_root.is_empty():
		TestFs.remove_dir_recursive(_wrap_root)


# A 1024-texel map whose texels vary (each row a window of a long varied run), as a 32-bit TGA.
func _varied_tga(seed: int) -> PackedByteArray:
	var run := PackedByteArray()
	run.resize(2048 * 4)
	for i in 2048:
		run[i * 4] = (i * 3 + seed) & 0xFF
		run[i * 4 + 1] = (i * 7 + seed * 5) & 0xFF
		run[i * 4 + 2] = (i ^ (seed * 11)) & 0x7F
		run[i * 4 + 3] = 255
	var bytes := TestFs.tga_bytes(Vector2i(1, 1)).slice(0, 18)
	bytes.encode_u16(12, 1024)
	bytes.encode_u16(14, 1024)
	for y in 1024:
		var start := ((y * 37) % 1024) * 4
		bytes.append_array(run.slice(start, start + 1024 * 4))
	return bytes


# A 1024-texel colour map and blend map over the staged ones.
func _stage_maps(root: String) -> void:
	TestFs.write_bytes(self, root.path_join("mnml_c.tga"), _varied_tga(1))
	TestFs.write_bytes(self, root.path_join("mnml_d1.tga"), _varied_tga(2))


func _loaded_data(trn := "") -> TerrainData:
	var data := TerrainData.new()
	data.set_trn_path(trn if not trn.is_empty() else TestFs.staged_tmap(TMAP_STAGE))
	assert_eq(data.load(), OK, "the Tmap terrain fixture must load")
	return data


func _viewed_terrain(data: TerrainData) -> Terrain:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)
	var terrain := Terrain.new()
	viewport.add_child(terrain)
	terrain.set_terrain_data(data)
	terrain.set_tile_cache_capture_diagnostics(true)
	terrain.build()
	terrain.set_debug_no_frustum(true)
	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()
	return terrain


# A painted square: `size` texels of one colour.
func _region(size: int, colour: Color) -> Image:
	var region := Image.create(size, size, false, Image.FORMAT_RGBA8)
	region.fill(colour)
	return region


# The texels (560, 560) of the 1024 map: the fourth quadrant's, which the sector under the camera draws (the grid's 4
# at its cell, slot 3: +512 on both axes).
const AT := Vector2i(560, 560)


# The same terrain built over `painted` (the live map the update left), viewed alike, settled.
func _built_over(painted: Image, trn := "") -> Dictionary:
	var fresh_data := _loaded_data(trn)
	fresh_data.set_colormap_image(painted.duplicate())
	return await TestFs.settle_tile_cache(self, _viewed_terrain(fresh_data))


func test_a_painted_colour_region_recomposes_only_its_pages() -> void:
	var data := _loaded_data()
	var terrain := _viewed_terrain(data)
	var settled := await TestFs.settle_tile_cache(self, terrain)
	var revision := int(settled["source_revision"])
	var resident := int(settled["ready_pages"])
	var before_hash := int(settled["resident_output_hash"])
	assert_gt(resident, 1)

	assert_true(terrain.update_colormap_region(_region(32, Color(1.0, 0.0, 0.0)), AT))
	var updated := terrain.get_tile_cache_diagnostics()
	assert_eq(int(updated["source_revision"]), revision, "a painted region keeps the sources: no rebuild")
	assert_eq(int(updated["colormap_region_updates"]), 1)
	var retired := int(updated["colormap_pages_retired"])
	assert_gt(retired, 0, "the pages that sample the region recompose")
	assert_lt(retired, resident, "the pages that do not keep their bytes")
	var live := data.get_colormap_image()
	assert_not_null(live, "the data keeps the painted map for its next build")
	if live == null:
		return
	assert_eq(live.get_pixelv(AT + Vector2i(5, 5)), Color(1.0, 0.0, 0.0))

	settled = await TestFs.settle_tile_cache(self, terrain)
	assert_ne(int(settled["resident_output_hash"]), before_hash, "the pages under the region show it")
	var fresh := await _built_over(live)
	assert_eq(int(settled["ready_pages"]), int(fresh["ready_pages"]))
	assert_eq(int(settled["resident_output_hash"]), int(fresh["resident_output_hash"]),
		"the pages hold what a build over the painted map composes")


# A grid whose edge cells are mapped, wrapping: the frame's window draws sectors past the grid, each its own page over
# the same quadrants. Every page that samples the region recomposes, the repeats included.
func test_pages_past_the_grid_recompose_too() -> void:
	_wrap_root = TestFs.stage_terrain_root(WRAP_STAGE)
	_stage_maps(_wrap_root)
	var trn := _wrap_root.path_join("Tmap.trn")
	var text := FileAccess.get_file_as_string(trn)
	assert_false(text.is_empty(), "the staged Tmap.trn reads: %s (%d)" % [trn, FileAccess.get_open_error()])
	# The staged .trn's lines end in CR LF, the break the game's reader splits at: each line rewritten keeps its CR.
	var lines := text.split("\n")
	var row := 0
	for i in lines.size():
		var cr := "\r" if lines[i].ends_with("\r") else ""
		var line := lines[i].trim_suffix("\r")
		if line.begins_with("polytrn_sectors"):
			# Rows 3 and 4 map their edge cells too (the grid's 4 at each end).
			if row == 3 or row == 4:
				var cells := line.split("\t", false)
				cells[1] = "4"
				cells[8] = "4"
				lines[i] = "\t".join(cells) + cr
			row += 1
		elif line.begins_with("polytrn_wrapx"):
			lines[i] = "polytrn_wrapx\t\t\t1" + cr
	TestFs.write_bytes(self, trn, "\n".join(lines).to_utf8_buffer())
	var data := _loaded_data(trn)
	var terrain := _viewed_terrain(data)
	await TestFs.settle_tile_cache(self, terrain)
	assert_true(terrain.update_colormap_region(_region(32, Color(0.0, 0.0, 1.0)), AT))
	var settled := await TestFs.settle_tile_cache(self, terrain)
	var fresh := await _built_over(data.get_colormap_image(), trn)
	assert_eq(int(settled["resident_output_hash"]), int(fresh["resident_output_hash"]),
		"every page the frame drew over the painted quadrant holds what a build over the painted map composes")


func test_a_region_outside_the_map_is_refused() -> void:
	var data := _loaded_data()
	var terrain := _viewed_terrain(data)
	await TestFs.settle_tile_cache(self, terrain)
	assert_false(terrain.update_colormap_region(_region(8, Color.WHITE), Vector2i(1020, 0)),
		"a region past the map's edge changes nothing")
	assert_eq(int(terrain.get_tile_cache_diagnostics()["colormap_region_updates"]), 0)


# What a dab costs on the main thread: a 32-metre one is 64 texels of the 1024 map (a metre a texel), its quadrant's
# DXT1 levels made again over the dab alone. The number is printed (the S25 plan's budget is 8 ms a dab).
func test_a_dab_costs_its_region() -> void:
	var data := _loaded_data()
	var terrain := _viewed_terrain(data)
	await TestFs.settle_tile_cache(self, terrain)
	var region := _region(64, Color(0.2, 0.7, 0.1))
	assert_true(terrain.update_colormap_region(region, AT)) # the live map taken from the texture the first time
	var total := 0
	for i in 8:
		var started := Time.get_ticks_usec()
		assert_true(terrain.update_colormap_region(region, AT + Vector2i(8 * i, 4 * i)))
		total += Time.get_ticks_usec() - started
	gut.p("a 64-texel colour dab in place: %.2f ms on average" % (total / 8000.0))
	var blend := _region(64, Color(0.0, 1.0, 0.0))
	assert_true(terrain.update_blendmap_region(blend, AT))
	total = 0
	for i in 8:
		var started := Time.get_ticks_usec()
		assert_true(terrain.update_blendmap_region(blend, AT + Vector2i(8 * i, 4 * i)))
		total += Time.get_ticks_usec() - started
	gut.p("a 64-texel blend dab in place: %.2f ms on average" % (total / 8000.0))


func test_a_painted_blend_map_updates_its_texture_in_place() -> void:
	var data := _loaded_data()
	assert_not_null(data.get_detailblendmap(), "the staged blend map loads")
	var terrain: Terrain = add_child_autofree(Terrain.new())
	terrain.set_terrain_data(data)
	terrain.build()
	var material := terrain.get_terrain_material()
	var held: Texture2D = material.get_shader_parameter("u_blendmap")
	assert_not_null(held)
	assert_true(terrain.update_blendmap_region(_region(16, Color(0.0, 1.0, 0.0)), AT))
	var after: Texture2D = material.get_shader_parameter("u_blendmap")
	assert_same(after, held, "the material keeps sampling the same texture")
	var live := data.get_blendmap_image()
	assert_not_null(live, "the data keeps the painted map for its next build")
	if live == null:
		return
	# The texture's map is a build's over the painted map: the region normalized (green alone: 255 there), the rest
	# as it was.
	var fresh_data := _loaded_data()
	fresh_data.set_blendmap_image(live.duplicate())
	var fresh: Terrain = add_child_autofree(Terrain.new())
	fresh.set_terrain_data(fresh_data)
	fresh.build()
	var mine := terrain.get_surface_inputs().get_normalized_blend_image()
	var theirs := fresh.get_surface_inputs().get_normalized_blend_image()
	assert_not_null(mine)
	if mine == null or theirs == null:
		return
	assert_eq(mine.get_pixelv(AT + Vector2i(3, 3)).g8, 255)
	assert_eq(mine.get_data(), theirs.get_data(), "the normalized map a build of the painted map makes")
