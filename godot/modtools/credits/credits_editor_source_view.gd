class_name CreditsEditorSourceView
extends VBoxContainer

@onready var _code_edit: CodeEdit = $CodeEdit
@onready var _status: Label = $StatusBar
@onready var _apply_button: Button = $ApplyBar/ApplyButton

var _resource: CbinCreditsResource
var _suppress := false
var _refresh_pending := false

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
	_suppress = false
	_status.text = ""

func _apply() -> void:
	if _resource == null or _suppress:
		return
	var ok := _resource.from_text(_code_edit.text)
	if ok:
		_status.text = "Applied"
	else:
		_status.text = "Parse error - text not applied"
