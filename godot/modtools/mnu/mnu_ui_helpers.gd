class_name MnuUiHelpers
extends RefCounted

## Stateless UI builders for the Menus workspace inspector, kept local so
## modtools/mnu/ stays self-contained (mirrors object_ui_helpers.gd). Every
## function returns the control(s) it adds and never reads instance state; the
## caller owns wiring the change signals. M7 made the inspector editable, so these
## return live controls (LineEdit / SpinBox / CheckBox / swatch) rather than
## label/value rows.

const PANEL_MARGIN := 10
const KEY_WIDTH := 104


static func make_inspector_box(host: Control) -> VBoxContainer:
	# "Box" is addressed by node path from the canvas code; keep the name.
	return UiBox.make_inspector_box(host, "Box")


static func add_heading(parent: Control, text: String) -> Label:
	var label := Label.new()
	label.theme_type_variation = &"Heading"
	label.clip_text = true
	label.text = text
	parent.add_child(label)
	return label


static func add_muted(parent: Control, text: String) -> Label:
	var label := Label.new()
	label.theme_type_variation = &"Muted"
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.text = text
	parent.add_child(label)
	return label


# A fixed-width muted key label so the editable controls line up in a column.
static func _key_label(text: String) -> Label:
	var label := Label.new()
	label.theme_type_variation = &"Muted"
	label.text = text
	label.clip_text = true
	label.custom_minimum_size = Vector2(KEY_WIDTH, 0)
	label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	return label


static func _row(parent: Control) -> HBoxContainer:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_theme_constant_override("separation", 6)
	parent.add_child(row)
	return row


# Editable text row: "key [LineEdit]". Returns the LineEdit so the caller wires
# text_submitted / focus_exited. Empty values show a "(none)" placeholder.
static func add_text_edit_row(parent: Control, key: String, value: String) -> LineEdit:
	var row := _row(parent)
	row.add_child(_key_label(key))
	var edit := LineEdit.new()
	edit.text = value
	edit.placeholder_text = "(none)"
	edit.tooltip_text = value
	edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(edit)
	return edit


# Editable integer spin row. Returns the SpinBox (value pre-set, no signal).
static func add_spin_row(parent: Control, key: String, value: int, min_v: int, max_v: int) -> SpinBox:
	var row := _row(parent)
	row.add_child(_key_label(key))
	var spin := _make_spin(min_v, max_v, value)
	row.add_child(spin)
	return spin


# Two integer spins on one row (Position X/Y, Size W/H). Returns [a, b].
static func add_spin_pair_row(parent: Control, key: String, a_value: int, b_value: int, min_v: int, max_v: int) -> Array:
	var row := _row(parent)
	row.add_child(_key_label(key))
	var a := _make_spin(min_v, max_v, a_value)
	row.add_child(a)
	var b := _make_spin(min_v, max_v, b_value)
	row.add_child(b)
	return [a, b]


static func _make_spin(min_v: int, max_v: int, value: int) -> SpinBox:
	var spin := SpinBox.new()
	spin.min_value = min_v
	spin.max_value = max_v
	spin.step = 1
	spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	spin.set_value_no_signal(value)
	return spin


# Editable color row: "key [swatch] [LineEdit raw]". Returns [swatch, edit]. A
# literal hex paints the swatch; a %VAR% reference, empty, or invalid token shows
# a transparent swatch so the raw string always survives a round-trip. The caller
# wires the edit and calls refresh_swatch as the text changes.
static func add_color_edit_row(parent: Control, key: String, raw: String) -> Array:
	var row := _row(parent)
	row.add_child(_key_label(key))
	var swatch := ColorRect.new()
	swatch.custom_minimum_size = Vector2(16, 16)
	refresh_swatch(swatch, raw)
	row.add_child(swatch)
	var edit := LineEdit.new()
	edit.text = raw
	edit.placeholder_text = "(none)"
	edit.tooltip_text = raw
	edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(edit)
	return [swatch, edit]


static func refresh_swatch(swatch: ColorRect, raw: String) -> void:
	var parsed = color_from_mnu(raw)
	swatch.color = parsed if parsed != null else Color(0, 0, 0, 0)


# An "add slot" row: "key [OptionButton] [Add]". options is an Array of
# [display_name, slot_id]; each becomes a picker item whose id is the slot enum,
# so the caller reads get_selected_id() on Add. Returns [OptionButton, Button], or
# [] when there is nothing to add (so the caller can skip the row entirely).
static func add_add_slot_row(parent: Control, key: String, options: Array) -> Array:
	if options.is_empty():
		return []
	var row := _row(parent)
	row.add_child(_key_label(key))
	var picker := OptionButton.new()
	picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for opt in options:
		picker.add_item(String(opt[0]), int(opt[1]))
	row.add_child(picker)
	var add_btn := Button.new()
	add_btn.text = "Add"
	row.add_child(add_btn)
	return [picker, add_btn]


# Editable toggle row. Returns the CheckBox (pressed state pre-set, no signal).
static func add_check_row(parent: Control, label: String, pressed: bool) -> CheckBox:
	var check := CheckBox.new()
	check.text = label
	check.set_pressed_no_signal(pressed)
	parent.add_child(check)
	return check


# Parse an MNU color token into a Color, or null when it is a %VAR% reference,
# empty, or not valid hex (so the caller can render it as "unresolved").
static func color_from_mnu(raw: String):
	var token := raw.strip_edges()
	if token.is_empty() or token.begins_with("%"):
		return null
	if not token.begins_with("#"):
		token = "#" + token
	if not Color.html_is_valid(token):
		return null
	return Color.html(token)
