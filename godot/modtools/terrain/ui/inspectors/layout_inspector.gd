class_name LayoutInspector
extends TerrainInspector

## Layout workflow: the quadrant sector board with a passive legend, plus map
## size / origin / water / wrap controls. Forces the sector-editing tool while
## active. Code-first; built into the inspector mount by the terrain workspace.

const LEGEND_LAYOUT := [
	1, -1, 3,
	-1, 0, -1,
	2, -1, 4,
]

const LEGEND_LABELS := {
	0: "Empty",
	1: "NW",
	2: "SW",
	3: "NE",
	4: "SE",
}

var _legend_grid: GridContainer
var _board: QuadrantBoard
var _sector_overlay_toggle: CheckBox
var _grid_size_spin: SpinBox
var _origin_x_spin: SpinBox
var _origin_y_spin: SpinBox
var _water_spin: SpinBox
var _water_visible_toggle: CheckBox
var _wrap_x_toggle: CheckBox
var _wrap_y_toggle: CheckBox


func set_editor(value: TerrainEditor) -> void:
	super.set_editor(value)
	if terrain_editor != null and terrain_editor.current_tool != TerrainEditor.Tool.EDIT_SECTORS:
		terrain_editor.set_tool(TerrainEditor.Tool.EDIT_SECTORS)


func build_main(mount: Control) -> void:
	var box := _make_inspector_box(mount)
	_root = box
	_add_section_heading(box, "Layout")

	var legend_center := CenterContainer.new()
	box.add_child(legend_center)
	_legend_grid = GridContainer.new()
	_legend_grid.name = "LegendGrid"
	_legend_grid.columns = 3
	legend_center.add_child(_legend_grid)
	_build_legend()

	_board = QuadrantBoard.new()
	_board.name = "QuadrantBoard"
	_board.custom_minimum_size = Vector2(240, 240)
	box.add_child(_board)
	_board.cell_painted.connect(_on_board_cell_painted)

	_sector_overlay_toggle = _add_layout_toggle(box, "SectorOverlayToggle", "Show sector overlay", _on_sector_overlay_toggled)
	_grid_size_spin = _add_connected_spin(box, "GridSizeSpin", "Sectors", 1, 16, 1, _on_grid_size_changed)
	_origin_x_spin = _add_connected_spin(box, "OriginXSpin", "Origin X", -64, 64, 1, _on_origin_x_changed)
	_origin_y_spin = _add_connected_spin(box, "OriginYSpin", "Origin Y", -64, 64, 1, _on_origin_y_changed)

	_add_section_heading(box, "Water")
	_water_spin = _add_connected_spin(box, "WaterSpin", "Water height", 0, 32767, 1, _on_water_changed)
	_water_visible_toggle = _add_layout_toggle(box, "WaterVisibleToggle", "Show water plane", _on_water_visible_toggled)
	_wrap_x_toggle = _add_layout_toggle(box, "WrapXToggle", "Wrap X", _on_wrap_x_toggled)
	_wrap_y_toggle = _add_layout_toggle(box, "WrapYToggle", "Wrap Y", _on_wrap_y_toggled)

	refresh()


func refresh() -> void:
	if not _ui_alive() or terrain_editor == null:
		return
	_syncing = true
	_sector_overlay_toggle.set_pressed_no_signal(terrain_editor.is_sector_overlay_visible())
	_grid_size_spin.set_value_no_signal(terrain_editor.get_sector_size())
	_origin_x_spin.set_value_no_signal(terrain_editor.get_origin_x())
	_origin_y_spin.set_value_no_signal(terrain_editor.get_origin_y())
	_water_spin.set_value_no_signal(terrain_editor.get_water_height())
	_water_visible_toggle.set_pressed_no_signal(terrain_editor.is_water_visible())
	_wrap_x_toggle.set_pressed_no_signal(terrain_editor.get_wrap_x_enabled())
	_wrap_y_toggle.set_pressed_no_signal(terrain_editor.get_wrap_y_enabled())
	_board.set_active_cell(terrain_editor.get_active_sector_cell())
	var data: NovaTerrainData = terrain_editor.get_data()
	if data:
		_board.set_grid_state(terrain_editor.get_sector_count(), terrain_editor.get_sector_rows(), data.get_sector_grid())
	_syncing = false


func _add_layout_toggle(parent: Control, node_name: String, text: String, handler: Callable) -> CheckBox:
	var check := CheckBox.new()
	check.name = node_name
	check.text = text
	parent.add_child(check)
	check.toggled.connect(handler)
	return check


func _add_connected_spin(parent: Control, node_name: String, label_text: String, min_value: float, max_value: float, step: float, handler: Callable) -> SpinBox:
	var spin := _add_spin_row(parent, node_name, label_text, min_value, max_value, step)
	spin.value_changed.connect(handler)
	return spin


func _build_legend() -> void:
	for child in _legend_grid.get_children():
		child.queue_free()
	for sector_id in LEGEND_LAYOUT:
		if sector_id < 0:
			var spacer := Control.new()
			spacer.custom_minimum_size = Vector2(44, 40)
			_legend_grid.add_child(spacer)
			continue
		_legend_grid.add_child(_build_legend_entry(sector_id))


func _build_legend_entry(sector_id: int) -> Control:
	var box := VBoxContainer.new()
	box.custom_minimum_size = Vector2(44, 40)
	box.alignment = BoxContainer.ALIGNMENT_CENTER
	box.add_theme_constant_override("separation", 3)
	var swatch := ColorRect.new()
	swatch.custom_minimum_size = Vector2(14, 14)
	swatch.color = QuadrantBoard.SECTOR_COLORS[clampi(sector_id, 0, QuadrantBoard.SECTOR_COLORS.size() - 1)]
	box.add_child(swatch)
	var label := Label.new()
	label.text = String(LEGEND_LABELS.get(sector_id, ""))
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	if sector_id == 0:
		label.theme_type_variation = &"Muted"
	box.add_child(label)
	return box


func _on_board_cell_painted(row: int, col: int, value: int) -> void:
	if terrain_editor == null:
		return
	terrain_editor.set_sector_cell(row, col, value)


func _on_grid_size_changed(value: float) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_sector_size(int(value))


func _on_sector_overlay_toggled(pressed: bool) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_sector_overlay_visible(pressed)


func _on_origin_x_changed(value: float) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_origin_x_value(int(value))


func _on_origin_y_changed(value: float) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_origin_y_value(int(value))


func _on_water_changed(value: float) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_water_height_value(int(value))


func _on_water_visible_toggled(pressed: bool) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_water_visible(pressed)


func _on_wrap_x_toggled(pressed: bool) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_wrap_x_enabled(pressed)


func _on_wrap_y_toggled(pressed: bool) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_wrap_y_enabled(pressed)
