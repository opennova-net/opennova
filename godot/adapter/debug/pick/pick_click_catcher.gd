class_name PickClickCatcher
extends Node
## The overlay-open mouse picker: while the F3 overlay is up (mouse released),
## a left-click on the world ray-picks through the live camera and adds to
## the injected pick list. It lives inside the world subtree so the root
## viewport routes game clicks here while overlay-panel clicks (consumed by
## Controls before the unhandled phase) never leak through.

var _world: Node = null  # duck-typed get_sim(); re-resolved every click
var _pick_list: NovaDebugPickList = null


func setup(world: Node, pick_list: NovaDebugPickList) -> void:
	_world = world
	_pick_list = pick_list


func _unhandled_input(event: InputEvent) -> void:
	var button := event as InputEventMouseButton
	if button == null or not button.pressed or button.button_index != MOUSE_BUTTON_LEFT:
		return
	if _pick_list == null or _world == null or not is_instance_valid(_world) \
			or not _world.has_method("get_sim"):
		return
	var viewport := get_viewport()
	if viewport == null:
		return
	var camera := viewport.get_camera_3d()
	if camera == null:
		return
	var pick := DebugEntityPicker.pick_with_camera(
			_world.get_sim(), camera, viewport.get_mouse_position(), "mouse_click")
	if pick.is_empty():
		return
	_pick_list.add(pick)
	viewport.set_input_as_handled()
