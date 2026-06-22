extends Node

var input_target: Object


func _unhandled_input(event: InputEvent) -> void:
	if input_target == null or not input_target.has_method("handle_viewport_input"):
		return
	if input_target.handle_viewport_input(event):
		get_viewport().set_input_as_handled()
