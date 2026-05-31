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

	_build_color_section(id)
	_build_texture_section(id)
	_build_flag_section(id)


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
				if (e[0] as CheckBox).button_pressed:
					mask |= int(e[1])
			_emit({"target": "widget", "id": id, "prop": "flags", "value": mask}))


# Commit a LineEdit on Enter and on blur. The edit's "value" is the field text at
# commit time; the editor no-ops if it equals the document, so a blur with no
# change (including the focus_exited fired when the row is torn down) is harmless.
func _wire_text(edit: LineEdit, base: Dictionary) -> void:
	var commit := func() -> void:
		var e := base.duplicate()
		e["value"] = edit.text
		_emit(e)
	edit.text_submitted.connect(func(_text: String) -> void: commit.call())
	edit.focus_exited.connect(func() -> void: commit.call())


# Any of the four spins changing commits the full rectangle (the editor no-ops if
# unchanged), so partial edits accumulate against the live document.
func _wire_rect(id: int, sx: SpinBox, sy: SpinBox, sw: SpinBox, sh: SpinBox) -> void:
	var commit := func(_v: float) -> void:
		_emit({"target": "widget", "id": id, "prop": "rect",
			"value": Rect2(sx.value, sy.value, sw.value, sh.value)})
	sx.value_changed.connect(commit)
	sy.value_changed.connect(commit)
	sw.value_changed.connect(commit)
	sh.value_changed.connect(commit)
