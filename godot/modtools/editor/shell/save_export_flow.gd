class_name ShellSaveExportFlow
extends RefCounted

## The shell's document save/open/export flows: the cached native file
## dialogs, the shared unsaved-changes / CDEP / export-flavor confirms, the
## save-then-continue helper, and the document-action dispatch. Dialogs are
## created once as children of the shell Control and given its theme
## explicitly (an embedded Window does not resolve the in-tree theme through
## the Control parent chain).

var _shell: Control
# func() -> EditorWorkspace: the active workspace.
var _active_workspace: Callable
# func(text, duration): the shell's status toast.
var _show_status: Callable
# func(workspace, on_pick): the kind-scoped resource browser (Open action).
var _open_resource_browser: Callable

var _file_dialogs: FileDialogHelper
var _unsaved_dialog: ConfirmationDialog
# The unsaved-changes dialog routes Save/Discard/Cancel to these
# prompt_unsaved_for callables. Consumed (cleared) on first dispatch, so a
# stale callable can never hijack a later prompt.
var _unsaved_on_save := Callable()
var _unsaved_on_discard := Callable()
var _unsaved_on_cancel := Callable()
var _cdep_dialog: ConfirmationDialog
var _cdep_fix_callback: Callable = Callable()
var _export_dialog: ExportFlavorDialog
var _pending_export_dir: String = ""
# The workspace that opened the flavor dialog; the confirm exports it, never
# the tab active at confirmation. Cleared with the dir on confirm and cancel.
var _pending_export_workspace: EditorWorkspace = null


func setup(
	shell: Control,
	active_workspace: Callable,
	show_status: Callable,
	open_resource_browser: Callable
) -> void:
	_shell = shell
	_active_workspace = active_workspace
	_show_status = show_status
	_open_resource_browser = open_resource_browser


# --- Native file dialogs ----------------------------------------------------
# File and directory pickers share one cached native dialog (FileDialogHelper)
# instead of building and freeing a new FileDialog per open.

func _ensure_file_dialogs() -> FileDialogHelper:
	if _file_dialogs == null:
		_file_dialogs = FileDialogHelper.new(_shell)
	return _file_dialogs


func open_file_dialog(title: String, filters: PackedStringArray, on_pick: Callable, current_dir: String = "") -> void:
	_ensure_file_dialogs().open(title, filters, on_pick, current_dir)


func open_dir_dialog(title: String, on_pick: Callable, current_dir: String = "") -> void:
	_ensure_file_dialogs().open_dir(title, on_pick, current_dir)


func open_files_dialog(title: String, filters: PackedStringArray, on_pick: Callable, current_dir: String = "") -> void:
	_ensure_file_dialogs().open_files(title, filters, on_pick, current_dir)


func open_save_as_dialog(workspace: EditorWorkspace, on_success: Callable = Callable(), failure_message: String = "") -> void:
	if workspace == null:
		return
	var report_failure := func(err: Error) -> void:
		var detail := workspace.get_save_failure_message(err)
		if not detail.is_empty():
			_show_status.call(detail, 6.0)
		elif not failure_message.is_empty():
			_show_status.call(failure_message, 6.0)
		else:
			_show_status.call("Save failed (error %d)" % err, 6.0)
	if workspace.uses_save_file_dialog():
		var save_file_as := func(path: String) -> void:
			var err: Error = workspace.save_as_file(path)
			if err == OK:
				if on_success.is_valid():
					on_success.call()
			else:
				report_failure.call(err)
		_ensure_file_dialogs().save_file(
			workspace.get_save_dialog_title(),
			workspace.get_save_file_dialog_filters(),
			workspace.get_save_file_dialog_default_name(),
			save_file_as,
			preferred_save_dir(workspace)
		)
		return
	var save_project_as := func(dir_path: String) -> void:
		var err: Error = workspace.save_as(dir_path)
		if err == OK:
			if on_success.is_valid():
				on_success.call()
		else:
			report_failure.call(err)
	open_dir_dialog(
		workspace.get_save_dialog_title(),
		save_project_as,
		preferred_save_dir(workspace)
	)


# Dialog start dirs come from the workspace hooks alone (get_save_dialog_dir /
# get_export_dialog_dir); an empty result falls back to the OS default.
func preferred_save_dir(workspace: EditorWorkspace = null) -> String:
	if workspace == null:
		workspace = _active_workspace.call()
	if workspace != null:
		return workspace.get_save_dialog_dir()
	return ""


func preferred_export_dir(workspace: EditorWorkspace = null) -> String:
	if workspace == null:
		workspace = _active_workspace.call()
	if workspace != null:
		return workspace.get_export_dialog_dir()
	return ""


# --- Save / export ----------------------------------------------------------

## Saves the workspace's current document, then runs on_done. A document with no
## path yet (ERR_INVALID_PARAMETER) routes through Save As and runs on_done only
## on success; any other failure toasts failure_message and drops on_done.
## Shared by the document-tab close and the workspaces' dirty-replace guards.
func save_then(workspace: EditorWorkspace, on_done: Callable, failure_message := "Save failed.") -> void:
	var err := workspace.save_current()
	if err == OK:
		if on_done.is_valid():
			on_done.call()
		return
	if err == ERR_INVALID_PARAMETER:
		open_save_as_dialog(workspace, on_done, failure_message)
		return
	var detail := workspace.get_save_failure_message(err)
	if not detail.is_empty():
		_show_status.call(detail, 6.0)
	else:
		_show_status.call("%s (error %d)" % [failure_message.trim_suffix("."), err], 6.0)


func on_save_pressed(workspace: EditorWorkspace = null) -> void:
	if workspace == null:
		workspace = _active_workspace.call()
	if workspace == null:
		return
	var err: Error = workspace.save_current()
	if err == ERR_PARSE_ERROR:
		_show_status.call("Resolve source parse errors before saving.", 6.0)
		return
	if err == ERR_INVALID_PARAMETER:
		open_save_as_dialog(workspace)
	elif err != OK:
		var detail := workspace.get_save_failure_message(err)
		if not detail.is_empty():
			_show_status.call(detail, 6.0)
		else:
			_show_status.call("%s save is not available." % workspace.get_workspace_label(), 4.0)


func on_export_pressed(workspace: EditorWorkspace = null) -> void:
	if workspace == null:
		workspace = _active_workspace.call()
	if workspace == null or not workspace.can_export():
		return
	var choose_export_dir := func(dir_path: String) -> void:
		if not workspace.get_export_flavors().is_empty():
			show_export_flavor_dialog(dir_path, workspace)
		else:
			var err: Error = workspace.begin_export(dir_path, 0)
			if err == OK:
				_show_status.call("%s exported." % workspace.get_workspace_label(), 4.0)
			else:
				_show_status.call("Export failed (error %d)" % err, 6.0)
	open_dir_dialog(
		workspace.get_export_dialog_title(),
		choose_export_dir,
		preferred_export_dir(workspace)
	)


func run_workspace_action(workspace: EditorWorkspace, action_id: int) -> void:
	if workspace == null:
		return
	var open_picked := func(path: String) -> void:
		var err: Error = workspace.open_file(path)
		if err != OK:
			_show_status.call("Open failed (error %d)" % err, 6.0)
	match action_id:
		ShellActionBar.Action.NEW:
			if workspace.can_new() and flush_workspace_or_status(workspace):
				workspace.new_current()
		ShellActionBar.Action.OPEN:
			if not workspace.can_open():
				return
			if flush_workspace_or_status(workspace):
				_open_resource_browser.call(workspace, open_picked)
		ShellActionBar.Action.SAVE:
			on_save_pressed(workspace)
		ShellActionBar.Action.SAVE_AS:
			if flush_workspace_or_status(workspace):
				open_save_as_dialog(workspace)
		ShellActionBar.Action.EXPORT:
			on_export_pressed(workspace)


func flush_workspace_or_status(workspace: EditorWorkspace) -> bool:
	if workspace == null:
		return false
	var err := workspace.flush_pending_edits()
	if err != OK:
		_show_status.call("Resolve source parse errors before continuing.", 6.0)
		return false
	return true


# --- Shared confirms ---------------------------------------------------------

## Pops the shared unsaved-changes dialog with caller-supplied outcomes (a
## document tab close: save-then-close / close / keep; a workspace's dirty
## guard before New/Open replaces the document). Pair with save_then() for the
## save outcome so path-less documents route through Save As.
func prompt_unsaved_for(on_save: Callable, on_discard: Callable, on_cancel := Callable()) -> void:
	_unsaved_on_save = on_save
	_unsaved_on_discard = on_discard
	_unsaved_on_cancel = on_cancel
	_ensure_unsaved_dialog()
	_unsaved_dialog.popup_centered()
	_show_status.call("Save or discard your changes to continue.", 6.0)


func _take_unsaved_callable(which: StringName) -> Callable:
	var cb := Callable()
	match which:
		&"save":
			cb = _unsaved_on_save
		&"discard":
			cb = _unsaved_on_discard
		&"cancel":
			cb = _unsaved_on_cancel
	_unsaved_on_save = Callable()
	_unsaved_on_discard = Callable()
	_unsaved_on_cancel = Callable()
	return cb


func prompt_cdep_violations(count: int, on_fix_callback: Callable) -> void:
	var plural := "" if count == 1 else "s"
	_cdep_fix_callback = on_fix_callback
	_ensure_cdep_dialog()
	_cdep_dialog.dialog_text = "%d area%s exceed the JO/DFX limit.\nBHD exports are unaffected." % [count, plural]
	_cdep_dialog.popup_centered()
	_show_status.call("%d area%s too steep for Joint Operations / DFX export." % [count, plural], 6.0)


func _on_prompt_save_changes() -> void:
	var cb := _take_unsaved_callable(&"save")
	if cb.is_valid():
		cb.call()


func _on_prompt_discard_changes() -> void:
	var cb := _take_unsaved_callable(&"discard")
	if cb.is_valid():
		cb.call()


func _on_prompt_keep_editing() -> void:
	var cb := _take_unsaved_callable(&"cancel")
	if cb.is_valid():
		cb.call()


func show_export_flavor_dialog(dir_path: String, workspace: EditorWorkspace = null) -> void:
	_pending_export_dir = dir_path
	# Retain the initiator: the confirm must export the workspace that opened the
	# dialog, not whichever tab is active when OK lands. The one-arg form falls
	# back to the workspace active at open time.
	_pending_export_workspace = workspace if workspace != null else _active_workspace.call()
	_ensure_export_dialog()
	_export_dialog.select_flavor(ExportFlavorDialog.FLAVOR_DFX_JO)
	_export_dialog.popup_centered()


func _on_prompt_export_confirmed() -> void:
	if _pending_export_workspace == null or _pending_export_dir.is_empty():
		return
	var workspace: EditorWorkspace = _pending_export_workspace
	var flavor: int = _export_dialog.get_flavor() if _export_dialog != null else ExportFlavorDialog.FLAVOR_DFX_JO
	var err: Error = workspace.begin_export(_pending_export_dir, flavor)
	_pending_export_dir = ""
	_pending_export_workspace = null
	if err != OK:
		_show_status.call("Export failed (error %d)" % err, 6.0)


func _on_prompt_cancel() -> void:
	_pending_export_dir = ""
	_pending_export_workspace = null


func _ensure_unsaved_dialog() -> void:
	if _unsaved_dialog != null and is_instance_valid(_unsaved_dialog):
		return
	_unsaved_dialog = ConfirmationDialog.new()
	_unsaved_dialog.name = "UnsavedChangesDialog"
	_unsaved_dialog.title = "Unsaved changes"
	_unsaved_dialog.dialog_text = "Save changes?"
	_unsaved_dialog.exclusive = true
	if _shell.theme != null:
		_unsaved_dialog.theme = _shell.theme
	_shell.add_child(_unsaved_dialog)
	_unsaved_dialog.get_ok_button().text = "Save"
	_unsaved_dialog.get_cancel_button().text = "Cancel"
	_unsaved_dialog.add_button("Discard", false, "discard")
	# OK confirms (Save), Cancel/Escape keeps editing, the custom Discard button
	# discards. Custom-action buttons do not auto-hide, so the handler hides it.
	_unsaved_dialog.confirmed.connect(_on_prompt_save_changes)
	_unsaved_dialog.canceled.connect(_on_prompt_keep_editing)
	_unsaved_dialog.custom_action.connect(_on_unsaved_custom_action)


func _on_unsaved_custom_action(action: StringName) -> void:
	if action == &"discard":
		if _unsaved_dialog != null:
			_unsaved_dialog.hide()
		_on_prompt_discard_changes()


func _ensure_cdep_dialog() -> void:
	if _cdep_dialog != null and is_instance_valid(_cdep_dialog):
		return
	_cdep_dialog = ConfirmationDialog.new()
	_cdep_dialog.name = "CdepFlattenDialog"
	_cdep_dialog.title = "Flatten before export?"
	_cdep_dialog.exclusive = true
	if _shell.theme != null:
		_cdep_dialog.theme = _shell.theme
	_shell.add_child(_cdep_dialog)
	_cdep_dialog.get_ok_button().text = "Flatten automatically"
	_cdep_dialog.get_cancel_button().text = "Leave as-is"
	_cdep_dialog.confirmed.connect(_on_cdep_flatten_confirmed)


func _on_cdep_flatten_confirmed() -> void:
	if _cdep_fix_callback.is_valid():
		_cdep_fix_callback.call()


func _ensure_export_dialog() -> void:
	if _export_dialog != null and is_instance_valid(_export_dialog):
		return
	_export_dialog = ExportFlavorDialog.new()
	_export_dialog.name = "ExportFlavorDialog"
	_export_dialog.exclusive = true
	if _shell.theme != null:
		_export_dialog.theme = _shell.theme
	_shell.add_child(_export_dialog)
	_export_dialog.confirmed.connect(_on_prompt_export_confirmed)
	_export_dialog.canceled.connect(_on_prompt_cancel)
