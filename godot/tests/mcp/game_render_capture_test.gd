extends GutTest


const CAPTURE_ROOT := "user://render-capture-test"


func after_each() -> void:
	var absolute := ProjectSettings.globalize_path(CAPTURE_ROOT)
	if DirAccess.dir_exists_absolute(absolute):
		_remove_tree(absolute)


func test_write_bundle_persists_png_and_correlated_state_sidecar() -> void:
	var image := Image.create(8, 4, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.25, 0.5, 0.75, 1.0))
	var png := image.save_png_to_buffer()
	var diagnostics := {
		"schema": "OpenNovaRenderDiagnosticsV1",
		"frame": {"process": 77},
		"camera": {"position": Vector3(1.0, 2.0, 3.0)},
	}
	var outcome: Dictionary = GameRenderCapture.write_bundle(
			png,
			8,
			4,
			diagnostics,
			"00TRa / courtyard",
			CAPTURE_ROOT,
			123456)
	assert_true(outcome["ok"])
	assert_eq(outcome["bundle"]["schema"], "OpenNovaRenderCaptureV1")
	var artifact: Dictionary = outcome["bundle"]["artifact"]
	assert_true(FileAccess.file_exists(artifact["png_path"]))
	assert_true(FileAccess.file_exists(artifact["state_path"]))
	assert_eq(artifact["width"], 8)
	assert_eq(artifact["height"], 4)
	assert_eq(artifact["bytes"], png.size())
	assert_eq(String(artifact["sha256"]).length(), 64)
	assert_true(String(artifact["png_path"]).contains("00TRa-courtyard"),
			"unsafe label characters are normalized in the fixed capture root")

	var sidecar: Variant = JSON.parse_string(
			FileAccess.get_file_as_string(artifact["state_path"]))
	assert_true(sidecar is Dictionary)
	assert_eq(int(sidecar["capture"]["process_frame"]), 77)
	assert_eq(int(sidecar["diagnostics"]["frame"]["process"]), 77)
	assert_eq(sidecar["diagnostics"]["camera"]["position"], [1.0, 2.0, 3.0])
	assert_eq(sidecar["capture"]["png_sha256"], artifact["sha256"])


func test_write_bundle_rejects_empty_png_without_creating_artifacts() -> void:
	var outcome: Dictionary = GameRenderCapture.write_bundle(
			PackedByteArray(), 8, 4, {}, "bad", CAPTURE_ROOT)
	assert_false(outcome["ok"])
	assert_false(DirAccess.dir_exists_absolute(
			ProjectSettings.globalize_path(CAPTURE_ROOT)))


func test_write_bundle_rejects_a_mismatched_draw_and_diagnostics_frame() -> void:
	var image := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	var outcome: Dictionary = GameRenderCapture.write_bundle(
			image.save_png_to_buffer(),
			1,
			1,
			{"frame": {"process": 77}},
			"uncorrelated",
			CAPTURE_ROOT,
			123456,
			78)
	assert_false(outcome["ok"])
	assert_true(String(outcome["error"]).contains("does not match"))
	assert_false(DirAccess.dir_exists_absolute(
			ProjectSettings.globalize_path(CAPTURE_ROOT)))


func test_capture_failure_restores_target_frame_presentation() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(8, 4)
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var begin_calls := [0]
	var finish_calls := [0]
	var start_frame := Engine.get_process_frames()
	var began_at_frame := [-1]

	var outcome: Dictionary = await GameRenderCapture.capture(
			viewport,
			func() -> Dictionary: return {},
			{
				"settle_frames": 2,
				"_presentation_begin": func() -> Error:
					begin_calls[0] += 1
					began_at_frame[0] = Engine.get_process_frames()
					return OK,
				"_presentation_finish": func() -> void:
					finish_calls[0] += 1,
			})

	assert_true(outcome.has("error"),
			"empty diagnostics make this a deterministic capture failure")
	assert_eq(begin_calls[0], 1,
			"presentation begins exactly once for the target screenshot")
	assert_gte(began_at_frame[0], start_frame + 2,
			"settle frames complete before HUD/FPS suppression begins")
	assert_eq(finish_calls[0], 1,
			"capture failure restores presentation immediately")


func test_capture_cancellation_restores_only_an_active_presentation() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(8, 4)
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var begin_calls := [0]
	var finish_calls := [0]
	var cancel_calls := [0]
	var cancel_after_begin := func() -> bool:
		cancel_calls[0] += 1
		return cancel_calls[0] >= 2
	var opts := {
		"settle_frames": 0,
		"_presentation_begin": func() -> Error:
			begin_calls[0] += 1
			return OK,
		"_presentation_finish": func() -> void:
			finish_calls[0] += 1,
	}

	var cancelled_active: Dictionary = await GameRenderCapture.capture(
			viewport, func() -> Dictionary: return {}, opts, cancel_after_begin)
	assert_true(String(cancelled_active.get("error", "")).contains("cancelled"))
	assert_eq(begin_calls[0], 1)
	assert_eq(finish_calls[0], 1,
			"disconnect/cancellation after begin restores presentation")

	begin_calls[0] = 0
	finish_calls[0] = 0
	var cancelled_before: Dictionary = await GameRenderCapture.capture(
			viewport, func() -> Dictionary: return {}, opts,
			func() -> bool: return true)
	assert_true(String(cancelled_before.get("error", "")).contains("cancelled"))
	assert_eq(begin_calls[0], 0,
			"a pre-cancelled bundle never mutates HUD/FPS presentation")
	assert_eq(finish_calls[0], 0,
			"no cleanup callback runs when presentation never began")


func _remove_tree(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	for name in dir.get_files():
		DirAccess.remove_absolute(path.path_join(name))
	for name in dir.get_directories():
		_remove_tree(path.path_join(name))
	DirAccess.remove_absolute(path)
