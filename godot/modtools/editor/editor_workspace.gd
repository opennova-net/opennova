class_name EditorWorkspace
extends RefCounted

# Generic editor shell contract. Domain adapters name their ported engine
# equivalents, for example EnvironmentEditorWorkspace -> sub_53FA70/sub_53E3F0.

var editor_shell: Node


func set_editor_shell(value: Node) -> void:
	editor_shell = value


func activate() -> void:
	pass


func deactivate() -> void:
	pass


func get_workspace_id() -> String:
	return ""


func get_workspace_label() -> String:
	return ""


func get_project_title() -> String:
	return get_workspace_label()


func get_status_tool() -> String:
	return get_workspace_label()


func get_status_context() -> String:
	return ""


func uses_asset_dock() -> bool:
	return false


func set_asset_dock(_dock: Control) -> void:
	pass


func sync_asset_dock() -> void:
	pass


func get_workflows() -> Array:
	return []


func get_active_workflow_id() -> int:
	return -1


func activate_workflow(_workflow_id: int) -> void:
	pass


func build_workflow_inspector(_workflow_id: int, _host: Control) -> void:
	pass


func get_export_progress_title() -> String:
	return "Exporting..."


func get_export_progress_phase() -> String:
	return ""


func get_export_progress_message() -> String:
	return ""


func get_export_progress_current() -> int:
	return 0


func get_export_progress_total() -> int:
	return 0


func get_export_progress_ratio() -> float:
	return 0.0


func is_busy() -> bool:
	return false


func has_unsaved_changes() -> bool:
	return false


func can_new() -> bool:
	return false


func new_current() -> Error:
	return ERR_UNAVAILABLE


func can_open() -> bool:
	return false


func get_open_dialog_title() -> String:
	return "Open"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray()


func get_open_dialog_dir() -> String:
	return ""


func open_file(_path: String) -> Error:
	return ERR_UNAVAILABLE


func can_save() -> bool:
	return false


func can_save_as() -> bool:
	return false


func save_current() -> Error:
	return ERR_UNAVAILABLE


func save_as(_dir_path: String) -> Error:
	return ERR_UNAVAILABLE


func can_export() -> bool:
	return false


func begin_export(_dir_path: String, _flavor: int) -> Error:
	return ERR_UNAVAILABLE


func get_save_dialog_title() -> String:
	return "Choose where to save"


func get_save_dialog_dir() -> String:
	return ""


func get_export_dialog_title() -> String:
	return "Choose where to export"


func get_export_dialog_dir() -> String:
	return ""


func build_inspector(_host: Control) -> void:
	pass


func can_undo() -> bool:
	return false


func can_redo() -> bool:
	return false


func undo() -> void:
	pass


func redo() -> void:
	pass
