class_name EditorWorkspace
extends RefCounted

# Generic editor shell contract. Domain adapters name their ported engine
# equivalents, for example EnvironmentEditorWorkspace -> sub_53FA70/sub_53E3F0.

var editor_shell: Node


func set_editor_shell(value: Node) -> void:
	editor_shell = value


# --- Capability hooks: the shell reads these instead of switching on workspace
# type. Defaults describe a plain main-rail workspace with no editor binding,
# tooltip, camera readout, or export flavors. ---
func bind_to_editor(_editor: Node) -> void:
	pass


func get_workspace_tooltip() -> String:
	return ""


func is_popup() -> bool:
	return false


func shows_camera_status() -> bool:
	return false


func get_export_flavors() -> Array:
	return []


func activate() -> void:
	pass


func deactivate() -> void:
	pass


func mount_viewport(_host: Control) -> void:
	pass


func unmount_viewport(_host: Control) -> void:
	pass


func release_viewport() -> void:
	pass


func get_viewport_camera() -> Camera3D:
	return null


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


func has_new_action() -> bool:
	return can_new()


func get_new_action_label() -> String:
	return "New"


func new_current() -> Error:
	return ERR_UNAVAILABLE


func can_open() -> bool:
	return false


func has_open_action() -> bool:
	return can_open()


func get_open_action_label() -> String:
	return "Open..."


func get_open_dialog_title() -> String:
	return "Open"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray()


func get_open_dialog_dir() -> String:
	return ""


func get_open_resource_kind() -> String:
	return ""


func get_current_resource_path() -> String:
	return ""


func open_file(_path: String) -> Error:
	return ERR_UNAVAILABLE


func can_save() -> bool:
	return false


func has_save_action() -> bool:
	return can_save() or can_save_as()


func get_save_action_label() -> String:
	return "Save"


func can_save_as() -> bool:
	return false


func has_save_as_action() -> bool:
	return can_save_as()


func get_save_as_action_label() -> String:
	return "Save As..."


func save_current() -> Error:
	return ERR_UNAVAILABLE


func save_as(_dir_path: String) -> Error:
	return ERR_UNAVAILABLE


func can_export() -> bool:
	return false


func has_export_action() -> bool:
	return can_export()


func get_export_action_label() -> String:
	return "Export..."


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
