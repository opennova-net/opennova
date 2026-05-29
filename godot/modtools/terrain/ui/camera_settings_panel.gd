class_name TerrainCameraSettingsPanel
extends VBoxContainer

@onready var _fly_speed_spin: SpinBox = %FlySpeedSpin
@onready var _near_plane_spin: SpinBox = %NearPlaneSpin
@onready var _far_plane_spin: SpinBox = %FarPlaneSpin

var editor: Node
var _syncing: bool = false


func _ready() -> void:
	_fly_speed_spin.value_changed.connect(_on_fly_speed_changed)
	_near_plane_spin.value_changed.connect(_on_near_plane_changed)
	_far_plane_spin.value_changed.connect(_on_far_plane_changed)
	_sync_from_editor()


func set_editor(value: Node) -> void:
	SignalRebind.rebind(editor, value, &"ui_state_changed", Callable(self, "_on_editor_ui_state_changed"))
	editor = value
	_sync_from_editor()


func sync_from_editor_state() -> void:
	_sync_from_editor()


func _on_editor_ui_state_changed(_version: int) -> void:
	_sync_from_editor()


func _sync_from_editor() -> void:
	var camera := _get_camera()
	if camera == null:
		return
	_syncing = true
	_fly_speed_spin.set_value_no_signal(camera.fly_speed)
	_near_plane_spin.set_value_no_signal(camera.near)
	_far_plane_spin.set_value_no_signal(camera.far)
	_syncing = false


func _get_camera() -> Camera3D:
	if editor == null:
		return null
	if editor.has_method("get_editor_camera"):
		return editor.get_editor_camera()
	return editor.get("camera") as Camera3D


func _on_fly_speed_changed(value: float) -> void:
	var camera := _get_camera()
	if _syncing or camera == null:
		return
	camera.fly_speed = value


func _on_near_plane_changed(value: float) -> void:
	var camera := _get_camera()
	if _syncing or camera == null:
		return
	camera.near = value


func _on_far_plane_changed(value: float) -> void:
	var camera := _get_camera()
	if _syncing or camera == null:
		return
	camera.far = value
