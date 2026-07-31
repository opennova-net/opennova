class_name MnsInspector
extends MarginContainer

# Right-dock inspector for the Menu Styles workspace: the selected variable's
# name, typed value editor (color swatch + picker / font with a Fonts jump /
# picture / plain text), inline comment, a "Used by" strip over the shell's
# reference graph (style_var edges from the .mnu extractor), per-line
# diagnostics, and Delete. The inspector never mutates the document: rows emit
# edit_requested and the workspace routes them to the editor's apply_edit.

const MnuUiHelpersScript = preload("res://modtools/mnu/mnu_ui_helpers.gd")
const MnsVariableTableScript = preload("res://modtools/mnu/mns_variable_table.gd")

signal edit_requested(edit: Dictionary)
signal font_jump_requested(font_name: String)
signal add_requested

var _box: VBoxContainer
var _sheet: MnsStyleSheet
var _name := ""
var _shell: Object
var _font_names := PackedStringArray()
var _authoring_enabled := true


func _init() -> void:
	add_theme_constant_override("margin_left", 12)
	add_theme_constant_override("margin_right", 12)
	add_theme_constant_override("margin_top", 12)
	add_theme_constant_override("margin_bottom", 12)
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL
	_box = MnuUiHelpersScript.make_inspector_box(self)


# The shell powers the Used-by strip + workspace jumps; null (headless) hides them.
func set_shell(shell: Object) -> void:
	_shell = shell


# Font names from the resource folder (basenames incl. .fnt) for the pick menu.
func set_font_names(names: PackedStringArray) -> void:
	_font_names = names


func set_authoring_enabled(enabled: bool) -> void:
	_authoring_enabled = enabled
	if enabled:
		_rebuild()
		return
	for node in find_children("*", "Control", true, false):
		if node is BaseButton:
			(node as BaseButton).disabled = true
		elif node is LineEdit:
			(node as LineEdit).editable = false
		elif node is TextEdit:
			(node as TextEdit).editable = false


func show_variable(sheet: MnsStyleSheet, name: String) -> void:
	_sheet = sheet
	_name = name
	_rebuild()


func show_none(sheet: MnsStyleSheet) -> void:
	show_variable(sheet, "")


func _clear() -> void:
	for child in _box.get_children():
		child.queue_free()


func _entry() -> Dictionary:
	if _sheet == null or _name.is_empty():
		return {}
	for entry_value in _sheet.get_entries():
		var entry := entry_value as Dictionary
		if String(entry.get("name", "")) == _name:
			return entry
	return {}


func _rebuild() -> void:
	_clear()
	if not _authoring_enabled:
		set_authoring_enabled.call_deferred(false)
	if _sheet == null:
		MnuUiHelpersScript.add_muted(_box, "No stylesheet open.")
		return
	var entry := _entry()
	if entry.is_empty():
		_build_overview()
		return

	var value := String(entry.get("value", ""))
	var value_type: String = MnsVariableTableScript.infer_type(value)

	MnuUiHelpersScript.add_heading(_box, _name)
	MnuUiHelpersScript.add_muted(_box, _type_label(value_type))

	var name_edit := MnuUiHelpersScript.add_text_edit_row(_box, "Name", _name)
	_wire(name_edit, func(text: String) -> void:
		if text != _name and not text.is_empty():
			edit_requested.emit({"op": "rename", "name": _name, "new_name": text}))

	match value_type:
		"color":
			_build_color_value(value)
		"font":
			_build_font_value(value)
		"image":
			_build_image_value(value)
			MnuUiHelpersScript.add_muted(_box, "A picture file (like a .tga) next to the menus.")
		_:
			_build_text_value(value)

	var comment := String(entry.get("inline_comment", "")).trim_prefix("//").strip_edges()
	var comment_edit := MnuUiHelpersScript.add_text_edit_row(_box, "Comment", comment)
	_wire(comment_edit, func(text: String) -> void:
		if text != comment:
			edit_requested.emit({"op": "set_comment", "name": _name, "comment": text}))

	_build_used_by()
	_build_diagnostics(int(entry.get("line", 0)))

	var delete_btn := Button.new()
	delete_btn.text = "Delete variable"
	delete_btn.pressed.connect(func() -> void:
		edit_requested.emit({"op": "remove", "name": _name}))
	_box.add_child(delete_btn)


# Artist-facing names for the inferred value shapes.
func _type_label(value_type: String) -> String:
	match value_type:
		"color":
			return "Color (AARRGGBB hex)"
		"font":
			return "Font"
		"image":
			return "Picture"
		_:
			return "Text"


func _build_color_value(value: String) -> void:
	var pair = MnuUiHelpersScript.add_color_edit_row(_box, "Value", value)
	var swatch: ColorRect = pair[0]
	var edit: LineEdit = pair[1]
	edit.text_changed.connect(func(text: String) -> void:
		if is_instance_valid(swatch):
			MnuUiHelpersScript.refresh_swatch(swatch, text))
	_wire(edit, func(text: String) -> void:
		if text != value:
			edit_requested.emit({"op": "set_value", "name": _name, "value": text}))

	var row := edit.get_parent() as HBoxContainer
	var picker := ColorPickerButton.new()
	picker.custom_minimum_size = Vector2(44, 0)
	picker.tooltip_text = "Pick a color"
	var parsed = MnuUiHelpersScript.color_from_mnu(value)
	picker.color = parsed if parsed != null else Color.WHITE
	var had_alpha := value.strip_edges().length() == 8
	var changed := {"value": false}
	picker.color_changed.connect(func(color: Color) -> void:
		changed["value"] = true
		var hex: String = MnuUiHelpersScript.color_to_mnu(color, had_alpha)
		edit.text = hex
		if is_instance_valid(swatch):
			MnuUiHelpersScript.refresh_swatch(swatch, hex))
	picker.popup_closed.connect(func() -> void:
		if not _authoring_enabled or not bool(changed["value"]):
			return
		changed["value"] = false
		var hex: String = MnuUiHelpersScript.color_to_mnu(picker.color, had_alpha)
		if hex != value:
			edit_requested.emit({"op": "set_value", "name": _name, "value": hex}))
	if row != null:
		row.add_child(picker)


func _build_font_value(value: String) -> void:
	var edit := MnuUiHelpersScript.add_text_edit_row(_box, "Value", value)
	_wire(edit, func(text: String) -> void:
		if text != value:
			edit_requested.emit({"op": "set_value", "name": _name, "value": text}))
	var row := edit.get_parent() as HBoxContainer
	if row != null and not _font_names.is_empty():
		var pick := MenuButton.new()
		pick.text = "Pick"
		pick.tooltip_text = "Choose a font from the resource folder"
		var popup := pick.get_popup()
		for i in _font_names.size():
			popup.add_item(String(_font_names[i]), i)
		popup.id_pressed.connect(func(id: int) -> void:
			edit_requested.emit({"op": "set_value", "name": _name, "value": String(_font_names[id])}))
		row.add_child(pick)
	var jump := Button.new()
	jump.text = "Edit in Fonts"
	jump.pressed.connect(func() -> void:
		font_jump_requested.emit(value.get_basename()))
	_box.add_child(jump)


func _build_image_value(value: String) -> void:
	var row := MnuUiHelpersScript._row(_box)
	row.add_child(MnuUiHelpersScript._key_label("Value"))
	var ref := ResourceRefWidget.new()
	ref.name = "MnsImageRef"
	ref.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	ref.set_value_from_path(func(path: String) -> String:
		return path.get_file())
	ref.configure("texture", "Picture",
		ResourceRefWidget.services_from_shell(_shell) if _shell != null else {})
	ref.set_value(value)
	ref.value_changed.connect(func(next: String) -> void:
		if _authoring_enabled and next != value:
			edit_requested.emit(
				{"op": "set_value", "name": _name, "value": next}))
	row.add_child(ref)


func _build_text_value(value: String) -> void:
	var edit := MnuUiHelpersScript.add_text_edit_row(_box, "Value", value)
	_wire(edit, func(text: String) -> void:
		if text != value:
			edit_requested.emit({"op": "set_value", "name": _name, "value": text}))


# "Used by" rides the shell's reference index: the .mnu extractor emits a
# style_var edge per menu per variable, so the strip lists the menus that use
# this one and jumps into the Menus workspace.
func _build_used_by() -> void:
	var services := ReferenceServices.from_shell(_shell)
	if services == null:
		return
	var strip := ReferenceStrip.new()
	strip.name = "UsedByStrip"
	strip.configure("style variable", services, PackedStringArray(["style_var"]))
	strip.set_target(PackedStringArray([_name]))
	_box.add_child(strip)


func _build_diagnostics(line: int) -> void:
	if _sheet == null or line <= 0:
		return
	for diag_value in _sheet.get_diagnostics():
		var diag := diag_value as Dictionary
		if int(diag.get("line", 0)) != line:
			continue
		var label := MnuUiHelpersScript.add_muted(_box, String(diag.get("message", "")))
		if String(diag.get("severity", "")) == "error":
			label.add_theme_color_override("font_color", Color(0.95, 0.45, 0.35, 1.0))


# No selection: the stylesheet at a glance + the Add affordance.
func _build_overview() -> void:
	MnuUiHelpersScript.add_heading(_box, "Menu stylesheet")
	var count := _sheet.get_entry_count()
	MnuUiHelpersScript.add_muted(_box,
			"%d named value(s) shared by every menu screen: colors, fonts, and pictures menus refer to as %%NAME%%." % count)
	var diagnostics: Array = _sheet.get_diagnostics()
	if diagnostics.size() > 0:
		MnuUiHelpersScript.add_muted(_box, "%d issue(s) in the file. The Source view lists them by line." % diagnostics.size())
	var add_btn := Button.new()
	add_btn.text = "Add variable"
	add_btn.pressed.connect(func() -> void:
		add_requested.emit())
	_box.add_child(add_btn)


# Commit on Enter or focus leave; the is_inside_tree guard keeps a commit from
# firing while the inspector is being torn down (the mnu inspector idiom).
func _wire(edit: LineEdit, commit: Callable) -> void:
	edit.text_submitted.connect(func(_text: String) -> void:
		if _authoring_enabled and is_instance_valid(edit) and edit.is_inside_tree():
			commit.call(edit.text))
	edit.focus_exited.connect(func() -> void:
		if _authoring_enabled and is_instance_valid(edit) and edit.is_inside_tree():
			commit.call(edit.text))
