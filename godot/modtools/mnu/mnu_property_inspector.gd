class_name MnuPropertyInspector
extends MarginContainer

# Editable property view for the selected MNU node (a screen container or a
# widget). Extends a container (not a bare Control) so the make_inspector_box
# margin/scroll/box chain it hosts gets a real size: the shell mounts this into a
# PanelContainer that sizes it to fill, and a MarginContainer in turn lays out its
# content (a plain Control would leave the size_flags-driven children at zero size,
# leaving the whole inspector blank). Every row commits through
# edit_requested(edit); the workspace adapter
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
# When >1, the inspector shows a read-only multi-selection summary instead of a single
# node's editable rows (editing requires selecting one widget).
var _multi_ids: PackedInt32Array = PackedInt32Array()
var _box: VBoxContainer


func _ready() -> void:
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL
	_rebuild()


func show_widget(doc: NovaMnuDocument, id: int) -> void:
	_document = doc
	_selected_id = id
	_multi_ids = PackedInt32Array()
	if is_node_ready():
		_rebuild()


# Show a summary for a multi-selection (>1 widget). Zero or one id delegates to the
# normal single-node view. The workspace routes the editor's selection_changed here.
func show_selection(doc: NovaMnuDocument, ids: PackedInt32Array) -> void:
	if ids.size() <= 1:
		show_widget(doc, ids[0] if ids.size() == 1 else -1)
		return
	_document = doc
	_selected_id = -1
	_multi_ids = ids
	if is_node_ready():
		_rebuild()


func _rebuild() -> void:
	for child in get_children():
		child.queue_free()
	_box = MnuUiHelpersScript.make_inspector_box(self)

	if _multi_ids.size() > 1:
		_build_multi_rows()
		return

	if _document == null or _selected_id < 0 or not _document.widget_exists(_selected_id):
		MnuUiHelpersScript.add_muted(_box, "Select a screen or widget to inspect.")
		return

	if _document.is_screen(_selected_id):
		_build_screen_rows(_selected_id)
	else:
		_build_widget_rows(_selected_id)


# Read-only summary for a multi-selection: a count heading + one muted line per member
# (matching the canvas caption format).
func _build_multi_rows() -> void:
	MnuUiHelpersScript.add_heading(_box, "%d widgets selected" % _multi_ids.size())
	if _document == null:
		return
	for id in _multi_ids:
		if not _document.widget_exists(id):
			continue
		var type_name := _document.get_widget_type_name(_document.get_widget_type(id))
		var wname := _document.get_widget_name(id)
		var label := "(%s)  #%d" % [type_name, id] if wname.is_empty() else "%s (%s)  #%d" % [wname, type_name, id]
		MnuUiHelpersScript.add_muted(_box, label)


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
	# Name + stable id under the type, so the selection is never ambiguous (the canvas
	# pick, the tree, and the inspector all key off this id).
	var wname := _document.get_widget_name(id)
	MnuUiHelpersScript.add_muted(_box, "#%d" % id if wname.is_empty() else "%s  #%d" % [wname, id])

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

	# Radio/checkbox group id: widgets sharing a group toggle as one set. The engine
	# stores it on every window, but it is only meaningful (and only worth surfacing)
	# for the grouped toggle types.
	if wtype == NovaMnuDocument.TYPE_RADIO or wtype == NovaMnuDocument.TYPE_CHECKBOX:
		var group_spin := MnuUiHelpersScript.add_spin_row(_box, "Group", _document.get_widget_group(id), 0, SIZE_MAX)
		group_spin.value_changed.connect(func(v: float) -> void:
			_emit({"target": "widget", "id": id, "prop": "group", "value": int(v)}))

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


# Color/texture sections show every populated slot as an editable row, then offer
# an "Add" picker over the still-empty slots so a previously uncolored / untextured
# widget can gain one. Adding sets the slot to a default and rebuilds so it appears
# as an editable row; it reuses the present rows' {prop, slot, value} op, so the
# editor records one undo step (undo clears the slot, dropping the row on rebuild).
func _build_color_section(id: int) -> void:
	var slots := [
		["Text", NovaMnuDocument.COLOR_DEFAULT_FG],
		["Background", NovaMnuDocument.COLOR_DEFAULT_BG],
		["Hover text", NovaMnuDocument.COLOR_MOUSEOVER_FG],
		["Hover bg", NovaMnuDocument.COLOR_MOUSEOVER_BG],
		["Selected text", NovaMnuDocument.COLOR_SELECTED_FG],
		["Selected bg", NovaMnuDocument.COLOR_SELECTED_BG],
		["Disabled text", NovaMnuDocument.COLOR_DISABLED_FG],
		["Disabled bg", NovaMnuDocument.COLOR_DISABLED_BG],
	]
	MnuUiHelpersScript.add_heading(_box, "Colors")
	var empty: Array = []
	for slot in slots:
		var slot_index := int(slot[1])
		var raw := _document.get_widget_color(id, slot_index)
		if raw.is_empty():
			empty.append([String(slot[0]), slot_index])
			continue
		var pair = MnuUiHelpersScript.add_color_edit_row(_box, String(slot[0]), raw)
		var swatch: ColorRect = pair[0]
		var edit: LineEdit = pair[1]
		edit.text_changed.connect(func(text: String) -> void:
			if is_instance_valid(swatch):
				MnuUiHelpersScript.refresh_swatch(swatch, text))
		_wire_text(edit, {"target": "widget", "id": id, "prop": "color", "slot": slot_index})
	_build_add_slot(id, "Add color", empty, "color", "FFFFFF")


func _build_texture_section(id: int) -> void:
	var slots := [
		["Normal", NovaMnuDocument.TEX_DEFAULT],
		["Hover", NovaMnuDocument.TEX_MOUSEOVER],
		["Selected", NovaMnuDocument.TEX_SELECTED],
		["Disabled", NovaMnuDocument.TEX_DISABLED],
	]
	MnuUiHelpersScript.add_heading(_box, "Textures")
	var empty: Array = []
	for slot in slots:
		var slot_index := int(slot[1])
		var raw := _document.get_widget_texture(id, slot_index)
		if raw.is_empty():
			empty.append([String(slot[0]), slot_index])
			continue
		var edit := MnuUiHelpersScript.add_text_edit_row(_box, String(slot[0]), raw)
		_wire_text(edit, {"target": "widget", "id": id, "prop": "texture", "slot": slot_index})
	# A new texture seeds a visible placeholder filename the author then repoints at
	# a real .tga (textures have no neutral default the way a color has white).
	_build_add_slot(id, "Add texture", empty, "texture", "texture.tga")


# Append the "Add <slot>" picker over the still-empty slots (no-op when none are
# empty). On Add, set the chosen slot to default_value through the normal prop-edit
# path, then rebuild so its editable row shows. The pressed button outlives this
# callback (the rebuild's queue_free is deferred), so emitting first is safe.
func _build_add_slot(id: int, label: String, empty: Array, prop: String, default_value: String) -> void:
	var controls = MnuUiHelpersScript.add_add_slot_row(_box, label, empty)
	if controls.is_empty():
		return
	var picker: OptionButton = controls[0]
	var add_btn: Button = controls[1]
	add_btn.pressed.connect(func() -> void:
		if not is_instance_valid(picker):
			return
		_emit({"target": "widget", "id": id, "prop": prop, "slot": picker.get_selected_id(), "value": default_value})
		_rebuild())


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


# Table COLUMN authoring: column count + spacing (scalar edits), the per-column
# HEADER definitions, and the value->image SUBST cells (both list editors). Headers
# and substitutions are keyed by their `column` attribute, so reorder is disabled.
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

	# Value->image SUBST cells: when column `column` holds `value`, the cell renders
	# image `file` (the Img flag = the FILE attribute). Keyed by column/value rather
	# than order, so reorder is disabled, mirroring headers.
	MnuUiHelpersScript.add_muted(_box, "Substitutions")
	var subst = MnuListEditorScript.new()
	subst.configure([
		{"key": "column", "label": "Col", "kind": "int", "min": 0, "max": 63},
		{"key": "value", "label": "Value", "kind": "text"},
		{"key": "is_file", "label": "Img", "kind": "bool"},
		{"key": "file", "label": "File", "kind": "text"},
	], false)
	_box.add_child(subst)
	subst.set_rows(_document.get_table_substs(id))
	subst.row_field_changed.connect(func(index: int, key: String, value: Variant) -> void:
		_emit({"id": id, "op": "subst_field", "index": index, "key": key, "value": value}))
	subst.row_added.connect(func() -> void:
		_emit({"id": id, "op": "subst_add",
			"row": {"column": 0, "value": "", "is_file": true, "file": ""}}))
	subst.row_removed.connect(func(index: int) -> void:
		_emit({"id": id, "op": "subst_remove", "index": index}))


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
