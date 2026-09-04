extends GameProbe

## loading_screen_render: the regression probe for the real
## menu -> mission loading handoff. From the main menu it drives the shell's
## public start seam, then accepts only a frame captured while
## MainGame.is_world_loading() is true that contains both the loading art and
## the witnessed red progress bar. With fullscreen_during_load, the initial
## real-stage checkpoint switches the actual Window to fullscreen
## while the ordinary SceneTree loop is blocked; the accepted frame must also
## prove that the game viewport, loading surface, and captured image all match
## the fullscreen window. This catches both a premature forced draw and a
## stale windowed-resolution loading surface. The
## mission (mnml.bms by default) must be in the launch's mounted root: the
## committed minimal fixture wants `--resource-dir <repo>/assets /d`.

const MISSION_DEFAULT := "mnml.bms"
const WINDOW_SIZE := Vector2i(960, 720)
const MENU_WAIT_FRAMES := 180
const LOAD_TIMEOUT_MS := 15_000

# The art occupies the upper screen; excluding the lower 15% keeps the progress
# bar from making a black background look non-black. The threshold is deliberately
# structural rather than pixel-golden so the probe is stable across render drivers.
const ART_HEIGHT_FRACTION := 0.85
const ART_MEAN_LUMA_MIN := 0.20
const ART_SAMPLE_GRID := 192
const EXTENDED_ART_SAMPLE_MIN := 32

# BAR_FILL is Color8(0xEB, 0, 0). Restrict the scan to its witnessed bottom-center
# neighborhood so incidental red pixels in the briefing art cannot satisfy it.
const BAR_SCAN_LEFT_FRACTION := 0.34
const BAR_SCAN_RIGHT_FRACTION := 0.66
const BAR_SCAN_TOP_FRACTION := 0.92
const RED_MIN := 0.75
const RED_OTHER_MAX := 0.10

var _ctx: ProbeContext
var _loading_frames := 0
var _qualified := false
var _qualified_metrics := {}
var _max_art_mean_luma := 0.0
var _max_red_pixels := 0
var _progress_callbacks := 0
var _progress_sequence := PackedInt32Array()
var _screen_progress_sequence := PackedInt32Array()
var _fullscreen_during_load := false
var _fullscreen_requested := false
var _fullscreen_observed := false
var _fullscreen_surface_matched := false
var _fullscreen_art_extended := false
var _max_extended_art_samples := 0
var _window: Window = null
var _windowed_size := Vector2i.ZERO


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	_loading_frames = 0
	_qualified = false
	_qualified_metrics = {}
	_max_art_mean_luma = 0.0
	_max_red_pixels = 0
	_progress_callbacks = 0
	_progress_sequence = PackedInt32Array()
	_screen_progress_sequence = PackedInt32Array()
	_fullscreen_during_load = bool(ctx.args.get("fullscreen_during_load", false))
	_fullscreen_requested = false
	_fullscreen_observed = false
	_fullscreen_surface_matched = false
	_fullscreen_art_extended = false
	_max_extended_art_samples = 0
	_windowed_size = Vector2i.ZERO
	var mission := String(ctx.args.get("mission", MISSION_DEFAULT)).strip_edges()
	var shell := ctx.game()
	var menu_shell := ctx.menu_shell()
	if shell == null or menu_shell == null:
		return ProbeVerdict.failed("the game shell and its menu are unavailable")
	var resource_root := ctx.resource_root()
	if resource_root == null or not resource_root.has_file(mission):
		return ProbeVerdict.failed("%s is not in the mounted --resource-dir" % mission)
	var world := ctx.world()
	if world != null and world.is_loaded():
		var leave := ctx.return_to_menu()
		if leave != OK:
			return ProbeVerdict.failed("could not return to the menu: %s" % error_string(leave))
	if not ctx.set_window_size(WINDOW_SIZE):
		return ProbeVerdict.failed("the shell has no window to size")
	var live_viewport := ctx.viewport()
	_window = live_viewport.get_window() if live_viewport != null else null
	if _window == null:
		return ProbeVerdict.failed("the shell has no live window")
	_windowed_size = _window.size

	var menu_ready := false
	for _frame in range(MENU_WAIT_FRAMES):
		await ctx.tree.process_frame
		if ctx.cancelled:
			return ProbeVerdict.failed("cancelled")
		var frame := menu_shell.get_frame()
		var file := String(menu_shell.get_current_menu_file())
		if file.to_lower() == "main.mnu" and frame != null and frame.is_visible_in_tree():
			menu_ready = true
			break
	if not menu_ready:
		return ProbeVerdict.failed("main menu did not become visible")

	# Connect before the public start seam. This observes both ordinary frames
	# and force_draw() frames emitted from inside the blocking load.
	RenderingServer.frame_post_draw.connect(_on_frame_post_draw)
	ctx.defer_restore(func() -> void:
		if RenderingServer.frame_post_draw.is_connected(_on_frame_post_draw):
			RenderingServer.frame_post_draw.disconnect(_on_frame_post_draw))
	if world == null:
		return ProbeVerdict.failed("the game world is unavailable")
	var started_ms := Time.get_ticks_msec()
	var deadline_ms := started_ms + LOAD_TIMEOUT_MS
	var start := ctx.start_mission(mission)
	if start != OK:
		return ProbeVerdict.failed("the shell refused to start %s: %s" % [mission, error_string(start)])
	# start_mission mounts the coordinator and then yields inside its two-frame
	# preparation barrier. Connecting here puts this observer after the
	# coordinator: every checkpoint callback can inspect the frame that
	# LoadingScreen.present just forced while the main loop is blocked.
	world.load_progress.connect(_on_load_progress)
	ctx.defer_restore(func() -> void:
		if is_instance_valid(world) and world.load_progress.is_connected(_on_load_progress):
			world.load_progress.disconnect(_on_load_progress))

	while not _qualified and Time.get_ticks_msec() < deadline_ms and not ctx.cancelled:
		if not is_instance_valid(shell) or not shell.is_world_loading():
			break
		await ctx.tree.process_frame

	var elapsed_ms := Time.get_ticks_msec() - started_ms
	var expected_progress := PackedInt32Array(
			[0, 10, 20, 30, 40, 50, 60, 70, 80, 90])
	var data := {
		"loading_frames": _loading_frames,
		"max_upper_mean_luma": _max_art_mean_luma,
		"max_red_pixels": _max_red_pixels,
		"progress_callbacks": _progress_callbacks,
		"progress_sequence": _progress_sequence,
		"screen_progress_sequence": _screen_progress_sequence,
		"progress_exact": _progress_sequence == expected_progress \
				and _screen_progress_sequence == expected_progress,
		"elapsed_ms": elapsed_ms,
		"fullscreen_during_load": _fullscreen_during_load,
		"fullscreen_requested": _fullscreen_requested,
		"fullscreen_observed": _fullscreen_observed,
		"fullscreen_surface_matched": _fullscreen_surface_matched,
		"fullscreen_art_extended": _fullscreen_art_extended,
		"max_extended_art_samples": _max_extended_art_samples,
	}
	if is_instance_valid(shell) and shell.is_world_loading():
		data["presentation_released"] = await ctx.wait_world_ready(LOAD_TIMEOUT_MS)
	else:
		data["presentation_released"] = true
	if not _qualified:
		var detail := ("no qualifying loading frame was presented "
				+ "(loading_frames=%d, max_upper_mean_luma=%.4f, max_red_pixels=%d, "
				+ "fullscreen_requested=%s, fullscreen_observed=%s, surface_matched=%s, "
				+ "art_extended=%s, elapsed_ms=%d)")
		detail = detail % [_loading_frames, _max_art_mean_luma, _max_red_pixels,
				_fullscreen_requested, _fullscreen_observed,
				_fullscreen_surface_matched, _fullscreen_art_extended, elapsed_ms]
		return ProbeVerdict.failed(detail, data)
	if not bool(data["progress_exact"]):
		return ProbeVerdict.failed(
				"loading progress did not follow the exact retail-mission checkpoints",
				data)
	data["qualified_upper_mean_luma"] = _qualified_metrics.get("art_mean_luma", 0.0)
	data["qualified_red_pixels"] = _qualified_metrics.get("red_pixels", 0)
	data["qualified_checkpoint"] = _qualified_metrics.get("checkpoint", -1)
	data["qualified_extended_art_samples"] = _qualified_metrics.get(
			"extended_art_samples", 0)
	data["qualified_window_size"] = _qualified_metrics.get("window_size", Vector2i.ZERO)
	data["qualified_viewport_size"] = _qualified_metrics.get("viewport_size", Vector2i.ZERO)
	data["qualified_surface_size"] = _qualified_metrics.get("surface_size", Vector2i.ZERO)
	data["qualified_image_size"] = _qualified_metrics.get("size", Vector2i.ZERO)
	ctx.log("OK loading_frames=%d qualified_upper_mean_luma=%.4f qualified_red_pixels=%d fullscreen=%s size=%s elapsed_ms=%d" % [
			_loading_frames, float(data["qualified_upper_mean_luma"]),
			int(data["qualified_red_pixels"]), _fullscreen_surface_matched,
			data["qualified_image_size"], elapsed_ms])
	var summary := "a fullscreen loading frame covered the live window" \
			if _fullscreen_during_load else "a loading frame carried the art and the red bar"
	return ProbeVerdict.passed(summary, data)


func _on_load_progress(percent: int) -> void:
	_progress_callbacks += 1
	if _progress_sequence.is_empty() or _progress_sequence[-1] != percent:
		_progress_sequence.append(percent)
	var shell := _ctx.game() if _ctx != null else null
	var screen_progress := shell.loading_progress_percent() if shell != null else -1
	if _screen_progress_sequence.is_empty() \
			or _screen_progress_sequence[-1] != screen_progress:
		_screen_progress_sequence.append(screen_progress)
	if _fullscreen_during_load and not _fullscreen_requested and percent >= 0 \
			and _window != null:
		_fullscreen_requested = true
		WindowState.set_fullscreen(_window, true)
		DisplayServer.process_events()
	# This callback runs after WorldLoadCoordinator presented the checkpoint.
	# Complete that queued draw before reading back its pixels; frame_post_draw
	# alone is delivered only after the synchronous loader yields to the loop.
	RenderingServer.force_draw(true, 0.0)
	_sample_loading_frame(percent)


func _on_frame_post_draw() -> void:
	_sample_loading_frame()


func _sample_loading_frame(checkpoint := -1) -> void:
	if _qualified or _ctx == null:
		return
	var shell := _ctx.game()
	if shell == null or not shell.is_world_loading():
		return
	var viewport := _ctx.viewport()
	var image: Image = viewport.get_texture().get_image() if viewport != null else null
	if image == null or image.is_empty():
		return
	_loading_frames += 1
	var metrics := _measure_frame(image)
	metrics["checkpoint"] = checkpoint if checkpoint >= 0 \
			else shell.loading_progress_percent()
	_max_art_mean_luma = maxf(_max_art_mean_luma, float(metrics["art_mean_luma"]))
	_max_red_pixels = maxi(_max_red_pixels, int(metrics["red_pixels"]))
	if _fullscreen_during_load:
		var viewport_size := Vector2i(viewport.get_visible_rect().size)
		var surface_size := shell.loading_surface_size()
		var image_size := image.get_size()
		var window_size := _window.size if _window != null else Vector2i.ZERO
		var fullscreen := _window != null and WindowState.is_fullscreen(_window)
		_fullscreen_observed = _fullscreen_observed or fullscreen
		var extended_art_samples := _count_extended_art_samples(image)
		_max_extended_art_samples = maxi(
				_max_extended_art_samples, extended_art_samples)
		var art_extended := extended_art_samples >= EXTENDED_ART_SAMPLE_MIN
		_fullscreen_art_extended = _fullscreen_art_extended or art_extended
		var surface_matched := fullscreen and window_size != _windowed_size \
				and viewport_size == window_size \
				and surface_size == viewport_size and image_size == viewport_size
		_fullscreen_surface_matched = _fullscreen_surface_matched or surface_matched
		metrics["fullscreen"] = fullscreen
		metrics["window_size"] = window_size
		metrics["viewport_size"] = viewport_size
		metrics["surface_size"] = surface_size
		metrics["extended_art_samples"] = extended_art_samples
		if not surface_matched or not art_extended:
			return
	if float(metrics["art_mean_luma"]) > ART_MEAN_LUMA_MIN and int(metrics["red_pixels"]) > 0:
		_qualified = true
		_qualified_metrics = metrics
		var artifact_name := "loading_frame_fullscreen" if _fullscreen_during_load else "loading_frame"
		var path := _ctx.artifact_dir.path_join(artifact_name + ".png")
		if image.save_png(path) == OK:
			_ctx.artifact(artifact_name, path, "png")


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


func _count_extended_art_samples(image: Image) -> int:
	var width := image.get_width()
	var height := image.get_height()
	if width <= _windowed_size.x and height <= _windowed_size.y:
		return EXTENDED_ART_SAMPLE_MIN
	var art_bottom := clampi(int(height * ART_HEIGHT_FRACTION), 1, height)
	@warning_ignore("integer_division")
	var step := maxi(1, maxi(width, art_bottom) / ART_SAMPLE_GRID)
	var samples := 0
	for y in range(0, art_bottom, step):
		for x in range(0, width, step):
			if x < _windowed_size.x and y < _windowed_size.y:
				continue
			var color := image.get_pixel(x, y)
			var luma := color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
			if luma > 0.01:
				samples += 1
	return samples
