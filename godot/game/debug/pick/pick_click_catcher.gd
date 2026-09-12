class_name PickClickCatcher
extends Node
## The tools-open mouse picker: while the dev tools are up in Interact (mouse
## released), a left-click on the world ray-picks through the view the surface
## shows (DebugEntityPicker: the presenter's stretched target while an aspect
## mode is live, else the live camera) and adds to the injected pick list. It
## lives inside the world subtree, so it sees the click in the coordinates of
## the viewport the world renders in: in the debug windowed runtime that is
## the SubViewport inside the ImGui Game window, whose size follows that
## window and whose events arrive with `position` already remapped to
## viewport-local pixels by the ImGui bridge. The event's own position is
## therefore the ray's screen point; the polled viewport mouse position is the
## root window's cursor and would land the ray off by the Game window's offset
## on screen.

var _view: WorldView = null  # re-resolved for its sim every click
var _presenter: LocalPlayerPresenter = null  # the world's local view presenter
var _pick_list: DebugPickList = null
## The card the last click's ray answered, carrying that ray (the test seam;
## null until a click ran).
var last_pick: DebugPickCard = null


## `presenter` is the presenter the world's surface draws through (null for a
## bare camera).
func setup(view: WorldView, presenter: LocalPlayerPresenter,
		pick_list: DebugPickList) -> void:
	_view = view
	_presenter = presenter
	_pick_list = pick_list


func presenter() -> LocalPlayerPresenter:
	return _presenter


func _unhandled_input(event: InputEvent) -> void:
	handle_click(event)


## The picker body: a left press ray-picks through the shown view and adds
## the hit to the pick list, consuming the event. Public so the tests can feed
## synthetic events without the viewport's input routing.
func handle_click(event: InputEvent) -> void:
	var button := event as InputEventMouseButton
	if button == null or not button.pressed or button.button_index != MOUSE_BUTTON_LEFT \
			or button.double_click:
		return
	if _pick_list == null or _view == null:
		return
	var viewport := get_viewport()
	if viewport == null:
		return
	var camera := viewport.get_camera_3d()
	if camera == null:
		return
	var pick := DebugEntityPicker.pick_with_camera(
			_view.sim(), camera, button.position, "mouse_click", _presenter)
	if pick == null:
		return
	last_pick = pick
	_pick_list.add(pick)
	viewport.set_input_as_handled()
