extends GutTest

# A painted region of the colour map or the detail blend map (an editor's paint stroke, ADR 0046 S25) updates the
# terrain in place: the tile cache makes again the colour quadrants the region touches and recomposes only the pages
# that show it, to the bytes a terrain built over the painted map composes; the blend texture is normalized again in
# place.

# The synthetic Tmap terrain (fixtures/terrain/tmap) staged over the minimal assets it names.
const TMAP_STAGE := "colormap_region"


func after_all() -> void:
	TestFs.release_staged_tmap(TMAP_STAGE)


func _loaded_data() -> TerrainData:
	var data := TerrainData.new()
	data.set_trn_path(TestFs.staged_tmap(TMAP_STAGE))
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


func _rgba(texture: Texture2D) -> Image:
	var image := texture.get_image()
	if image.is_compressed():
		image.decompress()
	image.convert(Image.FORMAT_RGBA8)
	return image


# The map's rect over the atlas texels (560, 560) to (592, 592): the fourth quadrant's, which the sector under the
# camera draws (the grid's 4 at its cell, slot 3: +512 on both axes).
func _rect_near_camera(image: Image) -> Rect2i:
	var w := image.get_width()
	var h := image.get_height()
	var x0 := 560 * w / 1024
	var y0 := 560 * h / 1024
	return Rect2i(x0, y0, maxi(1, 32 * w / 1024), maxi(1, 32 * h / 1024))


func test_a_painted_colour_region_recomposes_only_its_pages() -> void:
	var data := _loaded_data()
	var terrain := _viewed_terrain(data)
	var settled := await TestFs.settle_tile_cache(self, terrain)
	var revision := int(settled["source_revision"])
	var resident := int(settled["ready_pages"])
	assert_gt(resident, 1)

	var painted := _rgba(data.get_colormap())
	var rect := _rect_near_camera(painted)
	painted.fill_rect(rect, Color(1.0, 0.0, 0.0))
	assert_true(terrain.update_colormap_region(painted, rect))
	var updated := terrain.get_tile_cache_diagnostics()
	assert_eq(int(updated["source_revision"]), revision, "a painted region keeps the sources: no rebuild")
	assert_eq(int(updated["colormap_region_updates"]), 1)
	var retired := int(updated["colormap_pages_retired"])
	assert_gt(retired, 0, "the pages that show the region recompose")
	assert_lt(retired, resident, "the pages that do not keep their bytes")
	assert_same(data.get_colormap_image(), painted, "the data keeps the painted map for its next build")

	settled = await TestFs.settle_tile_cache(self, terrain)
	var fresh_data := _loaded_data()
	fresh_data.set_colormap_image(painted)
	var fresh := await TestFs.settle_tile_cache(self, _viewed_terrain(fresh_data))
	assert_eq(int(settled["ready_pages"]), int(fresh["ready_pages"]))
	assert_eq(int(settled["resident_output_hash"]), int(fresh["resident_output_hash"]),
		"the pages hold what a build over the painted map composes")


func test_a_map_of_another_size_is_refused() -> void:
	var data := _loaded_data()
	var terrain := _viewed_terrain(data)
	await TestFs.settle_tile_cache(self, terrain)
	var other := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	assert_false(terrain.update_colormap_region(other, Rect2i(0, 0, 4, 4)),
		"another size is a build's, never a region's")
	assert_eq(int(terrain.get_tile_cache_diagnostics()["colormap_region_updates"]), 0)


func test_a_painted_blend_map_updates_its_texture_in_place() -> void:
	var data := _loaded_data()
	if data.get_detailblendmap() == null:
		pending("the Tmap fixture names no readable blend map")
		return
	var terrain: Terrain = add_child_autofree(Terrain.new())
	terrain.set_terrain_data(data)
	terrain.build()
	var material := terrain.get_terrain_material()
	var held: Texture2D = material.get_shader_parameter("u_blendmap")
	assert_not_null(held)
	var painted := _rgba(data.get_detailblendmap())
	painted.fill_rect(_rect_near_camera(painted), Color(0.0, 1.0, 0.0))
	assert_true(terrain.update_blendmap_region(painted, _rect_near_camera(painted)))
	var after: Texture2D = material.get_shader_parameter("u_blendmap")
	assert_same(after, held, "the material keeps sampling the same texture")
	assert_same(data.get_blendmap_image(), painted, "the data keeps the painted map for its next build")
	# (A headless run's textures read back as they were made, so the texels the update uploads are the surface
	# inputs' normalisation of the painted map, the one a build makes: TerrainSurfaceInputs::update_blendmap.)
