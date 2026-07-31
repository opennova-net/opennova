class_name MnuListEditor
extends VBoxContainer

# Stateful row editor for the Menus inspector's nested list/table templates
# (combo / list / spinlist / multi item rows, table column headers). Unlike the
# stateless MnuUiHelpers factories, this owns a small stack of row controls and
# reports edit INTENT; it never touches the document. The inspector feeds rows
# via set_rows() and forwards the signals to the editor, which applies the
# mutation and records one undo step (mirrors the scalar edit channel).
#
# Configure once with a field spec: an Array of {key, label, kind, options?}.
#   kind: "text" | "int" | "bool" | "enum"   (enum needs options: Array[String])
# Rows are Array[Dictionary]; each dict carries the spec keys. Field commits fire
# on Enter and on blur (the editor no-ops unchanged values, like the scalar rows).

signal row_field_changed(index: int, key: String, value: Variant)
signal row_added()
signal row_removed(index: int)
signal row_moved(from: int, to: int)

const MnuUiHelpersScript = preload("res://modtools/mnu/mnu_ui_helpers.gd")

var _spec: Array = []
var _allow_move := true
var _add_label := "+ Add"
var _rows_box: VBoxContainer
var _ref_services: Dictionary = {}
var _color_resolver: Callable = Callable()
var _color_variables: Array = []


func configure(spec: Array, allow_move := true, add_label := "+ Add") -> void:
	_spec = spec
	_allow_move = allow_move
	_add_label = add_label


func set_reference_services(services: Dictionary) -> void:
	_ref_services = services


# Nested color rows preserve their raw %VAR% token, while initializing the
# literal picker from the stylesheet-resolved value. `variables` is the same
# [{name, value}] list used by scalar color rows.
func set_color_context(resolver: Callable, variables: Array) -> void:
	_color_resolver = resolver
	_color_variables = variables.duplicate(true)


func _ensure_built() -> void:
	if _rows_box != null:
		return
	add_theme_constant_override("separation", 4)
	_rows_box = VBoxContainer.new()
	_rows_box.add_theme_constant_override("separation", 4)
	_rows_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(_rows_box)
	var add_btn := Button.new()
	add_btn.text = _add_label
	add_btn.pressed.connect(func() -> void: row_added.emit())
	add_child(add_btn)


func set_rows(rows: Array) -> void:
	_ensure_built()
	for child in _rows_box.get_children():
		child.queue_free()
	for i in range(rows.size()):
		_build_row(i, rows[i], rows.size())


func _build_row(index: int, row: Dictionary, count: int) -> void:
	# Nested MNU rows routinely have 5–9 fields. A single unlabeled HBox makes
	# presence bits and table/appearance values impossible to identify in the
	# narrow inspector. Render each row as a compact, labeled card instead.
	var card := PanelContainer.new()
	card.name = "NestedRow%d" % index
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var content := VBoxContainer.new()
	content.add_theme_constant_override("separation", 3)
	card.add_child(content)
	var title := Label.new()
	title.text = "Row %d" % (index + 1)
	title.add_theme_color_override("font_color",
		get_theme_color("font_disabled_color", "Label"))
	content.add_child(title)
	var fields := GridContainer.new()
	fields.columns = 2
	fields.add_theme_constant_override("h_separation", 6)
	fields.add_theme_constant_override("v_separation", 3)
	fields.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for col in _spec:
		var label_text := String(col.get("label", col.get("key", "")))
		var label := Label.new()
		label.text = label_text
		label.tooltip_text = label_text
		label.custom_minimum_size.x = 74
		fields.add_child(label)
		var field := _make_field(index, col,
			row.get(String(col["key"]), ""), row)
		field.tooltip_text = label_text
		field.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		fields.add_child(field)
	content.add_child(fields)
	var actions := HBoxContainer.new()
	actions.alignment = BoxContainer.ALIGNMENT_END
	if _allow_move:
		var up := _icon_button("▲", index == 0)
		up.tooltip_text = "Move row up"
		up.pressed.connect(func() -> void: row_moved.emit(index, index - 1))
		actions.add_child(up)
		var down := _icon_button("▼", index >= count - 1)
		down.tooltip_text = "Move row down"
		down.pressed.connect(func() -> void: row_moved.emit(index, index + 1))
		actions.add_child(down)
	var del := _icon_button("✕", false)
	del.tooltip_text = "Remove row"
	del.pressed.connect(func() -> void: row_removed.emit(index))
	actions.add_child(del)
	content.add_child(actions)
	_rows_box.add_child(card)


func _icon_button(text: String, disabled: bool) -> Button:
	var btn := Button.new()
	btn.text = text
	btn.disabled = disabled
	return btn


func _make_field(index: int, col: Dictionary, value: Variant, row: Dictionary) -> Control:
	var key := String(col["key"])
	match String(col.get("kind", "text")):
		"int":
			var spin := SpinBox.new()
			spin.min_value = int(col.get("min", -4096))
			spin.max_value = int(col.get("max", 4096))
			spin.step = 1
			spin.custom_minimum_size = Vector2(60, 0)
			spin.set_value_no_signal(int(value))
			spin.value_changed.connect(func(v: float) -> void: row_field_changed.emit(index, key, int(v)))
			return spin
		"bool":
			var check := CheckBox.new()
			check.set_pressed_no_signal(bool(value))
			check.toggled.connect(func(pressed: bool) -> void: row_field_changed.emit(index, key, pressed))
			return check
		"enum":
			var option := OptionButton.new()
			var options: Array = col.get("options", [])
			for oi in range(options.size()):
				option.add_item(String(options[oi]), oi)
				if String(options[oi]) == String(value):
					option.select(oi)
			option.item_selected.connect(func(oi: int) -> void:
				row_field_changed.emit(index, key, option.get_item_text(oi)))
			return option
		"color":
			return _make_color_field(index, key, String(value))
		"color_or_text":
			if String(row.get("type", "")).to_lower() == "color":
				return _make_color_field(index, key, String(value))
			if String(row.get("type", "")).to_lower() == "image":
				return _make_asset_field(index, key, String(value), "texture")
			return _make_text_field(index, key, String(value), String(col.get("label", key)))
		"asset_or_color":
			var row_type := String(row.get("type", "")).to_lower()
			if row_type == "color" or row_type == "outline":
				return _make_color_field(index, key, String(value))
			if row_type == "image":
				return _make_asset_field(index, key, String(value), "texture")
			return _make_text_field(index, key, String(value), String(col.get("label", key)))
		"asset":
			return _make_asset_field(index, key, String(value), String(col.get("asset_kind", "texture")))
		_:
			return _make_text_field(index, key, String(value), String(col.get("label", key)))


func _make_text_field(index: int, key: String, value: String, placeholder: String) -> LineEdit:
	var edit := LineEdit.new()
	edit.text = value
	edit.placeholder_text = placeholder
	edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	# Commit on Enter and on blur; skip a commit fired while the row is being
	# torn down (set_rows rebuilds on every document change), mirroring the
	# scalar inspector's _wire_text guard.
	var commit := func() -> void:
		if not is_instance_valid(edit) or not edit.is_inside_tree():
			return
		row_field_changed.emit(index, key, edit.text)
	edit.text_submitted.connect(func(_t: String) -> void: commit.call())
	edit.focus_exited.connect(func() -> void: commit.call())
	return edit


func _make_color_field(index: int, key: String, raw: String) -> Control:
	var box := HBoxContainer.new()
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_theme_constant_override("separation", 4)
	var edit := _make_text_field(index, key, raw, "AARRGGBB or %VAR%")
	box.add_child(edit)
	var picker := ColorPickerButton.new()
	picker.name = "MnuNestedColorPicker"
	picker.custom_minimum_size = Vector2(36, 0)
	picker.tooltip_text = "Pick a literal color"
	var resolved := String(_color_resolver.call(raw)) \
		if _color_resolver.is_valid() else raw
	var parsed = MnuUiHelpersScript.color_from_mnu(resolved)
	picker.color = parsed if parsed != null else Color.WHITE
	picker.tooltip_text = ("Pick a literal color.\nRaw: %s\nResolved: %s\n"
		+ "Choosing a literal replaces the raw token.") % [raw, resolved]
	var force_alpha := raw.strip_edges().trim_prefix("#").length() == 8 \
		or raw.strip_edges().begins_with("%")
	var changed := {"value": false}
	picker.color_changed.connect(func(color: Color) -> void:
		changed["value"] = true
		edit.text = MnuUiHelpersScript.color_to_mnu(color, force_alpha))
	picker.popup_closed.connect(func() -> void:
		if bool(changed["value"]):
			changed["value"] = false
			row_field_changed.emit(index, key, edit.text))
	box.add_child(picker)
	if not _color_variables.is_empty():
		var vars := MenuButton.new()
		vars.text = "%"
		vars.tooltip_text = "Use a Menu Styles color variable"
		for i in range(_color_variables.size()):
			var entry: Dictionary = _color_variables[i]
			vars.get_popup().add_item(String(entry.get("name", "")), i)
		vars.get_popup().id_pressed.connect(func(id: int) -> void:
			if id < 0 or id >= _color_variables.size():
				return
			var entry: Dictionary = _color_variables[id]
			var token := "%" + String(entry.get("name", "")) + "%"
			edit.text = token
			var literal := String(entry.get("value", ""))
			var color = MnuUiHelpersScript.color_from_mnu(literal)
			if color != null:
				picker.color = color
			picker.tooltip_text = ("Pick a literal color.\nRaw: %s\nResolved: %s\n"
				+ "Choosing a literal replaces the raw token.") % [token, literal]
			row_field_changed.emit(index, key, token))
		box.add_child(vars)
	return box


func _make_asset_field(index: int, key: String, raw: String, kind: String) -> ResourceRefWidget:
	var widget := ResourceRefWidget.new()
	widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	widget.set_value_from_path(func(path: String) -> String: return path.get_file())
	widget.configure(kind, key.capitalize(), _ref_services)
	widget.set_value(raw)
	widget.value_changed.connect(func(value: String) -> void:
		row_field_changed.emit(index, key, value))
	return widget
