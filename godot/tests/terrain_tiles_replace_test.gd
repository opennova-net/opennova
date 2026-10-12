extends GutTest

# A bake's remade tiles put in place (ADR 0046 S25, the OpenNova Editor's heights): TerrainData.replace_tile_lods
# replaces tiles' LOD lists keeping each one's place and vertices (TerrainData::replace_tiles), with no
# terrain_changed, and Terrain.refresh_region with their indices makes their meshes again (both variants where the
# tile has the flat one), the terrain then equal to a fresh build of the same data; an index that is no tile, a
# count that differs or a tile without its eight lists is refused with nothing replaced.

var _root_dir := ""


func before_all() -> void:
	_root_dir = TestFs.stage_terrain_root("tiles_replace")


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


# A tile's lists made over: one triangle fewer at every LOD that has one (a bake's new simplification of its
# cell); a list of three indices or fewer is kept.
func _remade_lods(data: TerrainData, index: int) -> Array:
	var lods := []
	for lod in range(8):
		var indices := data.get_tile_lod_indices(index, lod)
		if indices.size() >= 6:
			indices.resize(indices.size() - 3)
		lods.append(indices)
	return lods


# The tiles of the four lowest levels below the root (indices 1..84): a one-leaf bake's leaf and ancestors, here
# one of each level in quadrant 1 (it has the flat variant): level offsets 1, 5, 21, 85.
const INDICES := [1, 5, 21, 85]


func _same_mesh(a: ArrayMesh, b: ArrayMesh) -> bool:
	if a == null or b == null:
		return a == null and b == null
	var left := a.surface_get_arrays(0)
	var right := b.surface_get_arrays(0)
	for slot in [Mesh.ARRAY_VERTEX, Mesh.ARRAY_NORMAL, Mesh.ARRAY_TEX_UV, Mesh.ARRAY_INDEX]:
		if left[slot] != right[slot]:
			return false
	return true


func _first_mesh_difference(a: Terrain, b: Terrain, tiles: int) -> String:
	for tile in range(1, tiles):
		for lod in range(8):
			for flat in [false, true]:
				if not _same_mesh(a.get_tile_lod_mesh(tile, lod, flat), b.get_tile_lod_mesh(tile, lod, flat)):
					return "tile %d lod %d%s" % [tile, lod, " flat" if flat else ""]
	return ""


func test_replaced_tiles_show_as_a_fresh_build_would() -> void:
	var data := _data()
	var terrain := _terrain(data)
	terrain.build()
	var changes := [0]
	data.terrain_changed.connect(func() -> void: changes[0] += 1)

	var indices := PackedInt32Array(INDICES)
	var lods := []
	for index in indices:
		lods.append(_remade_lods(data, index))
	var before := terrain.get_tile_lod_mesh(indices[0], 0, false)
	assert_true(data.replace_tile_lods(indices, lods), "tiles keeping their places replace")
	assert_eq(changes[0], 0, "no terrain_changed")
	for i in range(indices.size()):
		assert_eq(data.get_tile_lod_indices(indices[i], 0), lods[i][0], "the tile holds its new lists")
	assert_true(terrain.refresh_region(Rect2i(), indices), "the built terrain takes them")
	assert_eq(terrain.get_refresh_tiles_remeshed(), indices.size(), "their meshes alone made again")
	assert_ne(terrain.get_tile_lod_mesh(indices[0], 0, false), before, "the replaced tile's mesh is new")

	var fresh_data := _data()
	assert_true(fresh_data.replace_tile_lods(indices, lods))
	var fresh := _terrain(fresh_data)
	fresh.build()
	assert_eq(_first_mesh_difference(terrain, fresh, data.get_tile_count()), "",
			"every drawn tile's meshes equal a fresh build's")


func test_a_replacement_that_names_no_tile_is_refused() -> void:
	var data := _data()
	var indices := PackedInt32Array(INDICES)
	var kept := []
	var lods := []
	for index in indices:
		kept.append(data.get_tile_lod_indices(index, 0))
		lods.append(_remade_lods(data, index))
	var past := PackedInt32Array([data.get_tile_count()])
	assert_false(data.replace_tile_lods(past, [lods[0]]), "an index that is no tile is refused")
	assert_false(data.replace_tile_lods(indices, lods.slice(1)), "counts that differ are refused")
	var short := lods.duplicate()
	short[3] = (lods[3] as Array).slice(1)
	assert_false(data.replace_tile_lods(indices, short), "a tile without its eight lists is refused")
	for i in range(indices.size()):
		assert_eq(data.get_tile_lod_indices(indices[i], 0), kept[i], "nothing replaced")
