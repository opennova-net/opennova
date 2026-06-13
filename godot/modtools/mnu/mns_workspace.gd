class_name MnsWorkspace
extends EditorWorkspace

# Adapter for the Menu Styles workspace: the single shared .mns stylesheet
# (canonically menu_style.mns) every menu refers to as %NAME%. Single-document
# (the fonts shape, no tabs): the engine resolves exactly one stylesheet, and
# Save As covers variants. The main viewport hosts the MnsEditor (variable
# table / source view / live menu preview); the right dock hosts MnsInspector.

const MnsEditorDocumentScript = preload("res://modtools/mnu/mns_editor_document.gd")
const MnsEditorScript = preload("res://modtools/mnu/mns_editor.gd")
const MnsInspectorScript = preload("res://modtools/mnu/mns_inspector.gd")

# The fixed name the original engine looks for ("named menu_style.mns for the
# game to find it"); also the auto-open and Save-As default.
const CANONICAL_FILE := "menu_style.mns"

var _document   # MnsEditorDocument
var _editor: Control
var _inspector: Control
# Auto-open runs once per session so a deliberately closed stylesheet is not
# re-opened on the next activate.
var _auto_opened := false


func _init() -> void:
	_document = MnsEditorDocumentScript.new()


func get_workspace_id() -> String:
	return "mns"


func get_workspace_label() -> String:
	return "Menu Styles"


func get_workspace_tooltip() -> String:
	return "Edit the menu stylesheet (*.mns): the named colors, fonts, and pictures every menu screen refers to."


func get_project_title() -> String:
	var name := "untitled"
	if not _document.current_path.is_empty():
		name = _document.current_path.get_file().get_basename()
	var dirty := "*" if _document.is_dirty else ""
	return "%s%s" % [name, dirty]


func get_status_tool() -> String:
	return "Menu Styles"


func get_status_context() -> String:
	if _document.resource == null:
		return ""
	var context := "%d variable(s)" % (_editor.get_variable_count() if _editor != null else 0)
	var issues: int = _editor.get_diagnostic_count() if _editor != null else 0
	if issues > 0:
		context += ", %d issue(s)" % issues
	return context


# First activation auto-opens the canonical stylesheet when the resource root
# carries one and the workspace is still pristine (never clobbers user work).
func activate() -> void:
	if _auto_opened:
		return
	_auto_opened = true
	if _document.is_dirty or not _document.current_path.is_empty():
		return
	var root := _resource_root()
	if root == null or root.get_root_dir().is_empty():
		return
	var path := String(root.resolve_file(CANONICAL_FILE))
	if not path.is_empty():
		open_file(path)
	elif root.has_file(CANONICAL_FILE):
		open_file(CANONICAL_FILE)


func mount_viewport(host: Control) -> void:
	if host == null:
		return
	if _editor == null:
		_editor = MnsEditorScript.new()
		_editor.set_anchors_preset(Control.PRESET_FULL_RECT)
		_editor.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_editor.size_flags_vertical = Control.SIZE_EXPAND_FILL
		_editor.variable_selected.connect(_on_variable_selected)
	if _editor.get_parent() == null:
		host.add_child(_editor)
		_editor.set_anchors_preset(Control.PRESET_FULL_RECT)
	_editor.set_resource_root(_resource_root())
	_editor.set_menu_list_provider(_list_menu_names)
	_editor.set_document(_document)


func unmount_viewport(_host: Control) -> void:
	if _editor != null and _editor.get_parent() != null:
		_editor.get_parent().remove_child(_editor)


func release_viewport() -> void:
	if _inspector != null and is_instance_valid(_inspector):
		_disconnect_inspector(_inspector)
		_inspector.queue_free()
		_inspector = null
	if _editor != null and _editor.get_parent() != null:
		_editor.get_parent().remove_child(_editor)
	if _editor != null:
		_editor.free()
		_editor = null


func build_inspector(host: Control) -> void:
	if _inspector != null and is_instance_valid(_inspector):
		_disconnect_inspector(_inspector)
		_inspector.queue_free()
	_inspector = MnsInspectorScript.new()
	_inspector.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_inspector.size_flags_vertical = Control.SIZE_EXPAND_FILL
	# Row commits funnel through the editor so mutation + undo stay centralized.
	_inspector.edit_requested.connect(_on_inspector_edit)
	_inspector.font_jump_requested.connect(_on_font_jump)
	_inspector.add_requested.connect(_on_add_variable)
	_inspector.set_shell(editor_shell)
	_inspector.set_font_names(_list_font_names())
	host.add_child(_inspector)
	_populate_inspector()


func _populate_inspector() -> void:
	if _inspector == null or not is_instance_valid(_inspector):
		return
	var selected: String = _editor.get_selected_variable() if _editor != null else ""
	if selected.is_empty():
		_inspector.show_none(_document.resource)
	else:
		_inspector.show_variable(_document.resource, selected)


func _on_variable_selected(_name: String) -> void:
	_populate_inspector()


func _on_inspector_edit(edit: Dictionary) -> void:
	if _editor != null and is_instance_valid(_editor):
		_editor.apply_edit(edit)


func _on_font_jump(font_name: String) -> void:
	if editor_shell != null and editor_shell.has_method("open_font_workspace"):
		editor_shell.open_font_workspace(font_name)


func _on_add_variable() -> void:
	if _editor != null and is_instance_valid(_editor):
		_editor._on_add_pressed()


# Every recognized menu in the resource folder, for the preview's picker.
func _list_menu_names() -> PackedStringArray:
	var root := _resource_root()
	if root == null:
		return PackedStringArray()
	return root.list_files(".mnu")


# Font basenames for the inspector's font pick menu.
func _list_font_names() -> PackedStringArray:
	var root := _resource_root()
	if root == null:
		return PackedStringArray()
	var out := PackedStringArray()
	for path_value in root.list_files(".fnt"):
		out.append(String(path_value).get_file())
	return out


func _disconnect_inspector(inspector: Control) -> void:
	if inspector.edit_requested.is_connected(_on_inspector_edit):
		inspector.edit_requested.disconnect(_on_inspector_edit)
	if inspector.font_jump_requested.is_connected(_on_font_jump):
		inspector.font_jump_requested.disconnect(_on_font_jump)
	if inspector.add_requested.is_connected(_on_add_variable):
		inspector.add_requested.disconnect(_on_add_variable)


func has_unsaved_changes() -> bool:
	return _document.is_dirty


func can_new() -> bool:
	return true


func get_new_action_label() -> String:
	return "New Stylesheet"


func new_current() -> Error:
	return _document.create_new()


func can_open() -> bool:
	return true


func get_open_action_label() -> String:
	return "Open Stylesheet..."


func get_open_dialog_title() -> String:
	return "Open .mns"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.mns,*.MNS ; Nova menu styles"])


func get_open_dialog_dir() -> String:
	return _document.get_last_open_dir()


# "menu_style" is the resource-index kind for *.mns, so Open uses the shared
# indexed quick-open browser like the other workspaces.
func get_open_resource_kind() -> String:
	return "menu_style"


func get_current_resource_path() -> String:
	return _document.current_path


func open_file(path: String) -> Error:
	# A name picked from the resource browser may not be a loose path; the
	# shared VFS branch reads it by name through the mounted root.
	var vfs := _vfs_root_for_open(path)
	if vfs != null:
		return _document.open_mns_bytes(vfs.read_file(path), _vfs_display_path(vfs, path))
	return _document.open_mns(path)


# Cross-jump focus hook: {"variable": String} selects the named variable.
func focus_reference(focus: Dictionary) -> Error:
	var name := String(focus.get("variable", "")).strip_edges()
	if name.is_empty():
		return OK
	if _editor == null or not is_instance_valid(_editor):
		return ERR_UNAVAILABLE
	return OK if _editor.select_variable(name) else ERR_DOES_NOT_EXIST


func can_save() -> bool:
	return _document.is_dirty and not _document.current_path.is_empty()


func get_save_action_label() -> String:
	return "Save Stylesheet"


func can_save_as() -> bool:
	return _document.resource != null


func get_save_as_action_label() -> String:
	return "Save Stylesheet As..."


func save_current() -> Error:
	# The shell's dirty-close Save flow routes to Save As only on
	# ERR_INVALID_PARAMETER (the mnu workspace rationale).
	if _document.current_path.is_empty():
		return ERR_INVALID_PARAMETER
	return _after_save(_document.save_current())


func save_as(dir_path: String) -> Error:
	return _after_save(_document.save_as(dir_path))


# A save that CREATES the stylesheet inside the resource root must rescan it:
# the VFS name index is built once at mount, so the new file would otherwise
# stay invisible to by-name reads (the Menus preview, the quick-open browser,
# runtime parity) until the next mount.
func _after_save(err: Error) -> Error:
	if err != OK:
		return err
	var root := _resource_root()
	if root != null and not _document.current_path.is_empty():
		var name := _document.current_path.get_file()
		if not root.has_file(name) and editor_shell != null \
				and editor_shell.has_method("rescan_resource_root"):
			editor_shell.rescan_resource_root()
	return err


func get_save_dialog_title() -> String:
	return "Choose where to save the stylesheet"


# Default to the resource root: a loose menu_style.mns there shadows any
# PFF-archived one at runtime, which is the canonical modder flow.
func get_save_dialog_dir() -> String:
	var last := _document.get_last_save_dir()
	if not last.is_empty():
		return last
	var root := _resource_root()
	return root.get_root_dir() if root != null else ""


# Undo lives in the editor (shared by table rows, inspector commits, and the
# Source view) while dirty stays on the document, so has_unsaved_changes keeps
# its override and the base derives undo/redo from the editor control.
func get_editor_document() -> Object:
	return _editor


func flush_pending_edits() -> Error:
	if _editor != null and is_instance_valid(_editor):
		return _editor.flush_pending_edits()
	return OK
