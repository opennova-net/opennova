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
