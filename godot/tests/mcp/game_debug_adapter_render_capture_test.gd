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
	adapter.configure(
			func(): return null,
			func(): return world,
			func(): return null,
			func(): return "world",
			loading_source,
			func(): return false,
			func(): pass,
			func(): pass,
			func(): pass)
	return adapter


func test_capture_rejects_loading_or_start_splash_before_touching_viewport() -> void:
	var world: LoadedWorldHarness = autofree(LoadedWorldHarness.new())
	world.mark_loaded()
	var adapter := _adapter(world, func(): return true)
	var began := [false]
	adapter.set_render_capture_actions(
			func() -> Error:
				began[0] = true
				return OK,
			func(): pass)

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
	adapter.set_hud_hidden_capture_actions(
			func() -> Error:
				begin_calls[0] += 1
				return OK,
			func() -> void:
				finish_calls[0] += 1,
			func() -> HudHiddenCaptureWitness:
				var witness := HudHiddenCaptureWitness.new()
				witness.hud_detail_level = 3
				witness.gameplay_hud_visible = false
				witness.player_view_effects_active = true
				witness.hud_canvas_layer_active = true
				return witness)

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
