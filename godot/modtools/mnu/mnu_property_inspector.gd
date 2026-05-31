class_name MnuPropertyInspector
extends Control

# Editable property view for the selected MNU node (a screen container or a
# widget). Every row commits through edit_requested(edit); the workspace adapter
# forwards that to the editor, which applies the mutation and records one undo
# step. The inspector never mutates the document itself, so the editor owns
# before/after and the undo stack (it also no-ops unchanged values, which makes
# committing on both Enter and blur, including teardown, harmless).
#
# edit dict shape: {target: "widget"|"screen", id: int, prop: String,
#   slot: int (color/texture only), value: Variant}.

signal edit_requested(edit: Dictionary)

const MnuUiHelpersScript = preload("res://modtools/mnu/mnu_ui_helpers.gd")
const MnuListEditorScript = preload("res://modtools/mnu/mnu_list_editor.gd")

# Position spins span negative coords (a widget can sit off the authoring board);
# sizes never do.
const POS_MIN := -4096
const POS_MAX := 4096
const SIZE_MAX := 4096

var _document: NovaMnuDocument
var _selected_id := -1
var _box: VBoxContainer


func _ready() -> void:
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL
	_rebuild()


func show_widget(doc: NovaMnuDocument, id: int) -> void:
	_document = doc
	_selected_id = id
	if is_node_ready():
		_rebuild()


func _rebuild() -> void:
	for child in get_children():
		child.queue_free()
	_box = MnuUiHelpersScript.make_inspector_box(self)

	if _document == null or _selected_id < 0 or not _document.widget_exists(_selected_id):
		MnuUiHelpersScript.add_muted(_box, "Select a screen or widget to inspect.")
		return

	if _document.is_screen(_selected_id):
		_build_screen_rows(_selected_id)
	else:
		_build_widget_rows(_selected_id)


func _emit(edit: Dictionary) -> void:
	edit_requested.emit(edit)


# --- screens --------------------------------------------------------------------

func _build_screen_rows(id: int) -> void:
	MnuUiHelpersScript.add_heading(_box, "Screen")

	var name_edit := MnuUiHelpersScript.add_text_edit_row(_box, "Name", _document.get_screen_name(id))
	_wire_text(name_edit, {"target": "screen", "id": id, "prop": "name"})

	var music_spin := MnuUiHelpersScript.add_spin_row(_box, "Music var", _document.get_screen_music_var(id), 0, SIZE_MAX)
	music_spin.value_changed.connect(func(v: float) -> void:
		_emit({"target": "screen", "id": id, "prop": "music_var", "value": int(v)}))

	var rsrc_edit := MnuUiHelpersScript.add_text_edit_row(_box, "Text resource", _document.get_screen_text_rsrc(id))
	_wire_text(rsrc_edit, {"target": "screen", "id": id, "prop": "text_rsrc"})

	var cursor_edit := MnuUiHelpersScript.add_text_edit_row(_box, "Cursor", _document.get_screen_cursor_file(id))
	_wire_text(cursor_edit, {"target": "screen", "id": id, "prop": "cursor"})


# --- widgets --------------------------------------------------------------------

func _build_widget_rows(id: int) -> void:
	var type_name := _document.get_widget_type_name(_document.get_widget_type(id))
	MnuUiHelpersScript.add_heading(_box, type_name)

	var name_edit := MnuUiHelpersScript.add_text_edit_row(_box, "Name", _document.get_widget_name(id))
	_wire_text(name_edit, {"target": "widget", "id": id, "prop": "name"})

	var rect := _document.get_window_rect(id)
	var pos = MnuUiHelpersScript.add_spin_pair_row(_box, "Position", int(rect.position.x), int(rect.position.y), POS_MIN, POS_MAX)
	var sz = MnuUiHelpersScript.add_spin_pair_row(_box, "Size", int(rect.size.x), int(rect.size.y), 0, SIZE_MAX)
	_wire_rect(id, pos[0], pos[1], sz[0], sz[1])

	var text_edit := MnuUiHelpersScript.add_text_edit_row(_box, "Text", _document.get_widget_text(id))
	_wire_text(text_edit, {"target": "widget", "id": id, "prop": "text"})
	var id_check := MnuUiHelpersScript.add_check_row(_box, "Text is a string id", _document.get_widget_string_type(id) == "id")
	id_check.toggled.connect(func(pressed: bool) -> void:
		_emit({"target": "widget", "id": id, "prop": "string_type", "value": "id" if pressed else ""}))

	var font_edit := MnuUiHelpersScript.add_text_edit_row(_box, "Font", _document.get_widget_font(id))
	_wire_text(font_edit, {"target": "widget", "id": id, "prop": "font"})

	# Type-specific scalar template fields (M9). Authoring of the richer nested
	# structures (table columns, combo/spinlist item lists) is a later milestone.
	var wtype := _document.get_widget_type(id)
	if wtype == NovaMnuDocument.TYPE_SCROLL or wtype == NovaMnuDocument.TYPE_MARQUEE:
		var orient_edit := MnuUiHelpersScript.add_text_edit_row(_box, "Orientation", _document.get_widget_orientation(id))
		_wire_text(orient_edit, {"target": "widget", "id": id, "prop": "orientation"})
	if wtype == NovaMnuDocument.TYPE_MARQUEE:
		var ds_edit := MnuUiHelpersScript.add_text_edit_row(_box, "Datasource", _document.get_widget_datasource(id))
		_wire_text(ds_edit, {"target": "widget", "id": id, "prop": "datasource"})

	_build_color_section(id)
	_build_texture_section(id)
	_build_flag_section(id)

	# M10: nested template authoring. Item rows for list-like widgets; column
	# header/body definitions for tables. These emit op-tagged edits that the
	# editor routes through the snapshot-undo path (the row set is a collection,
	# not a single scalar, so the whole-list state is the natural undo unit).
	if wtype == NovaMnuDocument.TYPE_LIST or wtype == NovaMnuDocument.TYPE_MULTI \
			or wtype == NovaMnuDocument.TYPE_SPINLIST or wtype == NovaMnuDocument.TYPE_COMBO:
		_build_item_section(id)
	elif wtype == NovaMnuDocument.TYPE_TABLE:
		_build_table_section(id)


# Only slots that currently carry a value are shown (faithful to M6's display);
# setting a previously-empty slot is deferred to a later authoring increment.
func _build_color_section(id: int) -> void:
	var slots := [
		["Text", NovaMnuDocument.COLOR_DEFAULT_FG],
		["Background", NovaMnuDocument.COLOR_DEFAULT_BG],
		["Hover text", NovaMnuDocument.COLOR_MOUSEOVER_FG],
		["Selected text", NovaMnuDocument.COLOR_SELECTED_FG],
		["Disabled text", NovaMnuDocument.COLOR_DISABLED_FG],
	]
	var present: Array = []
	for slot in slots:
		var raw := _document.get_widget_color(id, int(slot[1]))
		if not raw.is_empty():
			present.append([String(slot[0]), int(slot[1]), raw])
	if present.is_empty():
		return
	MnuUiHelpersScript.add_heading(_box, "Colors")
	for row in present:
		var slot_index := int(row[1])
		var pair = MnuUiHelpersScript.add_color_edit_row(_box, String(row[0]), String(row[2]))
		var swatch: ColorRect = pair[0]
		var edit: LineEdit = pair[1]
		edit.text_changed.connect(func(text: String) -> void:
			if is_instance_valid(swatch):
				MnuUiHelpersScript.refresh_swatch(swatch, text))
		_wire_text(edit, {"target": "widget", "id": id, "prop": "color", "slot": slot_index})


func _build_texture_section(id: int) -> void:
	var slots := [
		["Normal", NovaMnuDocument.TEX_DEFAULT],
		["Hover", NovaMnuDocument.TEX_MOUSEOVER],
		["Selected", NovaMnuDocument.TEX_SELECTED],
		["Disabled", NovaMnuDocument.TEX_DISABLED],
	]
	var present: Array = []
	for slot in slots:
		var raw := _document.get_widget_texture(id, int(slot[1]))
		if not raw.is_empty():
			present.append([String(slot[0]), int(slot[1]), raw])
	if present.is_empty():
		return
	MnuUiHelpersScript.add_heading(_box, "Textures")
	for row in present:
		var slot_index := int(row[1])
		var edit := MnuUiHelpersScript.add_text_edit_row(_box, String(row[0]), String(row[2]))
		_wire_text(edit, {"target": "widget", "id": id, "prop": "texture", "slot": slot_index})


# All flag toggles are shown; any toggle re-derives the whole bitmask from the
# checkboxes so the edit carries one complete flags value.
func _build_flag_section(id: int) -> void:
	var labels := _document.get_flag_labels()
	if labels.is_empty():
		return
	var flags := _document.get_widget_flags(id)
	MnuUiHelpersScript.add_heading(_box, "Flags")
	var checks: Array = []
	for i in range(labels.size()):
		var bit := 1 << i
		var check := MnuUiHelpersScript.add_check_row(_box, String(labels[i]), (flags & bit) != 0)
		checks.append([check, bit])
	for entry in checks:
		var check_box: CheckBox = entry[0]
		check_box.toggled.connect(func(_pressed: bool) -> void:
			var mask := 0
			for e in checks:
				if is_instance_valid(e[0]) and (e[0] as CheckBox).button_pressed:
					mask |= int(e[1])
			_emit({"target": "widget", "id": id, "prop": "flags", "value": mask}))


# --- M10: nested template editors ----------------------------------------------

# Item rows for list / multi / spinlist / combo. The list editor renders the
# rows and reports intent; the editor applies the mutation + undo. Rows carry
# {text, value, type}; the type column is a small enum (the MNU item type tag).
func _build_item_section(id: int) -> void:
	MnuUiHelpersScript.add_heading(_box, "Items")
	var editor = MnuListEditorScript.new()
	editor.configure([
		{"key": "text", "label": "Text", "kind": "text"},
		{"key": "value", "label": "Value", "kind": "text"},
		{"key": "type", "label": "Type", "kind": "enum", "options": ["", "id", "color", "image"]},
	])
	_box.add_child(editor)
	editor.set_rows(_document.get_items(id))
	editor.row_field_changed.connect(func(index: int, key: String, value: Variant) -> void:
		_emit({"id": id, "op": "item_field", "index": index, "key": key, "value": value}))
	editor.row_added.connect(func() -> void:
		_emit({"id": id, "op": "item_add",
			"row": {"type": "", "value": str(_document.get_item_count(id)), "text": "New Item"}}))
	editor.row_removed.connect(func(index: int) -> void:
		_emit({"id": id, "op": "item_remove", "index": index}))
	editor.row_moved.connect(func(from: int, to: int) -> void:
		_emit({"id": id, "op": "item_move", "from": from, "to": to}))


# Table COLUMN authoring: column count + spacing (scalar edits) and the per-column
# HEADER definitions (a list editor). Headers are placed by their `column`
# attribute, so reorder is disabled. Value->image SUBST rows are not yet
# serialized by libs/mnu, so they are intentionally not editable here.
func _build_table_section(id: int) -> void:
	MnuUiHelpersScript.add_heading(_box, "Table columns")
	var count_spin := MnuUiHelpersScript.add_spin_row(_box, "Columns", _document.get_table_column_count(id), 0, 64)
	count_spin.value_changed.connect(func(v: float) -> void:
		_emit({"target": "widget", "id": id, "prop": "table_count", "value": int(v)}))
	var spacing_spin := MnuUiHelpersScript.add_spin_row(_box, "Spacing", _document.get_table_column_spacing(id), 0, 256)
	spacing_spin.value_changed.connect(func(v: float) -> void:
		_emit({"target": "widget", "id": id, "prop": "table_spacing", "value": int(v)}))

	MnuUiHelpersScript.add_muted(_box, "Headers")
	var editor = MnuListEditorScript.new()
	editor.configure([
		{"key": "column", "label": "Col", "kind": "int", "min": 0, "max": 63},
		{"key": "text", "label": "Title", "kind": "text"},
		{"key": "width", "label": "W", "kind": "int", "min": 0, "max": 4096},
		{"key": "justify", "label": "Justify", "kind": "enum", "options": ["", "LEFT", "CENTER", "RIGHT"]},
		{"key": "sort", "label": "Sort", "kind": "text"},
	], false)
	_box.add_child(editor)
	editor.set_rows(_document.get_table_headers(id))
	editor.row_field_changed.connect(func(index: int, key: String, value: Variant) -> void:
		_emit({"id": id, "op": "header_field", "index": index, "key": key, "value": value}))
	editor.row_added.connect(func() -> void:
		_emit({"id": id, "op": "header_add",
			"row": {"column": _document.get_table_headers(id).size(), "text": "Column", "width": 80, "justify": "LEFT"}}))
	editor.row_removed.connect(func(index: int) -> void:
		_emit({"id": id, "op": "header_remove", "index": index}))


# Commit a LineEdit on Enter and on blur. The edit's "value" is the field text at
# commit time; the editor no-ops if it equals the document. A focus_exited that
# fires while the field is being rebuilt or torn down (no longer in the tree, or
# already freed) is skipped: the captured control is mid-teardown and the row
# already reflects the document.
func _wire_text(edit: LineEdit, base: Dictionary) -> void:
	var commit := func() -> void:
		if not is_instance_valid(edit) or not edit.is_inside_tree():
			return
		var e := base.duplicate()
		e["value"] = edit.text
		_emit(e)
	edit.text_submitted.connect(func(_text: String) -> void: commit.call())
	edit.focus_exited.connect(func() -> void: commit.call())


# Any of the four spins changing commits the full rectangle (the editor no-ops if
# unchanged), so partial edits accumulate against the live document.
func _wire_rect(id: int, sx: SpinBox, sy: SpinBox, sw: SpinBox, sh: SpinBox) -> void:
	var commit := func(_v: float) -> void:
		if not (is_instance_valid(sx) and is_instance_valid(sy) and is_instance_valid(sw) and is_instance_valid(sh)):
			return
		_emit({"target": "widget", "id": id, "prop": "rect",
			"value": Rect2(sx.value, sy.value, sw.value, sh.value)})
	sx.value_changed.connect(commit)
	sy.value_changed.connect(commit)
	sw.value_changed.connect(commit)
	sh.value_changed.connect(commit)
