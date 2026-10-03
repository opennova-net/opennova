extends GutTest


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
	assert_false(camera.is_gameplay_locked(), "no player locks a fresh free camera")
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


func test_the_gameplay_lock_stands_free_flight_down() -> void:
	var camera := FlyCamera.new()
	add_child_autofree(camera)
	camera.set_gameplay_locked(true)
	assert_true(camera.is_gameplay_locked())
	var right_down := InputEventMouseButton.new()
	right_down.button_index = MOUSE_BUTTON_RIGHT
	right_down.pressed = true
	camera.get_viewport().push_input(right_down)
	var forward := InputEventKey.new()
	forward.keycode = KEY_W
	forward.pressed = true
	Input.parse_input_event(forward)
	await get_tree().process_frame
	assert_eq(camera.global_position, Vector3.ZERO,
			"a locked camera (a live player or the death screen's spectator) never flies")
	forward.pressed = false
	Input.parse_input_event(forward)
	right_down.pressed = false
	camera.get_viewport().push_input(right_down)
