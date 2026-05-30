class_name FontsEditorWorkspace
extends EditorWorkspace

const FntEditorDocument = preload("res://modtools/fonts/fnt_editor_document.gd")
const FntEditorScript = preload("res://modtools/fonts/fnt_editor.gd")
const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")

var _document: FntEditorDocument
var _editor: Control
var _inspector_root: Control


func _init() -> void:
	_document = FntEditorDocument.new()


func get_workspace_id() -> String:
	return "fonts"


func get_workspace_label() -> String:
	return "Fonts"


func get_workspace_tooltip() -> String:
	return "Edit Nova *.fnt bitmap fonts: glyphs, pages, and shadow offset."


func get_project_title() -> String:
	var name := "untitled"
	if not _document.current_path.is_empty():
		name = _document.current_path.get_file().get_basename()
	var dirty := "*" if _document.is_dirty else ""
	return "%s%s" % [name, dirty]


func get_status_tool() -> String:
	return "Fonts"


func get_status_context() -> String:
	if _document.resource == null:
		return ""
	return "%d page(s), %d glyphs" % [
		_document.resource.get_page_count(),
		_document.resource.get_glyph_count(),
	]


func mount_viewport(host: Control) -> void:
	if host == null:
		return
	if _editor == null:
		_editor = FntEditorScript.new()
		_editor.set_anchors_preset(Control.PRESET_FULL_RECT)
		_editor.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_editor.size_flags_vertical = Control.SIZE_EXPAND_FILL
	if _editor.get_parent() == null:
		host.add_child(_editor)
		_editor.set_anchors_preset(Control.PRESET_FULL_RECT)
	_editor.set_document(_document)


func unmount_viewport(_host: Control) -> void:
	if _editor != null and _editor.get_parent() != null:
		_editor.get_parent().remove_child(_editor)


func release_viewport() -> void:
	if _document != null and _document.state_changed.is_connected(_populate_inspector):
		_document.state_changed.disconnect(_populate_inspector)
	if _inspector_root != null and is_instance_valid(_inspector_root):
		_inspector_root.queue_free()
		_inspector_root = null
	if _editor != null and _editor.get_parent() != null:
		_editor.get_parent().remove_child(_editor)
	if _editor != null:
		_editor.free()
		_editor = null


func build_inspector(host: Control) -> void:
	if _inspector_root != null and is_instance_valid(_inspector_root):
		_inspector_root.queue_free()
	_inspector_root = _make_inspector()
	host.add_child(_inspector_root)
	_populate_inspector()
	if not _document.state_changed.is_connected(_populate_inspector):
		_document.state_changed.connect(_populate_inspector)


func _make_inspector() -> Control:
	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 12)
	margin.add_theme_constant_override("margin_right", 12)
	margin.add_theme_constant_override("margin_top", 12)
	margin.add_theme_constant_override("margin_bottom", 12)
	margin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.size_flags_vertical = Control.SIZE_EXPAND_FILL

	var box := VBoxContainer.new()
	box.name = "Box"
	box.add_theme_constant_override("separation", 8)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.add_child(box)

	var font_label := Label.new()
	font_label.name = "FontLabel"
	font_label.theme_type_variation = &"Heading"
	font_label.clip_text = true
	box.add_child(font_label)

	var meta_label := Label.new()
	meta_label.name = "MetaLabel"
	meta_label.theme_type_variation = &"Muted"
	meta_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(meta_label)

	return margin


func _populate_inspector() -> void:
	if _inspector_root == null or not is_instance_valid(_inspector_root):
		return
	var box := _inspector_root.get_node_or_null("Box")
	if box == null:
		return
	var font_label: Label = box.get_node("FontLabel")
	var meta_label: Label = box.get_node("MetaLabel")

	var name := "untitled"
	if not _document.current_path.is_empty():
		name = _document.current_path.get_file().get_basename()
	var dirty := "*" if _document.is_dirty else ""
	font_label.text = "%s%s" % [name, dirty]

	if _document.resource == null:
		meta_label.text = ""
	else:
		var res := _document.resource
		var first := res.get_first_char()
		var drawn := 0
		for i in range(res.get_glyph_count()):
			var r: Rect2i = res.get_glyph_rect(first + i)
			if r.size.x > 0 and r.size.y > 0:
				drawn += 1
		meta_label.text = "%d page(s)\n%d glyphs (%d drawn)\nshadow %d" % [
			res.get_page_count(),
			res.get_glyph_count(),
			drawn,
			res.get_shadow_offset(),
		]


func has_unsaved_changes() -> bool:
	return _document.is_dirty


func can_new() -> bool:
	return true


func get_new_action_label() -> String:
	return "New Font"


func new_current() -> Error:
	return _document.create_new()


func can_open() -> bool:
	return true


func get_open_action_label() -> String:
	return "Open Font..."


func get_open_dialog_title() -> String:
	return "Open .fnt"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.fnt,*.FNT ; Nova fonts"])


func get_open_dialog_dir() -> String:
	return _document.get_last_open_dir()


func get_open_resource_kind() -> String:
	return "font"


func get_current_resource_path() -> String:
	return _document.current_path


func open_file(path: String) -> Error:
	return _document.open_fnt(path)


func open_font_name(font_name: String) -> Error:
	if font_name.is_empty():
		return ERR_INVALID_PARAMETER
	var root := _resource_root_dir()
	if root.is_empty():
		return ERR_DOES_NOT_EXIST
	var exact := root.path_join("%s.fnt" % font_name)
	if FileAccess.file_exists(exact):
		return open_file(exact)
	var dir := DirAccess.open(root)
	if dir == null:
		return ERR_DOES_NOT_EXIST
	var lower := font_name.to_lower()
	dir.list_dir_begin()
	var filename := dir.get_next()
	while not filename.is_empty():
		if not dir.current_is_dir() and filename.get_extension().to_lower() == "fnt":
			if filename.get_basename().to_lower() == lower:
				dir.list_dir_end()
				return open_file(root.path_join(filename))
		filename = dir.get_next()
	dir.list_dir_end()
	return ERR_DOES_NOT_EXIST


func _resource_root_dir() -> String:
	if editor_shell != null and editor_shell.has_method("get_resource_root_dir"):
		var shell_root := String(editor_shell.get_resource_root_dir()).strip_edges()
		if not shell_root.is_empty():
			return shell_root
	return ResourceDirSettings.get_resource_dir()


func can_save() -> bool:
	return _document.is_dirty and not _document.current_path.is_empty()


func get_save_action_label() -> String:
	return "Save Font"


func can_save_as() -> bool:
	return _document.resource != null


func get_save_as_action_label() -> String:
	return "Save Font As..."


func save_current() -> Error:
	return _document.save_current()


func save_as(dir_path: String) -> Error:
	return _document.save_as(dir_path)


func get_save_dialog_title() -> String:
	return "Choose where to save the font"


func get_save_dialog_dir() -> String:
	return _document.get_last_save_dir()


func can_undo() -> bool:
	return _editor != null and _editor.has_method("can_undo") and _editor.can_undo()


func can_redo() -> bool:
	return _editor != null and _editor.has_method("can_redo") and _editor.can_redo()


func undo() -> void:
	if _editor != null and _editor.has_method("undo"):
		_editor.undo()


func redo() -> void:
	if _editor != null and _editor.has_method("redo"):
		_editor.redo()
