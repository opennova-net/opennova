class_name TerrainViewportInputRouter
extends Node

var terrain_editor: TerrainEditorBase
var edit_input_enabled: bool = true

# Optional second consumer of viewport input, independent of terrain editing. The
# Mission workspace sets this to its controller (with edit_input_enabled false) so it
# can pick / drag entities while the terrain brush stays dormant. Cleared on unmount.
var input_target: ViewportInputSink


func _unhandled_input(event: InputEvent) -> void:
	if edit_input_enabled and terrain_editor != null:
		terrain_editor.handle_viewport_input(event)
	if input_target != null:
		input_target.handle_viewport_input(event)
