extends GutTest

## The script device in the drawn workspace (ADR 0046 S13 V10): with the ImGui pass attached (a
## windowed run; a headless one never attaches it, tests/authoring/editor_script_test.gd covers the
## device there), the script's Document tab places its CodeEdit over the rect the script view
## reserves, on a layer over the pass's, and the control owns the keys there: a key typed reaches the
## control through the input pipeline (the pass's layer first, which wants none of it) and is a span
## edit of the document; Ctrl+Z is the editor's undo (its shortcut), never the control's, and takes
## the control back to the document's text. The pointer's pass-through is not driven here: the
## imgui-godot layer reads the OS cursor, which a test does not move. A tests/windowed/ script, run
## by `scripts/test_godot.sh --suite core --windowed`.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const SCRIPT := "scripts/text_document.wac"

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null


func after_each() -> void:
	_app = null
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()


## The editor booted in the test's tree. The ImGui context's frames come first: Dear ImGui refuses
## docking and viewports switched on between its first frame and its second (the editor's own launch
## switches them on before the first), and a script run first in the process would boot there.
func _boot() -> bool:
	await _frames(3)
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return false
	_app = packed.instantiate()
	_seam = EditorSeam.new(_app)
	var settings_dir := OS.get_cache_dir().path_join("opennova editor script device %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)
	await _frames(2)
	return true


func _frames(count := 3) -> void:
	for i in count:
		await get_tree().process_frame


func _edit(path: String) -> CodeEdit:
	for node: Node in _app.find_children("*", "ScriptEdit", true, false):
		if String(node.call("get_document_path")) == path:
			return node as CodeEdit
	return null


func _line(path: String, line: int) -> String:
	var lines: Array = _seam.query("document", {"path": path, "offset": line - 1, "limit": 1}).get("lines", [])
	return String(lines[0].get("text", "")) if not lines.is_empty() else ""


func _key(keycode: Key, pressed: bool, unicode := 0, ctrl := false) -> void:
	var event := InputEventKey.new()
	event.keycode = keycode
	event.physical_keycode = keycode
	event.unicode = unicode
	event.pressed = pressed
	event.ctrl_pressed = ctrl
	Input.parse_input_event(event)


func test_device_placed_and_owns_the_keys() -> void:
	if not await _boot():
		return
	if not bool(_app.call("is_available")):
		pending("the script device's placement needs the ImGui pass attached (a windowed run with imgui-godot)")
		return
	var dir := OS.get_cache_dir().path_join("opennova editor script device project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Script Device"))
	_seam.create_missing_files()
	assert_eq(DirAccess.make_dir_recursive_absolute(dir.path_join("scripts")), OK)
	var out := FileAccess.open(dir.path_join(SCRIPT), FileAccess.WRITE)
	out.store_buffer(FileAccess.get_file_as_bytes(ProjectSettings.globalize_path("res://../fixtures/wac/text_document.wac")))
	out.close()
	_seam.request({"kind": "rescan"})
	assert_true(_seam.open_document(SCRIPT))
	# The tab drawn: its view asks for the device, the next pump makes it, the frame after places it.
	await _frames(12)
	var edit := _edit(SCRIPT)
	assert_not_null(edit, "the script's device")
	if edit == null:
		return
	assert_true(edit.is_visible_in_tree(), "placed on a frame its view draws it")
	var rect := edit.get_global_rect()
	assert_true(rect.size.x > 100.0 and rect.size.y > 100.0, str(rect))
	assert_true(get_viewport().get_visible_rect().encloses(rect), "within the window: %s" % str(rect))
	var layer := edit.get_parent() as CanvasLayer
	assert_true(layer != null and layer.layer > 128, "drawn over the ImGui pass's layer")
	# A key typed reaches the control (the pass's layer wants none of it) and is a span edit.
	var first := _line(SCRIPT, 1)
	edit.grab_focus()
	edit.set_caret_line(0)
	edit.set_caret_column(0)
	await _frames(2)
	_key(KEY_Q, true, "Q".unicode_at(0))
	_key(KEY_Q, false)
	await _frames(4)
	assert_eq(_line(SCRIPT, 1), "Q" + first, "the key typed in the control edits the document")
	assert_eq(edit.get_line(0), "Q" + first)
	# Ctrl+Z: the editor's undo through its shortcut, the control's own never; the control taken back.
	_key(KEY_CTRL, true)
	_key(KEY_Z, true, 0, true)
	await _frames(2)
	_key(KEY_Z, false, 0, true)
	_key(KEY_CTRL, false)
	await _frames(4)
	assert_eq(_line(SCRIPT, 1), first, "Ctrl+Z undid the document's step once")
	assert_eq(edit.get_line(0), first, "the control holds the document's text again")
	assert_false(edit.has_undo(), "the control's own history stays empty")
