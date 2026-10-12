extends GutTest

# A sculpted terrain shown again over the region a stroke changed (ADR 0046 S25, the OpenNova Editor's heights):
# TerrainData.patch_heights_raw16 writes a rect of the loaded heights in place and Terrain.refresh_region shows
# them, and the terrain then equals a fresh build of the patched data: every drawn tile's meshes (the root's are
# never drawn) and the heightfield normal texture. Neither raises terrain_changed (which remakes every derived
# texture), the build is kept, the composed pages over the changed normals retire and the foliage over them grows
# again. A rect not inside the heights, a refresh before a build and an index that is no tile are refused with
# nothing changed.

const RECT := Rect2i(300, 300, 24, 20)
const RAISE := 768  # three world units, raw16

var _root_dir := ""


func before_all() -> void:
	_root_dir = TestFs.stage_terrain_root("region_patch")


func after_all() -> void:
	TestFs.remove_dir_recursive(_root_dir)


func _data() -> TerrainData:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_root_dir), OK, "the staged terrain root mounts")
	var data := TerrainData.new()
	assert_eq(data.load_from_resource_root(root, TestFs.TMAP_TRN), OK, "the Tmap fixture loads")
	return data


func _terrain(data: TerrainData) -> Terrain:
	var view := SubViewport.new()
	view.size = Vector2i(64, 64)
	view.own_world_3d = true
	add_child_autofree(view)
	var terrain := Terrain.new()
	view.add_child(terrain)
	terrain.set_terrain_data(data)
	return terrain


# The rect's heights raised by RAISE, as little-endian raw16 a row of the rect at a time.
func _raised(data: TerrainData, rect: Rect2i) -> PackedByteArray:
	var depth := data.get_depth_raw16()
	var out := PackedByteArray()
	out.resize(rect.size.x * rect.size.y * 2)
	var at := 0
	for z in range(rect.position.y, rect.end.y):
		for x in range(rect.position.x, rect.end.x):
			var index := (z * TerrainData.ATLAS_SIZE + x) * 2
			var value := mini(depth[index] | (depth[index + 1] << 8), 65535 - RAISE) + RAISE
			out[at] = value & 0xFF
			out[at + 1] = value >> 8
			at += 2
	return out


# The first byte where two arrays differ, -1 where they are equal (a size that differs: the shorter's size).
func _first_byte_difference(a: PackedByteArray, b: PackedByteArray) -> int:
	if a == b:
		return -1
	for i in range(mini(a.size(), b.size())):
		if a[i] != b[i]:
			return i
	return mini(a.size(), b.size())


func _same_mesh(a: ArrayMesh, b: ArrayMesh) -> bool:
	if a == null or b == null:
		return a == null and b == null
	var left := a.surface_get_arrays(0)
	var right := b.surface_get_arrays(0)
	for slot in [Mesh.ARRAY_VERTEX, Mesh.ARRAY_NORMAL, Mesh.ARRAY_TEX_UV, Mesh.ARRAY_INDEX]:
		if left[slot] != right[slot]:
			return false
	return true


# Every drawn tile's meshes (index 0, the root, is never drawn), both variants; the first tile that differs.
func _first_mesh_difference(a: Terrain, b: Terrain, tiles: int) -> String:
	for tile in range(1, tiles):
		for lod in range(8):
			for flat in [false, true]:
				if not _same_mesh(a.get_tile_lod_mesh(tile, lod, flat), b.get_tile_lod_mesh(tile, lod, flat)):
					return "tile %d lod %d%s" % [tile, lod, " flat" if flat else ""]
	return ""


func test_a_patch_shows_as_a_fresh_build_of_the_patched_heights() -> void:
	var data := _data()
	var terrain := _terrain(data)
	terrain.build()
	assert_true(terrain.is_built())
	var changes := [0]
	data.terrain_changed.connect(func() -> void: changes[0] += 1)

	var raised := _raised(data, RECT)
	var built_normals: PackedByteArray = terrain.get_heightfield_normal_texture().get_image().get_data()
	assert_true(data.patch_heights_raw16(RECT, raised), "a rect inside the heights patches")
	assert_true(terrain.refresh_region(RECT, PackedInt32Array()), "the built terrain refreshes the region")
	assert_gt(terrain.get_refresh_tiles_remeshed(), 0, "the tiles over the rect were made again")
	assert_lt(terrain.get_refresh_tiles_remeshed(), data.get_tile_count() / 4, "and only those")
	assert_true(terrain.is_refresh_normals_in_place(), "the normals refreshed in place")
	assert_eq(terrain.get_refresh_normal_quadrants(), 1, "one quadrant's normals written")
	assert_eq(changes[0], 0, "no terrain_changed")
	assert_true(terrain.is_built(), "the build is kept")
	assert_ne(_first_byte_difference(terrain.get_heightfield_normal_texture().get_image().get_data(), built_normals), -1,
			"the normal texture took the change")

	var fresh_data := _data()
	assert_true(fresh_data.patch_heights_raw16(RECT, raised))
	var fresh := _terrain(fresh_data)
	fresh.build()
	assert_eq(_first_mesh_difference(terrain, fresh, data.get_tile_count()), "",
			"every drawn tile's meshes equal a fresh build's")
	var normals: Texture2D = terrain.get_heightfield_normal_texture()
	var fresh_normals: Texture2D = fresh.get_heightfield_normal_texture()
	assert_not_null(normals)
	if normals and fresh_normals:
		assert_eq(_first_byte_difference(normals.get_image().get_data(), fresh_normals.get_image().get_data()), -1,
				"the heightfield normals equal a fresh build's")
	assert_eq(_first_byte_difference(data.get_depth_raw16(), fresh_data.get_depth_raw16()), -1,
			"the heights are the patched ones")


func test_the_composed_pages_over_the_changed_normals_retire() -> void:
	var data := _data()
	var terrain := _terrain(data)
	terrain.build()
	terrain.set_debug_no_frustum(true)
	var camera := Camera3D.new()
	terrain.get_parent().add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()
	var settled := await TestFs.settle_tile_cache(self, terrain)
	assert_gt(int(settled.get("frame_requests", 0)), 0, "pages are composed")
	# The whole atlas raised: every composed page shows a changed normal.
	var whole := Rect2i(0, 0, TerrainData.ATLAS_SIZE, TerrainData.ATLAS_SIZE)
	assert_true(data.patch_heights_raw16(whole, _raised(data, whole)))
	assert_true(terrain.refresh_region(whole, PackedInt32Array()))
	assert_eq(terrain.get_refresh_normal_quadrants(), 4)
	assert_gt(terrain.get_refresh_pages_retired(), 0, "the composed pages retired")
	var again := await TestFs.settle_tile_cache(self, terrain)
	assert_eq(int(again["pending_jobs"]), 0, "they compose again")


func test_refusals_change_nothing() -> void:
	var data := _data()
	var terrain := _terrain(data)
	assert_false(terrain.refresh_region(RECT, PackedInt32Array()), "a terrain not built refuses")
	terrain.build()
	var depth := data.get_depth_raw16()
	var bytes := PackedByteArray()
	bytes.resize(4 * 4 * 2)
	for rect in [Rect2i(-1, 0, 4, 4), Rect2i(1022, 0, 4, 4), Rect2i(0, 1021, 4, 4)]:
		assert_false(data.patch_heights_raw16(rect, bytes), "a rect not inside the heights is refused")
		assert_false(terrain.refresh_region(rect, PackedInt32Array()), "and by the refresh")
	assert_false(data.patch_heights_raw16(Rect2i(0, 0, 4, 4), PackedByteArray([1, 2, 3])),
			"a short source is refused")
	assert_eq(_first_byte_difference(data.get_depth_raw16(), depth), -1, "a refused patch writes nothing")
	assert_false(terrain.refresh_region(RECT, PackedInt32Array([data.get_tile_count()])),
			"an index that is no tile is refused")


func _sample_height(_world_x: float, _world_z: float) -> float:
	return 10.0


func _sample_foliage(_world_x: float, _world_z: float) -> int:
	return 1


## The foliage over a refreshed region grows again (FoliageDispatcher takes the terrain's refreshed rects): its cells
## dropped and generated again on the dispatcher's next frame, none in a frame with no refresh; the dispatcher's
## invalidate_cells drops the cells over a rect by itself.
func test_the_foliage_over_a_refreshed_region_grows_again() -> void:
	var data := _data()
	var terrain := _terrain(data)
	terrain.build()
	terrain.set_debug_no_frustum(true)
	var camera := Camera3D.new()
	terrain.get_parent().add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()
	var dispatcher := FoliageDispatcher.new()
	terrain.get_parent().add_child(dispatcher)
	var definition := TerrainFoliageDef.new()
	definition.graphic = "region_patch"
	definition.match = PackedInt32Array([1])
	dispatcher.configure_slots([definition], [BoxMesh.new()], [])
	dispatcher.height_sampler = Callable(self, "_sample_height")
	dispatcher.foliage_sampler = Callable(self, "_sample_foliage")
	dispatcher.set_terrain(terrain)
	for _frame in 3:
		terrain.render_frame()
		dispatcher.render_frame(camera.global_transform, 0)
	var steady := dispatcher.get_frame_stats()
	assert_gt(int(steady.detail_cache_residents), 0, "detail cells resident")
	assert_eq(int(steady.detail_cache_regenerations), 0, "a steady frame generates nothing")

	var whole := Rect2i(0, 0, TerrainData.ATLAS_SIZE, TerrainData.ATLAS_SIZE)
	assert_true(data.patch_heights_raw16(whole, _raised(data, whole)))
	assert_true(terrain.refresh_region(whole, PackedInt32Array()))
	terrain.render_frame()
	dispatcher.render_frame(camera.global_transform, 0)
	var after := dispatcher.get_frame_stats()
	assert_gt(int(after.detail_cache_regenerations), 0, "the cells over the refreshed region grown again")
	terrain.render_frame()
	dispatcher.render_frame(camera.global_transform, 0)
	assert_eq(int(dispatcher.get_frame_stats().detail_cache_regenerations), 0, "once")

	# By the dispatcher alone: every cell on the plane dropped.
	assert_gt(dispatcher.invalidate_cells(Rect2i(-32768, -32768, 65535, 65535)), 0, "the cells dropped")
	assert_eq(dispatcher.invalidate_cells(Rect2i(0, 0, 0, 0)), 0, "an empty rect drops nothing")
