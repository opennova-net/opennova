class_name MusicEditorWorkspace
extends EditorWorkspace

# One unified screen now: the live-lit section map with docked Tracks / Game
# Dials and a per-state Advanced script drawer. The old Bank / Script / Live
# modes are coexisting docks within this single "Map" workflow.
enum Workflow { MAP }

const WORKFLOW_DEFS := [
	{"id": Workflow.MAP, "label": "Map", "tooltip": "The live music map: states, tracks, game dials, and the script drawer on one screen."},
]

const RootScene = preload("res://modtools/music/ui/music_workspace_root.tscn")
const MusicEditorDocumentClass = preload("res://modtools/music/music_editor_document.gd")

const STATE_PATH := "user://music_editor_state.cfg"

var _active_workflow: int = Workflow.MAP
var _root: Control
var _document: RefCounted   # MusicEditorDocument


func _init(_arg = null) -> void:
	# Ignore the optional argument; later phases may take an editor handle.
	_document = MusicEditorDocumentClass.new()


func get_workspace_id() -> String:
	return "music"


func get_workspace_label() -> String:
	return "Music"


func get_project_title() -> String:
	return "Music"


func get_status_tool() -> String:
	return WORKFLOW_DEFS[_active_workflow]["label"]


func uses_asset_dock() -> bool:
	return false


# Music presents its whole UI in the viewport (the unified live screen). The
# section map already indexes every section as a live-lit, clickable node, so
# the shell's left lane (a single-choice "Map" picker + a redundant section TOC)
# is dead weight -- opt out so the unified screen gets that width.
func uses_left_lane() -> bool:
	return false


# The shell renders workflows as typed InspectorDef rows (_rebuild_workflow_rail
# does `entry as InspectorDef`), so expose them via the framework base's
# get_workflows() -> _ensure_inspector_defs() path. Labels/tooltips stay
# single-sourced with WORKFLOW_DEFS (also used by get_status_tool). The single
# "Map" workflow drives no left-lane picker (uses_left_lane is false), so the
# inspector script arg is null and build_workflow_inspector() is a no-op.
func _build_inspector_defs() -> Array:
	var defs: Array = []
	for wf in WORKFLOW_DEFS:
		defs.append(InspectorDef.make(int(wf["id"]), String(wf["label"]), String(wf["tooltip"]), null))
	return defs


func get_active_workflow_id() -> int:
	return _active_workflow


func activate_workflow(workflow_id: int) -> void:
	# Single screen: nothing to show/hide. Recorded for get_status_tool /
	# get_active_workflow_id. The live VM is stopped on workspace deactivate
	# (see _stop_live) rather than on a tab switch -- there are no tabs.
	_active_workflow = workflow_id
	if _root != null:
		_root.set_active_workflow(workflow_id)


func has_new_action() -> bool:
	return true


func has_open_action() -> bool:
	return true


func has_save_action() -> bool:
	return true


func has_save_as_action() -> bool:
	return true


func has_export_action() -> bool:
	return false


func get_new_action_label() -> String:
	return "New"


func get_open_action_label() -> String:
	return "Open"


func get_save_action_label() -> String:
	return "Save"


func get_save_as_action_label() -> String:
	return "Save As"


func can_new() -> bool:
	return true


func can_open() -> bool:
	return true


func can_save() -> bool:
	if _document == null:
		return false
	if not (_document.bank_loaded() or _document.script_loaded()):
		return false
	# Prefer to enable Save once anything is loaded; the workstation falls back
	# to Save As when save_current returns ERR_INVALID_PARAMETER.
	return true


func can_save_as() -> bool:
	return _document != null and (_document.bank_loaded() or _document.script_loaded())


func get_open_resource_kind() -> String:
	# "music" is the umbrella kind the resource index resolves to .sbf banks and
	# .bin (SCR0) scripts, so Open uses the shared indexed quick-open browser like
	# the other workspaces. When no resource directory is configured the browser
	# shows its empty-state + Settings shortcut (same as terrain/fonts/strings).
	return "music"


func get_open_dialog_title() -> String:
	return "Open music project"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray([
		"*.sbf, *.bin ; Music project (.sbf / .bin)",
		"*.sbf ; SBF audio bank",
		"*.bin ; Music script",
	])


func get_open_dialog_dir() -> String:
	if _document != null and not _document.bank_path.is_empty():
		return _document.bank_path.get_base_dir()
	return ""


func get_save_dialog_title() -> String:
	return "Choose where to save the music project"


func get_save_dialog_dir() -> String:
	if _document != null and not _document.bank_path.is_empty():
		return _document.bank_path.get_base_dir()
	return ""


# The domain document the EditorWorkspace base derives undo/redo + dirty from.
func get_editor_document() -> Object:
	return _document


# One unified screen means one consolidated edit history on the document: bank
# reorder / rename and the structured play edits all push do/undo pairs onto the
# same stack, reachable regardless of which dock has focus. The Bank panel
# repaints off the document's `changed` signal, which the undo callables emit.


func get_current_resource_path() -> String:
	if _document == null:
		return ""
	if not _document.bank_path.is_empty():
		return _document.bank_path
	return _document.script_path


func new_current() -> Error:
	if _document == null:
		return ERR_UNAVAILABLE
	# Mint a fresh script + empty bank so the user can author from scratch (the
	# old behaviour just cleared the workspace, leaving a dead "Open a project
	# first" screen with no way to make one). The shell already flushed unsaved
	# changes before calling this.
	return _document.new_project()


func open_file(path: String) -> Error:
	if _document == null:
		return ERR_UNAVAILABLE
	if path.is_empty():
		return ERR_INVALID_PARAMETER
	return _document.open_pair(path)


func save_current() -> Error:
	if _document == null:
		return ERR_UNAVAILABLE
	if _document.bank_path.is_empty() and _document.script_path.is_empty():
		# Tell the workstation to fall back to Save As.
		return ERR_INVALID_PARAMETER
	return _document.save_to_disk()


func save_as(dir_path: String) -> Error:
	if _document == null:
		return ERR_UNAVAILABLE
	if dir_path.is_empty():
		return ERR_INVALID_PARAMETER
	# Re-derive paths from the existing basenames; fall back to "untitled".
	var base := _basename_for_save_as()
	if _document.bank_loaded():
		_document.bank_path = "%s/%s.sbf" % [dir_path, base]
	if _document.script_loaded():
		_document.script_path = "%s/%s.bin" % [dir_path, base]
	return _document.save_to_disk()


func _basename_for_save_as() -> String:
	if _document == null:
		return "untitled"
	if not _document.bank_path.is_empty():
		return _document.bank_path.get_file().get_basename()
	if not _document.script_path.is_empty():
		return _document.script_path.get_file().get_basename()
	# Brand-new project (no paths yet): name the pair after the script chunk
	# (e.g. gamescript.sbf / gamescript.bin) instead of a bare "untitled".
	if _document.script_loaded():
		var n := String(_document.mus_script.get_default_script_name())
		if n != "":
			return n
	return "untitled"


func activate() -> void:
	_load_state()


func deactivate() -> void:
	# Leaving the workspace stops the live music so it doesn't keep playing in
	# the background (single screen: there's no Live tab to return to).
	_stop_live()
	if _document.is_dirty():
		var dialog := ConfirmationDialog.new()
		dialog.dialog_text = "Music project has unsaved changes. Save before closing?"
		dialog.add_button("Discard", true, "discard")
		dialog.confirmed.connect(func():
			_document.save_to_disk()
			_save_state()
			dialog.queue_free()
		)
		dialog.custom_action.connect(func(action: StringName):
			if action == "discard":
				_save_state()
			dialog.queue_free()
		)
		# Cancel still persists the current open-pair / active-workflow so the
		# user's project state survives bouncing in and out of the workspace.
		# Discard already saves; only Cancel was previously a silent drop.
		dialog.canceled.connect(func():
			_save_state()
			dialog.queue_free()
		)
		_mount_under_shell(dialog)
		dialog.popup_centered()
		return
	_save_state()


# Stop the live director if it's running. Called on workspace deactivate.
func _stop_live() -> void:
	if _root == null:
		return
	var live_node: Node = _root.get_panel("Live")
	if live_node != null and live_node.has_method("stop_director"):
		live_node.stop_director()


func _save_state() -> void:
	var cfg := ConfigFile.new()
	cfg.set_value("project", "bank_path", _document.bank_path)
	cfg.set_value("project", "script_path", _document.script_path)
	cfg.set_value("workflow", "active", _active_workflow)
	cfg.save(STATE_PATH)


func _load_state() -> void:
	var cfg := ConfigFile.new()
	if cfg.load(STATE_PATH) != OK:
		return
	var bank_path: String = cfg.get_value("project", "bank_path", "")
	var script_path: String = cfg.get_value("project", "script_path", "")
	if bank_path != "" and FileAccess.file_exists(bank_path):
		_document.open_bank(bank_path)
	if script_path != "" and FileAccess.file_exists(script_path):
		_document.open_script(script_path)
	_active_workflow = cfg.get_value("workflow", "active", 0)
	if _root != null:
		_root.set_active_workflow(_active_workflow)


func mount_viewport(mount: Control) -> void:
	if mount == null:
		return
	if _root == null:
		_root = RootScene.instantiate()
		# Bank import failures relayed by the root become shell toasts (the
		# console line stays at the source — B10's deferred reroutes).
		_root.error_reported.connect(_on_root_error)
	if _root.get_parent() != mount:
		if _root.get_parent() != null:
			_root.get_parent().remove_child(_root)
		mount.add_child(_root)
	_root.bind_document(_document)
	if _root.has_signal("workflow_requested"):
		if not _root.workflow_requested.is_connected(activate_workflow):
			_root.workflow_requested.connect(activate_workflow)
	_root.set_active_workflow(_active_workflow)


func _on_root_error(message: String) -> void:
	_notify_status(message, &"error")


func unmount_viewport(_released: Control) -> void:
	if _root != null:
		_root.queue_free()
		_root = null


# Music opts out of the shell left lane (uses_left_lane == false), so there is no
# inspector mount to populate. Explicit no-op: the live section map IS the section
# index (single-click selects/jumps, double-click opens its blueprint, right-click
# renames/deletes).
func build_workflow_inspector(_workflow_id: int, _mount: Control) -> void:
	pass
