class_name PlaceholderEditorWorkspace
extends "res://modtools/editor/editor_workspace.gd"

# No IDA equivalent is wired here yet; mission/entity workspaces should replace
# this with adapters that point at the matching ported engine routines.

var _workspace_id: String = ""
var _workspace_label: String = ""
var _status_context: String = ""


func _init(workspace_id: String = "", workspace_label: String = "", status_context: String = "") -> void:
	_workspace_id = workspace_id
	_workspace_label = workspace_label
	_status_context = status_context


func get_workspace_id() -> String:
	return _workspace_id


func get_workspace_label() -> String:
	return _workspace_label


func get_project_title() -> String:
	return _workspace_label


func get_status_tool() -> String:
	return _workspace_label


func get_status_context() -> String:
	return _status_context


func build_inspector(host: Control) -> void:
	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 12)
	margin.add_theme_constant_override("margin_top", 12)
	margin.add_theme_constant_override("margin_right", 12)
	margin.add_theme_constant_override("margin_bottom", 12)
	margin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.size_flags_vertical = Control.SIZE_EXPAND_FILL
	host.add_child(margin)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.add_child(box)

	var title := Label.new()
	title.text = "Coming soon"
	title.theme_type_variation = &"Heading"
	box.add_child(title)

	var body := Label.new()
	body.text = _status_context
	body.theme_type_variation = &"Muted"
	body.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(body)
