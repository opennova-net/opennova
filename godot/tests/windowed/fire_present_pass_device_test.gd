extends GutTest

# The methods of tests/fire_present_pass_test.gd that need a RenderingDevice, split out of
# it so the windowed run selects them (G-100).


# A current render camera above the points: the ribbons face it.
func _ribbon_camera(container: Node3D) -> Camera3D:
	var camera := Camera3D.new()
	container.add_child(camera)
	camera.position = Vector3(0, 5, 10)
	camera.current = true
	return camera


# The tracer pool's distortion ribbons (the styles with a +0x828 word: the
# rocket, the AT4, the sniper) publish into the effect world's FrameFX drawer
# and draw in the type-0 row with slot 2 = 256B (retail
# CEffectEmitterPool_RenderDistortionPass, called from FrameFX_DistortionPass
# @ 0x583928); a live channel of such a style is what opens the row (retail
# CEffectEmitterPool_HasDistortionChannels @ 0x5DB7F0). The stock styles have
# no distortion ribbon.
func test_distortion_style_tracers_draw_in_the_framefx_row() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 128)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var container := Node3D.new()
	viewport.add_child(container)
	_ribbon_camera(container)
	# FrameFx installs its terminal effect on the viewport's WorldEnvironment.
	var background := WorldEnvironment.new()
	background.environment = Environment.new()
	background.environment.background_mode = Environment.BG_COLOR
	background.environment.background_color = Color.BLACK
	viewport.add_child(background)
	var frame_fx := FrameFx.new()
	viewport.add_child(frame_fx)
	var effects := EffectWorld.new()
	viewport.add_child(effects)
	effects.attach_distortion_row(frame_fx)
	var audio := MissionAudio.create(null, null)
	autofree(audio)
	# The ribbons face the camera of the presenter's own viewport.
	var presenter := EntityPresenter.new()
	viewport.add_child(presenter)
	presenter.setup_passes(container, null, null, audio, effects, null, null)

	presenter.draw_tracer_rows(PackedFloat32Array([
		1.0, 1.0, 3.0,  # stdred
		0.0, 1.0, 0.0, 1.0,
		2.0, 1.0, 0.0, 1.0,
		4.0, 1.0, 0.0, 1.0,
	]))
	var report := effects.get_debug_draw_list_report()
	assert_false(bool(report.get("distortion_present", true)),
			"a stock channel does not open the distortion row")
	assert_eq(int(report.get("distortion_ribbon_indices", -1)), 0)

	presenter.draw_tracer_rows(PackedFloat32Array([
		3.0, 1.0, 3.0,  # rocket
		0.0, 1.0, 0.0, 1.0,
		2.0, 1.0, 0.0, 1.02,
		4.0, 1.0, 0.0, 0.98,
	]))
	report = effects.get_debug_draw_list_report()
	assert_true(bool(report.get("distortion_present", false)),
			"a live rocket channel opens the distortion row")
	assert_eq(int(report.get("distortion_ribbon_indices", -1)), 18,
			"one bridge between two four-vertex distortion cross-sections")

	frame_fx.set_view_effects(0, 0, false, false, 0, false, false, false, false, false, false, false)
	frame_fx.advance_screen_effects()
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	var fx_report := frame_fx.get_backend_report()
	if not bool(fx_report.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		presenter.teardown()
		return
	assert_eq(int(fx_report.get("distortion_sets", -1)), 2)
	assert_eq(int(effects.get_debug_draw_list_report().get("distortion_ribbon_draws", -1)), 1,
			"the row drew the ribbon set")
	presenter.teardown()
