extends GutTest

# The methods of tests/environment_cube_capture_test.gd that need a RenderingDevice, split
# out of it so the windowed run selects them (G-100).


# The device copy leg needs a live Forward+ RenderingDevice (a windowed run);
# headless and Compatibility runs report it pending.
func _device_available() -> bool:
	return DisplayServer.get_name() != "headless" and \
			RenderingServer.get_current_rendering_method() == "forward_plus" and \
			RenderingServer.get_rendering_device() != null


func _publish_once(capture: EnvironmentCubeCapture) -> void:
	# The six UPDATE_ONCE faces render with this frame's draw; the device copy
	# publishes on the following advance.
	capture.advance_frame(Vector3.ZERO)
	assert_true(capture.is_capture_pending())
	await get_tree().process_frame
	capture.advance_frame(Vector3.ZERO)


func test_device_copy_publishes_a_texture_cubemap_rd_without_readback() -> void:
	if not _device_available():
		pending("requires a windowed Forward+ RenderingDevice run")
		return
	var capture := EnvironmentCubeCapture.new()
	add_child_autofree(capture)
	await get_tree().process_frame
	assert_eq(capture.get_device_publish_count(), 0)
	await _publish_once(capture)
	assert_true(capture.is_cube_ready())
	assert_false(capture.is_capture_pending())
	assert_eq(capture.get_device_failure(), "")
	assert_eq(capture.get_device_publish_count(), 1)
	var cube := capture.get_environment_cube()
	assert_not_null(cube)
	assert_true(cube is TextureCubemapRD)
	assert_true(cube.texture_rd_rid.is_valid())
	assert_eq(cube.get_layers(), EnvironmentCubeCapture.FACE_COUNT)
	assert_eq(cube.get_width(), capture.get_capture_size())
	assert_eq(cube.get_height(), capture.get_capture_size())
	assert_eq(cube.get_format(), Image.FORMAT_RGBA8)
	# The RD cubemap itself: six RGBA8 UNORM layers, sampled by the object
	# shaders and drawn into by the blit; no Image or Cubemap resource exists.
	var rd := RenderingServer.get_rendering_device()
	var format := rd.texture_get_format(cube.texture_rd_rid)
	assert_eq(format.texture_type, RenderingDevice.TEXTURE_TYPE_CUBE)
	assert_eq(format.array_layers, EnvironmentCubeCapture.FACE_COUNT)
	assert_eq(format.format, RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM)
	assert_true(bool(format.usage_bits & RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT))
	assert_true(bool(format.usage_bits & RenderingDevice.TEXTURE_USAGE_COLOR_ATTACHMENT_BIT))
	# The shader global carries this same object; global_shader_parameter_get
	# is editor-only, so the consumer side is pinned by the D3D12 orientation
	# probe sampling the published cube through a samplerCube.


func test_cadence_recopies_every_128th_frame_into_the_same_cube() -> void:
	if not _device_available():
		pending("requires a windowed Forward+ RenderingDevice run")
		return
	var capture := EnvironmentCubeCapture.new()
	add_child_autofree(capture)
	await get_tree().process_frame
	await _publish_once(capture)
	assert_eq(capture.get_device_publish_count(), 1)
	var cube := capture.get_environment_cube()
	var cube_rid := cube.texture_rd_rid
	# No frame boundary is needed to reach the cadence: only the request
	# depends on the frame index, and nothing is copied until it is issued.
	var guard := 0
	while not capture.is_capture_pending() and guard < 200:
		capture.advance_frame(Vector3.ZERO)
		guard += 1
	assert_true(capture.is_capture_pending())
	assert_eq(capture.get_render_frame_index(), EnvironmentCubeCapture.REFRESH_FRAMES + 1)
	assert_eq(capture.get_device_publish_count(), 1)
	await get_tree().process_frame
	capture.advance_frame(Vector3.ZERO)
	assert_false(capture.is_capture_pending())
	assert_eq(capture.get_device_publish_count(), 2)
	assert_same(capture.get_environment_cube(), cube)
	assert_eq(cube.texture_rd_rid, cube_rid)


# The frame that renders the faces copies them in its own draw and samples them: the cube is ready at
# the request and the copy has run when that frame's draw ends, before any later advance, as retail
# renders the faces ahead of the scene's draws in the frame that samples them. A mission start
# unreadies the last mission's cube until its own first capture.
func test_the_request_frame_copies_the_faces_in_its_own_draw() -> void:
	if not _device_available():
		pending("requires a windowed Forward+ RenderingDevice run")
		return
	var view := SubViewport.new()
	view.size = Vector2i(64, 64)
	view.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(view)
	var environment := WorldEnvironment.new()
	environment.environment = Environment.new()
	view.add_child(environment)
	var camera := Camera3D.new()
	view.add_child(camera)
	camera.make_current()
	var capture := EnvironmentCubeCapture.new()
	view.add_child(capture)
	await get_tree().process_frame
	# The mission load's render subsystems make the cube ahead of the first capture.
	capture.create_cube_device()
	await RenderingServer.frame_post_draw
	capture.advance_frame(Vector3.ZERO)
	assert_true(capture.is_capture_pending())
	assert_true(capture.is_cube_ready(), "the request frame's cube is ready at its request")
	assert_eq(capture.get_device_publish_count(), 0)
	await RenderingServer.frame_post_draw
	assert_eq(capture.get_device_publish_count(), 1, "the copy ran in the request frame's draw")
	capture.advance_frame(Vector3.ZERO)
	assert_false(capture.is_capture_pending())
	assert_eq(capture.get_device_publish_count(), 1, "the next advance copies nothing again")
	capture.set_terrain_data(null)
	assert_false(capture.is_cube_ready(), "a mission start unreadies the last mission's cube")
	capture.advance_frame(Vector3.ZERO)
	assert_true(capture.is_cube_ready(), "its first capture readies it at the request")
	await RenderingServer.frame_post_draw
	assert_eq(capture.get_device_publish_count(), 2)


func test_exit_tree_releases_the_device_cube_and_re_entry_recopies() -> void:
	if not _device_available():
		pending("requires a windowed Forward+ RenderingDevice run")
		return
	var capture := EnvironmentCubeCapture.new()
	add_child_autofree(capture)
	await get_tree().process_frame
	await _publish_once(capture)
	var first_rid := capture.get_environment_cube().texture_rd_rid
	remove_child(capture)
	assert_false(capture.is_cube_ready())
	assert_null(capture.get_environment_cube())
	assert_eq(capture.get_device_publish_count(), 0)
	assert_false(RenderingServer.get_rendering_device().texture_is_valid(first_rid))
	add_child(capture)
	await get_tree().process_frame
	await _publish_once(capture)
	assert_true(capture.is_cube_ready())
	assert_eq(capture.get_device_publish_count(), 1)
	assert_true(capture.get_environment_cube().texture_rd_rid.is_valid())
	assert_ne(capture.get_environment_cube().texture_rd_rid, first_rid)
