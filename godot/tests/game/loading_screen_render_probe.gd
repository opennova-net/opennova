extends SceneTree

## Windowed regression probe for the real menu -> mission loading handoff. This
## is intentionally not collected by GUT (*_probe.gd): viewport readback needs a
## live renderer, and the mission load deliberately blocks the main thread.
##
## Run from the repository root (never with --headless):
##   "$GODOT_BIN" --path godot -s res://tests/game/loading_screen_render_probe.gd -- /d
##
## `/d` is load-bearing: the committed minimal fixture is loose authored data.
## The probe drives the public MenuShell.start_requested signal, then accepts only
## a frame captured while MainGame.is_world_loading() is true that contains both
## the loading art and the witnessed red progress bar. This catches the failure
## where a forced draw happens before the newly-mounted Control has reached a
## SceneTree frame, leaving the OS cursor responsive over a black client area.

const STATE_CONFIG_PATH := "user://terrain_editor_state.cfg"
const MINIMAL_RESOURCE_DIR := "res://../fixtures/minimal/resources"
const MISSION := "mnml.bms"
const WINDOW_SIZE := Vector2i(960, 720)
const MENU_WAIT_FRAMES := 180
const LOAD_TIMEOUT_MS := 15_000

# The art occupies the upper screen; excluding the lower 15% keeps the progress
# bar from making a black background look non-black. The threshold is deliberately
# structural rather than pixel-golden so the probe is stable across render drivers.
const ART_HEIGHT_FRACTION := 0.85
const ART_MEAN_LUMA_MIN := 0.20
const ART_SAMPLE_GRID := 192

# BAR_FILL is Color8(0xEB, 0, 0). Restrict the scan to its witnessed bottom-center
# neighborhood so incidental red pixels in the briefing art cannot satisfy it.
const BAR_SCAN_LEFT_FRACTION := 0.34
const BAR_SCAN_RIGHT_FRACTION := 0.66
const BAR_SCAN_TOP_FRACTION := 0.92
const RED_MIN := 0.75
const RED_OTHER_MAX := 0.10

var _scene
var _menu_shell
var _settings_snapshotted := false
var _had_state_config := false
var _saved_state_config := PackedByteArray()

var _loading_frames := 0
var _qualified := false
var _qualified_metrics := {}
var _max_art_mean_luma := 0.0
var _max_red_pixels := 0


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if not args.has("/d"):
		await _finish(1,
			"loading_screen_render_probe: /d is required for the committed loose minimal fixture")
		return
	if DisplayServer.get_name() == "headless":
		await _finish(1,
			"loading_screen_render_probe: a windowed renderer is required (do not pass --headless)")
		return

	_snapshot_settings()
	var fixture_dir := ProjectSettings.globalize_path(MINIMAL_RESOURCE_DIR)
	if not DirAccess.dir_exists_absolute(fixture_dir):
		await _finish(1,
			"loading_screen_render_probe: minimal fixture is missing: " + fixture_dir)
		return
	NovaResourceDirSettings.set_resource_dir(fixture_dir)
	NovaResourceDirSettings.set_expansion("")
	NovaResourceDirSettings.set_game("jo")

	DisplayServer.window_set_size(WINDOW_SIZE)
	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		await _finish(1, "loading_screen_render_probe: failed to load main_game.tscn")
		return
	_scene = packed.instantiate()
	root.add_child(_scene)
	_menu_shell = _scene.get_node_or_null("MenuLayer/MenuShell")
	if _menu_shell == null or not _menu_shell.has_signal("start_requested"):
		await _finish(1, "loading_screen_render_probe: public MenuShell.start_requested is unavailable")
		return

	var menu_ready := false
	for _frame in range(MENU_WAIT_FRAMES):
		await process_frame
		var menu = _menu_shell.get_menu() if _menu_shell.has_method("get_menu") else null
		var file := String(_menu_shell.get_current_menu_file()) \
			if _menu_shell.has_method("get_current_menu_file") else ""
		if file.to_lower() == "main.mnu" and menu != null and menu.is_visible_in_tree():
			menu_ready = true
			break
	if not menu_ready:
		await _finish(1, "loading_screen_render_probe: main menu did not become visible")
		return
	if not _scene.has_method("is_world_loading"):
		await _finish(1,
			"loading_screen_render_probe: MainGame.is_world_loading() public observation seam is missing")
		return

	# Connect before the public start signal. This observes both ordinary frames
	# and force_draw() frames emitted from inside the blocking load.
	RenderingServer.frame_post_draw.connect(_on_frame_post_draw)
	var started_ms := Time.get_ticks_msec()
	var deadline_ms := started_ms + LOAD_TIMEOUT_MS
	_menu_shell.emit_signal("start_requested", MISSION)

	while not _qualified and Time.get_ticks_msec() < deadline_ms:
		if not is_instance_valid(_scene) or not bool(_scene.is_world_loading()):
			break
		await process_frame

	var elapsed_ms := Time.get_ticks_msec() - started_ms
	if not _qualified:
		await _finish(1,
			("loading_screen_render_probe: no loading frame contained both art and the red bar "
			+ "(loading_frames=%d, max_upper_mean_luma=%.4f, max_red_pixels=%d, elapsed_ms=%d)"
			% [_loading_frames, _max_art_mean_luma, _max_red_pixels, elapsed_ms]))
		return

	print("loading_screen_render_probe: OK ", {
		"loading_frames": _loading_frames,
		"qualified_upper_mean_luma": _qualified_metrics.get("art_mean_luma", 0.0),
		"qualified_red_pixels": _qualified_metrics.get("red_pixels", 0),
		"elapsed_ms": elapsed_ms,
	})
	await _finish(0, "")


func _on_frame_post_draw() -> void:
	if _qualified or _scene == null or not is_instance_valid(_scene):
		return
	if not _scene.has_method("is_world_loading") or not bool(_scene.is_world_loading()):
		return
	var viewport := root.get_viewport()
	var image: Image = viewport.get_texture().get_image() if viewport != null else null
	if image == null or image.is_empty():
		return
	_loading_frames += 1
	var metrics := _measure_frame(image)
	_max_art_mean_luma = maxf(_max_art_mean_luma, float(metrics["art_mean_luma"]))
	_max_red_pixels = maxi(_max_red_pixels, int(metrics["red_pixels"]))
	if float(metrics["art_mean_luma"]) > ART_MEAN_LUMA_MIN and int(metrics["red_pixels"]) > 0:
		_qualified = true
		_qualified_metrics = metrics


func _measure_frame(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var art_bottom := clampi(int(height * ART_HEIGHT_FRACTION), 1, height)
	@warning_ignore("integer_division")
	var sample_step := maxi(1, maxi(width, art_bottom) / ART_SAMPLE_GRID)
	var luma_sum := 0.0
	var luma_samples := 0
	for y in range(0, art_bottom, sample_step):
		for x in range(0, width, sample_step):
			var color := image.get_pixel(x, y)
			luma_sum += color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
			luma_samples += 1

	var red_pixels := 0
	var bar_left := clampi(int(width * BAR_SCAN_LEFT_FRACTION), 0, width)
	var bar_right := clampi(int(width * BAR_SCAN_RIGHT_FRACTION), bar_left, width)
	var bar_top := clampi(int(height * BAR_SCAN_TOP_FRACTION), 0, height)
	for y in range(bar_top, height):
		for x in range(bar_left, bar_right):
			var color := image.get_pixel(x, y)
			if color.r >= RED_MIN and color.g <= RED_OTHER_MAX and color.b <= RED_OTHER_MAX:
				red_pixels += 1

	return {
		"art_mean_luma": luma_sum / float(maxi(1, luma_samples)),
		"red_pixels": red_pixels,
		"size": Vector2i(width, height),
	}


func _snapshot_settings() -> void:
	_settings_snapshotted = true
	_had_state_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_state_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) \
		if _had_state_config else PackedByteArray()


func _restore_settings() -> void:
	if not _settings_snapshotted:
		return
	if _had_state_config:
		var file := FileAccess.open(STATE_CONFIG_PATH, FileAccess.WRITE)
		if file != null:
			file.store_buffer(_saved_state_config)
			file.close()
	elif FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))


func _finish(code: int, failure: String) -> void:
	if not failure.is_empty():
		push_error(failure)
	if RenderingServer.frame_post_draw.is_connected(_on_frame_post_draw):
		RenderingServer.frame_post_draw.disconnect(_on_frame_post_draw)
	if _scene != null and is_instance_valid(_scene):
		_scene.queue_free()
		await process_frame
	# `-s` SceneTree scripts compile before autoload names enter the global scope;
	# resolve the two cleanup services through the public root nodes instead.
	var music_service := root.get_node_or_null("NovaMusicService")
	if music_service != null and music_service.has_method("stop_context"):
		music_service.stop_context()
	var strings := root.get_node_or_null("NovaStrings")
	if strings != null and strings.has_method("clear"):
		strings.clear()
	_restore_settings()
	quit(code)
