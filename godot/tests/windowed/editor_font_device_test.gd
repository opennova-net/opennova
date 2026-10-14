extends GutTest

## The font viewport's device in a window (round S23 lane A's font), the editor's canvas drawing it each frame: a
## font opened shows its text on the rendered picture at once, and still after the picture's Glyphs node was resized,
## hidden, shown and redrawn. Godot clears a node's own canvas item whenever it redraws the node (as it enters the
## tree, shows or is resized), so the device draws its glyph runs on an item of its own under the node (the
## maintainer's report: a font opened showed an empty grey picture until its text was edited).

const EditorFixture := preload("res://tests/authoring/editor_fixture.gd")

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null


func before_each() -> void:
	_seam = await EditorFixture.boot(self, "font device", _dirs, 3)
	_app = _seam.app if _seam != null else null


func after_each() -> void:
	await EditorFixture.teardown(self, _dirs)


## The glyph quads the device drew last, as the viewport's body reports them (-1 for no report yet).
func _quads(state: Dictionary) -> int:
	var drawn: Variant = state.get("body", {}).get("drawn", null)
	return int((drawn as Dictionary).get("quads", -1)) if drawn is Dictionary else -1


## The pixels of the device's last rendered picture well above its background's brightness (the text drawn white),
## a few frames on; -1 for no picture.
func _text_pixels(path: String) -> int:
	for _frame in 6:
		await get_tree().process_frame
	var device: SubViewport = _app.get_viewport_device(path, "font")
	if device == null:
		return -1
	var image := device.get_texture().get_image()
	if image == null or image.is_empty():
		return -1
	var background := image.get_pixel(1, 1)
	var floor_sum := background.r + background.g + background.b + 0.3
	var count := 0
	for y in image.get_height():
		for x in image.get_width():
			var pixel := image.get_pixel(x, y)
			if pixel.r + pixel.g + pixel.b > floor_sum:
				count += 1
	return count


func test_the_text_shows_at_once_and_after_its_node_redrew() -> void:
	if _app == null:
		return
	assert_true(_app.is_available(), "a window: the editor's ImGui pass attached, its canvases drawing")
	var dir := OS.get_cache_dir().path_join("opennova editor font device %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Font Device"))
	assert_true(_seam.create_file("Extra.fnt"), "\n".join(_seam.get_output_lines()))
	var path := "fonts/Extra.fnt"
	var drawn := func(shown: Dictionary) -> bool: return _quads(shown) > 0
	var state: Dictionary = await _seam.await_viewport(
		{"path": path, "kind": "font", "limit": 1}, 0, drawn, 900, false
	)
	assert_gt(_quads(state), 0, "the text's glyph quads drawn: %s" % str(state.get("body", {})))
	var first := await _text_pixels(path)
	assert_gt(first, 50, "the text on the picture at once, no edit made")
	var device: SubViewport = _app.get_viewport_device(path, "font")
	var glyphs := device.get_node_or_null("Glyphs") as Control if device != null else null
	assert_not_null(glyphs, "the device's Glyphs node")
	if glyphs == null:
		return
	glyphs.size = glyphs.size + Vector2(17.0, 9.0)
	glyphs.hide()
	await get_tree().process_frame
	glyphs.show()
	glyphs.queue_redraw()
	var after := await _text_pixels(path)
	assert_gt(after, 50, "the text still on the picture after its node redrew (%d pixels before)" % first)
