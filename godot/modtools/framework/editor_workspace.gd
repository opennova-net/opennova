class_name EditorWorkspace
extends RefCounted

# Generic editor shell contract. Domain adapters name their ported engine
# equivalents, for example EnvironmentEditorWorkspace -> sub_53FA70/sub_53E3F0.
#
# ============================================================================
# CONTRACT — what a workspace must / may implement
# ============================================================================
# The shell (EditorWorkstation) NEVER switches on workspace type; it reads the
# hooks below. Every hook has a safe default here, so a workspace overrides only
# the tiers it needs. Tiers:
#
#   Identity (required):
#     get_workspace_id, get_workspace_label
#     recommended: get_workspace_tooltip, get_status_tool, get_status_context,
#     get_project_title
#
#   Lifecycle:
#     bind_to_editor(editor)  — receive the domain editor/model
#     activate / deactivate   — workspace gained / lost focus
#
#   Inspector — pick ONE style:
#     single-pane    : build_inspector(host)
#     multi-workflow : get_workflows + get_active_workflow_id +
#                      activate_workflow + build_workflow_inspector(id, host)
#
#   Viewport (only if the workspace shows a 3D view):
#     mount_viewport / unmount_viewport / release_viewport,
#     get_viewport_camera, shows_camera_status
#     (use framework/viewport_mount.gd for the create/reparent/free mechanics)
#
#   Document actions (implement the cluster you support; each is gated by a
#   can_* hook so the shell shows the button only when available):
#     new      : can_new / new_current
#     open     : can_open / open_file + get_open_dialog_* + get_open_resource_kind
#     save     : can_save / save_current, can_save_as / save_as + get_save_dialog_*
#     export   : can_export / begin_export + get_export_flavors +
#                get_export_dialog_* + get_export_progress_*
#
#   Edit:  can_undo / undo, can_redo / redo
#   State: is_busy, has_unsaved_changes
#   Asset dock: uses_asset_dock, set_asset_dock, sync_asset_dock
#   Placement: set WorkspaceDef.popup=true for popup workspaces (Environment);
#              shows_tile_gizmo() to opt into the in-world tile gizmo.
#
# To register a new workspace see WorkspaceDef; to add a workflow inspector see
# InspectorDef. Minimal example adapter: editor/mission_workspace.gd.

var editor_shell: Node

# Cached workflow-inspector registry. Multi-workflow workspaces override
# _build_inspector_defs(); the base lazy-builds and caches it here so the shell
# and the subclass share one list. Single-pane workspaces leave it empty.
var _inspector_defs: Array = []


func set_editor_shell(value: Node) -> void:
	editor_shell = value


# --- Capability hooks: the shell reads these instead of switching on workspace
# type. Defaults describe a plain main-rail workspace with no editor binding,
# tooltip, camera readout, or export flavors. ---
func bind_to_editor(_editor: Node) -> void:
	pass


func get_workspace_tooltip() -> String:
	return ""


func shows_camera_status() -> bool:
	return false


func shows_tile_gizmo() -> bool:
	return false


# View guides (reference grid / origin axes) in a 3D preview. Workspaces with a
# guide overlay override shows_view_guides() so the shell offers Show grid / Show
# axes toggles, and the two setters to apply them; other workspaces stay silent.
func shows_view_guides() -> bool:
	return false


func set_grid_visible(_value: bool) -> void:
	pass


func set_axes_visible(_value: bool) -> void:
	pass


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


# Returns the workspace's workflow inspectors as typed InspectorDef rows (the
# shell reads .id/.label/.tooltip off them). Empty for single-pane workspaces.
func get_workflows() -> Array:
	return _ensure_inspector_defs()


# Override in multi-workflow workspaces to declare InspectorDef.make(...) rows.
func _build_inspector_defs() -> Array:
	return []


func _ensure_inspector_defs() -> Array:
	if _inspector_defs.is_empty():
		_inspector_defs = _build_inspector_defs()
	return _inspector_defs


func _def_for(workflow_id: int) -> InspectorDef:
	for def in _ensure_inspector_defs():
		if (def as InspectorDef).id == workflow_id:
			return def
	return null


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


# Workspaces with deferred/in-progress edits (e.g. a source text buffer not yet
# committed to the document) override this to apply them before a save/export.
# The default is a no-op for workspaces that commit edits immediately.
func flush_pending_edits() -> Error:
	return OK


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
