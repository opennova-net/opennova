class_name LocalPlayerHost
extends Node

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

const PLAYER_EYE_HEIGHT := 1.0
const PLAYER_PITCH_CLAMP_DEG := 80.0
const PLAYER_MOUSE_SENS_DEG := 0.12
const PLAYER_TP_DISTANCE := 5.0
const PLAYER_TP_HEIGHT := 1.5
const WEAPON_DEF_POS_SCALE := 256.0
const PLAYER_VIEWMODEL_POS_UNITS := Vector3(10.0, 0.0, -201.0)
const PLAYER_VIEWMODEL_ROT := Vector3(0.0, 180.0, 0.0)

var _world
var _camera: Camera3D
var _input_source := Callable()
var _look_yaw := 0.0
var _look_pitch := 0.0
var _look_seeded := false
var _third_person := false
var _crouch := false
var _prone := false
var _avatar: Node3D = null
var _viewmodel: Node3D = null


func setup(world, camera: Camera3D) -> void:
	_world = world
	_camera = camera
	_reset_state()


func teardown() -> void:
	_set_fly_camera_locked(false)
	_release_mouse_capture()
	_clear_models()
	_world = null
	_camera = null
	_input_source = Callable()
	_reset_state()


func set_input_source(source: Callable) -> void:
	_input_source = source


func set_third_person(enabled: bool) -> void:
	_third_person = enabled


func is_third_person() -> bool:
	return _third_person


func before_world_tick(_delta: float, capture_mouse: bool = false) -> void:
	if not _has_player():
		_set_fly_camera_locked(false)
		_release_mouse_capture()
		_clear_models()
		_look_seeded = false
		return
	_set_fly_camera_locked(true)
	_seed_look_from_world()
	if capture_mouse and Input.get_mouse_mode() != Input.MOUSE_MODE_CAPTURED:
		Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	_ensure_models()
	var state := _read_input_state()
	_world.set_local_player_input(
		_bool(state, "forward"),
		_bool(state, "back"),
		_bool(state, "left"),
		_bool(state, "right"),
		_bool(state, "run"),
		_crouch,
		_prone,
		_bool(state, "jump"),
		_look_yaw,
		_look_pitch)


func after_world_tick() -> void:
	if not _has_player():
		_set_fly_camera_locked(false)
		_release_mouse_capture()
		_clear_models()
		return
	_update_player_camera()


func handle_key_input(event: InputEvent, active: bool) -> bool:
	if not active or not _has_player() or not (event is InputEventKey):
		return false
	var key := event as InputEventKey
	if not key.pressed or key.echo:
		return false
	if key.keycode == KEY_F4:
		_third_person = not _third_person
		return true
	if key.keycode == KEY_C:
		_crouch = not _crouch
		if _crouch:
			_prone = false
		return true
	if key.keycode == KEY_Z:
		_prone = not _prone
		if _prone:
			_crouch = false
		return true
	return false


func handle_input(event: InputEvent, active: bool) -> bool:
	if not active or not _has_player() or not (event is InputEventMouseMotion):
		return false
	_seed_look_from_world()
	var mm := event as InputEventMouseMotion
	_look_yaw = _wrap_degrees(_look_yaw + mm.relative.x * PLAYER_MOUSE_SENS_DEG)
	_look_pitch = clampf(_look_pitch - mm.relative.y * PLAYER_MOUSE_SENS_DEG,
		-PLAYER_PITCH_CLAMP_DEG, PLAYER_PITCH_CLAMP_DEG)
	return true


func _reset_state() -> void:
	_look_yaw = 0.0
	_look_pitch = 0.0
	_look_seeded = false
	_third_person = false
	_crouch = false
	_prone = false


func _has_player() -> bool:
	if _world == null:
		return false
	if _world.has_method("is_loaded") and not _world.is_loaded():
		return false
	return _world.has_method("has_local_player") and _world.has_local_player()


func _seed_look_from_world() -> void:
	if _look_seeded or _world == null:
		return
	_look_yaw = _wrap_degrees(float(_world.local_player_yaw_deg()))
	_look_pitch = clampf(float(_world.local_player_pitch_deg()),
		-PLAYER_PITCH_CLAMP_DEG, PLAYER_PITCH_CLAMP_DEG)
	_look_seeded = true


func _read_input_state() -> Dictionary:
	if _input_source.is_valid():
		var out = _input_source.call()
		if out is Dictionary:
			return out
	return {
		"forward": Input.is_physical_key_pressed(KEY_W),
		"back": Input.is_physical_key_pressed(KEY_S),
		"left": Input.is_physical_key_pressed(KEY_A),
		"right": Input.is_physical_key_pressed(KEY_D),
		"run": Input.is_physical_key_pressed(KEY_SHIFT),
		"jump": Input.is_physical_key_pressed(KEY_SPACE),
	}


func _bool(state: Dictionary, key: String) -> bool:
	return bool(state.get(key, false))


func _ensure_models() -> void:
	if _world == null:
		return
	if (_avatar == null or not is_instance_valid(_avatar)) and _world.has_method("build_local_player_avatar"):
		_avatar = _world.build_local_player_avatar()
	if (_viewmodel == null or not is_instance_valid(_viewmodel)) and _world.has_method("build_local_player_viewmodel"):
		_viewmodel = _world.build_local_player_viewmodel()


func _clear_models() -> void:
	if _avatar != null and is_instance_valid(_avatar):
		_avatar.queue_free()
	if _viewmodel != null and is_instance_valid(_viewmodel):
		_viewmodel.queue_free()
	_avatar = null
	_viewmodel = null


func _set_fly_camera_locked(locked: bool) -> void:
	if _camera != null and _camera.has_method("set_gameplay_locked"):
		_camera.set_gameplay_locked(locked)


func _release_mouse_capture() -> void:
	if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
		Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


func _update_player_camera() -> void:
	if _world == null or _camera == null:
		return
	var pos: Vector3 = _world.local_player_position()
	var yr := deg_to_rad(_world.local_player_yaw_deg())
	var pr := deg_to_rad(_world.local_player_pitch_deg())
	var forward := Vector3(sin(yr) * cos(pr), sin(pr), -cos(yr) * cos(pr))
	var eye := pos + Vector3(0, PLAYER_EYE_HEIGHT, 0)
	if _third_person:
		_camera.global_position = eye - forward * PLAYER_TP_DISTANCE + Vector3(0, PLAYER_TP_HEIGHT, 0)
		_camera.look_at(eye, Vector3.UP)
	else:
		_camera.global_position = eye
		_camera.look_at(eye + forward, Vector3.UP)
	_update_avatar(pos)
	_update_viewmodel()


func _update_avatar(pos: Vector3) -> void:
	if _avatar == null or not is_instance_valid(_avatar) or _world == null:
		return
	_avatar.global_position = pos
	_avatar.global_basis = MissionObjectPlacer.bms_to_godot_basis(
		Vector3(0.0, _world.local_player_yaw_deg(), 0.0))
	_avatar.visible = _third_person
	var anim_key := String(_world.local_player_anim_key()) if _world.has_method("local_player_anim_key") else ""
	var anim_phase := int(_world.local_player_anim_phase_ticks()) if _world.has_method("local_player_anim_phase_ticks") else 0
	if not anim_key.is_empty() and _avatar.has_method("play_body_clip_at"):
		_avatar.play_body_clip_at(anim_key, anim_phase)
	elif not anim_key.is_empty() and _avatar.has_method("play_body_clip"):
		_avatar.play_body_clip(anim_key)
	elif _avatar.has_method("play_body_anim_at"):
		_avatar.play_body_anim_at(_world.local_player_anim_slot(), anim_phase)
	elif _avatar.has_method("play_body_anim"):
		_avatar.play_body_anim(_world.local_player_anim_slot())


func _update_viewmodel() -> void:
	if _viewmodel == null or not is_instance_valid(_viewmodel) or _camera == null:
		return
	var vm_basis := Basis.from_euler(Vector3(
		deg_to_rad(PLAYER_VIEWMODEL_ROT.x),
		deg_to_rad(PLAYER_VIEWMODEL_ROT.y),
		deg_to_rad(PLAYER_VIEWMODEL_ROT.z)))
	_viewmodel.global_transform = _camera.global_transform * Transform3D(
		vm_basis, _viewmodel_offset(PLAYER_VIEWMODEL_POS_UNITS))
	_viewmodel.visible = not _third_person


func _viewmodel_offset(units: Vector3) -> Vector3:
	return Vector3(
		units.x / WEAPON_DEF_POS_SCALE,
		units.z / WEAPON_DEF_POS_SCALE,
		-units.y / WEAPON_DEF_POS_SCALE)


func _wrap_degrees(degrees: float) -> float:
	var out := fmod(degrees, 360.0)
	if out < 0.0:
		out += 360.0
	return out
