extends GutTest


class LoadedWorldHarness:
	extends GameWorld

	func mark_loaded() -> void:
		_world_ready = true


# The in-world shell the adapter captures through: a GameShell over the test's
# world whose capture presentation legs count their calls.
class CaptureShell:
	extends GameShell

	var world: GameWorld = null
	var world_loading := false
	var world_only_begins := 0
	var hud_hidden_begins := 0
	var hud_hidden_finishes := 0
	var witness: HudHiddenCaptureWitness = null

	func get_world() -> GameWorld:
		return world

	func shell_state_name() -> String:
		return "world"

	func is_world_loading() -> bool:
		return world_loading

	func mcp_begin_world_only_capture() -> Error:
		world_only_begins += 1
		return OK

	func mcp_end_world_only_capture() -> void:
		pass

	func begin_hud_hidden_capture() -> Error:
		hud_hidden_begins += 1
		return OK

	func finish_hud_hidden_capture() -> void:
		hud_hidden_finishes += 1

	func hud_hidden_capture_witness() -> HudHiddenCaptureWitness:
		return witness


func _adapter(world: GameWorld, world_loading: bool) -> GameDebugAdapter:
	var adapter := GameDebugAdapter.new()
	add_child_autofree(adapter)
	var shell: CaptureShell = autofree(CaptureShell.new())
	shell.world = world
	shell.world_loading = world_loading
	adapter.configure(shell)
	return adapter


func test_capture_rejects_loading_or_start_splash_before_touching_viewport() -> void:
	var world: LoadedWorldHarness = autofree(LoadedWorldHarness.new())
	world.mark_loaded()
	var adapter := _adapter(world, true)
	var shell := adapter.get_shell() as CaptureShell

	var result: Dictionary = await adapter.capture_mcp_render_bundle({
		"settle_frames": 0,
		"world_only": true,
	})

	assert_true(result.has("error"))
	assert_true(String(result["error"]).contains("still loading"))
	assert_eq(shell.world_only_begins, 0, "the splash/loading gate runs before UI mutation")


func test_capture_rejects_a_loaded_world_without_a_current_gameplay_camera() -> void:
	var world: LoadedWorldHarness = autofree(LoadedWorldHarness.new())
	world.mark_loaded()
	var adapter := _adapter(world, false)

	var result: Dictionary = await adapter.capture_mcp_render_bundle({
		"settle_frames": 0,
		"world_only": false,
	})

	assert_true(result.has("error"))
	assert_true(String(result["error"]).contains("current gameplay camera"))


func test_render_diagnostics_reports_no_world_while_permanent_world_is_unloaded() -> void:
	var world: LoadedWorldHarness = autofree(LoadedWorldHarness.new())
	var adapter := _adapter(world, false)

	assert_true((adapter.get_mcp_render_diagnostics() as Dictionary).is_empty())


func test_hud_hidden_bundle_scopes_presentation_to_its_target_capture() -> void:
	var world: LoadedWorldHarness = autofree(LoadedWorldHarness.new())
	world.mark_loaded()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	camera.current = true
	await get_tree().process_frame
	var adapter := _adapter(world, false)
	var shell := adapter.get_shell() as CaptureShell
	shell.witness = HudHiddenCaptureWitness.new()
	shell.witness.hud_detail_level = 3
	shell.witness.gameplay_hud_visible = false
	shell.witness.player_view_effects_active = true
	shell.witness.hud_canvas_layer_active = true

	var result: Dictionary = await adapter.capture_mcp_render_bundle({
		"settle_frames": 1,
		"world_only": false,
		"presentation_mode": "hud_hidden",
	})

	assert_true(result.has("error"),
			"headless screenshot readback deterministically fails after presentation begins")
	assert_eq(shell.hud_hidden_begins, 1)
	assert_eq(shell.hud_hidden_finishes, 1,
			"the adapter restores HUD/FPS after the individual failed bundle")
