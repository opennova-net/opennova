extends GutTest

## Runtime terrain coordinate contract. Goldens are hand-derived from the
## retail-shaped 512-unit sector layout.

const ORIGIN_X := -4
const ORIGIN_Y := -4
const SECTOR_COUNT := 8
const SECTOR_ROWS := 8


func _make_data() -> TerrainData:
	var grid := PackedInt32Array()
	grid.resize(256)
	for index in 256:
		grid[index] = 1
	grid[1] = 2
	grid[16] = 3
	grid[17] = 4
	grid[2] = 0
	grid[3] = 7

	var data := TerrainData.new()
	data.origin_x = ORIGIN_X
	data.origin_y = ORIGIN_Y
	data.sector_count = SECTOR_COUNT
	data.sector_rows = SECTOR_ROWS
	data.sector_grid = grid
	return data


func test_runtime_coords_wrap_and_keep_raw_sector_ids() -> void:
	var data := _make_data()
	assert_eq(data.world_to_runtime_source_coords(-5000.0, -2000.0), Vector2(120.0, 48.0))
	assert_eq(data.world_to_runtime_source_coords(-300.0, -2000.0), Vector2(212.0, 48.0))
	assert_lt(data.world_to_runtime_source_coords(-800.0, -2000.0).x, 0.0)


func test_layout_constants() -> void:
	assert_eq(TerrainData.SECTOR_SIZE, 512)
	assert_eq(TerrainData.SECTOR_GRID_DIM, 16)
	assert_eq(TerrainData.ATLAS_SIZE, 1024)
	assert_eq(TerrainData.SECTOR_ID_MAX, 4)
