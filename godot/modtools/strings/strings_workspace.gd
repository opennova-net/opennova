class_name StringsEditorWorkspace
extends EditorWorkspace

## ONED workspace for editing RTXT localized string tables (strings/*.bin).
##
## A non-3D data editor. The center viewport host holds one self-contained view
## (StringsEditorView = table + per-entry detail in an HSplit); the left inspector
## holds search / section management / validation / CSV. There is no right asset
## dock, which keeps the detail editor always visible and avoids the shell's
## split-persistence pitfalls. This adapter owns the StringsEditor document and
## fans its two change channels out: `structure_changed` -> rebuild, `edited` ->
## refresh the title only.

const StringsEditorScript = preload("res://modtools/strings/strings_editor.gd")
const StringsEditorViewScript = preload("res://modtools/strings/ui/strings_editor_view.gd")
const StringsInspectorScript = preload("res://modtools/strings/ui/strings_inspector.gd")

## Session state (last open file), restored on activate like the music workspace.
const STATE_PATH := "user://strings_editor_state.cfg"

var strings_editor: StringsEditor

var _mount: ViewportMount
var _view: Control
var _inspector: Control
var _state_restored: bool = false

var _search: String = ""
var _section_filter: int = -1  # -1 = all sections


func set_editor_shell(value: Node) -> void:
	super.set_editor_shell(value)
	_ensure_editor()


# --- Identity ---

func get_workspace_id() -> String:
	return "strings"


func get_workspace_label() -> String:
	return "Strings"


func get_workspace_tooltip() -> String:
	return "Edit localized game string tables."


func get_project_title() -> String:
	return strings_editor.get_project_title() if strings_editor else "Strings"


func get_status_tool() -> String:
	return "Strings"


func get_status_context() -> String:
	return strings_editor.get_status_context() if strings_editor else "No string table loaded"


# --- Lifecycle ---

func activate() -> void:
	_ensure_editor()
	_restore_state()


func deactivate() -> void:
	_save_state()


func _restore_state() -> void:
	# Reopen the last edited table once per session, and only while the document
	# is still pristine (no path, no edits) so it never clobbers user work.
	if _state_restored:
		return
	_state_restored = true
	if strings_editor == null or strings_editor.is_dirty or not strings_editor.current_path.is_empty():
		return
	var cfg := ConfigFile.new()
	if cfg.load(STATE_PATH) != OK:
		return
	var path: String = cfg.get_value("session", "last_path", "")
	if not path.is_empty() and FileAccess.file_exists(path):
		strings_editor.open_strings(path)


func _save_state() -> void:
	if strings_editor == null:
		return
	var cfg := ConfigFile.new()
	cfg.load(STATE_PATH)  # keep unrelated values if the file exists
	cfg.set_value("session", "last_path", strings_editor.current_path)
	cfg.save(STATE_PATH)


func _ensure_editor() -> void:
	if strings_editor != null:
		return
	strings_editor = StringsEditorScript.new()
	strings_editor.name = "StringsEditor"
	if editor_shell != null:
		editor_shell.add_child(strings_editor)
	strings_editor.new_table(false)
	strings_editor.structure_changed.connect(_on_structure_changed)
	strings_editor.edited.connect(_on_edited)


# --- Center: self-contained table + detail view ---

func _ensure_mount() -> ViewportMount:
	if _mount == null:
		_mount = ViewportMount.new(&"StringsEditorView", _create_view)
	return _mount


func _create_view() -> Control:
	_view = StringsEditorViewScript.new()
	return _view


func mount_viewport(host: Control) -> void:
	if host == null:
		return
	_ensure_editor()
	_ensure_mount().mount(host)
	if _view != null:
		_view.set_document(strings_editor)
		_view.set_filter(_search, _section_filter)


func unmount_viewport(_host: Control) -> void:
	if _mount != null:
		_mount.unmount()


func release_viewport() -> void:
	if _mount != null:
		_mount.release()
	_view = null


# --- Left: inspector controls ---

func build_inspector(host: Control) -> void:
	_ensure_editor()
	_inspector = StringsInspectorScript.new()
	host.add_child(_inspector)
	_inspector.setup(self)
	_inspector.refresh()


# --- Coordinator state shared with the views ---

func get_document() -> StringsEditor:
	_ensure_editor()
	return strings_editor


func set_search(text: String) -> void:
	_search = text
	if _view != null:
		_view.set_filter(_search, _section_filter)


func set_section_filter(section_index: int) -> void:
	# Skip redundant re-filters: a structural rebuild already refreshes the table,
	# and the inspector re-applies the same value during its refresh.
	if section_index == _section_filter:
		return
	_section_filter = section_index
	if _view != null:
		_view.set_filter(_search, _section_filter)


func get_section_filter() -> int:
	return _section_filter


func add_entry_default() -> void:
	_ensure_editor()
	var section := _section_filter if _section_filter >= 0 else 0
	if strings_editor.string_table.get_section_count() == 0:
		section = strings_editor.add_section("default")
	strings_editor.add_entry("NEW_KEY", "", section, Vector2i())


func remove_selected_entry() -> void:
	_ensure_editor()
	if strings_editor.selected_index >= 0:
		strings_editor.remove_entry(strings_editor.selected_index)


func _on_structure_changed() -> void:
	if _view != null:
		_view.rebuild()
	if _inspector != null:
		_inspector.refresh()
	_sync_shell_title()


func _on_edited() -> void:
	_sync_shell_title()


func _sync_shell_title() -> void:
	# Cheap: refreshes the project title / action-button state. It does NOT rebuild
	# the inspector (the shell caches it) or remount the viewport.
	if editor_shell != null and editor_shell.has_method("sync_from_editor_state"):
		editor_shell.sync_from_editor_state()


# --- Document actions ---

# The domain document the EditorWorkspace base derives undo/redo + dirty from.
func get_editor_document() -> Object:
	return strings_editor


func can_new() -> bool:
	return true


func get_new_action_label() -> String:
	return "New Strings"


func new_current() -> Error:
	_ensure_editor()
	strings_editor.new_table(true)
	return OK


func can_open() -> bool:
	return true


func get_open_action_label() -> String:
	return "Open Strings..."


func get_open_dialog_title() -> String:
	return "Open strings .bin"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.bin,*.BIN ; Strings"])


func get_open_dialog_dir() -> String:
	return strings_editor.get_last_open_dir() if strings_editor else ""


func get_open_resource_kind() -> String:
	return "strings"


func get_current_resource_path() -> String:
	return strings_editor.current_path if strings_editor else ""


func open_file(path: String) -> Error:
	_ensure_editor()
	var vfs := _vfs_root_for_open(path)
	var err: Error
	if vfs != null:
		err = strings_editor.open_strings_bytes(vfs.read_file(path), _vfs_display_path(vfs, path))
	else:
		err = strings_editor.open_strings(path)
	if err == OK:
		_save_state()
	return err


# Cross-jump target for the Menus workspace's "Edit in Strings": open the given table
# (when not already open) and focus a key by filtering the table to it and selecting
# its entry. Runs before the shell mounts this workspace's viewport, so the search is
# stashed and applied when mount_viewport builds the view. Returns the open Error (OK
# when only focusing an already-open table).
func open_strings_table(path: String, key: String) -> Error:
	_ensure_editor()
	var err: Error = OK
	if not path.is_empty() and path != strings_editor.current_path:
		err = strings_editor.open_strings(path)
		if err != OK:
			return err
	if not key.is_empty():
		set_search(key)
		var idx := strings_editor.string_table.find_entry_by_key(key)
		if idx >= 0:
			strings_editor.selected_index = idx
	return err


func can_save() -> bool:
	return strings_editor != null and strings_editor.is_dirty and not strings_editor.current_path.is_empty()


func get_save_action_label() -> String:
	return "Save Strings"


func can_save_as() -> bool:
	return strings_editor != null


func get_save_as_action_label() -> String:
	return "Save Strings As..."


func save_current() -> Error:
	var err := strings_editor.save_current() if strings_editor else ERR_UNAVAILABLE
	if err == OK:
		_save_state()
	return err


func save_as(dir_path: String) -> Error:
	var err := strings_editor.save_as(dir_path) if strings_editor else ERR_UNAVAILABLE
	if err == OK:
		_save_state()
	return err


func get_save_dialog_title() -> String:
	return "Choose where to save the strings .bin"


func get_save_dialog_dir() -> String:
	return strings_editor.get_last_save_dir() if strings_editor else ""

