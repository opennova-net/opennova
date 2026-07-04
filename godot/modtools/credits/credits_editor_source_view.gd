class_name CreditsEditorSourceView
extends VBoxContainer

signal pending_edits_changed(has_pending_edits)

# Status-line tints (kept as overrides, not a theme variation: the line cycles
# through error/success/cleared states and there is no green theme variation).
const STATUS_ERROR_COLOR := Color(0.95, 0.45, 0.35, 1.0)
const STATUS_SUCCESS_COLOR := Color(0.55, 0.8, 0.55, 1.0)

@onready var _code_edit: CodeEdit = $CodeEdit
@onready var _status: Label = $StatusBar
@onready var _apply_button: Button = $ApplyBar/ApplyButton

var _resource: CbinCreditsResource
# The owning document: a source Apply lands as ONE undo step (B2). Optional —
# without a document the apply mutates directly.
var _document: CreditsEditorDocument
var _suppress := false
var _refresh_pending := false
var _refresh_force := false
var _source_dirty := false
var _last_applied_text := ""

func set_document(value: CreditsEditorDocument) -> void:
	_document = value


func set_resource(value: CbinCreditsResource) -> void:
	if _resource == value:
		refresh_from_resource(false)
		return
	if _resource and _resource.changed.is_connected(_on_resource_changed):
		_resource.changed.disconnect(_on_resource_changed)
	_resource = value
	if _resource:
		_resource.changed.connect(_on_resource_changed)
	refresh_from_resource(true)

func _ready() -> void:
	_apply_button.pressed.connect(_apply)
	_code_edit.focus_exited.connect(_apply)
	_code_edit.text_changed.connect(_on_text_changed)
	_refresh_apply_enabled()

func has_pending_edits() -> bool:
	if _resource == null or _code_edit == null:
		return false
	return _source_dirty or _code_edit.text != _last_applied_text

func refresh_from_resource(force := false) -> void:
	if _resource == null:
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
	if _resource == null:
		_refresh_apply_enabled()
		return
	if not force and visible and _code_edit.has_focus():
		return
	_suppress = true
	_code_edit.text = _resource.to_text()
	_last_applied_text = _code_edit.text
	_source_dirty = false
	pending_edits_changed.emit(false)
	_suppress = false
	_set_status("", false)
	_refresh_apply_enabled()

func _apply() -> void:
	apply_pending()

func apply_pending() -> Error:
	if _resource == null or _suppress:
		return OK
	if not _source_dirty and _code_edit.text == _last_applied_text:
		return OK
	var source_text := _code_edit.text
	# One undo step per Apply. The result rides an Array box: GDScript lambdas
	# capture locals by value, so a plain bool would not write back.
	var resource := _resource
	var result := [false]
	if _document != null:
		_document.push_undo_step(func() -> void: result[0] = resource.from_text(source_text))
	else:
		result[0] = resource.from_text(source_text)
	var ok: bool = result[0]
	if ok:
		_last_applied_text = source_text
		_source_dirty = false
		_set_status("Applied", false)
		pending_edits_changed.emit(false)
		_refresh_apply_enabled()
		return OK
	else:
		_set_status("Parse error - text not applied", true)
		return ERR_PARSE_ERROR

func _on_text_changed() -> void:
	if _suppress:
		return
	_source_dirty = true
	pending_edits_changed.emit(true)
	_set_status("", false)
	_refresh_apply_enabled()

# Enables Apply only when there is something to apply (a bound resource with
# pending edits), so the button is a live affordance rather than always-on.
func _refresh_apply_enabled() -> void:
	if _apply_button == null:
		return
	_apply_button.disabled = not has_pending_edits()


# Colors the status line: a calm success tint for applied text, a warning tint
# for parse failures, and the default color when cleared.
func _set_status(text: String, is_error: bool) -> void:
	if _status == null:
		return
	_status.text = text
	if text.is_empty():
		_status.remove_theme_color_override("font_color")
	elif is_error:
		_status.add_theme_color_override("font_color", STATUS_ERROR_COLOR)
	else:
		_status.add_theme_color_override("font_color", STATUS_SUCCESS_COLOR)
