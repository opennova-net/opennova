extends GutTest

## The font viewport's device headless through the editor's wire seam (ADR 0046 S23 A): a font opened draws its
## text through its device (authoring/font_viewport_applier: the pages uploaded, the glyph quads drawn through the
## font page's material), and keeps drawing it after an edit that moves no texel (the spacing, a glyph's rect: an
## Update over a new picture of the same pages) and after its undo, the viewport's body saying what the device drew.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null


func before_each() -> void:
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return
	_app = packed.instantiate()
	_seam = EditorSeam.new(_app)
	var settings_dir := OS.get_cache_dir().path_join("opennova editor font %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)


func after_each() -> void:
	_app = null
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()
	await get_tree().process_frame


func _viewport(path: String) -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": path, "kind": "font"})


## The glyph quads the device drew last, as the viewport's body reports them (-1 for no report yet).
func _quads(state: Dictionary) -> int:
	var drawn: Variant = state.get("body", {}).get("drawn", null)
	return int((drawn as Dictionary).get("quads", -1)) if drawn is Dictionary else -1


## The viewport over `path` after its device drew it: a frame at a time until the device reports quads drawn of the
## layout it holds now (the viewport's revision shown), at most `frames` frames.
func _await_drawn(path: String, frames := 600) -> Dictionary:
	var state := _viewport(path)
	for _frame in frames:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and _quads(state) > 0:
			break
		_app.pump()
		await get_tree().process_frame
		state = _viewport(path)
	return state


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
	var state := await _await_drawn(path)
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
