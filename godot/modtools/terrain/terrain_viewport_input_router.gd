class_name TerrainViewportInputRouter
extends Node

var terrain_editor: Node
var edit_input_enabled: bool = true

# Optional second consumer of viewport input, independent of terrain editing. The
# Mission workspace sets this to its controller (with edit_input_enabled false) so it
# can pick / drag entities while the terrain brush stays dormant. Anything exposing
# handle_viewport_input(event) works; cleared on unmount.
var input_target: Object


func _unhandled_input(event: InputEvent) -> void:
	if edit_input_enabled and terrain_editor != null and terrain_editor.has_method("handle_viewport_input"):
		terrain_editor.handle_viewport_input(event)
	if input_target != null and input_target.has_method("handle_viewport_input"):
		input_target.handle_viewport_input(event)
