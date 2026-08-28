extends GameProbe

## window_fullscreen: F11 on the live process. Presses F11 through the input
## stack (windowed -> fullscreen -> windowed, the shell's own handler) and
## proves each state still presents a frame: the root window's texture carries
## visible colour, and in the embedded game view (GameRuntimeRoot) the game
## SubViewport follows the window size and its own texture stays lit. A black
## frame in any state is the failure this pins (the ImGui multi-viewport flag
## beside a fullscreen size, 2026-08-28). `mode` picks the middle state for
## triage: the F11 path (`fullscreen`), `exclusive` (MODE_EXCLUSIVE_FULLSCREEN
## set directly) or `resize` (windowed at the screen's size, separating a size
## effect from a mode change).

const SETTLE_FRAMES := 16

var _failures: Array[String] = []
var _data: Dictionary = {}


func run(ctx: ProbeContext) -> ProbeVerdict:
	var live_viewport := ctx.viewport()
	var window: Window = live_viewport.get_window() if live_viewport != null else null
	if window == null:
		return ProbeVerdict.failed("the shell has no window")
	var previous_mode := window.mode
	var previous_size := window.size
	ctx.defer_restore(func() -> void:
		if is_instance_valid(window):
			window.mode = previous_mode
			window.size = previous_size)
	var runtime_root := ctx.tree.current_scene as GameRuntimeRoot
	var embedded := runtime_root != null and runtime_root.is_game_view_embedded()
	_data["embedded"] = embedded

	WindowState.set_fullscreen(window, false)
	await ctx.wait_frames(SETTLE_FRAMES)
	await _observe(ctx, window, runtime_root, "windowed_before")

	var mode := String(ctx.args.get("mode", "fullscreen"))
	_data["mode"] = mode
	match mode:
		"exclusive":
			window.mode = Window.MODE_EXCLUSIVE_FULLSCREEN
		"resize":
			window.size = DisplayServer.screen_get_size(window.current_screen)
		_:
			_press_f11()
	await ctx.wait_frames(SETTLE_FRAMES)
	if mode != "resize":
		_check(WindowState.is_fullscreen(window), "F11 enters fullscreen")
	await _observe(ctx, window, runtime_root, "fullscreen")

	if mode == "fullscreen":
		_press_f11()
	else:
		WindowState.set_fullscreen(window, false)
	await ctx.wait_frames(SETTLE_FRAMES)
	_check(not WindowState.is_fullscreen(window), "F11 leaves fullscreen")
	await _observe(ctx, window, runtime_root, "windowed_after")
	return _verdict()


## One state's evidence: the root frame, and in the embedded view the game
## viewport's size agreement and frame. Captures both as artifacts.
func _observe(ctx: ProbeContext, window: Window, runtime_root: GameRuntimeRoot,
		label: String) -> void:
	await RenderingServer.frame_post_draw
	var state := {
		"mode": window.mode,
		"window_size": [window.size.x, window.size.y],
		"root_visible": false,
	}
	var root_image: Image = _image_of(window)
	state["root_visible"] = root_image != null and _has_visible_color(root_image)
	_check(bool(state["root_visible"]), "%s: the root window presents a lit frame" % label)
	_save(ctx, root_image, label + "_root")
	if runtime_root != null and runtime_root.is_game_view_embedded():
		var game_viewport := runtime_root.get_node_or_null(
				"GameViewportContainer/GameViewport") as SubViewport
		var container := runtime_root.get_node_or_null(
				"GameViewportContainer") as SubViewportContainer
		if game_viewport != null and container != null:
			state["game_viewport_size"] = [game_viewport.size.x, game_viewport.size.y]
			state["container_size"] = [container.size.x, container.size.y]
			_check(game_viewport.size == window.size,
					"%s: the game viewport follows the window size" % label)
			var game_image: Image = _image_of(game_viewport)
			state["game_visible"] = game_image != null and _has_visible_color(game_image)
			_check(bool(state["game_visible"]),
					"%s: the embedded game viewport presents a lit frame" % label)
			_save(ctx, game_image, label + "_game")
	_data[label] = state
	ctx.log("%s: %s" % [label, JSON.stringify(state)])


## The real key path: the press and its release through the input stack.
static func _press_f11() -> void:
	for pressed in [true, false]:
		var key := InputEventKey.new()
		key.keycode = WindowState.TOGGLE_KEY
		key.physical_keycode = WindowState.TOGGLE_KEY
		key.pressed = pressed
		Input.parse_input_event(key)


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)


func _verdict() -> ProbeVerdict:
	_data["failures"] = _failures.duplicate()
	if _failures.is_empty():
		return ProbeVerdict.passed("fullscreen and windowed both present a frame", _data)
	return ProbeVerdict.failed("%d fullscreen check(s) failed" % _failures.size(), _data)


static func _image_of(viewport: Viewport) -> Image:
	var texture := viewport.get_texture()
	var image: Image = texture.get_image() if texture != null else null
	return null if image == null or image.is_empty() else image


func _save(ctx: ProbeContext, image: Image, label: String) -> void:
	if image == null:
		return
	var path := ctx.artifact_dir.path_join("%s.png" % label)
	if image.save_png(path) == OK:
		ctx.artifact(label, path, "png")


static func _has_visible_color(source: Image) -> bool:
	var image: Image = source.duplicate()
	image.convert(Image.FORMAT_RGBA8)
	var bytes: PackedByteArray = image.get_data()
	for offset in range(0, bytes.size(), 4):
		if bytes[offset] > 24 or bytes[offset + 1] > 24 or bytes[offset + 2] > 24:
			return true
	return false
