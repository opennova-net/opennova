class_name TerrainViewportInputRouter
extends Node

var terrain_editor: Node
var edit_input_enabled: bool = true


func _unhandled_input(event: InputEvent) -> void:
	if edit_input_enabled and terrain_editor != null and terrain_editor.has_method("handle_viewport_input"):
		terrain_editor.handle_viewport_input(event)
