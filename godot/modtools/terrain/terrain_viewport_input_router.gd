class_name TerrainViewportInputRouter
extends Node

var terrain_editor: Node


func _unhandled_input(event: InputEvent) -> void:
	if terrain_editor != null:
		terrain_editor.handle_viewport_input(event)
