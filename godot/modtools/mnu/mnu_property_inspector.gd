class_name MnuPropertyInspector
extends Control

# Read-only property view for the selected MNU node (a screen container or a
# widget). M6 surfaces the full property surface as label/value rows; M7 swaps
# these for editable rows that push undo. Kept presentation-only: it reads the
# document but never mutates it.

const MnuUiHelpersScript = preload("res://modtools/mnu/mnu_ui_helpers.gd")

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
		_build_screen_rows()
	else:
		_build_widget_rows()


func _build_screen_rows() -> void:
	var id := _selected_id
	MnuUiHelpersScript.add_heading(_box, "Screen")
	MnuUiHelpersScript.add_kv_row(_box, "Name", _document.get_screen_name(id))
	MnuUiHelpersScript.add_kv_row(_box, "Music var", str(_document.get_screen_music_var(id)))
	MnuUiHelpersScript.add_kv_row(_box, "Text resource", _document.get_screen_text_rsrc(id))
	MnuUiHelpersScript.add_kv_row(_box, "Cursor", _document.get_screen_cursor_file(id))


func _build_widget_rows() -> void:
	var id := _selected_id
	var type_name := _document.get_widget_type_name(_document.get_widget_type(id))
	MnuUiHelpersScript.add_heading(_box, type_name)
	MnuUiHelpersScript.add_kv_row(_box, "Name", _document.get_widget_name(id))

	var rect := _document.get_window_rect(id)
	MnuUiHelpersScript.add_kv_row(_box, "Position", "%d, %d" % [int(rect.position.x), int(rect.position.y)])
	MnuUiHelpersScript.add_kv_row(_box, "Size", "%d x %d" % [int(rect.size.x), int(rect.size.y)])

	var text := _document.get_widget_text(id)
	if not text.is_empty():
		var string_type := _document.get_widget_string_type(id)
		var suffix := "  (string id)" if string_type == "id" else ""
		MnuUiHelpersScript.add_kv_row(_box, "Text", text + suffix)

	var font := _document.get_widget_font(id)
	if not font.is_empty():
		MnuUiHelpersScript.add_kv_row(_box, "Font", font)

	_build_color_section()
	_build_texture_section()
	_build_flag_section()


func _build_color_section() -> void:
	# Color slot label / NovaMnuDocument.ColorSlot pairs, in display order. Built
	# locally (not a const) so the native enum values resolve at runtime.
	var slots := [
		["Text", NovaMnuDocument.COLOR_DEFAULT_FG],
		["Background", NovaMnuDocument.COLOR_DEFAULT_BG],
		["Hover text", NovaMnuDocument.COLOR_MOUSEOVER_FG],
		["Selected text", NovaMnuDocument.COLOR_SELECTED_FG],
		["Disabled text", NovaMnuDocument.COLOR_DISABLED_FG],
	]
	var rows: Array = []
	for slot in slots:
		var raw := _document.get_widget_color(_selected_id, int(slot[1]))
		if not raw.is_empty():
			rows.append([String(slot[0]), raw])
	if rows.is_empty():
		return
	MnuUiHelpersScript.add_heading(_box, "Colors")
	for row in rows:
		MnuUiHelpersScript.add_color_row(_box, String(row[0]), String(row[1]))


func _build_texture_section() -> void:
	var slots := [
		["Normal", NovaMnuDocument.TEX_DEFAULT],
		["Hover", NovaMnuDocument.TEX_MOUSEOVER],
		["Selected", NovaMnuDocument.TEX_SELECTED],
		["Disabled", NovaMnuDocument.TEX_DISABLED],
	]
	var rows: Array = []
	for slot in slots:
		var raw := _document.get_widget_texture(_selected_id, int(slot[1]))
		if not raw.is_empty():
			rows.append([String(slot[0]), raw])
	if rows.is_empty():
		return
	MnuUiHelpersScript.add_heading(_box, "Textures")
	for row in rows:
		MnuUiHelpersScript.add_kv_row(_box, String(row[0]), String(row[1]))


func _build_flag_section() -> void:
	var flags := _document.get_widget_flags(_selected_id)
	if flags == 0:
		return
	var labels := _document.get_flag_labels()
	var active: Array = []
	for i in range(labels.size()):
		if (flags & (1 << i)) != 0:
			active.append(String(labels[i]))
	if active.is_empty():
		return
	MnuUiHelpersScript.add_heading(_box, "Flags")
	MnuUiHelpersScript.add_muted(_box, ", ".join(active))
