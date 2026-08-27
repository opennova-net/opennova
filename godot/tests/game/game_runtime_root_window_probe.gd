extends Node

## Windowed smoke for the real imgui-godot bridge. Run with the debug extension
## and addon installed; this deliberately fails instead of silently accepting a
## direct-runtime fallback.

const RUNTIME_ROOT_SCENE := preload("res://game/game_runtime_root.tscn")
const SETTLE_FRAMES := 8

var _failures: Array[String] = []


func _ready() -> void:
	_run.call_deferred()


func _run() -> void:
	var runtime_root = RUNTIME_ROOT_SCENE.instantiate()
	add_child(runtime_root)
	await _wait_frames(SETTLE_FRAMES)
	_check(runtime_root.is_game_view_embedded(), "debug windowed startup embeds the game")
	var main_game: Node = runtime_root.get_main_game()
	_check(main_game != null, "runtime root exposes MainGame")
	if main_game == null:
		await _finish(runtime_root)
		return
	var tools: DevTools = main_game.get_dev_tools()
	_check(tools.is_available(), "the real ImGui context attached")
	var menu_shell := main_game.get_node("MenuLayer/MenuShell")
	_check(menu_shell.visible and menu_shell.get_current_menu_file().to_lower() == "main.mnu",
			"the embedded MainGame reaches its authored menu surface")
	var container := runtime_root.get_node_or_null("GameViewportContainer") \
			as SubViewportContainer
	var viewport := runtime_root.get_node_or_null("GameViewportContainer/GameViewport") \
			as SubViewport
	_check(container != null and viewport != null, "one container owns one game viewport")
	if container == null or viewport == null:
		await _finish(runtime_root)
		return
	_check(container.visible, "closed tools present the viewport directly")
	_check(viewport.render_target_update_mode == SubViewport.UPDATE_ALWAYS,
			"the shared texture keeps updating")
	_check(viewport.audio_listener_enable_2d and viewport.audio_listener_enable_3d,
			"the embedded runtime owns both audio listeners")
	var direct_image := viewport.get_texture().get_image()
	if direct_image != null and not direct_image.is_empty():
		var direct_path := OS.get_cache_dir().path_join("game_runtime_direct_probe.png")
		if direct_image.save_png(direct_path) == OK:
			print("[game-runtime-root-probe] direct=", direct_path)

	tools.set_open(true)
	await _wait_frames(SETTLE_FRAMES)
	_check(container.visible and container.visibility_layer == 0,
			"F3 workspace hides only the direct composite, not viewport updates")
	var rendered_size: Vector2i = tools.get_rendered_game_viewport_size()
	_check(rendered_size.x > 0 and rendered_size.y > 0,
			"Game content reports a responsive integer size")
	_check(viewport.size == rendered_size,
			"the ImGui adapter and shared SubViewport agree on the rendered size")
	var texture_image := viewport.get_texture().get_image()
	_check(texture_image != null and not texture_image.is_empty(),
			"the central Game window receives a rendered texture")
	if texture_image != null and not texture_image.is_empty():
		_check(_has_visible_color(texture_image),
				"resizing the hidden direct composite does not clear the hosted game frame")
		var viewport_path := OS.get_cache_dir().path_join("game_runtime_viewport_probe.png")
		if texture_image.save_png(viewport_path) == OK:
			print("[game-runtime-root-probe] viewport=", viewport_path)

	DisplayServer.window_set_size(Vector2i(1180, 720))
	await _wait_frames(SETTLE_FRAMES)
	var resized: Vector2i = tools.get_rendered_game_viewport_size()
	_check(resized.x > 0 and resized.y > 0 and resized != rendered_size,
			"resizing the workspace changes the requested game resolution")
	_check(viewport.size == resized, "the resize reaches the viewport without duplication")

	var screenshot := get_viewport().get_texture().get_image()
	if screenshot != null and not screenshot.is_empty():
		var screenshot_path := OS.get_cache_dir().path_join("game_runtime_root_probe.png")
		if screenshot.save_png(screenshot_path) == OK:
			print("[game-runtime-root-probe] screenshot=", screenshot_path)
	Input.warp_mouse(Vector2(300, 300))
	await _wait_frames(2)
	var f3 := InputEventKey.new()
	f3.keycode = KEY_F3
	f3.physical_keycode = KEY_F3
	f3.pressed = true
	Input.parse_input_event(f3)
	await _wait_frames(2)
	_check(not tools.is_open() and container.visibility_layer == 1,
			"F3 forwarded through the interactive Game texture closes the workspace")
	await _finish(runtime_root)


func _wait_frames(count: int) -> void:
	for _frame in range(count):
		await get_tree().process_frame


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)


func _has_visible_color(source: Image) -> bool:
	var image: Image = source.duplicate()
	image.convert(Image.FORMAT_RGBA8)
	var bytes: PackedByteArray = image.get_data()
	for offset in range(0, bytes.size(), 4):
		if bytes[offset] > 24 or bytes[offset + 1] > 24 or bytes[offset + 2] > 24:
			return true
	return false


func _finish(runtime_root) -> void:
	if is_instance_valid(runtime_root):
		runtime_root.queue_free()
		# queue_free is flushed at the end of the next frame; wait one more so
		# the probe does not report the hosted runtime as an exit-time leak.
		await _wait_frames(2)
	if _failures.is_empty():
		print("[game-runtime-root-probe] PASS")
		get_tree().quit(0)
		return
	for failure in _failures:
		push_error("game_runtime_root_window_probe: %s" % failure)
	get_tree().quit(1)
