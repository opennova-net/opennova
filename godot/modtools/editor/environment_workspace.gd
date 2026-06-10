class_name EnvironmentEditorWorkspace
extends EditorWorkspace

# Shell adapter for the EnvFile document. The IDA-backed format/TOD behavior is
# in libs/env (Environment_LoadTimeOfDayConfig @ 0x57db30, TimeOfDay_ParseProperty
# @ 0x57c590, Environment_ComputeTimeOfDayColors @ 0x57de40); this file stays UI-only.

const EnvironmentInspector = preload("res://modtools/environment/environment_inspector.gd")

var environment_editor


func _init(value = null) -> void:
	environment_editor = value


func set_environment_editor(value) -> void:
	environment_editor = value


func bind_to_editor(value: Node) -> void:
	var env = value.get_environment_editor() if value != null and value.has_method("get_environment_editor") else null
	set_environment_editor(env)


func get_workspace_tooltip() -> String:
	return "Edit .env weather, lighting, atmosphere, and time of day."


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
	if environment_editor == null:
		return ERR_UNAVAILABLE
	var resources := _resource_root()
	if not FileAccess.file_exists(path) and resources != null and resources.has_file(path):
		return environment_editor.open_env_from_resource_root(resources, path)
	return environment_editor.open_env(path)


func _resource_root() -> NovaResourceRoot:
	if editor_shell != null and editor_shell.has_method("get_resource_root"):
		return editor_shell.get_resource_root()
	return null


func has_unsaved_changes() -> bool:
	return environment_editor != null and environment_editor.is_dirty


func can_save() -> bool:
	# Save needs a path; without one the action is Save As (mirror fonts/credits).
	return environment_editor != null and environment_editor.is_dirty \
		and not String(environment_editor.current_path).is_empty()


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


func get_save_dialog_title() -> String:
	return "Choose where to save the environment"


func get_save_dialog_dir() -> String:
	return environment_editor.get_last_save_dir() if environment_editor else ""


func can_undo() -> bool:
	return environment_editor != null and environment_editor.can_undo()


func can_redo() -> bool:
	return environment_editor != null and environment_editor.can_redo()


func undo() -> void:
	if environment_editor:
		environment_editor.undo()


func redo() -> void:
	if environment_editor:
		environment_editor.redo()


func build_inspector(host: Control) -> void:
	if environment_editor == null:
		return
	var inspector := EnvironmentInspector.new()
	host.add_child(inspector)
	inspector.set_environment_editor(environment_editor)
