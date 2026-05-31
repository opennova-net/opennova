class_name MnuListEditor
extends VBoxContainer

# Stateful row editor for the Menus inspector's nested list/table templates
# (combo / list / spinlist / multi item rows, table column headers). Unlike the
# stateless MnuUiHelpers factories, this owns a small stack of row controls and
# reports edit INTENT; it never touches the document. The inspector feeds rows
# via set_rows() and forwards the signals to the editor, which applies the
# mutation and records one undo step (mirrors the scalar edit channel).
#
# Configure once with a column spec: an Array of {key, label, kind, options?}.
#   kind: "text" | "int" | "bool" | "enum"   (enum needs options: Array[String])
# Rows are Array[Dictionary]; each dict carries the spec keys. Field commits fire
# on Enter and on blur (the editor no-ops unchanged values, like the scalar rows).

signal row_field_changed(index: int, key: String, value: Variant)
signal row_added()
signal row_removed(index: int)
signal row_moved(from: int, to: int)

var _spec: Array = []
var _allow_move := true
var _add_label := "+ Add"
var _rows_box: VBoxContainer


func configure(spec: Array, allow_move := true, add_label := "+ Add") -> void:
	_spec = spec
	_allow_move = allow_move
	_add_label = add_label


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
	var hb := HBoxContainer.new()
	hb.add_theme_constant_override("separation", 4)
	hb.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for col in _spec:
		hb.add_child(_make_field(index, col, row.get(String(col["key"]), "")))
	if _allow_move:
		var up := _icon_button("▲", index == 0)
		up.pressed.connect(func() -> void: row_moved.emit(index, index - 1))
		hb.add_child(up)
		var down := _icon_button("▼", index >= count - 1)
		down.pressed.connect(func() -> void: row_moved.emit(index, index + 1))
		hb.add_child(down)
	var del := _icon_button("✕", false)
	del.pressed.connect(func() -> void: row_removed.emit(index))
	hb.add_child(del)
	_rows_box.add_child(hb)


func _icon_button(text: String, disabled: bool) -> Button:
	var btn := Button.new()
	btn.text = text
	btn.disabled = disabled
	return btn


func _make_field(index: int, col: Dictionary, value: Variant) -> Control:
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
		_:
			var edit := LineEdit.new()
			edit.text = String(value)
			edit.placeholder_text = String(col.get("label", key))
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
