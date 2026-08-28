extends GameProbe

## loading_screen_render: the windowed regression probe for the real
## menu -> mission loading handoff. From the main menu it drives the shell's
## public start seam, then accepts only a frame captured while
## MainGame.is_world_loading() is true that contains both the loading art and
## the witnessed red progress bar. This catches the failure where a forced
## draw happens before the newly-mounted Control has reached a SceneTree
## frame, leaving the OS cursor responsive over a black client area. The
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


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	var mission := String(ctx.args.get("mission", MISSION_DEFAULT)).strip_edges()
	var shell := ctx.game()
	var menu_shell := ctx.menu_shell()
	if shell == null or menu_shell == null:
		return ProbeVerdict.failed("the game shell and its menu are unavailable")
	var resource_root := ctx.resource_root()
	if resource_root == null or not resource_root.has_file(mission):
		return ProbeVerdict.failed("%s is not in the mounted root (launch with the fixture's --resource-dir)" % mission)
	var world := ctx.world()
	if world != null and world.is_loaded():
		var leave := ctx.return_to_menu()
		if leave != OK:
			return ProbeVerdict.failed("could not return to the menu: %s" % error_string(leave))
	if not ctx.set_window_size(WINDOW_SIZE):
		return ProbeVerdict.failed("the shell has no window to size")

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
	var started_ms := Time.get_ticks_msec()
	var deadline_ms := started_ms + LOAD_TIMEOUT_MS
	var start := ctx.start_mission(mission)
	if start != OK:
		return ProbeVerdict.failed("the shell refused to start %s: %s" % [mission, error_string(start)])

	while not _qualified and Time.get_ticks_msec() < deadline_ms and not ctx.cancelled:
		if not is_instance_valid(shell) or not shell.is_world_loading():
			break
		await ctx.tree.process_frame

	var elapsed_ms := Time.get_ticks_msec() - started_ms
	var data := {
		"loading_frames": _loading_frames,
		"max_upper_mean_luma": _max_art_mean_luma,
		"max_red_pixels": _max_red_pixels,
		"elapsed_ms": elapsed_ms,
	}
	if not _qualified:
		return ProbeVerdict.failed(("no loading frame contained both art and the red bar "
				+ "(loading_frames=%d, max_upper_mean_luma=%.4f, max_red_pixels=%d, elapsed_ms=%d)"
				% [_loading_frames, _max_art_mean_luma, _max_red_pixels, elapsed_ms]), data)
	data["qualified_upper_mean_luma"] = _qualified_metrics.get("art_mean_luma", 0.0)
	data["qualified_red_pixels"] = _qualified_metrics.get("red_pixels", 0)
	ctx.log("OK loading_frames=%d qualified_upper_mean_luma=%.4f qualified_red_pixels=%d elapsed_ms=%d" % [
			_loading_frames, float(data["qualified_upper_mean_luma"]),
			int(data["qualified_red_pixels"]), elapsed_ms])
	return ProbeVerdict.passed("a loading frame carried the art and the red bar", data)


func _on_frame_post_draw() -> void:
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
	_max_art_mean_luma = maxf(_max_art_mean_luma, float(metrics["art_mean_luma"]))
	_max_red_pixels = maxi(_max_red_pixels, int(metrics["red_pixels"]))
	if float(metrics["art_mean_luma"]) > ART_MEAN_LUMA_MIN and int(metrics["red_pixels"]) > 0:
		_qualified = true
		_qualified_metrics = metrics
		var path := _ctx.artifact_dir.path_join("loading_frame.png")
		if image.save_png(path) == OK:
			_ctx.artifact("loading_frame", path, "png")


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
