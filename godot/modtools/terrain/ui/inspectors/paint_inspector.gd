class_name PaintInspector
extends MarginContainer

enum SubTool { DETAILS, COLOR, SURFACE }

const SUB_LABELS := {
	SubTool.DETAILS: "Details",
	SubTool.COLOR: "Color",
	SubTool.SURFACE: "Surface",
}

const DETAIL_CHANNEL_BUTTONS := ["DetailAButton", "DetailBButton", "DetailCButton"]

@onready var _sub_buttons: Dictionary = {
	SubTool.DETAILS: %SubDetailsButton,
	SubTool.COLOR: %SubColorButton,
	SubTool.SURFACE: %SubSurfaceButton,
}
@onready var _sub_panels: Dictionary = {
	SubTool.DETAILS: %DetailsPanel,
	SubTool.COLOR: %ColorPanel,
	SubTool.SURFACE: %SurfacePanel,
}
@onready var _detail_a_btn: Button = %DetailAButton
@onready var _detail_b_btn: Button = %DetailBButton
@onready var _detail_c_btn: Button = %DetailCButton
@onready var _color_picker: ColorPickerButton = %ColorPicker
@onready var _clone_toggle: Button = %CloneToggle
@onready var _clone_status: Label = %CloneStatus
@onready var _clone_clear: Button = %CloneClearButton
@onready var _clone_hint: Label = %CloneHint
@onready var _surface_preset: OptionButton = %SurfacePreset
@onready var _surface_index_spin: SpinBox = %SurfaceIndexSpin
@onready var _surface_current: Label = %SurfaceCurrentLabel
@onready var _brush: BrushControls = %BrushControls

var editor: TerrainEditor
var _current_sub: int = SubTool.COLOR
var _detail_channel_buttons: Array[Button] = []
var _syncing: bool = false


func _ready() -> void:
	_detail_channel_buttons = [_detail_a_btn, _detail_b_btn, _detail_c_btn]

	for sub in _sub_buttons:
		var btn: Button = _sub_buttons[sub]
		btn.pressed.connect(_on_sub_pressed.bind(sub))

	for ch in _detail_channel_buttons.size():
		_detail_channel_buttons[ch].pressed.connect(_on_detail_channel_pressed.bind(ch))

	_color_picker.color_changed.connect(_on_color_changed)
	_color_picker.pressed.connect(_on_color_picker_pressed)
	_clone_toggle.toggled.connect(_on_clone_toggled)
	_clone_clear.pressed.connect(_on_clone_clear_pressed)
	_surface_preset.item_selected.connect(_on_surface_preset_selected)
	_surface_index_spin.value_changed.connect(_on_surface_index_changed)

	_brush.radius_changed.connect(_on_brush_radius)
	_brush.strength_changed.connect(_on_brush_strength)
	_brush.hardness_changed.connect(_on_brush_hardness)

	_populate_surface_presets()
	_show_sub(SubTool.COLOR)

	set_process(true)


func set_editor(value: TerrainEditor) -> void:
	editor = value
	if editor:
		var tool := editor.current_tool
		if tool == TerrainEditor.Tool.PAINT_DETAIL:
			_current_sub = SubTool.DETAILS
		elif tool == TerrainEditor.Tool.PAINT_COLORMAP or tool == TerrainEditor.Tool.CLONE_COLOR:
			_current_sub = SubTool.COLOR
		elif tool == TerrainEditor.Tool.SURFACE_PAINT:
			_current_sub = SubTool.SURFACE
		else:
			# Default the paint workflow to colormap painting when entering from
			# another mode.
			_current_sub = SubTool.COLOR
			editor.set_tool(TerrainEditor.Tool.PAINT_COLORMAP)
		_show_sub(_current_sub)
	_sync_from_editor()


func _process(_delta: float) -> void:
	_sync_from_editor()


func _sync_from_editor() -> void:
	if editor == null:
		return
	_syncing = true
	_brush.set_values(editor.brush_radius, editor.brush_strength, editor.brush_hardness)

	var channel := editor.get_paint_detail_channel()
	for i in _detail_channel_buttons.size():
		_detail_channel_buttons[i].set_pressed_no_signal(
			i == channel and editor.current_tool == TerrainEditor.Tool.PAINT_DETAIL
		)

	var color := editor.get_paint_color()
	if _color_picker.color != color:
		_color_picker.color = color

	_clone_toggle.set_pressed_no_signal(editor.current_tool == TerrainEditor.Tool.CLONE_COLOR)
	_clone_status.text = "Clone source ready." if editor.has_clone_source() else "No clone source. Ctrl-click the terrain to set one."
	_clone_clear.disabled = not editor.has_clone_source()

	var surface_index := editor.get_selected_surface_index()
	_surface_index_spin.set_value_no_signal(surface_index)
	_match_surface_preset_dropdown(surface_index)
	_surface_current.text = "Current: %s (%d)" % [editor.get_selected_surface_label(), surface_index]

	_syncing = false


func _on_sub_pressed(sub: int) -> void:
	if _syncing or editor == null:
		return
	_show_sub(sub)
	match sub:
		SubTool.DETAILS:
			editor.select_detail_paint_channel(editor.get_paint_detail_channel())
		SubTool.COLOR:
			editor.set_tool(TerrainEditor.Tool.PAINT_COLORMAP)
		SubTool.SURFACE:
			editor.set_tool(TerrainEditor.Tool.SURFACE_PAINT)


func _show_sub(sub: int) -> void:
	_current_sub = sub
	for s in _sub_buttons:
		_sub_buttons[s].set_pressed_no_signal(s == sub)
	for s in _sub_panels:
		_sub_panels[s].visible = (s == sub)


func _on_detail_channel_pressed(ch: int) -> void:
	if _syncing or editor == null:
		return
	editor.select_detail_paint_channel(ch)


func _on_color_changed(color: Color) -> void:
	if _syncing or editor == null:
		return
	editor.set_paint_color(color)


func _on_color_picker_pressed() -> void:
	if _syncing or editor == null:
		return
	# Opening the picker implies the user wants to paint color, not clone.
	if editor.current_tool != TerrainEditor.Tool.PAINT_COLORMAP:
		editor.set_tool(TerrainEditor.Tool.PAINT_COLORMAP)


func _on_clone_toggled(pressed: bool) -> void:
	if _syncing or editor == null:
		return
	editor.set_tool(TerrainEditor.Tool.CLONE_COLOR if pressed else TerrainEditor.Tool.PAINT_COLORMAP)


func _on_clone_clear_pressed() -> void:
	if _syncing or editor == null:
		return
	editor.clear_clone_source()


func _on_surface_preset_selected(idx: int) -> void:
	if _syncing or editor == null:
		return
	var meta = _surface_preset.get_item_metadata(idx)
	if typeof(meta) == TYPE_INT and int(meta) >= 0:
		editor.set_selected_surface_index(int(meta))
		_surface_index_spin.set_value_no_signal(int(meta))
	editor.set_tool(TerrainEditor.Tool.SURFACE_PAINT)


func _on_surface_index_changed(v: float) -> void:
	if _syncing or editor == null:
		return
	editor.set_selected_surface_index(int(v))
	_match_surface_preset_dropdown(int(v))
	editor.set_tool(TerrainEditor.Tool.SURFACE_PAINT)


func _on_brush_radius(v: float) -> void:
	if _syncing or editor == null:
		return
	editor.set_brush_radius_value(v)


func _on_brush_strength(v: float) -> void:
	if _syncing or editor == null:
		return
	editor.set_brush_strength_value(v)


func _on_brush_hardness(v: float) -> void:
	if _syncing or editor == null:
		return
	editor.set_brush_hardness_value(v)


func _populate_surface_presets() -> void:
	_surface_preset.clear()
	_surface_preset.add_item("Custom")
	_surface_preset.set_item_metadata(_surface_preset.item_count - 1, -1)
	for entry in TerrainEditor.TerrainEditorSurfacePaint.SURFACE_TYPES:
		var label := "%s - %d" % [entry.get("label", "?"), int(entry.get("id", 0))]
		_surface_preset.add_item(label)
		_surface_preset.set_item_metadata(_surface_preset.item_count - 1, int(entry.get("id", 0)))


func _match_surface_preset_dropdown(surface_index: int) -> void:
	for i in _surface_preset.item_count:
		if int(_surface_preset.get_item_metadata(i)) == surface_index:
			_surface_preset.select(i)
			return
	_surface_preset.select(0)  # Custom
