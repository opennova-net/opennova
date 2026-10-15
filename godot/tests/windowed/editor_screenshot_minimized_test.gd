extends GutTest

## editor_screenshot of a minimized editor (the editor MCP): a minimized window draws no frame, so the capture once
## waited out its frame budget ("No frame drawn within 90 frames") or took a stale frame of the window's taskbar
## size. The editor brings its window back without activation, under every other window (EditorApp's
## restore_window_for_capture: never the foreground), captures a frame of its whole size, and minimizes it again.
## Windows only: elsewhere the capture is refused saying the window is minimized.

const EditorFixture := preload("res://tests/authoring/editor_fixture.gd")
const McpTestClient := preload("res://tests/mcp/mcp_test_client.gd")
## The window's taskbar-icon size, which a capture of a minimized window once came back at.
const TINY := Vector2i(224, 92)

var _dirs: Array[String] = []
var _app: Node = null
var _service: EditorMcpService = null
var _client: RefCounted = null


func before_each() -> void:
	var seam: RefCounted = await EditorFixture.boot(self, "screenshot minimized", _dirs, 3)
	if seam == null:
		return
	_app = seam.app
	_service = add_child_autofree(EditorMcpService.new())
	assert_eq(_service.setup(_app, 0), OK)
	_client = McpTestClient.new()
	assert_true(await _client.connect_to(get_tree(), _service.get_port()))
	assert_not_null(await _client.initialize(get_tree()))


func after_each() -> void:
	# The window back for the rest of the run, without activation where the platform can.
	if is_instance_valid(_app) and DisplayServer.window_get_mode() == DisplayServer.WINDOW_MODE_MINIMIZED:
		if _app.restore_window_for_capture() < 0:
			DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_WINDOWED)
		for _frame in 3:
			await get_tree().process_frame
	if _client != null:
		_client.close()
		_client = null
	_app = null
	_service = null
	McpLogHub.instance = null
	await EditorFixture.teardown(self, _dirs)


func _minimize() -> bool:
	DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_MINIMIZED)
	for _frame in 30:
		if DisplayServer.window_get_mode() == DisplayServer.WINDOW_MODE_MINIMIZED:
			return true
		await get_tree().process_frame
	return DisplayServer.window_get_mode() == DisplayServer.WINDOW_MODE_MINIMIZED


func test_a_minimized_editor_is_captured_whole_and_minimized_again() -> void:
	if _app == null:
		return
	var shown := DisplayServer.window_get_size()
	assert_true(await _minimize(), "the window minimizes")
	var envelope: Variant = await _client.call_tool(get_tree(), "editor_screenshot", {"format": "png", "max_dim": 4096})
	assert_true(envelope is Dictionary)
	var result: Dictionary = (envelope as Dictionary).get("result", {})
	var text := ""
	var image_blocks := 0
	for block in result.get("content", []):
		if String(block.get("type", "")) == "text":
			text += String(block["text"])
		elif String(block.get("type", "")) == "image":
			image_blocks += 1
	if OS.get_name() != "Windows":
		assert_true(bool(result.get("isError", false)))
		assert_true(text.contains("minimized"), "refused saying the window is minimized: %s" % text)
		return
	assert_false(bool(result.get("isError", false)), "captured: %s" % text)
	assert_eq(image_blocks, 1)
	# The caption's size is the window's whole picture, never the taskbar icon's.
	var size := text.get_slice("(", 1).get_slice(")", 0)
	var width := int(size.get_slice("x", 0))
	var height := int(size.get_slice("x", 1))
	assert_gt(width, TINY.x, "the capture is the window's whole width (%s)" % text)
	assert_gt(height, TINY.y, "the capture is the window's whole height (%s)" % text)
	assert_eq(Vector2i(width, height), shown, "the window came back at the size it was shown at")
	for _frame in 3:
		await get_tree().process_frame
	assert_eq(DisplayServer.window_get_mode(), DisplayServer.WINDOW_MODE_MINIMIZED, "the window is minimized again")
