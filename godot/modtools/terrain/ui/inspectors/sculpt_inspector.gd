class_name SculptInspector
extends MarginContainer

@onready var _raise_btn: Button = %RaiseButton
@onready var _lower_btn: Button = %LowerButton
@onready var _smooth_btn: Button = %SmoothButton
@onready var _flatten_btn: Button = %FlattenButton
@onready var _brush: BrushControls = %BrushControls

var editor: TerrainEditor
var _tool_buttons: Dictionary = {}
var _syncing: bool = false


func _ready() -> void:
	_tool_buttons = {
		TerrainEditor.Tool.RAISE: _raise_btn,
		TerrainEditor.Tool.LOWER: _lower_btn,
		TerrainEditor.Tool.SMOOTH: _smooth_btn,
		TerrainEditor.Tool.FLATTEN: _flatten_btn,
	}
	for tool in _tool_buttons:
		var btn: Button = _tool_buttons[tool]
		btn.pressed.connect(_on_tool_pressed.bind(tool))
	_brush.radius_changed.connect(_on_brush_radius)
	_brush.strength_changed.connect(_on_brush_strength)
	_brush.hardness_changed.connect(_on_brush_hardness)
	set_process(true)


func set_editor(value: TerrainEditor) -> void:
	editor = value
	if editor and not _tool_buttons.has(editor.current_tool):
		editor.set_tool(TerrainEditor.Tool.RAISE)
	_sync_from_editor()


func _process(_delta: float) -> void:
	_sync_from_editor()


func _sync_from_editor() -> void:
	if editor == null:
		return
	_syncing = true
	_brush.set_values(editor.brush_radius, editor.brush_strength, editor.brush_hardness)
	for tool in _tool_buttons:
		var btn: Button = _tool_buttons[tool]
		btn.set_pressed_no_signal(editor.current_tool == tool)
	_syncing = false


func _on_tool_pressed(tool: int) -> void:
	if _syncing or editor == null:
		return
	editor.set_tool(tool)


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
