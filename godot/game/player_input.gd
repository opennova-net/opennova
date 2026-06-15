extends Node
## Local-player input reader.
##
## Samples movement/stance/fire actions and accumulates mouse-look deltas each frame, builds
## a PlayerInputCommand, and drains it into the simulation once per sim tick. Mirrors the
## original input layer: discrete intent + an 8-way move direction + the scaled mouse-look
## delta [orig: Input_ProcessFrame @0x49d520 -> Player_PackInputStateToEntity @0x4df450; mouse
## gain Input_ProcessMouseAxisBindings @0x499680; look apply Input_HandleActionBinding_0
## @0x4e0420]. The core motor (AiSystem::tick_player, the org2 motor @0x4b40e0) consumes the
## command; this reader owns the device layer. See docs/world/player-controller-re.md.
##
## Input actions are registered in code (robust + headless-testable) rather than via the
## project.godot Object-literal input map, which keeps that file untouched.

## Mouse sensitivity (the JO STRMISC_MOUSESCALE cvar, valid 1..511). The applied BAM look
## delta = ((raw_px * (sens << 11) + 0x8000) >> 16) << 16 [orig: @0x499680 scale, then the
## <<16 in the look action handler @0x4e0420].
@export var mouse_sensitivity: int = 200
## When false (the JO default, dword_24D2078 == 0), screen-up moves the view up.
@export var invert_y: bool = false

var enabled: bool = false

var _accum_mouse := Vector2.ZERO
var _pending_jump := false
var _pending_use := false
var _pending_reload := false
# Sticky toggles (crouch/prone are mutually exclusive; aim is ADS hold-or-toggle).
var _crouch := false
var _prone := false
var _aim := false

# action name -> default physical key.
const _KEY_ACTIONS := {
	"move_forward": KEY_W, "move_back": KEY_S,
	"strafe_left": KEY_A, "strafe_right": KEY_D,
	"jump": KEY_SPACE, "crouch": KEY_C, "prone": KEY_Z,
	"lean_left": KEY_Q, "lean_right": KEY_E, "reload": KEY_R, "use": KEY_F,
}

func _ready() -> void:
	ensure_actions()

## Register the player input actions (idempotent). Safe to call from tests.
func ensure_actions() -> void:
	for name in _KEY_ACTIONS:
		if not InputMap.has_action(name):
			InputMap.add_action(name)
			var ev := InputEventKey.new()
			ev.physical_keycode = _KEY_ACTIONS[name]
			InputMap.action_add_event(name, ev)
	_ensure_mouse_action("fire", MOUSE_BUTTON_LEFT)
	_ensure_mouse_action("aim", MOUSE_BUTTON_RIGHT)

func _ensure_mouse_action(name: String, button: int) -> void:
	if not InputMap.has_action(name):
		InputMap.add_action(name)
		var ev := InputEventMouseButton.new()
		ev.button_index = button
		InputMap.action_add_event(name, ev)

func set_enabled(on: bool) -> void:
	enabled = on
	if not on:
		reset()

## Clear per-tick accumulators + latches (call on resume / focus-in so a paused interval
## contributes nothing).
func reset() -> void:
	_accum_mouse = Vector2.ZERO
	_pending_jump = false
	_pending_use = false
	_pending_reload = false

func _input(event: InputEvent) -> void:
	if not enabled:
		return
	if event is InputEventMouseMotion:
		if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
			_accum_mouse += event.relative
		return
	if event is InputEventKey and event.is_echo():
		return
	# Edge-latch the toggles / one-shots so a tap between drains is never lost.
	if event.is_action_pressed("jump"):
		_pending_jump = true
	elif event.is_action_pressed("reload"):
		_pending_reload = true
	elif event.is_action_pressed("use"):
		_pending_use = true
	elif event.is_action_pressed("crouch"):
		_crouch = not _crouch
		if _crouch:
			_prone = false
	elif event.is_action_pressed("prone"):
		_prone = not _prone
		if _prone:
			_crouch = false
	elif event.is_action_pressed("aim"):
		_aim = not _aim

## Scale a raw pixel delta to the applied BAM look delta.
func _scale_look(raw_px: float) -> int:
	var delta := int(round(raw_px))
	var sens := clampi(mouse_sensitivity, 1, 511)
	var scaled := (delta * (sens << 11) + 0x8000) >> 16
	return scaled << 16

## Build the PlayerInputCommand for this tick (pure; safe to call in tests).
func build_command() -> Dictionary:
	var fwd := Input.is_action_pressed("move_forward")
	var back := Input.is_action_pressed("move_back")
	var left := Input.is_action_pressed("strafe_left")
	var right := Input.is_action_pressed("strafe_right")
	# Opposing pairs cancel (matches the dir-nibble switch @0x4df656).
	if fwd and back:
		fwd = false
		back = false
	if left and right:
		left = false
		right = false
	var is_moving := fwd or back or left or right
	# 8-way index, body-relative clockwise from forward
	# (0=fwd,1=fwd-left,2=strafe-left,3=back-left,4=back,5=back-right,6=strafe-right,7=fwd-right).
	var move_dir := 0
	if fwd and left:
		move_dir = 1
	elif fwd and right:
		move_dir = 7
	elif back and left:
		move_dir = 3
	elif back and right:
		move_dir = 5
	elif fwd:
		move_dir = 0
	elif back:
		move_dir = 4
	elif left:
		move_dir = 2
	elif right:
		move_dir = 6
	var pitch_raw := _accum_mouse.y
	if not invert_y:
		pitch_raw = -pitch_raw
	return {
		"move_dir": move_dir,
		"is_moving": is_moving,
		"crouch": _crouch,
		"prone": _prone,
		"fire": Input.is_action_pressed("fire"),
		"aim": _aim,
		"jump": _pending_jump,
		"use": _pending_use,
		"reload": _pending_reload,
		"lean_left": Input.is_action_pressed("lean_left"),
		"lean_right": Input.is_action_pressed("lean_right"),
		"look_yaw_delta": _scale_look(_accum_mouse.x),
		"look_pitch_delta": _scale_look(pitch_raw),
	}

## Build + push the command to the simulation, then consume per-tick accumulators/latches.
## Call once per sim tick (before the sim advances). `sim` is the NovaSimulation (or a fake).
func drain_into(sim) -> void:
	if sim != null and sim.has_method("set_player_input"):
		sim.set_player_input(build_command())
	_accum_mouse = Vector2.ZERO
	_pending_jump = false
	_pending_use = false
	_pending_reload = false
