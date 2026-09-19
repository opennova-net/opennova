extends GutTest


func test_spectator_entry_adopts_the_current_player_camera_pose() -> void:
	var camera := FlyCamera.new()
	add_child_autofree(camera)
	camera.rotation = Vector3(0.25, 0.70, 0.0)

	camera.set_spectator_mode(true)
	assert_true(camera.is_spectator_mode())
	assert_false(camera.is_gameplay_locked())

	var right_down := InputEventMouseButton.new()
	right_down.button_index = MOUSE_BUTTON_RIGHT
	right_down.pressed = true
	camera.get_viewport().push_input(right_down)
	var look := InputEventMouseMotion.new()
	look.relative = Vector2(10.0, -5.0)
	camera.get_viewport().push_input(look)

	assert_almost_eq(camera.rotation.y, 0.70 - 10.0 * camera.mouse_sensitivity,
			0.0001,
			"the first free-look delta starts from the presented player yaw")
	assert_almost_eq(camera.rotation.x, 0.25 + 5.0 * camera.mouse_sensitivity,
			0.0001,
			"the first free-look delta starts from the presented player pitch")

	camera.set_spectator_mode(false)
	assert_false(camera.is_spectator_mode())


# A parent frame consumer, matching the shell/world-before-camera tree order.
class CameraObserver extends Node:
	var camera: FlyCamera
	var sampled_position := Vector3.ZERO
	func _process(_delta: float) -> void:
		sampled_position = camera.global_position


func test_free_flight_precedes_parent_frame_consumers() -> void:
	var observer := CameraObserver.new()
	var camera := FlyCamera.new()
	observer.camera = camera
	observer.add_child(camera)
	add_child_autofree(observer)
	camera.set_spectator_mode(true)
	var right_down := InputEventMouseButton.new()
	right_down.button_index = MOUSE_BUTTON_RIGHT
	right_down.pressed = true
	camera.get_viewport().push_input(right_down)
	var forward := InputEventKey.new()
	forward.keycode = KEY_W
	forward.pressed = true
	Input.parse_input_event(forward)
	await get_tree().process_frame
	await get_tree().process_frame
	assert_gt(camera.global_position.length(), 0.0, "free flight moved the camera")
	assert_eq(observer.sampled_position, camera.global_position,
			"world preparation consumes the same camera pose that will render")
	forward.pressed = false
	Input.parse_input_event(forward)
	right_down.pressed = false
	camera.get_viewport().push_input(right_down)
