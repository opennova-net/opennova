class_name FontsEditorWorkspace
extends EditorWorkspace

const FntEditorDocument = preload("res://modtools/fonts/fnt_editor_document.gd")
const FntEditorScript = preload("res://modtools/fonts/fnt_editor.gd")

var _document: FntEditorDocument
var _editor: Control
var _inspector_root: Control

func _init() -> void:
	_document = FntEditorDocument.new()
	EditorCommandBus.get_instance().command_requested.connect(_on_command_requested)

func _on_command_requested(command_name: StringName, payload: Dictionary) -> void:
	if command_name == &"open_font":
		var font_name: String = payload.get("font_name", "")
		if font_name.is_empty(): return
		var err := open_font_name(font_name)
		if err == OK and editor_shell != null and editor_shell.has_method("set_active_workspace"):
			editor_shell.set_active_workspace(get_workspace_id())
		if err != OK and editor_shell != null and editor_shell.has_method("show_status_message"):
			editor_shell.show_status_message("Font not found: %s" % font_name, 5.0)
		elif err == OK and editor_shell != null and editor_shell.has_method("show_status_message"):
			editor_shell.show_status_message("Opened font %s." % font_name, 3.0)

func get_workspace_id() -> StringName:
	return &"fonts"

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

func get_capabilities() -> Array[EditorCapability]:
	var caps: Array[EditorCapability] = []
	
	if _editor == null:
		_editor = FntEditorScript.new()
		_editor.set_anchors_preset(Control.PRESET_FULL_RECT)
		_editor.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_editor.size_flags_vertical = Control.SIZE_EXPAND_FILL
		_editor.set_document(_document)
	caps.append(MainViewProvider.new(_editor))
	
	if _inspector_root == null or not is_instance_valid(_inspector_root):
		_inspector_root = _make_inspector()
		_populate_inspector()
		if not _document.state_changed.is_connected(_populate_inspector):
			_document.state_changed.connect(_populate_inspector)
	caps.append(InspectorProvider.new(_inspector_root))
	
	return caps

func get_commands() -> Array[EditorCommand]:
	return [
		EditorCommand.new(&"new", "New Font", Callable(self, "new_current")),
		EditorCommand.new(&"open", "Open Font...", Callable(self, "trigger_legacy_open")),
		EditorCommand.new(&"save", "Save Font", Callable(self, "save_current"), Callable(self, "can_save")),
		EditorCommand.new(&"save_as", "Save Font As...", Callable(self, "trigger_legacy_save_as"), Callable(self, "can_save_as"))
	]

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
		var res: NovaFntResource = _document.resource
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

func get_editor_document() -> EditorDocument:
	return _document

func new_current() -> Error:
	return _document.create_new()

func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.fnt,*.FNT ; Nova fonts"])

func can_save() -> bool:
	return _document.is_dirty and not _document.current_path.is_empty()

func can_save_as() -> bool:
	return _document.resource != null

func save_current() -> Error:
	return _document.save_current()

func save_as(dir_path: String) -> Error:
	return _document.save_as(dir_path)

func resolve_font_file(font_name: String) -> String:
	if font_name.is_empty():
		return ""
	var resources := EditorFileSystem.get_instance().get_resource_root_or_settings()
	if resources == null or resources.get_root_dir().is_empty():
		return ""
	var filename := "%s.fnt" % font_name
	var path := resources.resolve_file(filename)
	if not path.is_empty():
		return path
	return filename if resources.has_file(filename) else ""

func open_font_name(font_name: String) -> Error:
	if font_name.is_empty():
		return ERR_INVALID_PARAMETER
	var path := resolve_font_file(font_name)
	if path.is_empty():
		return ERR_DOES_NOT_EXIST
	return open_file(path)

func open_file(path: String) -> Error:
	var resources := EditorFileSystem.get_instance().get_resource_root_or_settings()
	if not FileAccess.file_exists(path) and resources != null and resources.has_file(path):
		var bytes := resources.read_file(path)
		return _document.open_fnt_bytes(bytes, EditorFileSystem.get_instance().get_vfs_display_path(resources, path))
	return _document.open_fnt(path)

func get_open_resource_kind() -> String:
	return "font"

func get_current_resource_path() -> String:
	return _document.current_path

func get_open_dialog_dir() -> String:
	return _document.get_last_open_dir()

func get_save_dialog_title() -> String:
	return "Choose where to save the font"

func get_save_dialog_dir() -> String:
	return _document.get_last_save_dir()
