extends GutTest

# The reflection target's first render in the load's one forced frame (Water.warm_reflection_target,
# GameWorld's water_mirror leg; engine mission/mission_load_plan.h carries the witness, D-NET-NEW-LF-23):
# retail makes the target and its surfaces with the mission start's render subsystems, so the warm
# installs the mirror through the view the camera draws, serves its raster, and arms the target for
# that one draw while the world's rendering is still shut (it opens at the load's end). No water,
# a mirror switched off or no camera arms nothing. Node state only: headless.

const TICK := 1.0 / 62.0


func _fixture(height: float) -> Dictionary:
	var view := SubViewport.new()
	view.size = Vector2i(1024, 600)
	add_child_autofree(view)
	var water := Water.new()
	view.add_child(water)
	water.water_height = height
	var camera := Camera3D.new()
	view.add_child(camera)
	camera.global_position = Vector3(100.3, 27.0, -33.7)
	camera.make_current()
	# The load's state: the water's world rendering opens at the load's end.
	water.set_world_rendering_enabled(false)
	return {"water": water, "camera": camera}


func test_the_warm_arms_the_target_for_one_draw_through_the_frames_view() -> void:
	var fixture := _fixture(7.0)
	var water: Water = fixture["water"]
	var mirror_vp: SubViewport = water.get_reflection_viewport()
	assert_eq(mirror_vp.render_target_update_mode, SubViewport.UPDATE_DISABLED,
			"shut while the world loads")
	assert_true(water.warm_reflection_target(fixture["camera"]))
	assert_eq(mirror_vp.render_target_update_mode, SubViewport.UPDATE_ONCE,
			"the forced frame draws the target once")
	assert_true(TargetProjectionXrInterface.is_serving(mirror_vp),
			"its raster served the view's frustum, as a frame's install serves it")
	assert_eq(mirror_vp.size_2d_override, Vector2i(1024, 600), "the source's frame")
	var served := TargetProjectionXrInterface.served_projection(mirror_vp)
	assert_almost_eq(served.y.y / served.x.x, 1024.0 / 600.0, 0.0001,
			"the source's aspect over the square")
	# The first frame past the load installs and arms the mirror as every frame does.
	water.set_world_rendering_enabled(true)
	water.advance_frame(TICK)
	assert_eq(mirror_vp.render_target_update_mode, SubViewport.UPDATE_ALWAYS)


func test_no_water_no_camera_or_no_mirror_arms_nothing() -> void:
	var dry := _fixture(0.0)
	var dry_water: Water = dry["water"]
	assert_false(dry_water.warm_reflection_target(dry["camera"]), "zero is the no-water sentinel")
	assert_eq(dry_water.get_reflection_viewport().render_target_update_mode,
			SubViewport.UPDATE_DISABLED)
	var wet := _fixture(7.0)
	var water: Water = wet["water"]
	assert_false(water.warm_reflection_target(null), "no camera")
	water.set_mirror_enabled(false)
	assert_false(water.warm_reflection_target(wet["camera"]), "the mirror switched off")
	assert_eq(water.get_reflection_viewport().render_target_update_mode,
			SubViewport.UPDATE_DISABLED)
