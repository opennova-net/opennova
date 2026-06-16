class_name CreditsEditorWorkspace
extends EditorWorkspace

# EditorWorkspace adapter for the Credits workspace.
# Owns a CreditsEditorDocument and mounts a CreditsEditor scene into ONED's
# viewport host.

const CREDITS_EDITOR_SCENE_PATH := "res://modtools/credits/credits_editor.tscn"
const CreditsEditorDocument = preload("res://modtools/credits/credits_editor_document.gd")

var _document: CreditsEditorDocument
var _editor: Control
var _inspector_root: Control

func _init() -> void:
	_document = CreditsEditorDocument.new()
	_document.state_changed.connect(_populate_inspector)

func get_workspace_id() -> StringName:
	return &"credits"

func get_workspace_label() -> String:
	return "Credits"

func get_workspace_tooltip() -> String:
	return "Author *.kda rolling credits: text, images, fonts, and scroll timing."

func get_project_title() -> String:
	var name := "untitled"
	if not _document.current_path.is_empty():
		name = _document.current_path.get_file().get_basename()
	var dirty := "*" if _document.is_dirty else ""
	return "%s%s" % [name, dirty]

func get_status_tool() -> String:
	return "Credits"

func get_status_context() -> String:
	if _document.resource == null:
		return ""
	return "%d entries" % _document.resource.get_entry_count()

func get_capabilities() -> Array[EditorCapability]:
	var caps: Array[EditorCapability] = []
	
	# Main View
	if _editor == null:
		var scene := load(CREDITS_EDITOR_SCENE_PATH) as PackedScene
		if scene != null:
			_editor = scene.instantiate()
			_editor.set_anchors_preset(Control.PRESET_FULL_RECT)
			_editor.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			_editor.size_flags_vertical = Control.SIZE_EXPAND_FILL
			if _editor.has_method("set_resource_root"):
				_editor.set_resource_root(EditorFileSystem.get_instance().get_resource_root())
			if _editor.has_signal("request_edit_font") and not _editor.is_connected("request_edit_font", Callable(self, "_on_editor_request_edit_font")):
				_editor.connect("request_edit_font", Callable(self, "_on_editor_request_edit_font"))
			_editor.set_document(_document)
	caps.append(MainViewProvider.new(_editor))
	
	# Inspector
	if _inspector_root == null or not is_instance_valid(_inspector_root):
		_inspector_root = _make_inspector()
		_populate_inspector()
	caps.append(InspectorProvider.new(_inspector_root))
	
	return caps

func deactivate() -> void:
	if _editor != null and _editor.has_method("flush_pending_edits"):
		_editor.flush_pending_edits()

func get_commands() -> Array[EditorCommand]:
	return [
		EditorCommand.new(&"new", "New Credits", Callable(self, "new_current")),
		EditorCommand.new(&"open", "Open Credits...", Callable(self, "trigger_legacy_open")), # Proxies to shell for now
		EditorCommand.new(&"save", "Save Credits", Callable(self, "save_current"), Callable(self, "can_save")),
		EditorCommand.new(&"save_as", "Save Credits As...", Callable(self, "trigger_legacy_save_as"), Callable(self, "can_save_as"))
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
	box.add_theme_constant_override("separation", 8)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.name = "Box"
	margin.add_child(box)

	var title := Label.new()
	title.name = "FileLabel"
	title.theme_type_variation = &"Heading"
	box.add_child(title)

	var entries_label := Label.new()
	entries_label.name = "EntriesLabel"
	entries_label.theme_type_variation = &"Muted"
	entries_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(entries_label)

	var missing_label := Label.new()
	missing_label.name = "MissingLabel"
	missing_label.theme_type_variation = &"Warn"
	missing_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(missing_label)

	var env_label := Label.new()
	env_label.name = "EnvLabel"
	env_label.theme_type_variation = &"Muted"
	env_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(env_label)

	return margin

func _populate_inspector() -> void:
	if _inspector_root == null or not is_instance_valid(_inspector_root):
		return
	var box := _inspector_root.get_node_or_null("Box")
	if box == null:
		return

	var file_label: Label = box.get_node("FileLabel")
	var entries_label: Label = box.get_node("EntriesLabel")
	var missing_label: Label = box.get_node("MissingLabel")
	var env_label: Label = box.get_node("EnvLabel")

	var name := "untitled"
	if not _document.current_path.is_empty():
		name = _document.current_path.get_file().get_basename()
	var dirty := "*" if _document.is_dirty else ""
	file_label.text = "%s%s" % [name, dirty]

	var resource: CbinCreditsResource = _document.resource
	if resource == null:
		entries_label.text = ""
		missing_label.visible = false
		env_label.text = ""
		return

	var total := resource.get_entry_count()
	var text_count := 0
	var newline_count := 0
	var image_count := 0
	var missing := 0
	for i in range(total):
		var e := resource.get_entry(i)
		if e is CbinTextEntry:
			text_count += 1
		elif e is CbinNewlineEntry:
			newline_count += 1
		elif e is CbinImageEntry:
			image_count += 1
			if e.get_texture() == null and not (e as CbinImageEntry).get_texture_path().is_empty():
				missing += 1

	entries_label.text = "%d entries\n%d text  %d newline  %d image" % [total, text_count, newline_count, image_count]

	if missing > 0:
		missing_label.text = "%d image(s) missing" % missing
		missing_label.visible = true
	else:
		missing_label.visible = false

	env_label.text = "scroll %.2f  spacing %d  center %d" % [resource.get_scroll_rate(), resource.get_vertical_space(), resource.get_center_x()]

func get_editor_document() -> EditorDocument:
	# Credits doesn't currently strictly implement EditorDocument's subclass, it uses duck-typing today.
	# We will return null and fallback to compatibility if needed, or wrap it.
	# Let's wrap it properly:
	return _document

func new_current() -> Error:
	return _document.create_new()

func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.kda,*.KDA ; Credits"])

func can_save() -> bool:
	return _document.is_dirty and not _document.current_path.is_empty()

func can_save_as() -> bool:
	return _document.resource != null

func save_current() -> Error:
	var flush_err := flush_pending_edits()
	if flush_err != OK:
		return flush_err
	return _document.save_current()

func save_as(dir_path: String) -> Error:
	var flush_err := flush_pending_edits()
	if flush_err != OK:
		return flush_err
	return _document.save_as(dir_path)

func flush_pending_edits() -> Error:
	if _editor != null and _editor.has_method("flush_pending_edits"):
		return _editor.flush_pending_edits()
	return OK

func _on_editor_request_edit_font(font_name: String) -> void:
	EditorCommandBus.dispatch(&"open_font", {"font_name": font_name})

# Remaining compatibility methods so the shell works while it's in transition
func open_file(path: String) -> Error:
	return _document.open_kda(path)

func get_open_resource_kind() -> String:
	return "credits"

func get_current_resource_path() -> String:
	return _document.current_path

func get_open_dialog_dir() -> String:
	return _document.get_last_open_dir()

func get_save_dialog_title() -> String:
	return "Choose where to save the credits"

func get_save_dialog_dir() -> String:
	return _document.get_last_save_dir()
