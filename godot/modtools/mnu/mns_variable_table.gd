class_name MnsVariableTable
extends VBoxContainer

# The Menu Styles workspace's grouped variable list. Renders the stylesheet's
# entries in document order: the file's leading comment block collapses into a
# "File header" disclosure, comment runs above a group become its heading, and
# blank lines split groups (all derived from MnsStyleSheet.get_entries(), which
# preserves the authored layout). Rows carry a typed value editor: colors get a
# live swatch, everything stays editable as raw text. The table never mutates
# the document: value commits emit edit_requested and the editor applies them.

const MnuUiHelpersScript = preload("res://modtools/mnu/mnu_ui_helpers.gd")

signal row_selected(name: String)
signal edit_requested(edit: Dictionary)

var _sheet: MnsStyleSheet
var _rows: Dictionary = {}        # authored name -> {panel, edit, swatch, comment}
var _selected_name := ""
var _header_body: Label
var _header_button: Button
var _selected_sb: StyleBoxFlat
var _normal_sb: StyleBoxEmpty


func _init() -> void:
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_theme_constant_override("separation", 2)
	_selected_sb = StyleBoxFlat.new()
	_selected_sb.bg_color = Color(0.35, 0.55, 0.85, 0.18)
	_selected_sb.set_corner_radius_all(3)
	_selected_sb.content_margin_left = 6
	_selected_sb.content_margin_right = 6
	_selected_sb.content_margin_top = 2
	_selected_sb.content_margin_bottom = 2
	_normal_sb = StyleBoxEmpty.new()
	_normal_sb.content_margin_left = 6
	_normal_sb.content_margin_right = 6
	_normal_sb.content_margin_top = 2
	_normal_sb.content_margin_bottom = 2


# Value type by shape: 6/8-digit hex is a color, *.fnt a font, common picture
# extensions an image, anything else plain text. Shared with the inspector and
# the Menus workspace's variable dropdowns.
static func infer_type(value: String) -> String:
	var v := value.strip_edges()
	var lower := v.to_lower()
	if lower.ends_with(".fnt"):
		return "font"
	if lower.ends_with(".tga") or lower.ends_with(".bmp") or lower.ends_with(".pcx"):
		return "image"
	if (v.length() == 6 or v.length() == 8) and v.is_valid_hex_number(false):
		return "color"
	return "text"


func set_stylesheet(sheet: MnsStyleSheet) -> void:
	_sheet = sheet
	refresh()


func refresh() -> void:
	var keep := _selected_name
	for child in get_children():
		child.queue_free()
	_rows.clear()
	_header_body = null
	_header_button = null
	if _sheet == null:
		return

	_build_file_header()

	var entries: Array = _sheet.get_entries()
	if entries.is_empty():
		var empty := Label.new()
		empty.theme_type_variation = &"Muted"
		empty.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		empty.text = "No variables yet. Add one, then point menu colors and fonts at it as %NAME%."
		add_child(empty)
		return

	var current_group := -1
	for entry_index in range(entries.size()):
		var entry := entries[entry_index] as Dictionary
		var group := int(entry.get("group", 0))
		if group != current_group:
			current_group = group
			_add_group_heading(entry)
		_add_row(entry, entry_index, entries.size())

	_selected_name = ""
	if not keep.is_empty():
		select_name(keep, false)


# The leading // block of the document (NovaLogic's format spec in the shipped
# file) collapses into one disclosure row instead of dozens of comment lines.
func _build_file_header() -> void:
	var lines := String(_sheet.get_source_text()).split("\n")
	var header := PackedStringArray()
	for line_value in lines:
		var line := String(line_value).strip_edges()
		if line.begins_with("//"):
			header.append(line.trim_prefix("//").strip_edges())
		else:
			break
	if header.is_empty():
		return
	_header_button = Button.new()
	_header_button.text = "File header  (%d lines)" % header.size()
	_header_button.flat = true
	_header_button.alignment = HORIZONTAL_ALIGNMENT_LEFT
	_header_button.toggle_mode = true
	_header_button.focus_mode = Control.FOCUS_NONE
	add_child(_header_button)
	_header_body = Label.new()
	_header_body.theme_type_variation = &"Muted"
	_header_body.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_header_body.text = "\n".join(header)
	_header_body.visible = false
	add_child(_header_body)
	_header_button.toggled.connect(func(on: bool) -> void:
		if _header_body != null and is_instance_valid(_header_body):
			_header_body.visible = on)


# A group's heading is the comment run directly above its first define; a
# heading-less group still gets breathing room.
func _add_group_heading(first_entry: Dictionary) -> void:
	var spacer := Control.new()
	spacer.custom_minimum_size = Vector2(0, 6)
	add_child(spacer)
	var comments: PackedStringArray = first_entry.get("preceding_comments", PackedStringArray())
	if comments.is_empty():
		return
	var parts := PackedStringArray()
	for comment_value in comments:
		parts.append(String(comment_value).trim_prefix("//").strip_edges())
	var heading := Label.new()
	heading.theme_type_variation = &"Heading"
	heading.clip_text = true
	heading.text = " ".join(parts)
	add_child(heading)


func _add_row(entry: Dictionary, entry_index: int, entry_count: int) -> void:
	var name := String(entry.get("name", ""))
	var value := String(entry.get("value", ""))
	var comment := String(entry.get("inline_comment", ""))

	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel", _normal_sb)
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(panel)

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	panel.add_child(row)

	var name_label := Label.new()
	name_label.text = name
	name_label.clip_text = true
	name_label.custom_minimum_size = Vector2(170, 0)
	name_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	name_label.tooltip_text = name
	row.add_child(name_label)

	var swatch: ColorRect = null
	if infer_type(value) == "color":
		swatch = ColorRect.new()
		swatch.custom_minimum_size = Vector2(16, 16)
		swatch.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		MnuUiHelpersScript.refresh_swatch(swatch, value)
		row.add_child(swatch)

	var edit := LineEdit.new()
	edit.text = value
	edit.tooltip_text = value
	edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(edit)

	var comment_label: Label = null
	if not comment.is_empty():
		comment_label = Label.new()
		comment_label.theme_type_variation = &"Muted"
		comment_label.clip_text = true
		comment_label.custom_minimum_size = Vector2(0, 0)
		comment_label.size_flags_stretch_ratio = 0.6
		comment_label.text = comment.trim_prefix("//").strip_edges()
		comment_label.tooltip_text = comment
		row.add_child(comment_label)

	var up := Button.new()
	up.text = "â–²"
	up.tooltip_text = "Move variable earlier"
	up.disabled = entry_index == 0
	up.pressed.connect(func() -> void:
		edit_requested.emit({"op": "move", "name": name, "to_entry_index": entry_index - 1}))
	row.add_child(up)
	var down := Button.new()
	down.text = "â–¼"
	down.tooltip_text = "Move variable later"
	down.disabled = entry_index >= entry_count - 1
	down.pressed.connect(func() -> void:
		edit_requested.emit({"op": "move", "name": name,
			"to_entry_index": entry_index + 2 if entry_index + 2 < entry_count else -1}))
	row.add_child(down)

	_rows[name] = {"panel": panel, "edit": edit, "swatch": swatch, "comment": comment_label}

	# Selection: clicking anywhere on the row, or focusing its editor.
	panel.gui_input.connect(func(event: InputEvent) -> void:
		if event is InputEventMouseButton and event.pressed \
				and (event as InputEventMouseButton).button_index == MOUSE_BUTTON_LEFT:
			select_name(name))
	edit.focus_entered.connect(func() -> void:
		select_name(name))
	if swatch != null:
		edit.text_changed.connect(func(text: String) -> void:
			if is_instance_valid(swatch):
				MnuUiHelpersScript.refresh_swatch(swatch, text))
	# Commit on Enter or focus leave; unchanged text is a no-op upstream.
	edit.text_submitted.connect(func(_text: String) -> void:
		_commit_value(name, edit))
	edit.focus_exited.connect(func() -> void:
		_commit_value(name, edit))


func _commit_value(name: String, edit: LineEdit) -> void:
	if _sheet == null or not is_instance_valid(edit) or not edit.is_inside_tree():
		return
	if edit.text == String(_sheet.get_variable(name)):
		return
	edit_requested.emit({"op": "set_value", "name": name, "value": edit.text})


# In-place value refresh after a set_value edit, so the row that issued it is
# not rebuilt under the user's caret.
func update_row_value(name: String, value: String) -> void:
	var row: Dictionary = _rows.get(name, {})
	if row.is_empty():
		refresh()
		return
	var edit := row.get("edit") as LineEdit
	if edit != null and is_instance_valid(edit) and not edit.has_focus():
		edit.text = value
		edit.tooltip_text = value
	var swatch := row.get("swatch") as ColorRect
	if swatch != null and is_instance_valid(swatch):
		MnuUiHelpersScript.refresh_swatch(swatch, value)


func get_selected_name() -> String:
	return _selected_name


# Programmatic + user selection share this; emit=false for refresh-preserving
# reselects so the inspector is not rebuilt mid-typing.
func select_name(name: String, emit := true) -> void:
	if _selected_name == name and emit:
		row_selected.emit(name)
		return
	var previous: Dictionary = _rows.get(_selected_name, {})
	if not previous.is_empty():
		var prev_panel := previous.get("panel") as PanelContainer
		if prev_panel != null and is_instance_valid(prev_panel):
			prev_panel.add_theme_stylebox_override("panel", _normal_sb)
	_selected_name = name if _rows.has(name) else ""
	var row: Dictionary = _rows.get(_selected_name, {})
	if not row.is_empty():
		var panel := row.get("panel") as PanelContainer
		if panel != null and is_instance_valid(panel):
			panel.add_theme_stylebox_override("panel", _selected_sb)
	if emit:
		row_selected.emit(_selected_name)


# Scroll the selected row into view (cross-jump focus); the table lives in a
# ScrollContainer owned by the editor.
func reveal_selected() -> void:
	var row: Dictionary = _rows.get(_selected_name, {})
	if row.is_empty():
		return
	var panel := row.get("panel") as PanelContainer
	var scroll := get_parent() as ScrollContainer
	if panel != null and scroll != null and is_instance_valid(panel):
		scroll.ensure_control_visible(panel)
