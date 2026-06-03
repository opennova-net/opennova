class_name MusicScriptMode
extends Control

# Script-mode panel: hosts a CodeEdit fed by mus_decompile output and
# routes edits + compile requests back through the document. The Compile
# button drives `document.compile_script()`; the result bar surfaces
# compile_finished. Phase F4 lands the C++ bridge for actual compilation,
# so until then Compile silently succeeds with empty bytecode.

signal compile_and_run_requested

var _document: RefCounted

@onready var _code_edit: CodeEdit = %CodeEdit
@onready var _compile_bar: Label = %CompileBar
@onready var _compile_button: Button = %CompileButton
@onready var _compile_run_button: Button = %CompileRunButton
@onready var _insert_play_template_button: Button = %InsertPlayTemplateButton
@onready var _error_panel: PanelContainer = %ErrorPanel
@onready var _error_list: ItemList = %ErrorList


func bind_document(document: RefCounted) -> void:
	if _document == document:
		return
	if _document != null and _document.has_signal("changed"):
		if _document.changed.is_connected(_refresh_text):
			_document.changed.disconnect(_refresh_text)
	if _document != null and _document.has_signal("compile_finished"):
		if _document.compile_finished.is_connected(_on_compile_finished):
			_document.compile_finished.disconnect(_on_compile_finished)
	_document = document
	if _document != null and _document.has_signal("changed"):
		_document.changed.connect(_refresh_text)
	if _document != null and _document.has_signal("compile_finished"):
		_document.compile_finished.connect(_on_compile_finished)
	if is_node_ready():
		_refresh_text()


func _ready() -> void:
	_code_edit.gutters_draw_line_numbers = true
	_code_edit.highlight_current_line = true
	_code_edit.auto_brace_completion_enabled = true
	_code_edit.syntax_highlighter = _build_highlighter()
	_code_edit.text_changed.connect(_on_text_changed)
	if _compile_button != null:
		_compile_button.pressed.connect(_on_compile_pressed)
	if _compile_run_button != null:
		_compile_run_button.pressed.connect(_on_compile_run_pressed)
	if _insert_play_template_button != null:
		_insert_play_template_button.pressed.connect(_on_insert_play_template_pressed)
	if _error_list != null:
		_error_list.item_activated.connect(_on_error_clicked)
	if _error_panel != null:
		_error_panel.visible = false
	_refresh_text()


# Mirrors the on-godot-oscarmike inspector plugin's palette so the in-editor
# look matches the standalone .tres editor: top-level forms in blue, control
# flow in purple, opcode/play mnemonics in green, intrinsic-method names in
# yellow. Comments + strings are colour-region driven.
func _build_highlighter() -> CodeHighlighter:
	var h := CodeHighlighter.new()
	h.add_color_region("//", "", Color(0.5, 0.5, 0.5), true)
	h.add_color_region("\"", "\"", Color(0.9, 0.6, 0.3))
	for kw in ["script", "bind", "section", "declsection", "global", "INT"]:
		h.add_keyword_color(kw, Color(0.4, 0.7, 1.0))
	# Control flow + statement keywords as actually emitted by libs/mus
	# decompiler. Includes "on" (tablexec table head), "done" (section
	# terminator), and "yield" (yield opcode).
	for kw in ["if", "else", "enter", "return", "done", "yield", "goto", "on"]:
		h.add_keyword_color(kw, Color(0.8, 0.5, 1.0))
	# Opcode mnemonics. The previous list mixed real mnemonics (push, pop_g)
	# with x86-flavoured stand-ins (mul, jmp, jmpif, jmpifnot, ret) that
	# the decompiler never emits, so the syntax highlighter only painted
	# half the tokens. List below matches kOps in libs/mus/src/mus_decompile.cpp.
	for op in [
		"nop", "push", "pop_g", "pop_l", "push_g", "push_l",
		"push_ga", "push_la", "pop_ga", "pop_la", "push_me", "pushstr",
		"add", "sub", "mult", "div", "mod",
		"l_and", "l_or", "and", "or", "xor", "neg", "not", "inverse",
		"lshift", "rshift",
		"equal", "notequal", "ge", "le", "gt", "lt",
		"inc_g", "dec_g", "inc_l", "dec_l",
		"brfalse", "brtrue",
		"callv", "callvl", "setstate",
		"play", "playw", "tablexec", "call",
		"empty", "method",
	]:
		h.add_keyword_color(op, Color(0.7, 0.9, 0.7))
	for fn in ["GEcho", "GGRnd", "GSV", "GSDV", "GFB",
		"FSet", "FClear", "FIsSet", "FIsClear", "TStart", "TStop"]:
		h.add_keyword_color(fn, Color(1.0, 0.9, 0.4))
	h.number_color = Color(0.4, 0.9, 0.9)
	h.symbol_color = Color(0.8, 0.8, 0.8)
	return h


func _refresh_text() -> void:
	if _code_edit == null:
		return
	if _document == null or not _document.script_loaded():
		_code_edit.text = ""
		return
	var script_name: StringName = StringName(_document.mus_script.get_default_script_name())
	# Prefer the names-aware decompile when the bank is loaded so play / bind
	# statements show real entry names. Falls back to the names-less path
	# when only a script (no bank) is open.
	var fresh: String = ""
	if _document.bank_loaded():
		fresh = _document.mus_script.get_decompiled_text_with_bank(
				script_name, _document.bank)
	else:
		fresh = _document.mus_script.get_decompiled_text(script_name)
	# Only swap the text when the displayed buffer diverges from the source,
	# otherwise round-tripping the same string would reset the caret while
	# the user is mid-edit.
	if _code_edit.text != fresh:
		_code_edit.text = fresh


func _on_text_changed() -> void:
	if _document == null or not _document.script_loaded():
		return
	var script_name: StringName = StringName(_document.mus_script.get_default_script_name())
	_document.set_script_text(script_name, _code_edit.text)


func _on_compile_pressed() -> void:
	if _document == null:
		return
	_document.compile_script()


func _on_compile_run_pressed() -> void:
	if _document == null:
		return
	var errors: Array = _document.compile_script()
	if errors.is_empty():
		compile_and_run_requested.emit()


func _on_insert_play_template_pressed() -> void:
	if _code_edit == null:
		return
	var sound_name := "sound_0"
	if _document != null and _document.bank_loaded():
		var entries: Array = _document.bank.get_entries()
		if not entries.is_empty():
			var first := String(entries[0].get("name", ""))
			if first != "":
				sound_name = first
	_code_edit.insert_text_at_caret("  play %s\n" % sound_name)
	_on_text_changed()


func _on_compile_finished(success: bool, errors: Array) -> void:
	if _compile_bar != null:
		if success:
			_compile_bar.text = "Compiled clean"
		else:
			_compile_bar.text = "%d errors" % errors.size()
	if _error_list != null and _error_panel != null:
		_error_list.clear()
		if success or errors.is_empty():
			_error_panel.visible = false
			return
		for e in errors:
			var line: int = int(e.get("line", 0))
			var col: int = int(e.get("col", 0))
			var msg: String = String(e.get("message", ""))
			var prefix: String = "" if line <= 0 else "%d:%d  " % [line, col]
			_error_list.add_item(prefix + msg)
		_error_panel.visible = true


func _on_error_clicked(item_index: int) -> void:
	# Click-to-jump on the error list: park the caret on the diagnostic line so
	# the user can fix the offending site without scrolling. We re-use the
	# stashed metadata since errors[i] isn't kept around once the popout
	# rebuilds; rely on the prefix in the displayed string instead.
	if _code_edit == null or _error_list == null:
		return
	if item_index < 0 or item_index >= _error_list.item_count:
		return
	var text: String = _error_list.get_item_text(item_index)
	var colon := text.find(":")
	if colon <= 0:
		return
	var line_str: String = text.substr(0, colon)
	if not line_str.is_valid_int():
		return
	# Clamp to the current document. The error list can outlive the text it was
	# compiled against (e.g. after switching to a shorter script), so a stale
	# line number must not run past the end of _code_edit.
	var line: int = clampi(int(line_str) - 1, 0, _code_edit.get_line_count() - 1)
	_code_edit.set_caret_line(line)
	_code_edit.grab_focus()
