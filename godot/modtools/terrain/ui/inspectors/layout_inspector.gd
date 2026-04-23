class_name LayoutInspector
extends MarginContainer

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

@onready var _legend_grid: GridContainer = %LegendGrid
@onready var _board: QuadrantBoard = %QuadrantBoard
@onready var _sector_overlay_toggle: CheckBox = %SectorOverlayToggle
@onready var _grid_size_spin: SpinBox = %GridSizeSpin
@onready var _origin_x_spin: SpinBox = %OriginXSpin
@onready var _origin_y_spin: SpinBox = %OriginYSpin
@onready var _water_spin: SpinBox = %WaterSpin
@onready var _water_visible_toggle: CheckBox = %WaterVisibleToggle
@onready var _wrap_x_toggle: CheckBox = %WrapXToggle
@onready var _wrap_y_toggle: CheckBox = %WrapYToggle

var editor: TerrainEditor
var _syncing: bool = false


func _ready() -> void:
	_build_legend()
	_board.cell_painted.connect(_on_board_cell_painted)

	_sector_overlay_toggle.toggled.connect(_on_sector_overlay_toggled)
	_grid_size_spin.value_changed.connect(_on_grid_size_changed)
	_origin_x_spin.value_changed.connect(_on_origin_x_changed)
	_origin_y_spin.value_changed.connect(_on_origin_y_changed)
	_water_spin.value_changed.connect(_on_water_changed)
	_water_visible_toggle.toggled.connect(_on_water_visible_toggled)
	_wrap_x_toggle.toggled.connect(_on_wrap_x_toggled)
	_wrap_y_toggle.toggled.connect(_on_wrap_y_toggled)


func set_editor(value: TerrainEditor) -> void:
	var callback := Callable(self, "_on_editor_ui_state_changed")
	if editor != null and editor.ui_state_changed.is_connected(callback):
		editor.ui_state_changed.disconnect(callback)
	editor = value
	if editor != null and not editor.ui_state_changed.is_connected(callback):
		editor.ui_state_changed.connect(callback)
	if editor and editor.current_tool != TerrainEditor.Tool.EDIT_SECTORS:
		editor.set_tool(TerrainEditor.Tool.EDIT_SECTORS)
	_sync_from_editor()


func _on_editor_ui_state_changed(_version: int) -> void:
	_sync_from_editor()


func _sync_from_editor() -> void:
	if editor == null:
		return
	_syncing = true

	_sector_overlay_toggle.set_pressed_no_signal(editor.is_sector_overlay_visible())
	_grid_size_spin.set_value_no_signal(editor.get_sector_size())
	_origin_x_spin.set_value_no_signal(editor.get_origin_x())
	_origin_y_spin.set_value_no_signal(editor.get_origin_y())
	_water_spin.set_value_no_signal(editor.get_water_height())
	_water_visible_toggle.set_pressed_no_signal(editor.is_water_visible())
	_wrap_x_toggle.set_pressed_no_signal(editor.get_wrap_x_enabled())
	_wrap_y_toggle.set_pressed_no_signal(editor.get_wrap_y_enabled())
	_board.set_active_cell(editor.get_active_sector_cell())

	var data: NovaTerrainData = editor.get_data()
	if data:
		_board.set_grid_state(
			editor.get_sector_count(),
			editor.get_sector_rows(),
			data.get_sector_grid()
		)

	_syncing = false


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
	if editor == null:
		return
	editor.set_sector_cell(row, col, value)


func _on_grid_size_changed(value: float) -> void:
	if _syncing or editor == null:
		return
	editor.set_sector_size(int(value))


func _on_sector_overlay_toggled(pressed: bool) -> void:
	if _syncing or editor == null:
		return
	editor.set_sector_overlay_visible(pressed)


func _on_origin_x_changed(value: float) -> void:
	if _syncing or editor == null:
		return
	editor.set_origin_x_value(int(value))


func _on_origin_y_changed(value: float) -> void:
	if _syncing or editor == null:
		return
	editor.set_origin_y_value(int(value))


func _on_water_changed(value: float) -> void:
	if _syncing or editor == null:
		return
	editor.set_water_height_value(int(value))


func _on_water_visible_toggled(pressed: bool) -> void:
	if _syncing or editor == null:
		return
	editor.set_water_visible(pressed)


func _on_wrap_x_toggled(pressed: bool) -> void:
	if _syncing or editor == null:
		return
	editor.set_wrap_x_enabled(pressed)


func _on_wrap_y_toggled(pressed: bool) -> void:
	if _syncing or editor == null:
		return
	editor.set_wrap_y_enabled(pressed)
