class_name WorkspaceShell
extends Control

## The typed contract a workspace (and the domain editors it mounts) may call
## on its owning shell. EditorWorkstation is the one production shell; headless
## tests either leave `editor_shell` null or bind a lightweight subclass of
## this base that records calls. Every method here is a safe default so a
## partial test shell overrides only what it asserts on — the ADR 0034
## replacement for the has_method probing that used to guard each seam.
##
## Keep this surface minimal: only what workspaces/domain editors reach
## through `editor_shell` / `workstation` belongs here. Shell internals
## (workspace registry, layout, MCP wiring) stay on EditorWorkstation.


## Transient status toast in the shell status bar. Severities: &"info",
## &"success", &"warn", &"error"; duration <= 0.0 picks the per-severity
## default.
func show_status_message(_text: String, _duration: float = 0.0, _severity: StringName = &"info") -> void:
	pass


## Refresh the shell chrome from workspace state (project title, dirty marker,
## action-button enablement).
func sync_from_editor_state() -> void:
	pass


## Re-sync the workflow rail + inspector after a workspace changed its active
## workflow programmatically.
func sync_workflow_from_workspace() -> void:
	pass


## The shell's mounted resource root; null when nothing is mounted.
func get_resource_root() -> ResourceRoot:
	return null


## The shared reference index over the resource root; null when the shell has
## none (reference strips are then skipped).
func get_reference_index() -> ReferenceIndex:
	return null


## Re-index the resource folder after a workspace created files under it.
func rescan_resource_root() -> Error:
	return ERR_UNAVAILABLE


## Open `path` in the workspace that declares `kind`, then forward `focus`.
func open_in_workspace(_kind: String, _path: String, _focus: FocusPayload = null) -> Error:
	return ERR_UNAVAILABLE


## Cross-jump to the Fonts workspace by font NAME (fonts resolve by name).
func open_font_workspace(_font_name: String) -> Error:
	return ERR_UNAVAILABLE


## Cross-jump: open a text table in Strings and focus `key`.
func open_strings_workspace(_table_path: String, _key: String) -> Error:
	return ERR_UNAVAILABLE


## Cross-jump: open a .mnu in Menus and focus `screen` when supplied.
func open_menu_workspace(_file: String, _screen: String = "") -> Error:
	return ERR_UNAVAILABLE


## In-app resource picker over an explicit file list (kind-independent).
func open_file_picker(_title: String, _files: PackedStringArray, _on_pick: Callable) -> void:
	pass


## Indexed resource picker over every resource of `kind`.
func open_kind_picker(_kind: String, _title: String, _on_pick: Callable) -> void:
	pass


## Shared unsaved-changes dialog with caller-supplied outcomes. The default
## never prompts and never runs an outcome: headless callers treat the action
## as unguarded (they run it directly after a false _prompt path).
func prompt_unsaved_for(_on_save: Callable, _on_discard: Callable, _on_cancel := Callable()) -> void:
	pass


## Save the workspace's current document, then run on_done (Save As fallback
## for path-less documents).
func save_then(_workspace: EditorWorkspace, _on_done: Callable, _failure_message := "Save failed.") -> void:
	pass


## CDEP steepness advisory prompt before terrain export.
func prompt_cdep_violations(_count: int, _on_fix_callback: Callable) -> void:
	pass


## Export lifecycle, called by exporting workspaces' domain editors.
func on_export_started(_dir_path: String) -> void:
	pass


func on_export_completed(_err: Error, _message: String) -> void:
	pass


## App-close entry (camera Escape): run the unified close guard.
func request_close() -> void:
	pass
