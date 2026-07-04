extends Control

## Per-entry detail editor for the Strings workspace. Lives inside the
## StringsEditorView (right side of the center HSplit). Edits the key, text,
## section, and position of the selected entry, plus the {hot} accelerator marker
## with a live preview.
##
## All field edits update the model immediately and are bracketed by the
## document's begin_edit()/commit_edit() so one editing session is one undo step.
## A row-changed callback patches just the edited row in the table — no rebuild,
## no shell round-trip.

const POS_MIN := -32768
const POS_MAX := 32767
const ACCEL_COLOR := "#ffd24a"

var _doc: StringsEditor
var _row_changed: Callable = Callable()
var _current_index: int = -1
var _loading: bool = false

var _empty_label: Label
var _fields_box: VBoxContainer
var _key_edit: LineEdit
var _text_edit: TextEdit
var _section_option: OptionButton
var _pos_x: SpinBox
var _pos_y: SpinBox
var _hotkey_button: Button
var _hotkey_preview: RichTextLabel


func setup(doc: StringsEditor, row_changed: Callable) -> void:
	_doc = doc
	_row_changed = row_changed


func set_document(doc: StringsEditor) -> void:
	_doc = doc


func _ready() -> void:
	set_anchors_preset(Control.PRESET_FULL_RECT)
	var box := InspectorForms.make_inspector_box(self)
	# make_inspector_box adds a MarginContainer to this bare Control, which does not
	# lay out its children — stretch that wrapper to fill the panel.
	(box.get_parent().get_parent() as Control).set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)

	_empty_label = InspectorForms.add_empty_state(box, "Select a string to edit its key, text, section, and position.", "StringsEmptyState")

	_fields_box = VBoxContainer.new()
	_fields_box.add_theme_constant_override("separation", 8)
	_fields_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(_fields_box)

	InspectorForms.add_section_heading(_fields_box, "Key")
	_key_edit = LineEdit.new()
	_key_edit.name = "StringsKeyEdit"
	_key_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_key_edit.focus_entered.connect(_begin)
	_key_edit.focus_exited.connect(_commit)
	_key_edit.text_changed.connect(_on_key_changed)
	_fields_box.add_child(_key_edit)

	InspectorForms.add_section_heading(_fields_box, "Text")
	_text_edit = TextEdit.new()
	_text_edit.name = "StringsTextEdit"
	_text_edit.custom_minimum_size = Vector2(0, 110)
	_text_edit.wrap_mode = TextEdit.LINE_WRAPPING_BOUNDARY
	_text_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_text_edit.focus_entered.connect(_begin)
	_text_edit.focus_exited.connect(_commit)
	_text_edit.text_changed.connect(_on_text_changed)
	_fields_box.add_child(_text_edit)

	var hotkey_row := HBoxContainer.new()
	hotkey_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_fields_box.add_child(hotkey_row)
	_hotkey_button = Button.new()
	_hotkey_button.name = "StringsHotkeyButton"
	_hotkey_button.text = "Toggle {hot} marker"
	_hotkey_button.tooltip_text = "Insert a {hot} accelerator marker at the caret, or remove the existing one."
	_hotkey_button.pressed.connect(_on_toggle_hotkey)
	hotkey_row.add_child(_hotkey_button)

	_hotkey_preview = RichTextLabel.new()
	_hotkey_preview.name = "StringsHotkeyPreview"
	_hotkey_preview.bbcode_enabled = true
	_hotkey_preview.fit_content = true
	_hotkey_preview.scroll_active = false
	_hotkey_preview.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_fields_box.add_child(_hotkey_preview)

	InspectorForms.add_section_heading(_fields_box, "Section")
	_section_option = OptionButton.new()
	_section_option.name = "StringsSectionOption"
	_section_option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_section_option.item_selected.connect(_on_section_selected)
	_fields_box.add_child(_section_option)

	InspectorForms.add_section_heading(_fields_box, "Position")
	_pos_x = InspectorForms.add_spin_row(_fields_box, "StringsPosX", "X", POS_MIN, POS_MAX, 1)
	_pos_y = InspectorForms.add_spin_row(_fields_box, "StringsPosY", "Y", POS_MIN, POS_MAX, 1)
	_wire_spin(_pos_x)
	_wire_spin(_pos_y)

	show_entry(_current_index)


func show_entry(index: int) -> void:
	# Flush any pending edit from the previously selected entry first.
	if _doc != null:
		_doc.commit_edit()
	_current_index = index
	var valid := _doc != null and index >= 0 and index < _doc.string_table.get_entry_count()
	if _empty_label != null:
		_empty_label.visible = not valid
	if _fields_box != null:
		_fields_box.visible = valid
	if not valid:
		return

	var table := _doc.string_table
	_loading = true
	_key_edit.text = table.get_entry_key(index)
	_text_edit.text = table.get_entry_text(index)
	_populate_sections(table, table.get_entry_section_index(index))
	var pos := table.get_entry_position(index)
	_pos_x.value = pos.x
	_pos_y.value = pos.y
	_loading = false
	_update_preview(_text_edit.text)


# --- Edit handlers ---

func _on_key_changed(new_text: String) -> void:
	if _loading or _current_index < 0:
		return
	_begin()  # idempotent: arms the session even if focus_entered did not fire
	_doc.set_entry_key_live(_current_index, new_text)
	_notify_row(_current_index)


func _on_text_changed() -> void:
	if _loading or _current_index < 0:
		return
	_begin()  # idempotent: arms the session even if focus_entered did not fire
	var text := _text_edit.text
	_doc.set_entry_text_live(_current_index, text)
	_notify_row(_current_index)
	_update_preview(text)


func _on_section_selected(item_index: int) -> void:
	if _loading or _current_index < 0:
		return
	# Moving an entry between sections is structural: the entry relocates to the
	# end of its new section's run (the game reads entries by contiguous section
	# runs). The document updates selected_index and emits structure_changed; the
	# rebuild re-selects the moved row. Deferred so the rebuild does not happen
	# from inside this dropdown's own signal handler.
	var section_id := _section_option.get_item_id(item_index)
	_doc.call_deferred("move_entry_to_section", _current_index, section_id)


func _wire_spin(spin: SpinBox) -> void:
	spin.value_changed.connect(_on_position_changed)
	var line := spin.get_line_edit()
	if line != null:
		line.focus_entered.connect(_begin)
		line.focus_exited.connect(_commit)


func _on_position_changed(_value: float) -> void:
	if _loading or _current_index < 0:
		return
	_begin()
	_doc.set_entry_position_live(_current_index, Vector2i(int(_pos_x.value), int(_pos_y.value)))
	_notify_row(_current_index)


func _on_toggle_hotkey() -> void:
	if _current_index < 0:
		return
	_begin()
	_loading = true
	var text := _text_edit.text
	var marker_at := text.find("{hot}")
	if marker_at >= 0:
		text = text.substr(0, marker_at) + text.substr(marker_at + 5)
		_text_edit.text = text
	else:
		_text_edit.insert_text_at_caret("{hot}")
		text = _text_edit.text
	_loading = false
	_doc.set_entry_text_live(_current_index, text)
	_notify_row(_current_index)
	_update_preview(text)
	_commit()


# --- Helpers ---

func _begin() -> void:
	if _doc != null and _current_index >= 0:
		_doc.begin_edit()


func _commit() -> void:
	if _doc != null:
		_doc.commit_edit()


func _notify_row(index: int) -> void:
	if _row_changed.is_valid():
		_row_changed.call(index)


func _populate_sections(table: RtxtStringFile, current_section: int) -> void:
	_section_option.clear()
	var matched := false
	for s in table.get_section_count():
		_section_option.add_item("%s (%d)" % [table.get_section_name(s), s], s)
		if s == current_section:
			_section_option.select(_section_option.get_item_count() - 1)
			matched = true
	if not matched:
		_section_option.add_item("Invalid (#%d)" % current_section, current_section)
		_section_option.select(_section_option.get_item_count() - 1)


func _update_preview(text: String) -> void:
	var result := RtxtStringFile.strip_hotkey_with_index(text)
	var stripped := String(result.get("text", text))
	var index := int(result.get("index", -1))
	if index >= 0 and index < stripped.length():
		var left := _escape_bbcode(stripped.substr(0, index))
		var ch := _escape_bbcode(stripped.substr(index, 1))
		var right := _escape_bbcode(stripped.substr(index + 1))
		_hotkey_preview.text = "In-game: %s[u][color=%s]%s[/color][/u]%s" % [left, ACCEL_COLOR, ch, right]
	else:
		_hotkey_preview.text = "In-game: %s" % _escape_bbcode(stripped)


func _escape_bbcode(text: String) -> String:
	return text.replace("[", "[lb]")
