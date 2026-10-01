extends GutTest

## The script device (ADR 0046 S13 V10): a text document's Main view, a Godot CodeEdit (ScriptEdit),
## in a headless editor. No ImGui context attaches headless, so no canvas places the control; the
## Shell's device cache gives the active document's Main view a device all the same, which follows
## its viewport each pump: a .wac opened shows in the control, its compiler's words coloured; a
## compile error is a gutter mark on its line, its message the mark's tip; a rename through the editor
## MCP reaches the control's text; a keystroke burst typed into the control is one undo step, the
## control's own history kept empty, and the undo takes the control back to the document's text; a
## Go to's reveal selects its span; a credits file its text form cannot carry shows read only. The
## control's placement over the reserved rect and the pointer and keys it owns there need the
## workspace drawn (tests/windowed/editor_script_device_test.gd).

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const McpTestClient := preload("res://tests/mcp/mcp_test_client.gd")
const SCRIPT := "scripts/text_document.wac"
const CREDITS := "menus/nlist.kda"
const FIXTURES := "res://../fixtures/"

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null
var _service: Node = null
var _client: RefCounted = null


func before_each() -> void:
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return
	_app = packed.instantiate()
	_seam = EditorSeam.new(_app)
	var settings_dir := OS.get_cache_dir().path_join("opennova editor script %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)


func after_each() -> void:
	if _client != null:
		_client.close()
		_client = null
	_service = null
	_app = null
	McpLogHub.instance = null
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()


func _fixture(relative: String) -> PackedByteArray:
	return FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(FIXTURES + relative))


func _write(path: String, bytes: PackedByteArray) -> void:
	assert_eq(DirAccess.make_dir_recursive_absolute(path.get_base_dir()), OK)
	var out := FileAccess.open(path, FileAccess.WRITE)
	out.store_buffer(bytes)
	out.close()


## A project of the fixture script and the effect and the ammo its operands name (its text key's table
## left out: that reference is missing, a mark on line 7), and the minted credits file.
func _project() -> bool:
	var dir := OS.get_cache_dir().path_join("opennova editor script project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	if not _seam.new_project(dir, "Script Device"):
		return false
	_seam.create_missing_files()
	_write(dir.path_join(SCRIPT), _fixture("wac/text_document.wac"))
	_write(dir.path_join("particles/effects.ptl"), _fixture("particle/synth_minimal_effect.ptl"))
	_write(dir.path_join("defs/ammo.def"),
			"ammo AT_CONTRACT\nmax_age 1.5\nend\nammo ammo_satchel\nmax_age 2\nend\n".to_utf8_buffer())
	_write(dir.path_join(CREDITS), _fixture("cbin/synth_nlist.kda"))
	_seam.request({"kind": "rescan"})
	return _seam.settled


## The control of the document at `path` (null while none is made).
func _edit(path: String) -> CodeEdit:
	for node: Node in _app.find_children("*", "ScriptEdit", true, false):
		if String(node.call("get_document_path")) == path:
			return node as CodeEdit
	return null


## `count` frames: the Shell pumps each (its devices follow their viewports), and the control's
## deferred text_changed reaches its device.
func _frames(count := 3) -> void:
	for i in count:
		await get_tree().process_frame


## The document's line `line` (from 1) as the session holds it.
func _line(path: String, line: int) -> String:
	var page: Dictionary = _seam.query("document", {"path": path, "offset": line - 1, "limit": 1})
	var lines: Array = page.get("lines", [])
	return String(lines[0].get("text", "")) if not lines.is_empty() else ""


## The script opened and its control made: the control.
func _open_script() -> CodeEdit:
	assert_true(_project(), "the project")
	assert_true(_seam.open_document(SCRIPT), "the script opens")
	await _frames()
	var edit := _edit(SCRIPT)
	assert_not_null(edit, "the active script's Main view has a device: its control")
	return edit


func test_script_shows_in_its_control() -> void:
	var edit := await _open_script()
	if edit == null:
		return
	var text := _fixture("wac/text_document.wac").get_string_from_utf8()
	assert_eq(edit.text, text.replace("\r\n", "\n"), "the control shows the document's text, a line end an LF")
	assert_eq(edit.get_line_count(), 9)
	assert_true(edit.editable, "a script takes edits")
	assert_false(edit.has_undo(), "the control's own history is empty")
	# The WAC compiler's words, coloured: If a keyword, fxrain a command, FX_Buildup an operand; 14 in all.
	var highlighter: SyntaxHighlighter = edit.syntax_highlighter
	assert_not_null(highlighter)
	assert_true(highlighter != null and highlighter.is_class("ScriptHighlighter"), "the script's colours are the device's")
	if highlighter != null and highlighter.is_class("ScriptHighlighter"):
		assert_eq(int(highlighter.call("get_highlight_count")), 14)
		assert_eq(String(highlighter.call("get_highlight_kind", 1, 0)), "keyword")
		assert_eq(String(highlighter.call("get_highlight_kind", 2, 1)), "command")
		assert_eq(String(highlighter.call("get_highlight_kind", 2, 9)), "operand")
		assert_eq(String(highlighter.call("get_highlight_kind", 0, 3)), "", "a comment is no word of the compiler's")
	# The text key no table defines: its missing reference a mark on line 7.
	assert_eq(int(edit.call("get_mark_count")), 1)
	assert_ne(String(edit.call("get_mark_severity", 6)), "")
	assert_true(String(edit.call("get_mark_tip", 6)).contains("MISSION_START"), String(edit.call("get_mark_tip", 6)))


func test_compile_error_is_a_gutter_mark() -> void:
	var edit := await _open_script()
	if edit == null:
		return
	# " )" after FX_Buildup, through the wire as a span: the control takes the document's text, and the
	# compiler's report is a warning on line 3, its message the mark's tip.
	assert_true(_seam.done({"kind": "edit_record", "path": SCRIPT,
			"edits": [{"op": "apply", "payload": "text.span", "line": 3, "column": 19, "text": " )"}]}))
	await _frames()
	assert_eq(edit.get_line(2), "\tfxrain FX_Buildup )")
	assert_eq(String(edit.call("get_mark_severity", 2)), "warning")
	assert_true(String(edit.call("get_mark_tip", 2)).contains("Unexpected )"), String(edit.call("get_mark_tip", 2)))
	assert_eq(int(edit.call("get_mark_count")), 2)
	assert_false(edit.has_undo())


func test_rename_through_the_mcp_reaches_the_text() -> void:
	var edit := await _open_script()
	if edit == null:
		return
	_service = add_child_autofree(EditorMcpService.new())
	assert_eq(_service.call("setup", _app, 0), OK)
	_client = McpTestClient.new()
	assert_true(await _client.connect_to(get_tree(), int(_service.call("get_port"))))
	assert_not_null(await _client.initialize(get_tree()))
	var symbols: Dictionary = await _tool("editor_query", {"query": "symbols", "kind": "ammo"})
	var contract := {}
	for symbol: Variant in symbols.get("symbols", []):
		if symbol is Dictionary and String(symbol.get("name", "")) == "AT_CONTRACT":
			contract = symbol
	assert_false(contract.is_empty(), str(symbols))
	if contract.is_empty():
		return
	var renamed: Dictionary = await _tool("editor_request", {"kind": "rename_symbol", "path": String(contract.get("file", "")),
			"locator": String(contract.get("locator", "")), "field": String(contract.get("field", "")),
			"new_name": "AT_RENAMED", "wait": true})
	assert_true(bool(renamed.get("outcome", {}).get("done", false)), str(renamed))
	await _frames()
	edit = _edit(SCRIPT)
	assert_not_null(edit)
	if edit != null:
		assert_eq(edit.get_line(4), "\tammoarea AMMO_AT_RENAMED 8", "the rename's span reached the control")


func test_keystroke_burst_is_one_undo_step() -> void:
	var edit := await _open_script()
	if edit == null:
		return
	var first := _line(SCRIPT, 1)
	edit.set_caret_line(0)
	edit.set_caret_column(0)
	for typed in ["a", "b", "c"]:
		edit.insert_text_at_caret(typed)
		await _frames(1)
	assert_eq(_line(SCRIPT, 1), "abc" + first, "each keystroke a span edit of the document")
	assert_false(edit.has_undo(), "the control's own history stays empty: undo is the editor's")
	assert_true(_seam.query("document", {"path": SCRIPT, "limit": 1}).get("dirty", false))
	# One undo takes the whole burst back, and the control with it.
	_seam.undo()
	assert_eq(_line(SCRIPT, 1), first, "the burst one undo step")
	await _frames()
	assert_eq(edit.get_line(0), first, "the undo takes the control back to the document's text")
	assert_false(_seam.is_document_dirty())


func test_reveal_selects_the_span() -> void:
	var edit := await _open_script()
	if edit == null:
		return
	# A Go to's place: the reference AT_CONTRACT starts at 5:16, selected whole.
	_seam.request({"kind": "open_document", "path": SCRIPT, "locator": "5:16"})
	await _frames()
	assert_eq(edit.get_selected_text(), "AT_CONTRACT")
	assert_eq(edit.get_caret_line(), 4)
	assert_eq(edit.get_caret_column(), 26)
	# A keyword's place: If.
	_seam.request({"kind": "open_document", "path": SCRIPT, "locator": "2:1"})
	await _frames()
	assert_eq(edit.get_selected_text(), "If")


func test_credits_held_read_only() -> void:
	assert_true(_project(), "the project")
	assert_true(_seam.open_document(CREDITS), "the credits open")
	await _frames()
	var edit := _edit(CREDITS)
	assert_not_null(edit, "the credits' control")
	if edit == null:
		return
	assert_false(edit.editable, "a file its text form cannot carry shows read only")
	assert_true(edit.text.length() > 0)


## One tools/call through the editor MCP: the structuredContent, or {"_error": text}.
func _tool(name: String, args: Dictionary) -> Dictionary:
	var envelope: Variant = await _client.call_tool(get_tree(), name, args)
	if not (envelope is Dictionary):
		return {"_error": "no answer"}
	var result: Dictionary = envelope.get("result", {})
	if bool(result.get("isError", false)):
		return {"_error": str(result.get("content", []))}
	return result.get("structuredContent", {})
