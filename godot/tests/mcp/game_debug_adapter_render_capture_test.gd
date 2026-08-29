extends GutTest


class LoadedWorldHarness:
	extends GameWorld

	func mark_loaded() -> void:
		_world_ready = true


func _adapter(
		world: GameWorld,
		loading_source: Callable) -> GameDebugAdapter:
	var adapter := GameDebugAdapter.new()
	add_child_autofree(adapter)
	var seams := GameShellSeams.new()
	seams.runtime_source = func(): return null
	seams.world_source = func(): return world
	seams.presenter_source = func(): return null
	seams.shell_state_source = func(): return "world"
	seams.world_loading_source = loading_source
	seams.dev_tools_open_source = func(): return false
	seams.resume_action = func(): pass
	seams.quit_action = func(): pass
	adapter.configure(seams)
	return adapter


func test_capture_rejects_loading_or_start_splash_before_touching_viewport() -> void:
	var world: LoadedWorldHarness = autofree(LoadedWorldHarness.new())
	world.mark_loaded()
	var adapter := _adapter(world, func(): return true)
	var began := [false]
	var capture_seams := adapter.get_shell_seams()
	capture_seams.render_capture_begin_action = func() -> Error:
		began[0] = true
		return OK
	capture_seams.render_capture_end_action = func(): pass

	var result: Dictionary = await adapter.capture_mcp_render_bundle({
		"settle_frames": 0,
		"world_only": true,
	})

	assert_true(result.has("error"))
	assert_true(String(result["error"]).contains("still loading"))
	assert_false(bool(began[0]), "the splash/loading gate runs before UI mutation")


func test_capture_rejects_a_loaded_world_without_a_current_gameplay_camera() -> void:
	var world: LoadedWorldHarness = autofree(LoadedWorldHarness.new())
	world.mark_loaded()
	var adapter := _adapter(world, func(): return false)

	var result: Dictionary = await adapter.capture_mcp_render_bundle({
		"settle_frames": 0,
		"world_only": false,
	})

	assert_true(result.has("error"))
	assert_true(String(result["error"]).contains("current gameplay camera"))


func test_render_diagnostics_reports_no_world_while_permanent_world_is_unloaded() -> void:
	var world: LoadedWorldHarness = autofree(LoadedWorldHarness.new())
	var adapter := _adapter(world, func(): return false)

	assert_true((adapter.get_mcp_render_diagnostics() as Dictionary).is_empty())


func test_hud_hidden_bundle_scopes_presentation_to_its_target_capture() -> void:
	var world: LoadedWorldHarness = autofree(LoadedWorldHarness.new())
	world.mark_loaded()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	camera.current = true
	await get_tree().process_frame
	var adapter := _adapter(world, func(): return false)
	var begin_calls := [0]
	var finish_calls := [0]
	var hud_seams := adapter.get_shell_seams()
	hud_seams.hud_hidden_capture_begin_action = func() -> Error:
		begin_calls[0] += 1
		return OK
	hud_seams.hud_hidden_capture_end_action = func() -> void:
		finish_calls[0] += 1
	hud_seams.hud_hidden_capture_witness_source = func() -> HudHiddenCaptureWitness:
		var witness := HudHiddenCaptureWitness.new()
		witness.hud_detail_level = 3
		witness.gameplay_hud_visible = false
		witness.player_view_effects_active = true
		witness.hud_canvas_layer_active = true
		return witness

	var result: Dictionary = await adapter.capture_mcp_render_bundle({
		"settle_frames": 1,
		"world_only": false,
		"presentation_mode": "hud_hidden",
	})

	assert_true(result.has("error"),
			"headless screenshot readback deterministically fails after presentation begins")
	assert_eq(begin_calls[0], 1)
	assert_eq(finish_calls[0], 1,
			"the adapter restores HUD/FPS after the individual failed bundle")
