extends GutTest

## The font viewport's device headless through the editor's wire seam (ADR 0046 S23 A): a font opened draws its
## text through its device (authoring/font_viewport_applier: the pages uploaded, the glyph quads drawn through the
## font page's material), and keeps drawing it after an edit that moves no texel (the spacing, a glyph's rect: an
## Update over a new picture of the same pages) and after its undo, the viewport's body saying what the device drew.
## Its glyph runs are drawn on an item of the device's own under the Glyphs node, so a redraw of the node (Godot's as
## it enters the tree, shows or is resized) leaves the first draw standing; a page shown, then the text again.

const EditorFixture := preload("res://tests/authoring/editor_fixture.gd")

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null


func before_each() -> void:
	_seam = await EditorFixture.boot(self, "font", _dirs)
	_app = _seam.app if _seam != null else null


func after_each() -> void:
	await EditorFixture.teardown(self, _dirs)


func _viewport(path: String) -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": path, "kind": "font"})


## The glyph quads the device drew last, as the viewport's body reports them (-1 for no report yet).
func _quads(state: Dictionary) -> int:
	var drawn: Variant = state.get("body", {}).get("drawn", null)
	return int((drawn as Dictionary).get("quads", -1)) if drawn is Dictionary else -1


## A few frames pumped, then the viewport as it stands.
func _settle(path: String, frames := 8) -> Dictionary:
	for _frame in frames:
		_app.pump()
		await get_tree().process_frame
	return _viewport(path)


func test_the_font_stays_drawn_through_an_edit_and_its_undo() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor font project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Font Viewport"))
	# The blank font (the engine's stroke font), made and opened: its text drawn through the device.
	assert_true(_seam.create_file("Extra.fnt"), "\n".join(_seam.get_output_lines()))
	var path := "fonts/Extra.fnt"
	# Until the device reports quads drawn of the layout it holds now (the viewport's revision shown).
	var drawn := func(shown: Dictionary) -> bool: return _quads(shown) > 0
	var state: Dictionary = await _seam.await_viewport(
		{"path": path, "kind": "font"}, 0, drawn, 600
	)
	assert_eq(String(state.get("status", "")), "ready", "the font shows")
	assert_true(bool(state.get("device", {}).get("attached", false)), "on its device")
	var first := _quads(state)
	assert_gt(first, 0, "its text's glyph quads drawn: %s" % str(state.get("body", {})))
	var builds := int(state.get("builds", 0))
	# The spacing edited: no texel moves (an Update, no Rebuild), and the text is still drawn.
	var font_row: int = _seam.get_row_id(0)
	assert_gt(font_row, 0)
	assert_true(_seam.set_field(font_row, "spacing", 3), "the spacing set")
	state = await _settle(path)
	assert_eq(int(state.get("builds", 0)), builds, "no Rebuild: the pages are the same")
	assert_eq(_quads(state), first, "the text still drawn after the edit")
	# Undone: still drawn.
	_seam.undo()
	state = await _settle(path)
	assert_eq(_quads(state), first, "the text still drawn after the undo")
	# A glyph's rect edited (the A one texel wider), and the options moved: still drawn.
	assert_true(_seam.done({"kind": "set_viewport", "path": path,
			"viewport": {"kind": "font", "options": {"text": "AAAA"}}}))
	state = await _settle(path)
	assert_eq(_quads(state), 4, "the new text's four glyphs drawn")
	var glyph_a := 0
	for id in _seam.get_child_records(font_row, "glyph"):
		if _seam.get_record_name(id).ends_with(" 65"):
			glyph_a = id
	assert_gt(glyph_a, 0, "the A's glyph record")
	var width := int(_seam.get_field(glyph_a, "width"))
	assert_true(_seam.set_field(glyph_a, "width", width + 1))
	state = await _settle(path)
	assert_eq(_quads(state), 4, "still drawn after a glyph's rect moved")


## The item the device drew its glyph runs on, as the viewport's body reports it ("" for no report yet).
func _item(state: Dictionary) -> String:
	var drawn: Variant = state.get("body", {}).get("drawn", null)
	return String((drawn as Dictionary).get("item", "")) if drawn is Dictionary else ""


func test_the_glyphs_survive_a_redraw_of_their_node() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor font redraw %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Font Redraw"))
	assert_true(_seam.create_file("Extra.fnt"), "\n".join(_seam.get_output_lines()))
	var path := "fonts/Extra.fnt"
	var drawn := func(shown: Dictionary) -> bool: return _quads(shown) > 0
	var state: Dictionary = await _seam.await_viewport({"path": path, "kind": "font"}, 0, drawn, 600)
	var first := _quads(state)
	assert_gt(first, 0, "its text's glyph quads drawn: %s" % str(state.get("body", {})))
	var device: SubViewport = _app.get_viewport_device(path, "font")
	assert_not_null(device, "the font's device")
	if device == null:
		return
	var glyphs := device.get_node_or_null("Glyphs") as Control
	assert_not_null(glyphs, "the device's Glyphs node")
	if glyphs == null:
		return
	# Godot clears a node's own canvas item whenever it redraws the node (entering the tree, shown, resized): the
	# glyph runs are drawn on an item of the device's own under it, which no redraw of the node clears, so the first
	# draw stands until the layout moves (the maintainer's report: an empty picture until the text was edited).
	var item := _item(state)
	assert_ne(item, "", "the device reports the item it draws on")
	assert_ne(item, str(glyphs.get_canvas_item().get_id()), "never the Glyphs node's own item")
	glyphs.size = glyphs.size + Vector2(17.0, 9.0)
	glyphs.hide()
	await get_tree().process_frame
	glyphs.show()
	glyphs.queue_redraw()
	state = await _settle(path)
	assert_eq(_item(state), item, "the same item after the node redrew")
	assert_eq(_quads(state), first, "the text drawn once, and standing: no layout moved")


func test_a_page_shown() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor font page %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Font Page"))
	assert_true(_seam.create_file("Extra.fnt"), "\n".join(_seam.get_output_lines()))
	var path := "fonts/Extra.fnt"
	var drawn := func(shown: Dictionary) -> bool: return _quads(shown) > 0
	await _seam.await_viewport({"path": path, "kind": "font"}, 0, drawn, 600)
	# Page 0 shown (the option an agent set when an editor went down): the page drawn, then the text again.
	assert_true(_seam.done({"kind": "set_viewport", "path": path, "viewport": {"kind": "font", "options": {"page": 0}}}))
	var state: Dictionary = await _settle(path)
	assert_eq(String(state.get("status", "")), "ready", "the page shows")
	assert_eq(int(state.get("body", {}).get("page_shown", -2)), 0, "page 0 shown")
	assert_true(_seam.done({"kind": "set_viewport", "path": path, "viewport": {"kind": "font", "options": {"page": -1}}}))
	state = await _settle(path)
	assert_gt(_quads(state), 0, "the text drawn again")
