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

var strings_editor: StringsEditor

var _mount: ViewportMount
var _view: Control
var _inspector: Control

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


func deactivate() -> void:
	pass


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

func has_unsaved_changes() -> bool:
	return strings_editor != null and strings_editor.is_dirty


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
	var resources := _resource_root()
	if not FileAccess.file_exists(path) and resources != null and resources.has_file(path):
		return strings_editor.open_strings_bytes(resources.read_file(path), resources.get_root_dir().path_join(path.get_file()))
	return strings_editor.open_strings(path)


func _resource_root() -> NovaResourceRoot:
	if editor_shell != null and editor_shell.has_method("get_resource_root"):
		return editor_shell.get_resource_root()
	return null


func can_save() -> bool:
	return strings_editor != null and strings_editor.is_dirty and not strings_editor.current_path.is_empty()


func get_save_action_label() -> String:
	return "Save Strings"


func can_save_as() -> bool:
	return strings_editor != null


func get_save_as_action_label() -> String:
	return "Save Strings As..."


func save_current() -> Error:
	return strings_editor.save_current() if strings_editor else ERR_UNAVAILABLE


func save_as(dir_path: String) -> Error:
	return strings_editor.save_as(dir_path) if strings_editor else ERR_UNAVAILABLE


func get_save_dialog_title() -> String:
	return "Choose where to save the strings .bin"


func get_save_dialog_dir() -> String:
	return strings_editor.get_last_save_dir() if strings_editor else ""


# --- Undo / redo ---

func can_undo() -> bool:
	return strings_editor != null and strings_editor.can_undo()


func can_redo() -> bool:
	return strings_editor != null and strings_editor.can_redo()


func undo() -> void:
	if strings_editor:
		strings_editor.undo()


func redo() -> void:
	if strings_editor:
		strings_editor.redo()
