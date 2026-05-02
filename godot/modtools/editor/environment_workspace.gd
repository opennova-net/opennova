class_name EnvironmentEditorWorkspace
extends "res://modtools/editor/editor_workspace.gd"

# Shell adapter for the EnvFile document. The IDA-backed format/TOD behavior is
# in libs/env (sub_53FA70, sub_53E3F0, sub_53FCC0); this file stays UI-only.

const EnvironmentInspector = preload("res://modtools/environment/environment_inspector.gd")

var environment_editor


func _init(value = null) -> void:
	environment_editor = value


func set_environment_editor(value) -> void:
	environment_editor = value


func get_workspace_id() -> String:
	return "environment"


func get_workspace_label() -> String:
	return "Environment"


func get_project_title() -> String:
	return environment_editor.get_project_title() if environment_editor else "Environment"


func get_status_tool() -> String:
	return "Environment"


func get_status_context() -> String:
	return environment_editor.get_status_context() if environment_editor else ""


func can_new() -> bool:
	return environment_editor != null


func get_new_action_label() -> String:
	return "New Environment"


func new_current() -> Error:
	if environment_editor == null:
		return ERR_UNAVAILABLE
	environment_editor.create_default_environment(true)
	return OK


func can_open() -> bool:
	return environment_editor != null


func get_open_action_label() -> String:
	return "Open Environment..."


func get_open_dialog_title() -> String:
	return "Open .env"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.env,*.ENV ; Environment"])


func get_open_dialog_dir() -> String:
	return environment_editor.get_last_open_dir() if environment_editor else ""


func get_open_resource_kind() -> String:
	return "environment"


func get_current_resource_path() -> String:
	if environment_editor == null:
		return ""
	return String(environment_editor.current_path)


func open_file(path: String) -> Error:
	return environment_editor.open_env(path) if environment_editor else ERR_UNAVAILABLE


func has_unsaved_changes() -> bool:
	return environment_editor != null and environment_editor.is_dirty


func can_save() -> bool:
	return environment_editor != null and environment_editor.is_dirty


func get_save_action_label() -> String:
	return "Save Environment"


func can_save_as() -> bool:
	return environment_editor != null


func get_save_as_action_label() -> String:
	return "Save Environment As..."


func save_current() -> Error:
	return environment_editor.save_current() if environment_editor else ERR_UNAVAILABLE


func save_as(dir_path: String) -> Error:
	return environment_editor.save_as(dir_path) if environment_editor else ERR_UNAVAILABLE


func can_export() -> bool:
	return environment_editor != null and environment_editor.env_file != null


func has_export_action() -> bool:
	return false


func begin_export(dir_path: String, _flavor: int) -> Error:
	return environment_editor.export_to_dir(dir_path) if environment_editor else ERR_UNAVAILABLE


func get_save_dialog_title() -> String:
	return "Choose where to save the environment"


func get_save_dialog_dir() -> String:
	return environment_editor.get_last_save_dir() if environment_editor else ""


func get_export_dialog_title() -> String:
	return "Choose where to export the environment"


func get_export_dialog_dir() -> String:
	return environment_editor.get_last_export_dir() if environment_editor else ""


func build_inspector(host: Control) -> void:
	if environment_editor == null:
		return
	var inspector := EnvironmentInspector.new()
	host.add_child(inspector)
	inspector.set_environment_editor(environment_editor)
