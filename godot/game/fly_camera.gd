class_name FlyCamera
extends Camera3D
## Editor-style free camera for runtime. RDP-compatible (no mouse capture).
## Middle mouse: orbit. Scroll: zoom. Shift+middle: pan.
## Right click + move: look. Right click + WASD: fly.
## Right click + scroll: adjust fly speed. Shift: 3x speed boost.

# Esc was pressed. Shell-neutral: each shell decides what the key means.
signal escape_pressed

@export var mouse_sensitivity: float = 0.003
@export var orbit_sensitivity: float = 0.005
@export var zoom_speed: float = 20.0
@export var pan_sensitivity: float = 0.5

const SPEED_MIN := 5.0
const SPEED_MAX := 2000.0
const SPEED_STEP := 1.2

var fly_speed: float = 300.0

var _yaw: float = 0.0
var _pitch: float = 0.0
var _pivot: Vector3 = Vector3.ZERO
var _distance: float = 200.0
var _orbiting: bool = false
var _panning: bool = false
var _flying: bool = false
var _gameplay_locked: bool = false
var _spectator_mode: bool = false

func _ready() -> void:
	_yaw = rotation.y
	_pitch = rotation.x
	_pivot = global_position - global_transform.basis.z * _distance
	# A beauty camera: exclude the caster marker layers (real casters also
	# carry a camera-visible world bit — Godot's directional caster cull
	# intersects each camera's visible_layers with the instance layer, see
	# LocalPlayerPresenter.setup(); the FP body hides via SHADOWS_ONLY, not
	# by layer); the FP viewmodel layer stays admitted (its instances apply
	# their own projection + depth band).
	cull_mask = (cull_mask | Water.VISUAL_LAYER_VIEWMODEL) \
			& ~Water.VISUAL_LAYER_SHADOW_CASTER_MASK

func set_gameplay_locked(locked: bool) -> void:
	_gameplay_locked = locked
	if locked:
		_orbiting = false
		_panning = false
		_flying = false


func is_gameplay_locked() -> bool:
	return _gameplay_locked


## Adopt the camera pose the player presenter last stamped before spectator
## free-flight takes ownership. Without this edge sync, the first mouse motion
## would jump back to FlyCamera's scene-start yaw/pitch.
func set_spectator_mode(active: bool) -> void:
	if active == _spectator_mode:
		return
	_spectator_mode = active
	_flying = false
	_orbiting = false
	_panning = false
	if active:
		_yaw = rotation.y
		_pitch = rotation.x
		_pivot = global_position - global_transform.basis.z * _distance


func is_spectator_mode() -> bool:
	return _spectator_mode

func _unhandled_input(event: InputEvent) -> void:
	if _gameplay_locked:
		if event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
			escape_pressed.emit()
		return
	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_MIDDLE:
			if event.pressed:
				if event.shift_pressed:
					_panning = true
					_orbiting = false
				else:
					_orbiting = true
					_panning = false
			else:
				_orbiting = false
				_panning = false

		elif event.button_index == MOUSE_BUTTON_RIGHT:
			_flying = event.pressed

		elif event.button_index == MOUSE_BUTTON_WHEEL_UP and event.pressed:
			if _flying:
				fly_speed = minf(fly_speed * SPEED_STEP, SPEED_MAX)
			else:
				_distance = maxf(_distance - zoom_speed, 1.0)
				_update_orbit()
		elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN and event.pressed:
			if _flying:
				fly_speed = maxf(fly_speed / SPEED_STEP, SPEED_MIN)
			else:
				_distance += zoom_speed
				_update_orbit()

	if event is InputEventMouseMotion:
		if _flying:
			_yaw -= event.relative.x * mouse_sensitivity
			_pitch -= event.relative.y * mouse_sensitivity
			_pitch = clampf(_pitch, -PI * 0.49, PI * 0.49)
			rotation = Vector3(_pitch, _yaw, 0)
			_pivot = global_position - global_transform.basis.z * _distance

		elif _orbiting:
			_yaw -= event.relative.x * orbit_sensitivity
			_pitch -= event.relative.y * orbit_sensitivity
			_pitch = clampf(_pitch, -PI * 0.49, PI * 0.49)
			_update_orbit()

		elif _panning:
			var right := global_transform.basis.x
			var up := global_transform.basis.y
			_pivot -= right * event.relative.x * pan_sensitivity
			_pivot += up * event.relative.y * pan_sensitivity
			_update_orbit()

	if event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
		escape_pressed.emit()

func _update_orbit() -> void:
	var rot := Transform3D()
	rot = rot.rotated(Vector3.UP, _yaw)
	rot = rot.rotated(rot.basis.x.normalized(), _pitch)
	global_position = _pivot + rot.basis.z * _distance
	look_at(_pivot, Vector3.UP)
	_yaw = rotation.y
	_pitch = rotation.x


func frame_bounds_custom(center: Vector3, radius: float, distance_scale: float = 1.35, max_distance: float = 1200.0, yaw: float = 0.0, pitch: float = -0.55) -> void:
	_pivot = center
	_distance = clampf(radius * distance_scale, 1.0, max_distance)
	_yaw = yaw
	_pitch = clampf(pitch, -PI * 0.49, PI * 0.49)
	_update_orbit()

func _process(delta: float) -> void:
	if _gameplay_locked or not _flying:
		return

	var speed: float = fly_speed * (3.0 if Input.is_key_pressed(KEY_SHIFT) else 1.0)
	var dir := Vector3.ZERO

	if Input.is_key_pressed(KEY_W): dir -= transform.basis.z
	if Input.is_key_pressed(KEY_S): dir += transform.basis.z
	if Input.is_key_pressed(KEY_A): dir -= transform.basis.x
	if Input.is_key_pressed(KEY_D): dir += transform.basis.x
	if Input.is_key_pressed(KEY_E): dir += Vector3.UP
	if Input.is_key_pressed(KEY_Q) or Input.is_key_pressed(KEY_CTRL): dir -= Vector3.UP

	if dir.length_squared() > 0:
		position += dir.normalized() * speed * delta
		_pivot = global_position - global_transform.basis.z * _distance
