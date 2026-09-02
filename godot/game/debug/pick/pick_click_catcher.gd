class_name PickClickCatcher
extends Node
## The tools-open mouse picker: while the dev tools are up in Interact (mouse
## released), a left-click on the world ray-picks through the live camera and
## adds to the injected pick list. It lives inside the world subtree, so it
## sees the click in the coordinates of the viewport the world renders in:
## in the debug windowed runtime that is the SubViewport inside the ImGui Game
## window, whose size follows that window and whose events arrive with
## `position` already remapped to viewport-local pixels by the ImGui bridge.
## The event's own position is therefore the ray's screen point; the polled
## viewport mouse position is the root window's cursor and would land the ray
## off by the Game window's offset on screen.

var _world: GameWorld = null  # re-resolved for its sim every click
var _pick_list: DebugPickList = null
## The viewport-local position the last pick ray was built from (the test
## seam; INF until a click ran).
var last_pick_position := Vector2.INF


func setup(world: GameWorld, pick_list: DebugPickList) -> void:
	_world = world
	_pick_list = pick_list


func _unhandled_input(event: InputEvent) -> void:
	handle_click(event)


## The picker body: a left press ray-picks through the live camera and adds
## the hit to the pick list, consuming the event. Public so the tests can feed
## synthetic events without the viewport's input routing.
func handle_click(event: InputEvent) -> void:
	var button := event as InputEventMouseButton
	if button == null or not button.pressed or button.button_index != MOUSE_BUTTON_LEFT \
			or button.double_click:
		return
	if _pick_list == null or _world == null or not is_instance_valid(_world):
		return
	var viewport := get_viewport()
	if viewport == null:
		return
	var camera := viewport.get_camera_3d()
	if camera == null:
		return
	last_pick_position = button.position
	var pick := DebugEntityPicker.pick_with_camera(
			_world.get_sim(), camera, button.position, "mouse_click")
	if pick == null:
		return
	_pick_list.add(pick)
	viewport.set_input_as_handled()
