extends GutTest

const LocalPlayerHost := preload("res://engine/world/local_player_host.gd")


class FakeWorld:
	extends Node3D
	var input_calls: Array = []
	var avatar_count := 0
	var viewmodel_count := 0
	var _has_player := true
	var _loaded := true

	func is_loaded() -> bool:
		return _loaded

	func has_local_player() -> bool:
		return _has_player

	func build_local_player_avatar() -> Node3D:
		avatar_count += 1
		var node := Node3D.new()
		add_child(node)
		return node

	func build_local_player_viewmodel() -> Node3D:
		viewmodel_count += 1
		var node := Node3D.new()
		add_child(node)
		return node

	func set_local_player_input(forward: bool, back: bool, left: bool, right: bool, run: bool,
			crouch: bool, prone: bool, jump: bool, look_yaw_deg: float, look_pitch_deg: float) -> void:
		input_calls.append({
			"forward": forward,
			"back": back,
			"left": left,
			"right": right,
			"run": run,
			"crouch": crouch,
			"prone": prone,
			"jump": jump,
			"yaw": look_yaw_deg,
			"pitch": look_pitch_deg,
		})

	func local_player_position() -> Vector3:
		return Vector3.ZERO

	func local_player_yaw_deg() -> float:
		return 0.0

	func local_player_pitch_deg() -> float:
		return 0.0

	func local_player_anim_key() -> String:
		return ""

	func local_player_anim_phase_ticks() -> int:
		return 0

	func local_player_body_anim_slot() -> int:
		return -1


func after_each() -> void:
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


func test_shared_host_drives_simultaneous_raw_input_before_world_tick() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	host.setup(world, camera)
	host.set_input_source(func() -> Dictionary:
		return {"forward": true, "left": true, "run": true, "jump": true})

	host.before_world_tick(0.016)

	assert_eq(world.input_calls.size(), 1)
	var call: Dictionary = world.input_calls[0]
	assert_true(call["forward"])
	assert_true(call["left"])
	assert_true(call["run"])
	assert_true(call["jump"])
	assert_false(call["back"])
	assert_false(call["right"])
	assert_eq(world.avatar_count, 1, "3P avatar is owned by the shared host")
	assert_eq(world.viewmodel_count, 1, "FP viewmodel is owned by the shared host")


func test_shared_host_mouse_yaw_wraps_and_pitch_clamps() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	host.setup(world, camera)
	host.set_input_source(func() -> Dictionary:
		return {})

	var motion := InputEventMouseMotion.new()
	motion.relative = Vector2(4000.0, -4000.0)
	assert_true(host.handle_input(motion, true))
	host.before_world_tick(0.016)

	var call: Dictionary = world.input_calls[0]
	assert_gte(call["yaw"], 0.0)
	assert_lt(call["yaw"], 360.0)
	assert_eq(call["pitch"], 80.0)


func test_shared_host_teardown_releases_captured_mouse() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	host.setup(world, camera)

	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	host.teardown()

	assert_eq(Input.get_mouse_mode(), Input.MOUSE_MODE_VISIBLE)
