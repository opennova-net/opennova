class_name PaintInspector
extends TerrainInspector

## Paint workflow: three sub-tools (detail layers, vertex color + clone, surface
## type) selected by a button row that shows one panel at a time, plus the
## shared brush controls. Code-first; built into the inspector mount by the
## terrain workspace.


enum SubTool { DETAILS, COLOR, SURFACE }

const SUB_LABELS := {
	SubTool.DETAILS: "Details",
	SubTool.COLOR: "Color",
	SubTool.SURFACE: "Surface",
}
const DETAIL_LABELS := ["Detail A", "Detail B", "Detail C"]

var _current_sub: int = SubTool.COLOR
var _sub_buttons: Dictionary = {}
var _sub_panels: Dictionary = {}
var _detail_channel_buttons: Array[Button] = []
var _color_picker: ColorPickerButton
var _clone_toggle: Button
var _clone_status: Label
var _clone_clear: Button
var _surface_preset: OptionButton
var _surface_index_spin: SpinBox
var _surface_current: Label


func build_main(mount: Control) -> void:
	var box := _make_inspector_box(mount)
	_root = box
	_add_section_heading(box, "Paint")

	var sub_row := HBoxContainer.new()
	sub_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(sub_row)
	_sub_buttons = {}
	for sub in [SubTool.DETAILS, SubTool.COLOR, SubTool.SURFACE]:
		var btn := Button.new()
		btn.name = "Sub%sButton" % String(SUB_LABELS[sub])
		btn.text = String(SUB_LABELS[sub])
		btn.toggle_mode = true
		btn.focus_mode = Control.FOCUS_NONE
		btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		sub_row.add_child(btn)
		_sub_buttons[sub] = btn
		btn.pressed.connect(_on_sub_pressed.bind(sub))

	_sub_panels = {}

	var details_panel := VBoxContainer.new()
	details_panel.name = "DetailsPanel"
	details_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(details_panel)
	_sub_panels[SubTool.DETAILS] = details_panel
	var detail_grid := GridContainer.new()
	detail_grid.columns = 3
	detail_grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	details_panel.add_child(detail_grid)
	_detail_channel_buttons = []
	for ch in DETAIL_LABELS.size():
		var btn := Button.new()
		btn.name = "Detail%sButton" % char(65 + ch)
		btn.text = DETAIL_LABELS[ch]
		btn.toggle_mode = true
		btn.focus_mode = Control.FOCUS_NONE
		btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		detail_grid.add_child(btn)
		_detail_channel_buttons.append(btn)
		btn.pressed.connect(_on_detail_channel_pressed.bind(ch))

	var color_panel := VBoxContainer.new()
	color_panel.name = "ColorPanel"
	color_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(color_panel)
	_sub_panels[SubTool.COLOR] = color_panel
	var color_row := HBoxContainer.new()
	color_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	color_panel.add_child(color_row)
	_color_picker = ColorPickerButton.new()
	_color_picker.name = "ColorPicker"
	_color_picker.custom_minimum_size = Vector2(0, 34)
	_color_picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	color_row.add_child(_color_picker)
	_clone_toggle = Button.new()
	_clone_toggle.name = "CloneToggle"
	_clone_toggle.text = "Clone"
	_clone_toggle.toggle_mode = true
	color_row.add_child(_clone_toggle)
	_add_muted_label(color_panel, "Ctrl-click the terrain to set a clone source, then paint to copy color.")
	_clone_status = _add_muted_label(color_panel, "")
	_clone_clear = Button.new()
	_clone_clear.name = "CloneClearButton"
	_clone_clear.text = "Clear clone source"
	color_panel.add_child(_clone_clear)

	var surface_panel := VBoxContainer.new()
	surface_panel.name = "SurfacePanel"
	surface_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(surface_panel)
	_sub_panels[SubTool.SURFACE] = surface_panel
	var surface_label := Label.new()
	surface_label.text = "Surface preset"
	surface_panel.add_child(surface_label)
	_surface_preset = OptionButton.new()
	_surface_preset.name = "SurfacePreset"
	_surface_preset.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	surface_panel.add_child(_surface_preset)
	_surface_index_spin = _add_spin_row(surface_panel, "SurfaceIndexSpin", "Surface index", 0, 255, 1)
	_surface_current = _add_muted_label(surface_panel, "")

	box.add_child(HSeparator.new())
	_attach_brush_controls(box)

	_color_picker.color_changed.connect(_on_color_changed)
	_color_picker.pressed.connect(_on_color_picker_pressed)
	_clone_toggle.toggled.connect(_on_clone_toggled)
	_clone_clear.pressed.connect(_on_clone_clear_pressed)
	_surface_preset.item_selected.connect(_on_surface_preset_selected)
	_surface_index_spin.value_changed.connect(_on_surface_index_changed)

	_populate_surface_presets()
	_show_sub(SubTool.COLOR)
	refresh()


func set_editor(value: TerrainEditor) -> void:
	super.set_editor(value)
	if terrain_editor == null or not _ui_alive():
		return
	var tool := terrain_editor.current_tool
	if tool == TerrainEditor.Tool.PAINT_DETAIL:
		_current_sub = SubTool.DETAILS
	elif tool == TerrainEditor.Tool.PAINT_COLORMAP or tool == TerrainEditor.Tool.CLONE_COLOR:
		_current_sub = SubTool.COLOR
	elif tool == TerrainEditor.Tool.SURFACE_PAINT:
		_current_sub = SubTool.SURFACE
	else:
		_current_sub = SubTool.COLOR
		terrain_editor.set_tool(TerrainEditor.Tool.PAINT_COLORMAP)
	_show_sub(_current_sub)


func refresh() -> void:
	if not _ui_alive() or terrain_editor == null:
		return
	_syncing = true
	_sync_brush_values()
	var channel := terrain_editor.get_paint_detail_channel()
	for i in _detail_channel_buttons.size():
		_detail_channel_buttons[i].set_pressed_no_signal(i == channel and terrain_editor.current_tool == TerrainEditor.Tool.PAINT_DETAIL)
	var color := terrain_editor.get_paint_color()
	if _color_picker.color != color:
		_color_picker.color = color
	_clone_toggle.set_pressed_no_signal(terrain_editor.current_tool == TerrainEditor.Tool.CLONE_COLOR)
	_clone_status.text = "Clone source ready." if terrain_editor.has_clone_source() else "No clone source. Ctrl-click the terrain to set one."
	_clone_clear.disabled = not terrain_editor.has_clone_source()
	var surface_index := terrain_editor.get_selected_surface_index()
	_surface_index_spin.set_value_no_signal(surface_index)
	_match_surface_preset_dropdown(surface_index)
	_surface_current.text = "Current: %s (%d)" % [terrain_editor.get_selected_surface_label(), surface_index]
	_syncing = false


func _on_sub_pressed(sub: int) -> void:
	if _syncing or terrain_editor == null:
		return
	_show_sub(sub)
	match sub:
		SubTool.DETAILS:
			terrain_editor.select_detail_paint_channel(terrain_editor.get_paint_detail_channel())
		SubTool.COLOR:
			terrain_editor.set_tool(TerrainEditor.Tool.PAINT_COLORMAP)
		SubTool.SURFACE:
			terrain_editor.set_tool(TerrainEditor.Tool.SURFACE_PAINT)


func _show_sub(sub: int) -> void:
	_current_sub = sub
	for s in _sub_buttons:
		(_sub_buttons[s] as Button).set_pressed_no_signal(s == sub)
	for s in _sub_panels:
		(_sub_panels[s] as Control).visible = (s == sub)


func _on_detail_channel_pressed(ch: int) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.select_detail_paint_channel(ch)


func _on_color_changed(color: Color) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_paint_color(color)


func _on_color_picker_pressed() -> void:
	if _syncing or terrain_editor == null:
		return
	if terrain_editor.current_tool != TerrainEditor.Tool.PAINT_COLORMAP:
		terrain_editor.set_tool(TerrainEditor.Tool.PAINT_COLORMAP)


func _on_clone_toggled(pressed: bool) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_tool(TerrainEditor.Tool.CLONE_COLOR if pressed else TerrainEditor.Tool.PAINT_COLORMAP)


func _on_clone_clear_pressed() -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.clear_clone_source()


func _on_surface_preset_selected(idx: int) -> void:
	if _syncing or terrain_editor == null:
		return
	var meta = _surface_preset.get_item_metadata(idx)
	if typeof(meta) == TYPE_INT and int(meta) >= 0:
		terrain_editor.set_selected_surface_index(int(meta))
		_surface_index_spin.set_value_no_signal(int(meta))
	terrain_editor.set_tool(TerrainEditor.Tool.SURFACE_PAINT)


func _on_surface_index_changed(v: float) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_selected_surface_index(int(v))
	_match_surface_preset_dropdown(int(v))
	terrain_editor.set_tool(TerrainEditor.Tool.SURFACE_PAINT)




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
	_surface_preset.select(0)
