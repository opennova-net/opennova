class_name CreditsEditorSourceView
extends VBoxContainer

signal pending_edits_changed(has_pending_edits)

@onready var _code_edit: CodeEdit = $CodeEdit
@onready var _status: Label = $StatusBar
@onready var _apply_button: Button = $ApplyBar/ApplyButton

var _resource: CbinCreditsResource
var _suppress := false
var _refresh_pending := false
var _source_dirty := false
var _last_applied_text := ""

func set_resource(value: CbinCreditsResource) -> void:
	if _resource == value:
		return
	if _resource and _resource.changed.is_connected(_refresh_text):
		_resource.changed.disconnect(_refresh_text)
	_resource = value
	if _resource:
		_resource.changed.connect(_refresh_text)
	_refresh_text()

func _ready() -> void:
	_apply_button.pressed.connect(_apply)
	_code_edit.focus_exited.connect(_apply)
	_code_edit.text_changed.connect(_on_text_changed)

func _refresh_text() -> void:
	if _resource == null:
		return
	if _code_edit.has_focus():
		return
	if _refresh_pending:
		return
	_refresh_pending = true
	call_deferred("_apply_refresh")

func _apply_refresh() -> void:
	_refresh_pending = false
	if _resource == null:
		return
	if _code_edit.has_focus():
		return
	_suppress = true
	_code_edit.text = _resource.to_text()
	_last_applied_text = _code_edit.text
	_source_dirty = false
	pending_edits_changed.emit(false)
	_suppress = false
	_status.text = ""

func _apply() -> void:
	apply_pending()

func apply_pending() -> Error:
	if _resource == null or _suppress:
		return OK
	if not _source_dirty and _code_edit.text == _last_applied_text:
		return OK
	var source_text := _code_edit.text
	var ok := _resource.from_text(source_text)
	if ok:
		_last_applied_text = source_text
		_source_dirty = false
		_status.text = "Applied"
		pending_edits_changed.emit(false)
		return OK
	else:
		_status.text = "Parse error - text not applied"
		return ERR_PARSE_ERROR

func _on_text_changed() -> void:
	if _suppress:
		return
	_source_dirty = true
	pending_edits_changed.emit(true)
	if _status != null:
		_status.text = ""
