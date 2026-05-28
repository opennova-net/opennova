class_name SculptInspector
extends TerrainInspector

## Sculpt workflow: raise / lower / smooth / flatten tool buttons plus the
## shared brush controls. Code-first; built into the inspector host by the
## terrain workspace and synced from the editor's ui_state_changed signal.

const BrushControlsScene = preload("res://modtools/terrain/ui/widgets/brush_controls.tscn")

const SCULPT_TOOLS := [
	TerrainEditor.Tool.RAISE,
	TerrainEditor.Tool.LOWER,
	TerrainEditor.Tool.SMOOTH,
	TerrainEditor.Tool.FLATTEN,
]
const TOOL_DEFS := [
	{"tool": TerrainEditor.Tool.RAISE, "label": "Raise", "name": "RaiseButton"},
	{"tool": TerrainEditor.Tool.LOWER, "label": "Lower", "name": "LowerButton"},
	{"tool": TerrainEditor.Tool.SMOOTH, "label": "Smooth", "name": "SmoothButton"},
	{"tool": TerrainEditor.Tool.FLATTEN, "label": "Flatten", "name": "FlattenButton"},
]

var _tool_buttons: Dictionary = {}
var _brush: BrushControls


func set_editor(value: TerrainEditor) -> void:
	super.set_editor(value)
	if terrain_editor != null and not SCULPT_TOOLS.has(terrain_editor.current_tool):
		terrain_editor.set_tool(TerrainEditor.Tool.RAISE)


func build_main(host: Control) -> void:
	var box := _make_inspector_box(host)
	_root = box
	_add_section_heading(box, "Sculpt")

	var grid := GridContainer.new()
	grid.columns = 2
	grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(grid)

	_tool_buttons = {}
	for tool_def in TOOL_DEFS:
		var btn := Button.new()
		btn.name = String(tool_def["name"])
		btn.text = String(tool_def["label"])
		btn.toggle_mode = true
		btn.focus_mode = Control.FOCUS_NONE
		btn.custom_minimum_size = Vector2(0, 40)
		btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		grid.add_child(btn)
		var tool_id := int(tool_def["tool"])
		_tool_buttons[tool_id] = btn
		btn.pressed.connect(_on_tool_pressed.bind(tool_id))

	box.add_child(HSeparator.new())

	_brush = BrushControlsScene.instantiate()
	box.add_child(_brush)
	_brush.radius_changed.connect(_on_brush_radius)
	_brush.strength_changed.connect(_on_brush_strength)
	_brush.hardness_changed.connect(_on_brush_hardness)

	refresh()


func refresh() -> void:
	if not _ui_alive() or terrain_editor == null:
		return
	_syncing = true
	_brush.set_values(terrain_editor.brush_radius, terrain_editor.brush_strength, terrain_editor.brush_hardness)
	for tool in _tool_buttons:
		(_tool_buttons[tool] as Button).set_pressed_no_signal(terrain_editor.current_tool == tool)
	_syncing = false


func _on_tool_pressed(tool: int) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_tool(tool)


func _on_brush_radius(v: float) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_brush_radius_value(v)


func _on_brush_strength(v: float) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_brush_strength_value(v)


func _on_brush_hardness(v: float) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_brush_hardness_value(v)
