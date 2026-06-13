class_name MnsSourceView
extends VBoxContainer

# Raw text view of the stylesheet (the credits source-view pattern): a CodeEdit
# over MnsStyleSheet.get_source_text(), Apply on demand or focus-leave, and a
# live diagnostics list (duplicate names, bad #if arguments, stray escapes...)
# that re-reads on every document change. Apply routes through a callback the
# editor injects, so source edits land on the same undo stack as typed-row
# edits. The .mns parse is permissive (it never rejects text), so Apply always
# lands; problems surface as diagnostics rows, not as a blocked apply.

signal pending_edits_changed(has_pending_edits)

const DIAG_ERROR_COLOR := Color(0.95, 0.45, 0.35, 1.0)
const DIAG_WARNING_COLOR := Color(0.92, 0.78, 0.45, 1.0)
const STATUS_SUCCESS_COLOR := Color(0.55, 0.8, 0.55, 1.0)

var _code_edit: CodeEdit
var _status: Label
var _apply_button: Button
var _diagnostics_box: VBoxContainer

var _sheet: MnsStyleSheet
# The editor's apply_edit({op:"source", text}) entry point, so undo covers it.
var apply_callback: Callable = Callable()

var _suppress := false
var _refresh_pending := false
var _refresh_force := false
var _source_dirty := false
var _last_applied_text := ""


func _init() -> void:
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_theme_constant_override("separation", 4)

	_code_edit = CodeEdit.new()
	_code_edit.name = "CodeEdit"
	_code_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_code_edit.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_code_edit.gutters_draw_line_numbers = true
	add_child(_code_edit)

	_diagnostics_box = VBoxContainer.new()
	_diagnostics_box.name = "Diagnostics"
	_diagnostics_box.add_theme_constant_override("separation", 1)
	add_child(_diagnostics_box)

	var apply_bar := HBoxContainer.new()
	apply_bar.name = "ApplyBar"
	add_child(apply_bar)
	_status = Label.new()
	_status.name = "StatusBar"
	_status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_status.theme_type_variation = &"Muted"
	apply_bar.add_child(_status)
	_apply_button = Button.new()
	_apply_button.name = "ApplyButton"
	_apply_button.text = "Apply"
	apply_bar.add_child(_apply_button)

	_apply_button.pressed.connect(_apply)
	_code_edit.focus_exited.connect(_apply)
	_code_edit.text_changed.connect(_on_text_changed)
	_refresh_apply_enabled()


func set_stylesheet(value: MnsStyleSheet) -> void:
	if _sheet == value:
		refresh_from_resource(false)
		return
	if _sheet != null and _sheet.changed.is_connected(_on_resource_changed):
		_sheet.changed.disconnect(_on_resource_changed)
	_sheet = value
	if _sheet != null:
		_sheet.changed.connect(_on_resource_changed)
	refresh_from_resource(true)


func has_pending_edits() -> bool:
	if _sheet == null or _code_edit == null:
		return false
	return _source_dirty or _code_edit.text != _last_applied_text


func refresh_from_resource(force := false) -> void:
	if _sheet == null:
		return
	if not force and visible and _code_edit.has_focus():
		return
	if force:
		_refresh_force = true
	if _refresh_pending:
		return
	_refresh_pending = true
	call_deferred("_apply_refresh")


func _on_resource_changed() -> void:
	refresh_from_resource(false)


func _apply_refresh() -> void:
	_refresh_pending = false
	var force := _refresh_force
	_refresh_force = false
	if _sheet == null:
		_refresh_apply_enabled()
		return
	if not force and visible and _code_edit.has_focus():
		_rebuild_diagnostics()
		return
	_suppress = true
	_code_edit.text = String(_sheet.get_source_text())
	_last_applied_text = _code_edit.text
	_source_dirty = false
	pending_edits_changed.emit(false)
	_suppress = false
	_set_status("", false)
	_rebuild_diagnostics()
	_refresh_apply_enabled()


func _apply() -> void:
	apply_pending()


func apply_pending() -> Error:
	if _sheet == null or _suppress:
		return OK
	if not _source_dirty and _code_edit.text == _last_applied_text:
		return OK
	var source_text := _code_edit.text
	if apply_callback.is_valid():
		apply_callback.call(source_text)
	else:
		_sheet.set_source_text(source_text)
	_last_applied_text = source_text
	_source_dirty = false
	_set_status("Applied", false)
	pending_edits_changed.emit(false)
	_rebuild_diagnostics()
	_refresh_apply_enabled()
	return OK


func _on_text_changed() -> void:
	if _suppress:
		return
	_source_dirty = true
	pending_edits_changed.emit(true)
	_set_status("", false)
	_refresh_apply_enabled()


func _refresh_apply_enabled() -> void:
	if _apply_button == null:
		return
	_apply_button.disabled = not has_pending_edits()


func _set_status(text: String, is_error: bool) -> void:
	if _status == null:
		return
	_status.text = text
	if text.is_empty():
		_status.remove_theme_color_override("font_color")
	elif is_error:
		_status.add_theme_color_override("font_color", DIAG_ERROR_COLOR)
	else:
		_status.add_theme_color_override("font_color", STATUS_SUCCESS_COLOR)


# One row per parser diagnostic; clicking moves the caret to the line.
func _rebuild_diagnostics() -> void:
	for child in _diagnostics_box.get_children():
		child.queue_free()
	if _sheet == null:
		return
	for diag_value in _sheet.get_diagnostics():
		var diag := diag_value as Dictionary
		var line := int(diag.get("line", 0))
		var severity := String(diag.get("severity", "warning"))
		var button := Button.new()
		button.flat = true
		button.alignment = HORIZONTAL_ALIGNMENT_LEFT
		button.focus_mode = Control.FOCUS_NONE
		button.text = "Line %d: %s" % [line, String(diag.get("message", ""))]
		button.add_theme_color_override("font_color",
				DIAG_ERROR_COLOR if severity == "error" else DIAG_WARNING_COLOR)
		button.pressed.connect(func() -> void:
			if _code_edit != null and line > 0:
				_code_edit.set_caret_line(line - 1)
				_code_edit.grab_focus())
		_diagnostics_box.add_child(button)


func get_diagnostic_count() -> int:
	return _sheet.get_diagnostics().size() if _sheet != null else 0
