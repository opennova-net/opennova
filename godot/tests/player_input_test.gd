extends GutTest

# M1 local-player input reader: action -> 8-way move intent, mouse-delta accumulation +
# BAM scaling, and drain (push once + zero the accumulator). Pure-Godot: a fake sim records
# set_player_input; no GDExtension needed. The reader registers its actions in code, so the
# test drives Input.action_press against those.

const PlayerInput := preload("res://game/player_input.gd")

class FakeSim:
	extends RefCounted
	var last_cmd: Dictionary = {}
	var push_count: int = 0
	func set_player_input(cmd: Dictionary) -> void:
		last_cmd = cmd
		push_count += 1

var _pi

func before_each() -> void:
	_pi = PlayerInput.new()
	add_child_autofree(_pi)
	_pi.ensure_actions()
	_pi.set_enabled(true)
	_release_all()

func after_each() -> void:
	_release_all()

func _release_all() -> void:
	for a in ["move_forward", "move_back", "strafe_left", "strafe_right",
			"fire", "lean_left", "lean_right"]:
		if InputMap.has_action(a):
			Input.action_release(a)

func test_forward_sets_dir0_moving() -> void:
	Input.action_press("move_forward")
	var cmd: Dictionary = _pi.build_command()
	assert_true(cmd["is_moving"], "forward held -> moving")
	assert_eq(cmd["move_dir"], 0, "forward -> dir 0")

func test_diagonal_and_opposing_cancel() -> void:
	Input.action_press("move_forward")
	Input.action_press("strafe_left")
	assert_eq(_pi.build_command()["move_dir"], 1, "fwd+left -> dir 1")
	Input.action_press("move_back") # fwd+back cancel -> strafe-left only
	assert_eq(_pi.build_command()["move_dir"], 2, "fwd+back cancel, left -> dir 2")

func test_idle_no_motion_no_look() -> void:
	var cmd: Dictionary = _pi.build_command()
	assert_false(cmd["is_moving"], "no keys -> not moving")
	assert_eq(cmd["look_yaw_delta"], 0)
	assert_eq(cmd["look_pitch_delta"], 0)

func test_mouse_accumulates_and_scales() -> void:
	# Two motions sum (the reader sums relative motion across frames).
	_pi._accum_mouse += Vector2(6, 0)
	_pi._accum_mouse += Vector2(4, 0)
	var sens: int = clampi(_pi.mouse_sensitivity, 1, 511)
	var expect: int = (((10 * (sens << 11)) + 0x8000) >> 16) << 16
	assert_eq(_pi.build_command()["look_yaw_delta"], expect, "yaw = scaled sum of 10px")

func test_drain_pushes_once_and_zeroes() -> void:
	var sim := FakeSim.new()
	_pi._accum_mouse += Vector2(10, 0)
	_pi.drain_into(sim)
	assert_eq(sim.push_count, 1, "one push per drain")
	assert_true(sim.last_cmd.has("look_yaw_delta"))
	assert_ne(sim.last_cmd["look_yaw_delta"], 0, "non-zero yaw pushed")
	# A second drain with no new motion pushes a zeroed look delta (accumulator cleared).
	_pi.drain_into(sim)
	assert_eq(sim.push_count, 2)
	assert_eq(sim.last_cmd["look_yaw_delta"], 0, "drain zeroed the mouse accumulator")

func test_crouch_toggle_excludes_prone() -> void:
	# Drive the toggle latches directly (the toggle edge is handled in _input on capture).
	_pi._crouch = true
	_pi._prone = false
	var cmd: Dictionary = _pi.build_command()
	assert_true(cmd["crouch"])
	assert_false(cmd["prone"])
