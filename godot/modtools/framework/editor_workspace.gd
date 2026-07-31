class_name EditorWorkspace
extends RefCounted

# Generic editor shell contract. Domain adapters name their ported engine
# equivalents, for example EnvironmentEditorWorkspace -> Environment_LoadTimeOfDayConfig
# @ 0x57db30 / TimeOfDay_ParseProperty @ 0x57c590 (Jointops retail).
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
#     single-pane    : build_inspector(mount)
#     multi-workflow : get_workflows + get_active_workflow_id +
#                      activate_workflow + build_workflow_inspector(id, mount)
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
#     jump     : focus_reference(focus) — focus an element after a cross-workspace
#                jump (shell open_in_workspace); reads its own FocusPayload fields
#
#   Edit + state: override get_editor_document() to return the domain
#     document/controller owning edit history + dirty state; the base derives
#     can_undo / can_redo / undo / redo / has_unsaved_changes from it (and
#     folds is_busy() into the can_* pair). Workspaces whose dirty flag lives
#     on a different object than their edit history (fonts/mnu) keep their own
#     has_unsaved_changes override.
#   Shell services (call, don't override): _notify_status, _sync_shell,
#     _mount_under_shell, get_reference_services, _resource_root — the guarded
#     seams to the owning shell. Workspaces call these instead of duck-typing
#     editor_shell; every one is a safe no-op without a shell (headless tests).
#   Asset dock: uses_asset_dock, set_asset_dock, sync_asset_dock
#   Placement: set WorkspaceDef.popup=true for popup workspaces (Environment);
#              shows_tile_gizmo() to opt into the in-world tile gizmo.
#
# To register a new workspace see WorkspaceDef; to add a workflow inspector see
# InspectorDef. Minimal example adapter: editor/credits_workspace.gd.

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


# In-world tile gizmo payload (shows_tile_gizmo() opt-in). The shell polls this
# each frame and shows the gizmo while it returns a TileGizmoState.
# Null means no active selection - gizmo hidden.
func get_tile_gizmo_state() -> TileGizmoState:
	return null


# Gizmo button actions: &"done", &"rotate", &"flip_x", &"flip_y", &"delete".
func run_tile_gizmo_action(_action: StringName) -> void:
	pass


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


func mount_viewport(_mount: Control) -> void:
	pass


func unmount_viewport(_released: Control) -> void:
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


# The shell's left lane (workflow picker + inspector mount). Workspaces that
# present their whole UI in the viewport can opt out to reclaim the width (e.g.
# Music's unified screen, whose section map already indexes every section).
func uses_left_lane() -> bool:
	return true


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


# Lazy per-workflow inspector cache (terrain and object both hand-rolled this):
# one instance per InspectorDef row, created on first use and reused on every
# workflow revisit. Override _instantiate_inspector when the inspector scripts
# take constructor args (object's take the workspace).
var _workflow_inspectors: Dictionary = {}


func _instantiate_inspector(def: InspectorDef) -> Object:
	return def.inspector_script.new()


func get_workflow_inspector(workflow_id: int) -> Object:
	var cached: Object = _workflow_inspectors.get(workflow_id)
	if cached != null:
		return cached
	var def := _def_for(workflow_id)
	if def == null or def.inspector_script == null:
		return null
	var inspector := _instantiate_inspector(def)
	_workflow_inspectors[workflow_id] = inspector
	return inspector


func get_active_workflow_id() -> int:
	return -1


func activate_workflow(_workflow_id: int) -> void:
	pass


func build_workflow_inspector(_workflow_id: int, _mount: Control) -> void:
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


# --- Domain document ------------------------------------------------------
# The single domain document/controller that owns this workspace's edit history
# and dirty state (a domain editor Node, a RefCounted document, or a controller —
# duck-typed, so the base guards every call with has_method). Single-document
# workspaces override only this; the base derives the edit + dirty hooks below.
# Documents without an edit history (credits, object) still serve dirty through
# it: can_undo/can_redo simply stay false.
func get_editor_document() -> Object:
	return null


# --- Documents (multi-document tabs) ---------------------------------------
# A workspace that holds N open documents opts in by overriding this tier; the
# shell then shows a tab strip above the viewport. get_editor_document() must
# keep returning the ACTIVE document so the save/undo/dirty wiring above stays
# untouched. The strip is rebuilt ONLY from documents_changed and workspace
# switches — never from the per-frame shell poll — so tabbed workspaces emit it
# on every membership/active/label/dirty change (DocumentTabSet does this).

## Emitted by multi-document workspaces when the tab set changes (membership,
## active index, labels, dirty badges). Single-document workspaces never emit it.
signal documents_changed


func supports_document_tabs() -> bool:
	return false


## One DocumentTabRow per open document (see framework/document_tab_row.gd).
func get_document_tabs() -> Array[DocumentTabRow]:
	return []


func get_active_document_index() -> int:
	return -1


func activate_document(_index: int) -> Error:
	return ERR_UNAVAILABLE


## Closes unconditionally — the dirty guard (save/discard/keep prompt) lives in
## the shell, which resolves it BEFORE calling this.
func close_document(_index: int) -> Error:
	return ERR_UNAVAILABLE


func has_unsaved_changes() -> bool:
	var doc := get_editor_document()
	if doc != null:
		# Both dirty shapes exist today: a method (mission controller, music
		# document) and a plain bool property (the EditorDocument family).
		# Property reads on a doc with neither return null — compare, don't
		# bool()-construct (bool(null) is a nonexistent constructor).
		if doc.has_method("is_dirty"):
			if doc.is_dirty():
				return true
		elif doc.get("is_dirty") == true:
			return true
	# Any open tab with unsaved work counts, not just the active one (B6: the
	# fold every multi-document workspace used to override for).
	for row in get_document_tabs():
		if row.dirty:
			return true
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


# Every resource kind this workspace accepts via open_in_workspace. Defaults to
# [get_open_resource_kind()] so single-kind workspaces (the common case) need no
# override. The Menus workspace overrides this to claim both "menu" and
# "menu_style" since the stylesheet authoring tab now lives there.
func get_open_resource_kinds() -> PackedStringArray:
	var single := get_open_resource_kind()
	return PackedStringArray([single]) if not single.is_empty() else PackedStringArray()


func get_current_resource_path() -> String:
	return ""


func open_file(_path: String) -> Error:
	return ERR_UNAVAILABLE


# Focus an element of the open document after a cross-workspace jump: the shell's
# open_in_workspace(kind, path, focus) forwards its FocusPayload here once the
# target file is open. Each workspace reads only the payload fields it owns
# (strings: key, menus: screen/variable) and documents them on the override.
# Default: nothing to focus.
func focus_reference(_focus: FocusPayload) -> Error:
	return OK


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


func uses_save_file_dialog() -> bool:
	return false


func get_save_file_dialog_filters() -> PackedStringArray:
	return PackedStringArray()


func get_save_file_dialog_default_name() -> String:
	return ""


func save_as_file(_path: String) -> Error:
	return ERR_UNAVAILABLE


## Optional detail for a failed save. The shell uses this instead of replacing
## a workspace's domain-specific validation message with a generic error.
func get_save_failure_message(_error: Error) -> String:
	return ""


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


func build_inspector(_mount: Control) -> void:
	pass


# Undo/redo, derived from get_editor_document(). Availability folds in is_busy()
# (terrain disables the buttons while an export runs); the actions themselves are
# deliberately unguarded by busy, matching the long-standing terrain behavior.
func can_undo() -> bool:
	var doc := get_editor_document()
	return doc != null and not is_busy() and doc.has_method("can_undo") and doc.can_undo()


func can_redo() -> bool:
	var doc := get_editor_document()
	return doc != null and not is_busy() and doc.has_method("can_redo") and doc.can_redo()


func undo() -> void:
	var doc := get_editor_document()
	if doc != null and doc.has_method("undo"):
		doc.undo()


func redo() -> void:
	var doc := get_editor_document()
	if doc != null and doc.has_method("redo"):
		doc.redo()


# --- Shell services ---------------------------------------------------------
# The guarded seams to the owning shell. Workspaces call these instead of
# duck-typing editor_shell (the has_method guards live here, in one place);
# every one is a safe no-op without a shell (headless tests).

## Show a toast in the shell status bar. Severities: &"info", &"success",
## &"warn", &"error"; duration <= 0.0 picks the per-severity default. Returns
## true when a shell displayed it, so a caller whose failure must not vanish
## headless can fall back to push_warning.
func _notify_status(message: String, severity: StringName = &"info", duration: float = 0.0) -> bool:
	if editor_shell != null and editor_shell.has_method("show_status_message"):
		editor_shell.show_status_message(message, duration, severity)
		return true
	return false


## Refresh the shell chrome from this workspace's state (project title, `*`
## dirty marker, action-button enablement). Cheap: it does NOT rebuild the
## inspector (the shell caches it) or remount the viewport.
func _sync_shell() -> void:
	if editor_shell != null and editor_shell.has_method("sync_from_editor_state"):
		editor_shell.sync_from_editor_state()


## Re-sync the shell's workflow rail + inspector after this workspace changed
## its active workflow programmatically (activate_workflow from workspace UI
## rather than the rail). No-op without a shell (headless tests).
func _sync_shell_workflow() -> void:
	if editor_shell != null and editor_shell.has_method("sync_workflow_from_workspace"):
		editor_shell.sync_workflow_from_workspace()


## Parent a Node under the shell: domain editors that need _process/audio,
## transient dialogs, preview players. RefCounted workspaces have no tree of
## their own; without a shell the node stays parentless and the caller's
## setup continues (headless).
func _mount_under_shell(node: Node) -> void:
	if editor_shell != null:
		editor_shell.add_child(node)


## Reference-strip services riding the shell's reference index — the
## ReferenceServices record ReferenceStrip.configure takes; null when headless
## or the shell lacks the index (callers then skip the strip). Inspectors ask
## their workspace for this instead of reaching for the shell.
func get_reference_services() -> ReferenceServices:
	# from_shell receives the shell itself, so the service lambdas never hold
	# this RefCounted workspace through a member access.
	return ReferenceServices.from_shell(editor_shell)


# --- Resource root (the shared VFS the shell mounts) -----------------------

# The shell's mounted resource root, or null when no shell is bound (headless tests).
func _resource_root() -> NovaResourceRoot:
	if editor_shell != null and editor_shell.has_method("get_resource_root"):
		return editor_shell.get_resource_root()
	return null


# Shell root, falling back to a fresh mount of the persisted resource directory.
# Only fonts/mnu carry the fallback (their open-by-name paths must resolve without
# a shell); the other workspaces stay shell-only on purpose — do not widen.
func _resource_root_or_settings() -> NovaResourceRoot:
	var root := _resource_root()
	if root != null:
		return root
	var dir := NovaResourceDirSettings.get_resource_dir()
	if dir.is_empty():
		return null
	var resources := NovaResourceRoot.new()
	return resources if resources.set_root_dir(dir) == OK else null


# VFS-open fallback shared by every open_file(): when `path` is not a loose file
# on disk but resolves inside the mounted root (a name picked from the resource
# browser), returns that root; null means "open from disk". Callers branch:
#   var vfs := _vfs_root_for_open(path)
#   if vfs != null:
#       return editor.open_x_bytes(vfs.read_file(path), _vfs_display_path(vfs, path))
#   return editor.open_x(path)
func _vfs_root_for_open(path: String) -> NovaResourceRoot:
	var resources := _resource_root()
	if not FileAccess.file_exists(path) and resources != null and resources.has_file(path):
		return resources
	return null


# The display path an editor records for a VFS-opened file (the mounted dir + bare name).
func _vfs_display_path(root: NovaResourceRoot, path: String) -> String:
	return root.get_root_dir().path_join(path.get_file())
