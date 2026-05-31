class_name MnuEditorWorkspace
extends EditorWorkspace

# Single-pane adapter for the Menus workspace (mirrors fonts_workspace.gd). The
# main viewport hosts the MnuEditor (widget tree + WYSIWYG preview); the right
# dock hosts the read-only property inspector. The adapter forwards the editor's
# selection to the inspector so the two shell regions stay in sync. M6 is
# read-only browse; M7 adds property edit + undo, M8 adds canvas gestures.

const MnuEditorDocumentScript = preload("res://modtools/mnu/mnu_editor_document.gd")
const MnuEditorScript = preload("res://modtools/mnu/mnu_editor.gd")
const MnuPropertyInspectorScript = preload("res://modtools/mnu/mnu_property_inspector.gd")

var _document   # MnuEditorDocument
var _editor: Control
var _inspector: Control
var _selected_id := -1


func _init() -> void:
	_document = MnuEditorDocumentScript.new()


func get_workspace_id() -> String:
	return "mnu"


func get_workspace_label() -> String:
	return "Menus"


func get_workspace_tooltip() -> String:
	return "Open and preview Nova *.mnu menu screens: widget tree and layout."


func get_project_title() -> String:
	var name := "untitled"
	if not _document.current_path.is_empty():
		name = _document.current_path.get_file().get_basename()
	var dirty := "*" if _document.is_dirty else ""
	return "%s%s" % [name, dirty]


func get_status_tool() -> String:
	return "Menus"


func get_status_context() -> String:
	if _document.resource == null:
		return ""
	var screens: int = _document.resource.get_screen_count()
	var context := "%d screen(s)" % screens
	if _editor != null:
		var unresolved: int = _editor.get_unresolved_asset_count()
		if unresolved > 0:
			context += ", %d unresolved asset(s)" % unresolved
	return context


func mount_viewport(host: Control) -> void:
	if host == null:
		return
	if _editor == null:
		_editor = MnuEditorScript.new()
		_editor.set_anchors_preset(Control.PRESET_FULL_RECT)
		_editor.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_editor.size_flags_vertical = Control.SIZE_EXPAND_FILL
		_editor.widget_selected.connect(_on_widget_selected)
	if _editor.get_parent() == null:
		host.add_child(_editor)
		_editor.set_anchors_preset(Control.PRESET_FULL_RECT)
	_editor.set_resource_root(_resource_root())
	_editor.set_document(_document)


func unmount_viewport(_host: Control) -> void:
	if _editor != null and _editor.get_parent() != null:
		_editor.get_parent().remove_child(_editor)


func release_viewport() -> void:
	if _inspector != null and is_instance_valid(_inspector):
		_inspector.queue_free()
		_inspector = null
	if _editor != null and _editor.get_parent() != null:
		_editor.get_parent().remove_child(_editor)
	if _editor != null:
		_editor.free()
		_editor = null


func build_inspector(host: Control) -> void:
	if _inspector != null and is_instance_valid(_inspector):
		_inspector.queue_free()
	_inspector = MnuPropertyInspectorScript.new()
	_inspector.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_inspector.size_flags_vertical = Control.SIZE_EXPAND_FILL
	host.add_child(_inspector)
	# Populate from the editor's current selection. Subsequent selection changes
	# (user + document reloads, which re-select the first screen) reach the
	# inspector through the editor's widget_selected push (_on_widget_selected), so
	# no document-signal subscription is needed here.
	_populate_inspector()


func _populate_inspector() -> void:
	if _inspector == null or not is_instance_valid(_inspector):
		return
	if _editor != null:
		_selected_id = _editor.get_selected_id()
	_inspector.show_widget(_document.resource, _selected_id)


func _on_widget_selected(id: int) -> void:
	_selected_id = id
	if _inspector != null and is_instance_valid(_inspector):
		_inspector.show_widget(_document.resource, id)


func _resource_root() -> NovaResourceRoot:
	if editor_shell != null and editor_shell.has_method("get_resource_root"):
		return editor_shell.get_resource_root()
	var dir := NovaResourceDirSettings.get_resource_dir()
	if dir.is_empty():
		return null
	var resources := NovaResourceRoot.new()
	return resources if resources.set_root_dir(dir) == OK else null


func has_unsaved_changes() -> bool:
	return _document.is_dirty


func can_new() -> bool:
	return true


func get_new_action_label() -> String:
	return "New Menu"


func new_current() -> Error:
	return _document.create_new()


func can_open() -> bool:
	return true


func get_open_action_label() -> String:
	return "Open Menu..."


func get_open_dialog_title() -> String:
	return "Open .mnu"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.mnu,*.MNU ; Nova menus"])


func get_open_dialog_dir() -> String:
	return _document.get_last_open_dir()


# Empty kind: the shell's resource browser falls back to a native *.mnu file
# dialog. Indexed menu browsing is a future increment (the resource index has no
# menu kind yet).
func get_open_resource_kind() -> String:
	return ""


func get_current_resource_path() -> String:
	return _document.current_path


func open_file(path: String) -> Error:
	return _document.open_mnu(path)


func can_save() -> bool:
	return _document.is_dirty and not _document.current_path.is_empty()


func get_save_action_label() -> String:
	return "Save Menu"


func can_save_as() -> bool:
	return _document.resource != null


func get_save_as_action_label() -> String:
	return "Save Menu As..."


func save_current() -> Error:
	return _document.save_current()


func save_as(dir_path: String) -> Error:
	return _document.save_as(dir_path)


func get_save_dialog_title() -> String:
	return "Choose where to save the menu"


func get_save_dialog_dir() -> String:
	return _document.get_last_save_dir()
