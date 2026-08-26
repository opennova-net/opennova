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
