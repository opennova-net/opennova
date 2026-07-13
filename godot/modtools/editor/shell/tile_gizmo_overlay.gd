class_name TileGizmoOverlay
extends RefCounted

## The in-world tile gizmo overlay: a small button card that follows the
## active workspace's selected tile. Purely capability-driven — visibility
## and label come from get_tile_gizmo_state(), the five buttons route back
## through run_tile_gizmo_action(); the overlay never names a workspace.

var _gizmo: PanelContainer
var _label: Label
var _viewport_lane: Control
# func() -> EditorWorkspace: the active workspace.
var _active_workspace: Callable


func setup(
	gizmo: PanelContainer,
	label: Label,
	viewport_lane: Control,
	active_workspace: Callable
) -> void:
	_gizmo = gizmo
	_label = label
	_viewport_lane = viewport_lane
	_active_workspace = active_workspace


func wire_buttons(
	done: Button,
	rotate: Button,
	flip_x: Button,
	flip_y: Button,
	delete: Button
) -> void:
	done.pressed.connect(_run_action.bind(&"done"))
	rotate.pressed.connect(_run_action.bind(&"rotate"))
	flip_x.pressed.connect(_run_action.bind(&"flip_x"))
	flip_y.pressed.connect(_run_action.bind(&"flip_y"))
	delete.pressed.connect(_run_action.bind(&"delete"))


func refresh() -> void:
	if _gizmo == null or _viewport_lane == null or _label == null:
		return
	_gizmo.visible = false
	var workspace: EditorWorkspace = _active_workspace.call()
	if workspace == null or not workspace.shows_tile_gizmo():
		return
	var state := workspace.get_tile_gizmo_state()
	if state == null:
		return

	var camera: Camera3D = workspace.get_viewport_camera()
	if camera == null:
		return

	var anchor_world := state.anchor_world
	var lane_rect := _viewport_lane.get_global_rect()
	var lane_end := lane_rect.position + lane_rect.size
	_label.text = state.label

	var gizmo_size := _gizmo.get_combined_minimum_size()
	_gizmo.size = gizmo_size
	var target := Vector2(
		lane_rect.position.x + (lane_rect.size.x - gizmo_size.x) * 0.5,
		lane_rect.position.y + 18.0
	)
	if not camera.is_position_behind(anchor_world):
		var screen_pos: Vector2 = camera.unproject_position(anchor_world)
		target = Vector2(
			screen_pos.x - gizmo_size.x * 0.5,
			screen_pos.y - gizmo_size.y - 24.0
		)
	target.x = clampf(target.x, lane_rect.position.x + 12.0, lane_end.x - gizmo_size.x - 12.0)
	target.y = clampf(target.y, lane_rect.position.y + 12.0, lane_end.y - gizmo_size.y - 12.0)
	_gizmo.global_position = target
	_gizmo.visible = true


func _run_action(action: StringName) -> void:
	var workspace: EditorWorkspace = _active_workspace.call()
	if workspace != null:
		workspace.run_tile_gizmo_action(action)
