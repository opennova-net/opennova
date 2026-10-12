extends GutTest

# A sculpted terrain shown again over the regions a stroke changed (ADR 0046 S25, the OpenNova Editor's heights):
# TerrainData.patch_heights_raw16 writes rects of the loaded heights in place and Terrain.refresh_region shows them,
# and the terrain then equals a fresh build of the patched data: every drawn tile's meshes (the root's are never
# drawn), the heightfield normals (their texture updated in place) and the composed pages. Neither raises
# terrain_changed (which remakes every derived texture), the build is kept, the composed pages that sample the
# changed normals retire, the static shadows' casters are read again, and the foliage over them grows again, now or
# with a later refresh. A change across the atlas's wrap arrives as two rects in one refresh. A rect not inside the
# heights, a refresh before a build and an index that is no tile are refused with nothing changed. A 32-unit brush's
# refresh and the frame after it are timed (the plan's 8 ms budget).

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


func _camera(terrain: Terrain) -> Camera3D:
	terrain.set_debug_no_frustum(true)
	var camera := Camera3D.new()
	terrain.get_parent().add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()
	return camera


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


# Patches `rects` into `data` and refreshes them on `terrain` in one call, then checks the terrain against a fresh
# build of the same patches: meshes, normals and heights.
func _patch_and_compare(rects: Array[Rect2i], what: String) -> void:
	var data := _data()
	var terrain := _terrain(data)
	terrain.build()
	var fresh_data := _data()
	for rect in rects:
		var raised := _raised(data, rect)
		assert_true(data.patch_heights_raw16(rect, raised), "%s: a rect inside the heights patches" % what)
		assert_true(fresh_data.patch_heights_raw16(rect, raised))
	assert_true(terrain.refresh_region(rects, PackedInt32Array()), "%s: the built terrain refreshes" % what)
	var fresh := _terrain(fresh_data)
	fresh.build()
	assert_eq(_first_mesh_difference(terrain, fresh, data.get_tile_count()), "",
			"%s: every drawn tile's meshes equal a fresh build's" % what)
	assert_eq(_first_byte_difference(terrain.get_heightfield_normal_bytes(), fresh.get_heightfield_normal_bytes()), -1,
			"%s: the heightfield normals equal a fresh build's" % what)
	assert_eq(_first_byte_difference(data.get_depth_raw16(), fresh_data.get_depth_raw16()), -1,
			"%s: the heights are the patched ones" % what)


func test_a_patch_shows_as_a_fresh_build_of_the_patched_heights() -> void:
	var data := _data()
	var terrain := _terrain(data)
	terrain.build()
	assert_true(terrain.is_built())
	var changes := [0]
	data.terrain_changed.connect(func() -> void: changes[0] += 1)
	var texture: Texture2D = terrain.get_heightfield_normal_texture()
	var built_normals := terrain.get_heightfield_normal_bytes()
	var serial := data.get_heights_serial()
	assert_true(data.patch_heights_raw16(RECT, _raised(data, RECT)), "a rect inside the heights patches")
	assert_eq(data.get_heights_serial(), serial + 1, "the patch moves the heights serial")
	assert_true(terrain.refresh_region([RECT], PackedInt32Array()), "the built terrain refreshes the region")
	assert_gt(terrain.get_refresh_tiles_remeshed(), 0, "the tiles over the rect were made again")
	assert_lt(terrain.get_refresh_tiles_remeshed(), data.get_tile_count() / 4, "and only those")
	assert_true(terrain.is_refresh_normals_in_place(), "the normals refreshed in place")
	assert_eq(terrain.get_refresh_normal_quadrants(), 1, "one box of normals written")
	assert_eq(changes[0], 0, "no terrain_changed")
	assert_true(terrain.is_built(), "the build is kept")
	assert_ne(_first_byte_difference(terrain.get_heightfield_normal_bytes(), built_normals), -1,
			"the normals took the change")
	assert_same(terrain.get_heightfield_normal_texture(), texture, "the normal texture is updated in place")
	_patch_and_compare([RECT], "an inner rect")


## A change across the atlas's wrap arrives as two rects, refreshed together; one across a quadrant seam as one.
func test_rects_across_the_wrap_and_a_seam() -> void:
	_patch_and_compare([Rect2i(1016, 300, 8, 20), Rect2i(0, 300, 12, 20)] as Array[Rect2i], "across the wrap")
	_patch_and_compare([Rect2i(505, 505, 14, 14)] as Array[Rect2i], "across the seam")


func test_the_composed_pages_over_the_changed_normals_retire() -> void:
	var data := _data()
	var terrain := _terrain(data)
	terrain.build()
	_camera(terrain)
	var settled := await TestFs.settle_tile_cache(self, terrain)
	assert_gt(int(settled.get("frame_requests", 0)), 0, "pages are composed")
	# A small rect: only the pages that sample its changed normals retire, and they compose as a fresh build's do.
	var raised := _raised(data, RECT)
	assert_true(data.patch_heights_raw16(RECT, raised))
	assert_true(terrain.refresh_region([RECT], PackedInt32Array()))
	var some := terrain.get_refresh_pages_retired()
	assert_gt(some, 0, "the pages over the rect's normals retired")
	settled = await TestFs.settle_tile_cache(self, terrain)
	var fresh_data := _data()
	assert_true(fresh_data.patch_heights_raw16(RECT, raised))
	var fresh := _terrain(fresh_data)
	fresh.build()
	_camera(fresh)
	var fresh_settled := await TestFs.settle_tile_cache(self, fresh)
	assert_eq(int(settled.get("resident_output_pages", -1)), int(fresh_settled.get("resident_output_pages", -2)),
			"as many pages resident as a fresh build's")
	assert_eq(int(settled.get("resident_output_hash", -1)), int(fresh_settled.get("resident_output_hash", -2)),
			"the composed pages are a fresh build's, byte for byte")
	# The whole atlas raised: every composed page shows a changed normal.
	var whole := Rect2i(0, 0, TerrainData.ATLAS_SIZE, TerrainData.ATLAS_SIZE)
	assert_true(data.patch_heights_raw16(whole, _raised(data, whole)))
	assert_true(terrain.refresh_region([whole], PackedInt32Array()))
	assert_eq(terrain.get_refresh_normal_quadrants(), 4)
	assert_gt(terrain.get_refresh_pages_retired(), some, "every composed page retired, more than the rect's")
	var again := await TestFs.settle_tile_cache(self, terrain)
	assert_eq(int(again["pending_jobs"]), 0, "they compose again")


## The static terrain shadows' casters stand on the heights (each caster's ground height): a patch has their
## rasterizer take the casters again on the next frame.
func test_the_static_shadows_read_the_patched_heights() -> void:
	var data := _data()
	var terrain := _terrain(data)
	terrain.build()
	_camera(terrain)
	terrain.render_frame()
	var before := int(terrain.get_tile_cache_diagnostics().get("shadow_provider_heights_rebuilds", -1))
	assert_true(data.patch_heights_raw16(RECT, _raised(data, RECT)))
	assert_true(terrain.refresh_region([RECT], PackedInt32Array()))
	terrain.render_frame()
	var diagnostics := terrain.get_tile_cache_diagnostics()
	assert_eq(int(diagnostics.get("shadow_provider_heights_rebuilds", -1)), before + 1,
			"the casters taken again over the new ground")
	assert_eq(int(diagnostics.get("shadow_provider_heights_serial", -1)), data.get_heights_serial())


## A 32-unit brush (a 34-texel rect with its taps) refreshed, and the frame after it composing what it retired: the
## numbers printed against the plan's 8 ms for the refresh.
func test_a_brush_refresh_is_timed() -> void:
	var data := _data()
	var terrain := _terrain(data)
	terrain.build()
	_camera(terrain)
	await TestFs.settle_tile_cache(self, terrain)
	var brush := Rect2i(400, 400, 34, 34)
	assert_true(data.patch_heights_raw16(brush, _raised(data, brush)))
	assert_true(terrain.refresh_region([brush], PackedInt32Array()))
	var refresh_us := terrain.get_refresh_us()
	terrain.render_frame()
	var diagnostics := terrain.get_tile_cache_diagnostics()
	var compose_us := int(diagnostics.get("frame_compose_us", 0))
	gut.p("a 32-unit brush: refresh_region %d us (%d tiles, %d normal boxes, %d pages retired); the next frame composes %d us"
			% [refresh_us, terrain.get_refresh_tiles_remeshed(), terrain.get_refresh_normal_quadrants(),
			terrain.get_refresh_pages_retired(), compose_us])
	print("terrain_region_patch timing: refresh_region_us=%d next_frame_compose_us=%d" % [refresh_us, compose_us])
	assert_gt(refresh_us, 0, "the refresh is timed")


func test_refusals_change_nothing() -> void:
	var data := _data()
	var terrain := _terrain(data)
	assert_false(terrain.refresh_region([RECT], PackedInt32Array()), "a terrain not built refuses")
	terrain.build()
	var depth := data.get_depth_raw16()
	var bytes := PackedByteArray()
	bytes.resize(4 * 4 * 2)
	for rect in [Rect2i(-1, 0, 4, 4), Rect2i(1022, 0, 4, 4), Rect2i(0, 1021, 4, 4)]:
		assert_false(data.patch_heights_raw16(rect, bytes), "a rect not inside the heights is refused")
		assert_false(terrain.refresh_region([RECT, rect], PackedInt32Array()), "and by the refresh, the whole call")
	assert_false(data.patch_heights_raw16(Rect2i(0, 0, 4, 4), PackedByteArray([1, 2, 3])),
			"a short source is refused")
	assert_eq(_first_byte_difference(data.get_depth_raw16(), depth), -1, "a refused patch writes nothing")
	assert_false(terrain.refresh_region([RECT], PackedInt32Array([data.get_tile_count()])),
			"an index that is no tile is refused")


func _sample_height(_world_x: float, _world_z: float) -> float:
	return 10.0


func _sample_foliage(_world_x: float, _world_z: float) -> int:
	return 1


func _dispatcher(terrain: Terrain) -> FoliageDispatcher:
	var dispatcher := FoliageDispatcher.new()
	terrain.get_parent().add_child(dispatcher)
	var definition := TerrainFoliageDef.new()
	definition.graphic = "region_patch"
	definition.match = PackedInt32Array([1])
	dispatcher.configure_slots([definition], [BoxMesh.new()], [])
	dispatcher.height_sampler = Callable(self, "_sample_height")
	dispatcher.foliage_sampler = Callable(self, "_sample_foliage")
	dispatcher.set_terrain(terrain)
	return dispatcher


func _frame(terrain: Terrain, dispatcher: FoliageDispatcher, camera: Camera3D) -> FoliageFrameStats:
	terrain.render_frame()
	dispatcher.render_frame(camera.global_transform, 0)
	return dispatcher.get_frame_stats()


## The foliage over a refreshed region grows again (FoliageDispatcher takes the terrain's refreshed rects): its cells
## dropped and generated again on the dispatcher's next frame, none in a frame with no refresh and the cells away
## from a small rect kept; a refresh that keeps its foliage back grows it with the next that grows it; the
## dispatcher's invalidate_cells drops the cells over a rect by itself.
func test_the_foliage_over_a_refreshed_region_grows_again() -> void:
	var data := _data()
	var terrain := _terrain(data)
	terrain.build()
	var camera := _camera(terrain)
	var dispatcher := _dispatcher(terrain)
	var steady: FoliageFrameStats = null
	for _frame_index in 3:
		steady = _frame(terrain, dispatcher, camera)
	assert_gt(int(steady.detail_cache_residents), 0, "detail cells resident")
	assert_eq(int(steady.detail_cache_regenerations), 0, "a steady frame generates nothing")

	# A small rect under the camera (Tmap lays the atlas texel (512 + x, 512 + z) under Godot's (x, z)): some cells
	# grown again, the rest kept.
	var near := Rect2i(572, 572, 8, 8)
	assert_true(data.patch_heights_raw16(near, _raised(data, near)))
	assert_true(terrain.refresh_region([near], PackedInt32Array()))
	var small := _frame(terrain, dispatcher, camera)
	assert_gt(int(small.detail_cache_regenerations), 0, "the cells over the small rect grown again")
	assert_lt(int(small.detail_cache_regenerations), int(small.detail_cache_residents), "the others kept")

	# Kept back, then grown with the next refresh that grows the foliage.
	assert_true(data.patch_heights_raw16(near, _raised(data, near)))
	assert_true(terrain.refresh_region([near], PackedInt32Array(), false))
	assert_eq(int(_frame(terrain, dispatcher, camera).detail_cache_regenerations), 0, "a kept-back refresh grows none")
	assert_true(terrain.refresh_region([], PackedInt32Array()))
	assert_gt(int(_frame(terrain, dispatcher, camera).detail_cache_regenerations), 0, "grown with the next")

	var whole := Rect2i(0, 0, TerrainData.ATLAS_SIZE, TerrainData.ATLAS_SIZE)
	assert_true(data.patch_heights_raw16(whole, _raised(data, whole)))
	assert_true(terrain.refresh_region([whole], PackedInt32Array()))
	assert_gt(int(_frame(terrain, dispatcher, camera).detail_cache_regenerations), 0,
			"the cells over the refreshed region grown again")
	assert_eq(int(_frame(terrain, dispatcher, camera).detail_cache_regenerations), 0, "once")

	# By the dispatcher alone: every cell on the plane dropped.
	assert_gt(dispatcher.invalidate_cells(Rect2i(-32768, -32768, 65535, 65535)), 0, "the cells dropped")
	assert_eq(dispatcher.invalidate_cells(Rect2i(0, 0, 0, 0)), 0, "an empty rect drops nothing")
	assert_eq(dispatcher.invalidate_cells(Rect2i(2147483000, 0, 2147483000, 10)), 0, "a rect past the plane drops nothing")
