extends GutTest


func test_highest_quality_capture_constants_and_face_axes_match_retail() -> void:
	assert_eq(EnvironmentCubeCapture.CAPTURE_SIZE, 256)
	assert_eq(EnvironmentCubeCapture.REFRESH_FRAMES, 128)
	assert_eq(EnvironmentCubeCapture.SKY_DIM_BYTE, 0x60)
	assert_eq(EnvironmentCubeCapture.FACE_COUNT, 6)

	var capture := EnvironmentCubeCapture.new()
	add_child_autofree(capture)
	var directions := capture.get_face_directions()
	var ups := capture.get_face_up_vectors()
	# Capture viewport order is LEFT/RIGHT/BOTTOM/TOP/FRONT/BACK. Retail's
	# render-float -> Godot map swaps X/Z, so these are retail faces
	# 5/4/3/2/1/0 respectively.
	assert_eq(directions, PackedVector3Array([
		Vector3.LEFT, Vector3.RIGHT, Vector3.DOWN, Vector3.UP,
		Vector3.FORWARD, Vector3.BACK,
	]))
	assert_eq(ups, PackedVector3Array([
		Vector3.UP, Vector3.UP, Vector3.RIGHT, Vector3.LEFT,
		Vector3.UP, Vector3.UP,
	]))


func test_capture_uses_player_plus_one_without_terrain_and_isolates_layer() -> void:
	var capture := EnvironmentCubeCapture.new()
	add_child_autofree(capture)
	await get_tree().process_frame
	capture.advance_frame(Vector3(12.0, 34.0, 56.0))
	assert_eq(capture.get_capture_origin(), Vector3(12.0, 35.0, 56.0))
	assert_true(capture.is_capture_pending())
	assert_eq(capture.get_render_frame_index(), 1)
	assert_eq(capture.get_child_count(), 6)
	for child in capture.get_children():
		var viewport := child as SubViewport
		assert_not_null(viewport)
		assert_eq(viewport.size, Vector2i(256, 256))
		var camera := viewport.get_node("Camera") as Camera3D
		assert_not_null(camera)
		assert_eq(camera.cull_mask, Water.VISUAL_LAYER_ENVIRONMENT_CAPTURE)
		assert_almost_eq(camera.fov, 90.0, 0.0001)
		assert_almost_eq(camera.near, 0.5, 0.0001)
		assert_almost_eq(camera.far, 1000.0, 0.0001)


func test_world_admits_only_sky_and_sun_moon_to_capture_layer() -> void:
	var scene := load("res://game/world/game_world.tscn") as PackedScene
	assert_not_null(scene)
	var world := scene.instantiate()
	add_child_autofree(world)
	await get_tree().process_frame

	var capture := world.get_node_or_null("EnvironmentCubeCapture") \
			as EnvironmentCubeCapture
	var sky := world.get_node_or_null("SkyDome") as SkyDome
	var celestial := world.get_node_or_null("Celestial") as Celestial
	assert_not_null(capture)
	assert_not_null(sky)
	assert_not_null(celestial)
	assert_eq(Water.VISUAL_LAYER_ENVIRONMENT_CAPTURE,
			Water.VISUAL_LAYER_WATER)
	assert_eq(sky.environment_capture_layer_mask,
			Water.VISUAL_LAYER_ENVIRONMENT_CAPTURE)
	assert_eq(celestial.environment_capture_layer_mask,
			Water.VISUAL_LAYER_ENVIRONMENT_CAPTURE)


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
	assert_eq(cube.get_width(), EnvironmentCubeCapture.CAPTURE_SIZE)
	assert_eq(cube.get_height(), EnvironmentCubeCapture.CAPTURE_SIZE)
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
