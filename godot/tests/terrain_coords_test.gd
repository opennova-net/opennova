extends GutTest

## Characterization + parity for the terrain world->source coordinate transforms
## (A2). The golden values are hand-derived from the original GDScript formula in
## EditorTerrainMesh. The test asserts THREE things agree on every point:
##   1. the live EditorTerrainMesh GDScript path (forwarder after the refactor),
##   2. the new C++ NovaTerrainData methods (the port),
##   3. the hand-derived goldens.
## Run before the mesh refactor it proves the C++ port reproduces the original
## GDScript byte-for-byte; run after, it proves the forwarder still matches.

const ORIGIN_X := -4
const ORIGIN_Y := -4
const SECTOR_COUNT := 8
const SECTOR_ROWS := 8


func _make_grid() -> PackedInt32Array:
	# 16x16 row-major. Authored 8x8 region; specific cells exercise every quadrant
	# id plus an empty cell and an over-range (>4) cell:
	#   (0,0) id1  (0,1) id2  (1,0) id3  (1,1) id4  (0,2) id0  (0,3) id7
	# all others id1.
	var grid := PackedInt32Array()
	grid.resize(256)
	for i in 256:
		grid[i] = 1
	grid[0 * 16 + 0] = 1
	grid[0 * 16 + 1] = 2
	grid[1 * 16 + 0] = 3
	grid[1 * 16 + 1] = 4
	grid[0 * 16 + 2] = 0
	grid[0 * 16 + 3] = 7
	return grid


func _make_mesh() -> EditorTerrainMesh:
	var mesh := EditorTerrainMesh.new()
	mesh.set_sector_layout(SECTOR_COUNT, SECTOR_ROWS, _make_grid(), ORIGIN_X, ORIGIN_Y)
	return mesh


func _make_data() -> NovaTerrainData:
	var data := NovaTerrainData.new()
	data.origin_x = ORIGIN_X
	data.origin_y = ORIGIN_Y
	data.sector_count = SECTOR_COUNT
	data.sector_rows = SECTOR_ROWS
	data.sector_grid = _make_grid()
	return data


func test_world_to_source_coords_matches_goldens_and_cpp() -> void:
	var mesh := _make_mesh()
	var data := _make_data()
	# Wire the data into the mesh so the post-refactor forwarder has its source.
	if mesh.has_method("set_terrain_data"):
		mesh.set_terrain_data(data)

	# point -> expected source (Vector2), or null for the (-1,-1) sentinel.
	var cases := [
		[Vector2(-2000.0, -2000.0), Vector2(48.0, 48.0)],   # cell(0,0) id1
		[Vector2(-1500.0, -2000.0), Vector2(36.0, 560.0)],  # cell(0,1) id2 quadrant z
		[Vector2(-2000.0, -1500.0), Vector2(560.0, 36.0)],  # cell(1,0) id3 quadrant x
		[Vector2(-1500.0, -1500.0), Vector2(548.0, 548.0)], # cell(1,1) id4 quadrant xz
		[Vector2(-300.0, -2000.0), Vector2(724.0, 560.0)],  # cell(0,3) id7 -> clampi 4
		[Vector2(-800.0, -2000.0), null],                   # cell(0,2) id0 empty -> sentinel
		[Vector2(-5000.0, -2000.0), null],                  # out of extent -> bounds reject
	]
	for case in cases:
		var p: Vector2 = case[0]
		var expected = case[1]
		var got_mesh: Vector2 = mesh.world_to_source_coords(p.x, p.y)
		var got_cpp: Vector2 = data.world_to_source_coords(p.x, p.y)
		assert_eq(got_mesh, got_cpp, "mesh == C++ for %s" % p)
		if expected == null:
			assert_lt(got_mesh.x, 0.0, "sentinel for %s" % p)
		else:
			assert_eq(got_mesh, expected, "golden for %s" % p)

	mesh.free()


func test_world_to_source_local_clamp_at_boundary() -> void:
	var mesh := _make_mesh()
	var data := _make_data()
	if mesh.has_method("set_terrain_data"):
		mesh.set_terrain_data(data)

	# local_x = 511.9995 -> clamped to 512 - 0.001 = 511.999 (stays in the quadrant).
	var p := Vector2(-1536.0005, -2000.0)
	var got_mesh: Vector2 = mesh.world_to_source_coords(p.x, p.y)
	var got_cpp: Vector2 = data.world_to_source_coords(p.x, p.y)
	assert_eq(got_mesh, got_cpp, "mesh == C++ at clamp boundary")
	assert_lt(got_mesh.x, 512.0, "local clamped below 512")
	assert_almost_eq(got_mesh.x, 511.999, 0.0005, "clamped to 511.999")
	assert_almost_eq(got_mesh.y, 48.0, 0.0005, "z unaffected")

	mesh.free()


func test_runtime_world_to_source_coords_wraps_and_keeps_raw_sector_ids() -> void:
	var data := _make_data()
	# Outside the authored 8x8 editor extent, runtime wraps the 16x16 grid.
	assert_eq(
		data.world_to_runtime_source_coords(-5000.0, -2000.0),
		Vector2(120.0, 48.0)
	)
	assert_lt(data.world_to_source_coords(-5000.0, -2000.0).x, 0.0)

	# Cell (0,3) contains raw id 7. Runtime preserves it, so it receives no
	# quadrant offset; the editor clamps it to id 4 and offsets both axes.
	assert_eq(
		data.world_to_runtime_source_coords(-300.0, -2000.0),
		Vector2(212.0, 48.0)
	)
	assert_eq(data.world_to_source_coords(-300.0, -2000.0), Vector2(724.0, 560.0))

	# Empty cells remain invalid in both modes.
	assert_lt(data.world_to_runtime_source_coords(-800.0, -2000.0).x, 0.0)


func test_world_to_cell_source_coords_unclamped() -> void:
	var mesh := _make_mesh()
	var data := _make_data()
	if mesh.has_method("set_terrain_data"):
		mesh.set_terrain_data(data)

	# [world_x, world_z, row, col] -> expected (or null for the -1e9 sentinel).
	var cases := [
		[-1500.0, -1500.0, 1, 1, Vector2(548.0, 548.0)],  # in-cell id4
		[-1000.0, -2000.0, 0, 0, Vector2(1048.0, 48.0)],  # UNCLAMPED: local > 512
		[-1500.0, -1500.0, 0, 2, null],                   # empty cell id0
		[0.0, 0.0, 9, 0, null],                           # row 9 >= sector_rows
	]
	for case in cases:
		var wx: float = case[0]
		var wz: float = case[1]
		var row: int = case[2]
		var col: int = case[3]
		var expected = case[4]
		var got_mesh: Vector2 = mesh.world_to_cell_source_coords(wx, wz, row, col)
		var got_cpp: Vector2 = data.world_to_cell_source_coords(wx, wz, row, col)
		assert_eq(got_mesh, got_cpp, "mesh == C++ for cell (%d,%d)" % [row, col])
		if expected == null:
			assert_lt(got_mesh.x, 0.0, "cell sentinel for (%d,%d)" % [row, col])
		else:
			assert_eq(got_mesh, expected, "cell golden for (%d,%d)" % [row, col])

	mesh.free()


func test_get_cell_atlas_rect() -> void:
	var mesh := _make_mesh()
	var data := _make_data()
	if mesh.has_method("set_terrain_data"):
		mesh.set_terrain_data(data)

	var cases := [
		[0, 0, Rect2i(0, 0, 512, 512)],     # id1
		[0, 1, Rect2i(0, 512, 512, 512)],   # id2
		[1, 0, Rect2i(512, 0, 512, 512)],   # id3
		[1, 1, Rect2i(512, 512, 512, 512)], # id4
		[0, 2, Rect2i(0, 0, 0, 0)],         # id0 empty
		[0, 3, Rect2i(512, 512, 512, 512)], # id7 -> clampi 4
	]
	for case in cases:
		var row: int = case[0]
		var col: int = case[1]
		var expected: Rect2i = case[2]
		var got_mesh: Rect2i = mesh.get_cell_atlas_rect(row, col)
		var got_cpp: Rect2i = data.get_cell_atlas_rect(row, col)
		assert_eq(got_mesh, got_cpp, "mesh == C++ rect for (%d,%d)" % [row, col])
		assert_eq(got_mesh, expected, "rect golden for (%d,%d)" % [row, col])

	mesh.free()


func test_layout_constants_and_sector_cell_match_the_engine() -> void:
	# Contract pin: the GDScript layer consumes these bound constants, so a C++
	# layout-value change must trip a GDScript-visible test.
	assert_eq(NovaTerrainData.SECTOR_SIZE, 512, "SECTOR_SIZE")
	assert_eq(NovaTerrainData.SECTOR_GRID_DIM, 16, "SECTOR_GRID_DIM")
	assert_eq(NovaTerrainData.ATLAS_SIZE, 1024, "ATLAS_SIZE")
	assert_eq(NovaTerrainData.SECTOR_ID_MAX, 4, "SECTOR_ID_MAX")

	var mesh := _make_mesh()
	var data := _make_data()
	mesh.set_terrain_data(data)

	# point -> expected cell (row from world_z, col from world_x), or the (-1,-1)
	# sentinel outside the authored extent. No grid-value check: the empty cell
	# (0,2) still reports its row/col (contrast world_to_source_coords).
	var cases := [
		[Vector2(-2000.0, -2000.0), Vector2i(0, 0)],   # in-extent, negative world coords
		[Vector2(-1500.0, -2000.0), Vector2i(0, 1)],   # col from world_x
		[Vector2(-2000.0, -1500.0), Vector2i(1, 0)],   # row from world_z
		[Vector2(-800.0, -2000.0), Vector2i(0, 2)],    # empty cell id0 still reported
		[Vector2(-2048.0, -2048.0), Vector2i(0, 0)],   # exact lower extent boundary
		[Vector2(100.0, 2048.0), Vector2i(-1, -1)],    # exact upper boundary: row == rows
		[Vector2(-5000.0, -2000.0), Vector2i(-1, -1)], # out of extent (col < 0)
		[Vector2(3000.0, 100.0), Vector2i(-1, -1)],    # out of extent (col >= count)
	]
	for case in cases:
		var p: Vector2 = case[0]
		var expected: Vector2i = case[1]
		var got_mesh: Vector2i = mesh.world_to_sector_cell(p.x, p.y)
		var got_cpp: Vector2i = data.world_to_sector_cell(p.x, p.y)
		assert_eq(got_mesh, got_cpp, "mesh == C++ for %s" % p)
		assert_eq(got_mesh, expected, "cell golden for %s" % p)

	mesh.free()
