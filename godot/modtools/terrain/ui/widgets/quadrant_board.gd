class_name QuadrantBoard
extends Control

## A 2D board for authoring the sector grid.
##
## The grid is N cols x M rows. Each cell holds a sector id:
##   0 = empty, 1 = NW, 2 = SW, 3 = NE, 4 = SE
## The ids match the terrain data/shader encoding.

signal cell_painted(row: int, col: int, value: int)

const SECTOR_COLORS := [
	Color(0.22, 0.23, 0.26),  # 0 empty
	Color(0.28, 0.68, 0.36),  # 1 NW green
	Color(0.82, 0.72, 0.20),  # 2 SW yellow
	Color(0.26, 0.44, 0.82),  # 3 NE blue
	Color(0.86, 0.34, 0.30),  # 4 SE red
]

const GRID_LINE_COLOR := Color(0.08, 0.09, 0.10, 1.0)
const HOVER_COLOR := Color(1.0, 1.0, 1.0, 0.12)

var cols: int = 8
var rows: int = 8
var grid: PackedInt32Array = PackedInt32Array()
var active_cell: Vector2i = Vector2i(-1, -1)

var _hover_cell: Vector2i = Vector2i(-1, -1)
var _mouse_down_button: int = -1
var _last_drag_cell: Vector2i = Vector2i(-1, -1)


func _ready() -> void:
	custom_minimum_size = Vector2(240, 240)
	mouse_filter = Control.MOUSE_FILTER_STOP
	if grid.is_empty():
		grid.resize(rows * cols)
	resized.connect(queue_redraw)


func set_grid_state(new_cols: int, new_rows: int, values: PackedInt32Array) -> void:
	cols = maxi(1, new_cols)
	rows = maxi(1, new_rows)
	grid = PackedInt32Array()
	grid.resize(rows * cols)
	for row in rows:
		for col in cols:
			var src_idx := row * NovaTerrainData.SECTOR_GRID_DIM + col
			var value := 0
			if src_idx >= 0 and src_idx < values.size():
				value = clampi(values[src_idx], 0, NovaTerrainData.SECTOR_ID_MAX)
			grid[row * cols + col] = value
	queue_redraw()


func set_active_cell(cell: Vector2i) -> void:
	if active_cell == cell:
		return
	active_cell = cell
	queue_redraw()


func _draw() -> void:
	var rect := _board_rect()
	if rect.size.x <= 0 or rect.size.y <= 0:
		return
	var cell_w := rect.size.x / cols
	var cell_h := rect.size.y / rows

	for row in rows:
		for col in cols:
			var idx := row * cols + col
			var value: int = grid[idx] if idx < grid.size() else 0
			var color: Color = SECTOR_COLORS[clampi(value, 0, NovaTerrainData.SECTOR_ID_MAX)]
			var pos := rect.position + Vector2(col * cell_w, row * cell_h)
			var cell_rect := Rect2(pos, Vector2(cell_w, cell_h))
			draw_rect(cell_rect, color, true)
			draw_rect(cell_rect, GRID_LINE_COLOR, false, 1.0)

	if _hover_cell.x >= 0 and _hover_cell.y >= 0:
		var hover_pos := rect.position + Vector2(_hover_cell.y * cell_w, _hover_cell.x * cell_h)
		draw_rect(Rect2(hover_pos, Vector2(cell_w, cell_h)), HOVER_COLOR, true)

	if active_cell.x >= 0 and active_cell.y >= 0:
		var active_pos := rect.position + Vector2(active_cell.y * cell_w, active_cell.x * cell_h)
		var active_border := Color(get_theme_color(&"accent", &"EditorPalette"), 0.95)
		draw_rect(Rect2(active_pos + Vector2(1, 1), Vector2(cell_w - 2, cell_h - 2)), active_border, false, 2.0)


func _gui_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion:
		var cell := _cell_at(event.position)
		if cell != _hover_cell:
			_hover_cell = cell
			queue_redraw()
		if _mouse_down_button != -1 and cell.x >= 0 and cell != _last_drag_cell:
			_stamp_at(cell, _mouse_down_button)
			_last_drag_cell = cell
	elif event is InputEventMouseButton:
		if event.pressed:
			_mouse_down_button = event.button_index
			_last_drag_cell = Vector2i(-1, -1)
			var cell := _cell_at(event.position)
			if cell.x >= 0:
				_stamp_at(cell, event.button_index)
				_last_drag_cell = cell
		else:
			_mouse_down_button = -1
			_last_drag_cell = Vector2i(-1, -1)


func _notification(what: int) -> void:
	if what == NOTIFICATION_MOUSE_EXIT and _hover_cell != Vector2i(-1, -1):
		_hover_cell = Vector2i(-1, -1)
		queue_redraw()


func _stamp_at(cell: Vector2i, button: int) -> void:
	var idx := cell.x * cols + cell.y
	if idx < 0 or idx >= grid.size():
		return
	var value := 0
	if button == MOUSE_BUTTON_LEFT:
		value = _next_cycle_value(grid[idx])
	elif button == MOUSE_BUTTON_RIGHT:
		value = 0
	else:
		return
	if grid[idx] == value:
		set_active_cell(cell)
		cell_painted.emit(cell.x, cell.y, value)
		return
	grid[idx] = value
	set_active_cell(cell)
	queue_redraw()
	cell_painted.emit(cell.x, cell.y, value)


func _next_cycle_value(current_value: int) -> int:
	match current_value:
		1:
			return 3
		3:
			return 2
		2:
			return 4
		4:
			return 1
		_:
			return 1


func _cell_at(pos: Vector2) -> Vector2i:
	var rect := _board_rect()
	if not rect.has_point(pos):
		return Vector2i(-1, -1)
	var cell_w := rect.size.x / cols
	var cell_h := rect.size.y / rows
	var col := clampi(int((pos.x - rect.position.x) / cell_w), 0, cols - 1)
	var row := clampi(int((pos.y - rect.position.y) / cell_h), 0, rows - 1)
	return Vector2i(row, col)


func _board_rect() -> Rect2:
	var inner_size: Vector2 = size
	if cols <= 0 or rows <= 0:
		return Rect2(Vector2.ZERO, inner_size)
	var side := minf(inner_size.x / float(cols), inner_size.y / float(rows))
	side = maxf(side, 12.0)
	var board_w := side * cols
	var board_h := side * rows
	var origin := (inner_size - Vector2(board_w, board_h)) * 0.5
	return Rect2(origin, Vector2(board_w, board_h))
