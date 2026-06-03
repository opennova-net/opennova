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
const SectionNavigatorScene = preload("res://modtools/music/ui/section_navigator.tscn")
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


# The shell renders workflows as typed InspectorDef rows (_rebuild_workflow_rail
# does `entry as InspectorDef`), so expose them via the framework base's
# get_workflows() -> _ensure_inspector_defs() path. Labels/tooltips stay
# single-sourced with WORKFLOW_DEFS (also used by get_status_tool). No
# per-workflow inspector script: build_workflow_inspector() is overridden below to
# mount the section navigator directly, so the script arg is null.
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


func has_unsaved_changes() -> bool:
	return _document != null and _document.is_dirty()


# --- Edit: undo / redo capability hooks (matching Mission / MNU). One unified
# screen means one consolidated edit history on the document: bank reorder /
# rename (and the structured play edits added later) all push do/undo pairs onto
# the same stack, reachable regardless of which dock has focus. The Bank panel
# repaints off the document's `changed` signal, which the undo callables emit,
# so the track list refreshes without extra wiring here.
func can_undo() -> bool:
	return _document != null and _document.can_undo_bank()


func can_redo() -> bool:
	return _document != null and _document.can_redo_bank()


func undo() -> void:
	if _document != null:
		_document.undo_bank()


func redo() -> void:
	if _document != null:
		_document.redo_bank()


func get_current_resource_path() -> String:
	if _document == null:
		return ""
	if not _document.bank_path.is_empty():
		return _document.bank_path
	return _document.script_path


func new_current() -> Error:
	if _document == null:
		return ERR_UNAVAILABLE
	_document.close_pair()
	return OK


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
		if editor_shell != null:
			editor_shell.add_child(dialog)
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


func mount_viewport(host: Control) -> void:
	if host == null:
		return
	if _root == null:
		_root = RootScene.instantiate()
	if _root.get_parent() != host:
		if _root.get_parent() != null:
			_root.get_parent().remove_child(_root)
		host.add_child(_root)
	_root.bind_document(_document)
	if _root.has_signal("workflow_requested"):
		if not _root.workflow_requested.is_connected(activate_workflow):
			_root.workflow_requested.connect(activate_workflow)
	_root.set_active_workflow(_active_workflow)


func unmount_viewport(_host: Control) -> void:
	if _root != null:
		_root.queue_free()
		_root = null


# Inspector host is the workstation's middle column. The unified screen always
# shows the section TOC there (clicking a section scrolls the Advanced script
# drawer to it); there are no longer separate modes to gate it on.
func build_workflow_inspector(_workflow_id: int, host: Control) -> void:
	if host == null:
		return
	for c in host.get_children():
		c.queue_free()
	var nav: Control = SectionNavigatorScene.instantiate()
	host.add_child(nav)
	if nav.has_method("bind_document"):
		nav.bind_document(_document)
	# Connect the navigator's selection so clicking a section in the TOC scrolls
	# the Script panel to it. The signal was previously emitted but never
	# connected, leaving the navigator inert.
	if nav.has_signal("section_selected"):
		if not nav.section_selected.is_connected(_on_section_selected):
			nav.section_selected.connect(_on_section_selected)


func _on_section_selected(section_name: StringName) -> void:
	if _root == null:
		return
	# Route through the Live screen so the Advanced drawer opens before its
	# CodeEdit is scrolled (scrolling the hidden drawer did nothing and errored
	# on grab_focus). Fall back to a direct scroll if the method is unavailable.
	var live_node: Node = _root.get_panel("Live")
	if live_node != null and live_node.has_method("reveal_section_in_script"):
		live_node.reveal_section_in_script(section_name)
		return
	var script_node: Node = _root.get_panel("Script")
	if script_node != null and script_node.has_method("scroll_to_section"):
		script_node.scroll_to_section(section_name)
