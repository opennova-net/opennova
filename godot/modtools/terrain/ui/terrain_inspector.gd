class_name TerrainInspector
extends WorkflowInspector

## Base for code-first terrain workflow inspectors (B7: the fork is retired —
## the build/refresh contract and the InspectorForms wrappers come from
## WorkflowInspector). Wired to a TerrainEditor rather than a workspace
## coordinator and single-pane (no detail dock). Both the workspace and the
## tests drive it the same way: set_editor(editor) + build_main(mount).
##
## The inspector re-syncs on the editor's ui_state_changed signal. Because the
## mount is cleared when the workflow/workspace changes (freeing the built nodes)
## while the inspector instance persists, refresh() guards on the stored root so
## a stray signal after teardown is a no-op until build_main rebuilds.

const _BrushControlsScene = preload("res://modtools/terrain/ui/widgets/brush_controls.tscn")

var terrain_editor: TerrainEditor
var _root: Control
var _syncing: bool = false
# The shared radius/strength/hardness widget, when a workflow paints with a
# brush. Set by _attach_brush_controls(); null for non-brush workflows.
var _brush: BrushControls


func _init(editor: TerrainEditor = null) -> void:
	terrain_editor = editor


func set_editor(value: TerrainEditor) -> void:
	SignalRebind.rebind(terrain_editor, value, &"ui_state_changed", Callable(self, "_on_editor_ui_state_changed"))
	terrain_editor = value
	refresh()


func _on_editor_ui_state_changed(_version: int) -> void:
	refresh()


# True while the built UI is alive; subclasses guard refresh() with this so a
# ui_state_changed that arrives after the mount was cleared is a safe no-op.
func _ui_alive() -> bool:
	return _root != null and is_instance_valid(_root)


# --- Shared brush controls (sculpt / paint / scatter) ---
# Instantiates the radius/strength/hardness widget under `parent`, wires its
# signals to the editor, and stores it as _brush. Call from build_main();
# pair with _sync_brush_values() in refresh().
func _attach_brush_controls(parent: Control) -> BrushControls:
	_brush = _BrushControlsScene.instantiate()
	parent.add_child(_brush)
	_brush.radius_changed.connect(_on_brush_radius)
	_brush.strength_changed.connect(_on_brush_strength)
	_brush.hardness_changed.connect(_on_brush_hardness)
	return _brush


func _sync_brush_values() -> void:
	if _brush != null and terrain_editor != null:
		_brush.set_values(terrain_editor.brush_radius, terrain_editor.brush_strength, terrain_editor.brush_hardness)


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
