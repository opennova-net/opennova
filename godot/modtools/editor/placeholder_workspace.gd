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
