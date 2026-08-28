extends GameProbe

## runtime_root_window: the embedded game view (GameRuntimeRoot, ADR 0039's
## debug windowed startup) on the live process: the game runs inside one
## always-updating SubViewport under the runtime root, the real ImGui context
## attached, opening the tools hides only the direct composite while the
## shared texture keeps rendering at the size the Game window requests, a
## window resize reaches the viewport, and F3 forwarded through the Game
## texture closes the workspace. Needs a debug build with the imgui-godot
## addon and a window; refuses a direct-runtime fallback instead of accepting it.

const SETTLE_FRAMES := 8
const RESIZED_WINDOW := Vector2i(1180, 720)

var _failures: Array[String] = []


func run(ctx: ProbeContext) -> ProbeVerdict:
	var runtime_root := ctx.tree.current_scene as GameRuntimeRoot
	if runtime_root == null or not runtime_root.is_game_view_embedded():
		return ProbeVerdict.failed(
				"the game view is not embedded (release build, headless, or no imgui-godot addon)",
				{"current_scene": String(ctx.tree.current_scene.name) if ctx.tree.current_scene != null else ""})
	var main_game: MainGame = runtime_root.get_main_game()
	_check(main_game != null, "runtime root exposes MainGame")
	if main_game == null:
		return _verdict()
	var tools: DevTools = main_game.get_dev_tools()
	_check(tools.is_available(), "the real ImGui context attached")
	var container := runtime_root.get_node_or_null("GameViewportContainer") as SubViewportContainer
	var viewport := runtime_root.get_node_or_null("GameViewportContainer/GameViewport") as SubViewport
	_check(container != null and viewport != null, "one container owns one game viewport")
	if container == null or viewport == null:
		return _verdict()
	var tools_were_open := tools.is_open()
	ctx.defer_restore(func() -> void: tools.set_open(tools_were_open))
	tools.set_open(false)
	await ctx.wait_frames(SETTLE_FRAMES)
	_check(container.visible and container.visibility_layer == 1,
			"closed tools present the viewport directly")
	_check(viewport.render_target_update_mode == SubViewport.UPDATE_ALWAYS,
			"the shared texture keeps updating")
	_check(viewport.audio_listener_enable_2d and viewport.audio_listener_enable_3d,
			"the embedded runtime owns both audio listeners")
	var direct := viewport.get_texture().get_image()
	if direct != null and not direct.is_empty():
		var direct_path := ctx.artifact_dir.path_join("direct.png")
		if direct.save_png(direct_path) == OK:
			ctx.artifact("direct", direct_path, "png")

	tools.set_open(true)
	await ctx.wait_frames(SETTLE_FRAMES)
	_check(container.visible and container.visibility_layer == 0,
			"the tools workspace hides only the direct composite, not viewport updates")
	var rendered_size: Vector2i = tools.get_rendered_game_viewport_size()
	_check(rendered_size.x > 0 and rendered_size.y > 0,
			"Game content reports a responsive integer size")
	_check(viewport.size == rendered_size,
			"the ImGui adapter and the shared SubViewport agree on the rendered size")
	var texture := viewport.get_texture().get_image()
	_check(texture != null and not texture.is_empty(),
			"the central Game window receives a rendered texture")
	if texture != null and not texture.is_empty():
		_check(_has_visible_color(texture),
				"resizing the hidden direct composite does not clear the embedded game frame")
		var viewport_path := ctx.artifact_dir.path_join("viewport.png")
		if texture.save_png(viewport_path) == OK:
			ctx.artifact("viewport", viewport_path, "png")

	if ctx.set_window_size(RESIZED_WINDOW):
		await ctx.wait_frames(SETTLE_FRAMES)
		var resized: Vector2i = tools.get_rendered_game_viewport_size()
		_check(resized.x > 0 and resized.y > 0 and resized != rendered_size,
				"resizing the workspace changes the requested game resolution")
		_check(viewport.size == resized, "the resize reaches the viewport without duplication")
	else:
		_check(false, "the window could be resized")
	ctx.capture_png("workspace")

	Input.warp_mouse(Vector2(300, 300))
	await ctx.wait_frames(2)
	var f3 := InputEventKey.new()
	f3.keycode = KEY_F3
	f3.physical_keycode = KEY_F3
	f3.pressed = true
	Input.parse_input_event(f3)
	await ctx.wait_frames(2)
	_check(not tools.is_open() and container.visibility_layer == 1,
			"F3 forwarded through the interactive Game texture closes the workspace")
	return _verdict()


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)


func _verdict() -> ProbeVerdict:
	var data := {"failures": _failures.duplicate()}
	if _failures.is_empty():
		return ProbeVerdict.passed("the embedded game view behaves", data)
	return ProbeVerdict.failed("%d embedded-view check(s) failed" % _failures.size(), data)


static func _has_visible_color(source: Image) -> bool:
	var image: Image = source.duplicate()
	image.convert(Image.FORMAT_RGBA8)
	var bytes: PackedByteArray = image.get_data()
	for offset in range(0, bytes.size(), 4):
		if bytes[offset] > 24 or bytes[offset + 1] > 24 or bytes[offset + 2] > 24:
			return true
	return false
