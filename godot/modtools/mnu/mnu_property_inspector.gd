class_name MnuPropertyInspector
extends MarginContainer

# Editable property view for a selected MNU screen or widget. MarginContainer
# gives the mounted margin/scroll/box chain a real size inside the shell.
# Every row emits edit_requested; the workspace applies it and owns snapshot
# undo. The inspector never mutates the document, and unchanged edits no-op.
#
# edit dict shape: {target: "widget"|"screen", id: int, prop: String,
#   slot: int (color/texture only), value: Variant}.

signal edit_requested(edit: Dictionary)
# Cross-workspace jump intents are routed through the workspace and shell.
signal string_jump_requested(key: String)
signal font_jump_requested(font: String)
signal menu_jump_requested(file: String, screen: String)
signal style_jump_requested(variable: String)
signal sound_preview_requested(trigger: String, file: String)

const MnuUiHelpersScript = preload("res://modtools/mnu/mnu_ui_helpers.gd")
const MnuListEditorScript = preload("res://modtools/mnu/mnu_list_editor.gd")
const MnuStringPickerScript = preload("res://modtools/mnu/mnu_string_picker.gd")
const MnsVariableTableScript = preload("res://modtools/mnu/mns_variable_table.gd")
const MnuWidgetInteractionEditorScript = preload("res://modtools/mnu/mnu_widget_interaction_editor.gd")

# Positions allow off-board widgets; sizes do not.
const POS_MIN := -4096
const POS_MAX := 4096
const SIZE_MAX := 4096

var _document: NovaMnuDocument
var _selected_id := -1
# When >1, the inspector shows a compact batch editor for common font, text
# layout, and color properties plus a member summary.
var _multi_ids: PackedInt32Array = PackedInt32Array()
var _box: VBoxContainer
# The resolved string table for the open menu (passed by the workspace from the
# editor) and its path (for the string widgets' jump/badge). Null/empty when
# none is loaded; the string-key helpers then stay hidden.
var _text_resource: RtxtStringFile
var _text_resource_path := ""
# Shell link-widget services (resolve/pick/jump) for FILE references like the
# screen's text_rsrc; string KEYS resolve through the table above instead.
var _ref_services: Dictionary = {}
var _picker: PopupPanel
# The pending pick consumer (a StringRefWidget's on_pick); picker results
# route through it so the widget commits exactly one edit.
var _picker_on_pick: Callable = Callable()
# Sound-set names from the open menu's .lwf profile (set by the workspace). When
# present, the per-sound trigger field becomes a dropdown over the real sets.
var _sound_sets: PackedStringArray = PackedStringArray()
# The menu stylesheet the editor resolved (set by the workspace; null when the
# root carries none). Color/texture/font rows resolve %VAR% swatches through it
# and offer a dropdown over its type-matching variables.
var _stylesheet: MnsStyleSheet
var _authoring_enabled := true


func _ready() -> void:
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL
	_rebuild()


func show_widget(doc: NovaMnuDocument, id: int, text_res: RtxtStringFile = null, text_res_path: String = "") -> void:
	_document = doc
	_selected_id = id
	_multi_ids = PackedInt32Array()
	_text_resource = text_res
	_text_resource_path = text_res_path
	if is_node_ready():
		_rebuild()


## Wires the shell link-widget services (see ResourceRefWidget.services_from_shell).
## Idempotent; safe before or after the form is built.
func set_reference_services(services: Dictionary) -> void:
	_ref_services = services
	if is_node_ready():
		_rebuild()


# The open menu's .lwf set names (the valid sound triggers). The workspace calls
# this when the profile loads; the Sounds section turns its trigger field into a
# dropdown over these. Empty -> free-text trigger entry (no profile loaded).
func set_sound_sets(sets: PackedStringArray) -> void:
	_sound_sets = sets
	if is_node_ready() and _selected_id >= 0:
		_rebuild()


# The resolved menu stylesheet (or null). No rebuild here: the workspace always
# follows with show_widget/show_selection, which rebuilds with it in hand.
func set_stylesheet(sheet: MnsStyleSheet) -> void:
	_stylesheet = sheet


func set_authoring_enabled(enabled: bool) -> void:
	_authoring_enabled = enabled
	if not is_node_ready():
		return
	if enabled:
		# Rebuild instead of blindly enabling every control: optional-value
		# fields and auto-size dimensions have their own disabled state.
		_rebuild()
	else:
		_apply_authoring_lock()


func _apply_authoring_lock() -> void:
	if _authoring_enabled:
		return
	for node in find_children("*", "Control", true, false):
		if node is BaseButton:
			(node as BaseButton).disabled = true
		elif node is LineEdit:
			(node as LineEdit).editable = false
		elif node is TextEdit:
			(node as TextEdit).editable = false
		elif node is SpinBox:
			(node as SpinBox).editable = false


# Show a summary for a multi-selection (>1 widget). Zero or one id delegates to the
# normal single-node view. The workspace routes the editor's selection_changed here.
func show_selection(doc: NovaMnuDocument, ids: PackedInt32Array, text_res: RtxtStringFile = null, text_res_path: String = "") -> void:
	if ids.size() <= 1:
		show_widget(doc, ids[0] if ids.size() == 1 else -1, text_res, text_res_path)
		return
	_document = doc
	_selected_id = -1
	_multi_ids = ids
	_text_resource = text_res
	_text_resource_path = text_res_path
	if is_node_ready():
		_rebuild()


func _rebuild() -> void:
	for child in get_children():
		child.queue_free()
	# The picker (a direct child) dies with the rows above; drop any pending
	# pick consumer with it so a stale callable never outlives its widget.
	_picker_on_pick = Callable()
	_box = MnuUiHelpersScript.make_inspector_box(self)
	if not _authoring_enabled:
		_apply_authoring_lock.call_deferred()

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

	MnuUiHelpersScript.add_heading(_box, "Common properties")
	var font_result := _multi_common(func(id: int) -> Variant: return _document.get_widget_font(id))
	var font_edit := MnuUiHelpersScript.add_text_edit_row(_box, "Font",
		String(font_result.get("value", "")) if bool(font_result.get("same", false)) else "")
	if not bool(font_result.get("same", false)):
		font_edit.placeholder_text = "(mixed)"
	_wire_multi_text(font_edit, {"target": "widgets", "ids": _multi_ids, "prop": "font"})

	var first_state: Dictionary = _document.get_widget_authoring_state(_multi_ids[0])
	for row in [["Horizontal", "justify", ["", "LEFT", "CENTER", "RIGHT"]],
			["Vertical", "vjustify", ["", "TOP", "CENTER", "BOTTOM"]]]:
		var key := String(row[1])
		var result := _multi_common(func(id: int) -> Variant:
			return (_document.get_widget_authoring_state(id).get("string", {}) as Dictionary).get(key, ""))
		var options: Array = (row[2] as Array).duplicate()
		var value := String(result.get("value", "")) if bool(result.get("same", false)) else "(mixed)"
		if value == "(mixed)":
			options.push_front("(mixed)")
		var option := MnuUiHelpersScript.add_option_row(_box, String(row[0]), value, options)
		option.item_selected.connect(func(index: int) -> void:
			var selected := option.get_item_text(index)
			if selected != "(mixed)":
				_emit({"target": "widgets", "ids": _multi_ids, "prop": "patch",
					"value": {"string": {key: selected}}}))

	MnuUiHelpersScript.add_heading(_box, "Common colors")
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
	for slot in slots:
		var slot_index := int(slot[1])
		var result := _multi_common(func(id: int) -> Variant:
			return _document.get_widget_color(id, slot_index))
		var raw := String(result.get("value", "")) if bool(result.get("same", false)) else ""
		var label := String(slot[0]) if bool(result.get("same", false)) else String(slot[0]) + " (mixed)"
		var pair = MnuUiHelpersScript.add_color_edit_row(_box, label, raw)
		var swatch: ColorRect = pair[0]
		var edit: LineEdit = pair[1]
		if not bool(result.get("same", false)):
			edit.placeholder_text = "(mixed)"
		MnuUiHelpersScript.refresh_swatch(swatch, raw, _stylesheet)
		_wire_multi_text(edit, {"target": "widgets", "ids": _multi_ids,
			"prop": "color", "slot": slot_index})
		_append_color_picker(edit, swatch, raw, {"target": "widgets", "ids": _multi_ids,
			"prop": "color", "slot": slot_index})


func _multi_common(getter: Callable) -> Dictionary:
	if _multi_ids.is_empty():
		return {"same": false}
	var value: Variant = getter.call(_multi_ids[0])
	for i in range(1, _multi_ids.size()):
		if getter.call(_multi_ids[i]) != value:
			return {"same": false}
	return {"same": true, "value": value}


func _wire_multi_text(edit: LineEdit, base: Dictionary) -> void:
	var initial := edit.text
	var commit := func(force: bool) -> void:
		if not is_instance_valid(edit) or not edit.is_inside_tree():
			return
		if not force and edit.text == initial:
			return
		var event := base.duplicate()
		event["value"] = edit.text
		_emit(event)
	edit.text_submitted.connect(func(_text: String) -> void: commit.call(true))
	edit.focus_exited.connect(func() -> void: commit.call(false))


func _emit(edit: Dictionary) -> void:
	if not _authoring_enabled:
		return
	edit_requested.emit(edit)


# --- screens --------------------------------------------------------------------

func _build_screen_rows(id: int) -> void:
	MnuUiHelpersScript.add_heading(_box, "Screen")

	var name_edit := MnuUiHelpersScript.add_text_edit_row(_box, "Name", _document.get_screen_name(id))
	_wire_text(name_edit, {"target": "screen", "id": id, "prop": "name"})

	var has_music := MnuUiHelpersScript.add_check_row(_box, "Authored MUSICVAR",
		_document.get_screen_has_music_var(id))
	var music_spin := MnuUiHelpersScript.add_spin_row(_box, "Music var",
		_document.get_screen_music_var(id), 0, SIZE_MAX)
	music_spin.editable = has_music.button_pressed
	has_music.toggled.connect(func(on: bool) -> void:
		music_spin.editable = on
		_emit({"target": "screen", "id": id, "prop": "has_music_var", "value": on}))
	music_spin.value_changed.connect(func(v: float) -> void:
		_emit({"target": "screen", "id": id, "prop": "music_var", "value": int(v)}))

	# The screen's string table is a FILE reference: badge + browse resolve the
	# named .bin against the resource folder, jump opens it in Strings.
	var rsrc_row := MnuUiHelpersScript._row(_box)
	rsrc_row.add_child(MnuUiHelpersScript._key_label("Text resource"))
	var rsrc_widget := ResourceRefWidget.new()
	rsrc_widget.name = "MnuScreenTextRsrc"
	rsrc_widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	# text_rsrc names carry their extension (menutxt.BIN); keep it on pick.
	rsrc_widget.set_value_from_path(func(path: String) -> String: return path.get_file())
	rsrc_widget.configure("strings", "string table", _ref_services)
	rsrc_widget.set_value(_document.get_screen_text_rsrc(id))
	rsrc_widget.value_changed.connect(func(value: String) -> void:
		_emit({"target": "screen", "id": id, "prop": "text_rsrc", "value": value}))
	rsrc_row.add_child(rsrc_widget)
	# Make the live linkage explicit: is the named table the one actually loaded?
	if not _document.get_screen_text_rsrc(id).is_empty():
		if _text_resource != null:
			MnuUiHelpersScript.add_muted(_box, "loaded: %d strings" % _text_resource.get_entry_count())
		else:
			MnuUiHelpersScript.add_muted(_box, "not found (set the resource folder to resolve it)")

	_add_asset_row("Cursor", _document.get_screen_cursor_file(id), "texture",
		func(value: String) -> void:
			_emit({"target": "screen", "id": id, "prop": "cursor", "value": value}))
	var cursor_flags := MnuUiHelpersScript.add_text_edit_row(_box, "Cursor flags",
		_document.get_screen_cursor_flags(id))
	_wire_text(cursor_flags, {"target": "screen", "id": id, "prop": "cursor_flags"})


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

	var authoring: Dictionary = _document.get_widget_authoring_state(id)
	var authored_type := MnuUiHelpersScript.add_text_edit_row(_box, "Authored type",
		String(authoring.get("type_token", "")))
	authored_type.placeholder_text = type_name
	authored_type.tooltip_text = "Raw MNU TYPE token; unknown game-supplied widget types are preserved verbatim"
	_wire_patch_text(authored_type, id, ["type_token"])
	_add_asset_row("Widget text resource", String(authoring.get("text_rsrc", "")),
		"strings", func(value: String) -> void:
			_emit_patch_path(id, ["text_rsrc"], value))

	var rect := _document.get_window_rect(id)
	var pos = MnuUiHelpersScript.add_spin_pair_row(_box, "Position", int(rect.position.x), int(rect.position.y), POS_MIN, POS_MAX)
	var sz = MnuUiHelpersScript.add_spin_pair_row(_box, "Size", int(rect.size.x), int(rect.size.y), 0, SIZE_MAX)
	var rect_flags := _document.get_window_rect_flags(id)
	var auto_width := MnuUiHelpersScript.add_check_row(_box, "Auto width",
		(rect_flags & NovaMnuDocument.RECT_HAS_RIGHT) == 0)
	var auto_height := MnuUiHelpersScript.add_check_row(_box, "Auto height",
		(rect_flags & NovaMnuDocument.RECT_HAS_BOTTOM) == 0)
	(sz[0] as SpinBox).editable = not auto_width.button_pressed
	(sz[1] as SpinBox).editable = not auto_height.button_pressed
	auto_width.toggled.connect(func(on: bool) -> void: (sz[0] as SpinBox).editable = not on)
	auto_height.toggled.connect(func(on: bool) -> void: (sz[1] as SpinBox).editable = not on)
	_wire_rect(id, pos[0], pos[1], sz[0], sz[1], auto_width, auto_height)

	var is_id := _document.get_widget_string_type(id) == "id"
	if is_id:
		# The text IS a string-table key: one link widget carries the key field,
		# the found/missing badge, the table picker, the Strings jump, and the
		# resolved-text preview - all gated on a table actually being loaded.
		_build_string_ref_row(id)
	else:
		var text_edit := MnuUiHelpersScript.add_text_edit_row(_box, "Text", _document.get_widget_text(id))
		_wire_text(text_edit, {"target": "widget", "id": id, "prop": "text"})
	var id_check := MnuUiHelpersScript.add_check_row(_box, "Text is a string id", is_id)
	id_check.toggled.connect(func(pressed: bool) -> void:
		_emit({"target": "widget", "id": id, "prop": "string_type", "value": "id" if pressed else ""}))

	var font_raw := _document.get_widget_font(id)
	var font_ref := _add_asset_row("Font", font_raw, "font",
		func(value: String) -> void:
			_emit({"target": "widget", "id": id, "prop": "font", "value": value}))
	_append_style_var_menu(font_ref.name_edit, "font",
		{"target": "widget", "id": id, "prop": "font"})
	_append_style_jump(font_ref.name_edit, font_raw)
	if not font_raw.is_empty():
		var font_jump := Button.new()
		font_jump.text = "Open in Fonts"
		font_jump.tooltip_text = "Open this font in the Fonts workspace"
		# A %VAR% font resolves through the stylesheet before the jump (the
		# token itself names no file); the basename strips a .fnt the menus
		# author with, so the Fonts workspace's name resolution lands.
		font_jump.pressed.connect(func() -> void:
			font_jump_requested.emit(_resolve_style_token(font_ref.name_edit.text).get_basename()))
		_box.add_child(font_jump)

	var wtype := _document.get_widget_type(id)
	var interaction_editor = MnuWidgetInteractionEditorScript.new()
	add_child(interaction_editor)
	interaction_editor.configure(
		_document, id, wtype, authoring, _sound_sets, _ref_services)
	interaction_editor.edit_requested.connect(func(edit: Dictionary) -> void:
		_emit(edit))
	interaction_editor.rebuild_requested.connect(_rebuild)
	interaction_editor.menu_jump_requested.connect(
		func(file: String, screen: String) -> void:
			menu_jump_requested.emit(file, screen))
	interaction_editor.sound_preview_requested.connect(
		func(trigger: String, file: String) -> void:
			sound_preview_requested.emit(trigger, file))

	_build_string_layout_section(id, authoring)
	interaction_editor.append_behavior_sections(_box)
	_build_frame_section(id, authoring)

	_build_color_section(id)
	_build_texture_section(id)
	_build_appearance_section(id, authoring)
	if wtype == NovaMnuDocument.TYPE_SCROLL:
		_build_scroll_parts_section(id, authoring)
	interaction_editor.append_command_sections(_box)

	# M10: nested template authoring. Item rows for list-like widgets; column
	# header/body definitions for tables. These emit op-tagged edits that the
	# editor routes through the snapshot-undo path (the row set is a collection,
	# not a single scalar, so the whole-list state is the natural undo unit).
	if wtype == NovaMnuDocument.TYPE_LIST or wtype == NovaMnuDocument.TYPE_MULTI \
			or wtype == NovaMnuDocument.TYPE_LAN_LIST \
			or wtype == NovaMnuDocument.TYPE_SPINLIST:
		_build_item_section(id, authoring)
		if wtype == NovaMnuDocument.TYPE_LIST or wtype == NovaMnuDocument.TYPE_MULTI \
				or wtype == NovaMnuDocument.TYPE_LAN_LIST:
			_build_direct_scrollbar_section(id, authoring)
		if wtype == NovaMnuDocument.TYPE_SPINLIST:
			_build_spin_section(id, authoring)
	elif wtype == NovaMnuDocument.TYPE_COMBO:
		# COMBO may author two deliberately independent collections. The
		# top-level ITEMS is the closed/fallback presentation; LIST_BOX/ITEMS is
		# the popup collection. Never mirror one into the other.
		_build_items_block(id, "Closed / fallback items", ["items"],
			authoring.get("items", {}))
		_build_list_box_section(id, authoring)
	elif wtype == NovaMnuDocument.TYPE_MULTILINE_EDIT:
		_build_direct_scrollbar_section(id, authoring)
	elif wtype == NovaMnuDocument.TYPE_TABLE or wtype == NovaMnuDocument.TYPE_GLB_TABLE:
		_build_item_section(id, authoring)
		_build_table_section(id, authoring)


# The Text row for a string-table key: a StringRefWidget resolving against the
# loaded table. With no table the widget degrades to a plain key field (no
# badge/picker/jump/preview), matching the old raw-key behavior.
func _build_string_ref_row(id: int) -> void:
	var row := MnuUiHelpersScript._row(_box)
	row.add_child(MnuUiHelpersScript._key_label("Text"))
	var widget := StringRefWidget.new()
	widget.name = "MnuWidgetTextRef"
	widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	widget.configure("string", _string_key_services())
	widget.set_value(_document.get_widget_text(id))
	widget.value_changed.connect(func(value: String) -> void:
		_emit({"target": "widget", "id": id, "prop": "text", "value": value}))
	row.add_child(widget)


# String-KEY services over the loaded table (context-local - never the shell's
# file services). Empty when no table is loaded, which hides every affordance.
func _string_key_services() -> Dictionary:
	if _text_resource == null:
		return {}
	return {
		"resolve": _resolve_string_key,
		"pick": _pick_string_key,
		"jump": _jump_string_key,
	}


func _resolve_string_key(key: String) -> Dictionary:
	if _text_resource == null or key.is_empty():
		return {}
	if _text_resource.has_string(key):
		return {
			"status": "found",
			"path": _text_resource_path,
			"text": RtxtStringFile.strip_hotkey(_text_resource.get_string(key)),
		}
	return {"status": "missing", "path": _text_resource_path, "text": ""}


func _pick_string_key(current_key: String, on_pick: Callable) -> void:
	_open_string_picker(on_pick, current_key)


func _jump_string_key(key: String, _table_path: String) -> void:
	string_jump_requested.emit(key)


func _open_string_picker(on_pick: Callable, current_key: String) -> void:
	if _text_resource == null:
		return
	if _picker == null or not is_instance_valid(_picker):
		_picker = MnuStringPickerScript.new()
		add_child(_picker)
		_picker.picked.connect(_on_string_picked)
	_picker_on_pick = on_pick
	_picker.open_for(_text_resource, current_key)


func _on_string_picked(key: String) -> void:
	# Route the result back to the widget that asked; its commit emits exactly
	# one edit through the normal path and refreshes its own preview (no rebuild
	# needed - the edit applies silently, so the widget keeps focus/state).
	if _picker_on_pick.is_valid():
		_picker_on_pick.call(key)
	_picker_on_pick = Callable()


func _emit_patch_path(id: int, path: Array, value: Variant) -> void:
	var patch: Variant = value
	for i in range(path.size() - 1, -1, -1):
		patch = {String(path[i]): patch}
	_emit({"target": "widget", "id": id, "prop": "patch", "value": patch})


func _wire_patch_text(edit: LineEdit, id: int, path: Array) -> void:
	var commit := func() -> void:
		if is_instance_valid(edit) and edit.is_inside_tree():
			_emit_patch_path(id, path, edit.text)
	edit.text_submitted.connect(func(_text: String) -> void: commit.call())
	edit.focus_exited.connect(func() -> void: commit.call())


func _add_asset_row(label: String, value: String, kind: String,
		on_change: Callable) -> ResourceRefWidget:
	var row := MnuUiHelpersScript._row(_box)
	row.add_child(MnuUiHelpersScript._key_label(label))
	var widget := ResourceRefWidget.new()
	widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	widget.set_value_from_path(func(path: String) -> String: return path.get_file())
	widget.configure(kind, label, _ref_services)
	widget.set_value(value)
	widget.value_changed.connect(func(next: String) -> void: on_change.call(next))
	row.add_child(widget)
	return widget


func _wire_patch_option(option: OptionButton, id: int, path: Array) -> void:
	option.item_selected.connect(func(index: int) -> void:
		_emit_patch_path(id, path, option.get_item_text(index)))


func _build_optional_int(id: int, label: String, state: Dictionary,
		has_key: String, value_key: String, path: Array, min_value: int, max_value: int) -> void:
	var enabled := MnuUiHelpersScript.add_check_row(_box, "Use " + label.to_lower(),
		bool(state.get(has_key, false)))
	var spin := MnuUiHelpersScript.add_spin_row(_box, label, int(state.get(value_key, 0)), min_value, max_value)
	spin.editable = enabled.button_pressed
	enabled.toggled.connect(func(on: bool) -> void:
		spin.editable = on
		_emit_patch_path(id, path + [has_key], on))
	spin.value_changed.connect(func(value: float) -> void:
		_emit_patch_path(id, path + [value_key], int(value)))


func _build_string_layout_section(id: int, authoring: Dictionary) -> void:
	var data: Dictionary = authoring.get("string", {})
	MnuUiHelpersScript.add_heading(_box, "Text layout")
	var present := MnuUiHelpersScript.add_check_row(_box, "Authored STRING block",
		bool(data.get("present", false)))
	present.toggled.connect(func(on: bool) -> void:
		_emit_patch_path(id, ["string", "present"], on))
	var justify := MnuUiHelpersScript.add_option_row(_box, "Horizontal",
		String(data.get("justify", "")), ["", "LEFT", "CENTER", "RIGHT"])
	_wire_patch_option(justify, id, ["string", "justify"])
	var vjustify := MnuUiHelpersScript.add_option_row(_box, "Vertical",
		String(data.get("vjustify", "")), ["", "TOP", "CENTER", "BOTTOM"])
	_wire_patch_option(vjustify, id, ["string", "vjustify"])
	_build_optional_int(id, "Edge padding", data, "has_edge", "edge", ["string"], 0, SIZE_MAX)


func _rows_at_path(id: int, path: Array) -> Array:
	var current: Variant = _document.get_widget_authoring_state(id)
	for segment in path:
		if not (current is Dictionary):
			return []
		current = (current as Dictionary).get(String(segment), [])
	if not (current is Array):
		return []
	return (current as Array).duplicate(true)


func _set_path_row_field(id: int, path: Array, index: int, field: String, value: Variant) -> void:
	var rows := _rows_at_path(id, path)
	if index < 0 or index >= rows.size():
		return
	var row: Dictionary = (rows[index] as Dictionary).duplicate()
	if row.get(field) == value:
		return
	row[field] = value
	rows[index] = row
	_emit_patch_path(id, path, rows)


func _add_path_row(id: int, path: Array, row: Dictionary) -> void:
	var rows := _rows_at_path(id, path)
	rows.append(row)
	_emit_patch_path(id, path, rows)
	_rebuild()


func _remove_path_row(id: int, path: Array, index: int) -> void:
	var rows := _rows_at_path(id, path)
	if index < 0 or index >= rows.size():
		return
	rows.remove_at(index)
	_emit_patch_path(id, path, rows)
	_rebuild()


func _move_path_row(id: int, path: Array, from: int, to: int) -> void:
	var rows := _rows_at_path(id, path)
	if from < 0 or from >= rows.size() or to < 0 or to >= rows.size():
		return
	var row = rows[from]
	rows.remove_at(from)
	rows.insert(to, row)
	_emit_patch_path(id, path, rows)
	_rebuild()


func _build_frame_section(id: int, authoring: Dictionary) -> void:
	var frame: Dictionary = authoring.get("frame", {})
	MnuUiHelpersScript.add_heading(_box, "Frame")
	for row in [
		["Stencil", "stencil"],
		["Brush", "brush"],
		["Monogram", "monogram"],
	]:
		var field := String(row[1])
		_add_asset_row(String(row[0]), String(frame.get(field, "")), "texture",
			func(value: String) -> void:
				_emit_patch_path(id, ["frame", field], value))
	_build_optional_int(id, "Stencil size", frame, "has_stencil_size",
		"stencil_size", ["frame"], 0, SIZE_MAX)
	_build_optional_int(id, "Horizontal inset", frame, "has_insetx", "insetx",
		["frame"], 0, SIZE_MAX)
	_build_optional_int(id, "Vertical inset", frame, "has_insety", "insety",
		["frame"], 0, SIZE_MAX)


func _build_appearance_section(id: int, authoring: Dictionary) -> void:
	MnuUiHelpersScript.add_heading(_box, "Appearance rows")
	_build_appearance_rows_editor(id, ["appearances"],
		Array(authoring.get("appearances", [])))


func _build_appearance_rows_editor(id: int, path: Array, rows: Array) -> void:
	var editor = MnuListEditorScript.new()
	editor.configure([
		{"key": "state", "label": "State", "kind": "enum",
			"options": ["", "default", "mouseover", "selected", "disabled"]},
		{"key": "type", "label": "Type", "kind": "enum",
			"options": ["", "image", "custom", "outline", "color"]},
		# A picker is always available beside the raw value. For image rows it
		# remains inert unless deliberately used; for color/outline rows it is the
		# purpose-built editor while the LineEdit preserves %VAR% verbatim.
		{"key": "value", "label": "Picture / color", "kind": "asset_or_color"},
		{"key": "has_map_state", "label": "Use map", "kind": "bool"},
		{"key": "map_state", "label": "Map", "kind": "int", "min": -1, "max": SIZE_MAX},
		{"key": "has_height", "label": "Use height", "kind": "bool"},
		{"key": "height", "label": "Height", "kind": "int", "min": 0, "max": SIZE_MAX},
	])
	editor.set_reference_services(_ref_services)
	editor.set_color_context(func(raw: String) -> String:
		return _resolve_style_token(raw), _style_vars_of_type("color"))
	_box.add_child(editor)
	editor.set_rows(rows)
	editor.row_field_changed.connect(func(index: int, key: String, value: Variant) -> void:
		_set_path_row_field(id, path, index, key, value))
	editor.row_added.connect(func() -> void:
		_add_path_row(id, path,
			{"state": "default", "type": "", "value": "",
				"has_map_state": false, "map_state": -1,
				"has_height": false, "height": 0}))
	editor.row_removed.connect(func(index: int) -> void:
		_remove_path_row(id, path, index))
	editor.row_moved.connect(func(from: int, to: int) -> void:
		_move_path_row(id, path, from, to))


func _build_scroll_parts_section(id: int, authoring: Dictionary) -> void:
	var parts: Dictionary = authoring.get("scroll_parts", {})
	MnuUiHelpersScript.add_heading(_box, "Scroll artwork")
	for part in ["shuttle", "scrollup", "scrolldown"]:
		MnuUiHelpersScript.add_muted(_box, String(part))
		_build_appearance_rows_editor(id, ["scroll_parts", part],
			Array(parts.get(part, [])))


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
		# The stylesheet resolves %VAR% values, so a themed color previews as
		# its real color instead of a transparent "unresolved" swatch.
		MnuUiHelpersScript.refresh_swatch(swatch, raw, _stylesheet)
		edit.text_changed.connect(func(text: String) -> void:
			if is_instance_valid(swatch):
				MnuUiHelpersScript.refresh_swatch(swatch, text, _stylesheet))
		_wire_text(edit, {"target": "widget", "id": id, "prop": "color", "slot": slot_index})
		_append_color_picker(edit, swatch, raw,
			{"target": "widget", "id": id, "prop": "color", "slot": slot_index})
		_append_style_var_menu(edit, "color", {"target": "widget", "id": id, "prop": "color", "slot": slot_index}, swatch)
		_append_style_jump(edit, raw)
	_build_add_slot(id, "Add color", empty, "color", "FFFFFF")


# A real color picker sits beside the editable raw token. Merely opening and
# closing it never materializes a literal, which is essential for preserving a
# %VAR% authored value. Once the user changes the color the field previews the
# AARRGGBB/RRGGBB value live, then commits exactly once when the popup closes.
func _append_color_picker(edit: LineEdit, swatch: ColorRect, raw: String, base: Dictionary,
		on_commit: Callable = Callable()) -> void:
	var row := edit.get_parent() as HBoxContainer
	if row == null:
		return
	var picker := ColorPickerButton.new()
	picker.name = "MnuColorPicker"
	picker.custom_minimum_size = Vector2(44, 0)
	picker.tooltip_text = "Pick a literal color (replaces a style variable)"
	var resolved := _resolve_style_token(raw).strip_edges().trim_prefix("#")
	var parsed = MnuUiHelpersScript.color_from_mnu(resolved)
	picker.color = parsed if parsed != null else Color.WHITE
	var keep_alpha := resolved.length() == 8 or raw.strip_edges().begins_with("%")
	var pending := {"changed": false, "value": raw}
	picker.color_changed.connect(func(color: Color) -> void:
		var value: String = MnuUiHelpersScript.color_to_mnu(color, keep_alpha)
		pending["changed"] = true
		pending["value"] = value
		if is_instance_valid(edit):
			edit.text = value
			edit.tooltip_text = value
		if is_instance_valid(swatch):
			MnuUiHelpersScript.refresh_swatch(swatch, value))
	picker.popup_closed.connect(func() -> void:
		if not bool(pending["changed"]):
			return
		pending["changed"] = false
		var value := String(pending["value"])
		if value == raw:
			return
		if on_commit.is_valid():
			on_commit.call(value)
			return
		var e := base.duplicate()
		e["value"] = value
		_emit(e))
	row.add_child(picker)


func _build_patch_color_row(id: int, label: String, raw: String, path: Array) -> void:
	var pair = MnuUiHelpersScript.add_color_edit_row(_box, label, raw)
	var swatch: ColorRect = pair[0]
	var edit: LineEdit = pair[1]
	MnuUiHelpersScript.refresh_swatch(swatch, raw, _stylesheet)
	edit.text_changed.connect(func(text: String) -> void:
		if is_instance_valid(swatch):
			MnuUiHelpersScript.refresh_swatch(swatch, text, _stylesheet))
	_wire_patch_text(edit, id, path)
	_append_color_picker(edit, swatch, raw, {},
		func(value: String) -> void: _emit_patch_path(id, path, value))
	var row := edit.get_parent() as HBoxContainer
	if row != null:
		MnuUiHelpersScript.add_var_menu_button(row, _style_vars_of_type("color"),
			func(name: String) -> void:
				var token := "%" + name + "%"
				edit.text = token
				MnuUiHelpersScript.refresh_swatch(swatch, token, _stylesheet)
				_emit_patch_path(id, path, token))


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
		var ref := _add_asset_row(String(slot[0]), raw, "texture",
			func(value: String) -> void:
				_emit({"target": "widget", "id": id, "prop": "texture",
					"slot": slot_index, "value": value}))
		if ref.name_edit != null:
			_append_style_var_menu(ref.name_edit, "image",
				{"target": "widget", "id": id, "prop": "texture", "slot": slot_index})
			_append_style_jump(ref.name_edit, raw)
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


# --- M10: nested template editors ----------------------------------------------

# Item rows for list / multi / spinlist / combo. The list editor renders the
# rows and reports intent; the editor applies the mutation + undo. Rows carry
# {text, value, type}; the type column is a small enum (the MNU item type tag).
func _build_item_section(id: int, authoring: Dictionary) -> void:
	var items: Dictionary = authoring.get("items", {})
	var type := int(authoring.get("type", NovaMnuDocument.TYPE_UNKNOWN))
	_build_items_block(id, "Items", ["items"], items,
		type != NovaMnuDocument.TYPE_TABLE and type != NovaMnuDocument.TYPE_GLB_TABLE)


func _build_items_block(id: int, heading: String, path: Array,
		items: Dictionary, show_selection_color: bool = true) -> void:
	MnuUiHelpersScript.add_heading(_box, heading)
	var present := MnuUiHelpersScript.add_check_row(_box, "Authored ITEMS block",
		bool(items.get("present", false)))
	present.toggled.connect(func(on: bool) -> void:
		_emit_patch_path(id, path + ["present"], on))
	var multiselect := MnuUiHelpersScript.add_check_row(_box, "Multiple selection",
		bool(items.get("multiselect", false)))
	multiselect.toggled.connect(func(on: bool) -> void:
		_emit_patch_path(id, path + ["multiselect"], on))
	var justify := MnuUiHelpersScript.add_option_row(_box, "Horizontal",
		String(items.get("justify", "")), ["", "LEFT", "CENTER", "RIGHT"])
	_wire_patch_option(justify, id, path + ["justify"])
	var vjustify := MnuUiHelpersScript.add_option_row(_box, "Vertical",
		String(items.get("vjustify", "")), ["", "TOP", "CENTER", "BOTTOM"])
	_wire_patch_option(vjustify, id, path + ["vjustify"])
	if show_selection_color:
		_build_patch_color_row(id, "Selection color",
			String(items.get("selection_color", "")), path + ["selection_color"])
	MnuUiHelpersScript.add_muted(_box, "Item appearances")
	_build_appearance_rows_editor(id, path + ["appearances"],
		Array(items.get("appearances", [])))
	var editor = MnuListEditorScript.new()
	editor.configure([
		{"key": "text", "label": "Text", "kind": "color_or_text"},
		{"key": "value", "label": "Value", "kind": "text"},
		{"key": "type", "label": "Type", "kind": "enum", "options": ["", "id", "color", "image"]},
	])
	editor.set_reference_services(_ref_services)
	editor.set_color_context(func(raw: String) -> String:
		return _resolve_style_token(raw), _style_vars_of_type("color"))
	_box.add_child(editor)
	editor.set_rows(Array(items.get("rows", [])))
	editor.row_field_changed.connect(func(index: int, key: String, value: Variant) -> void:
		_set_path_row_field(id, path + ["rows"], index, key, value))
	editor.row_added.connect(func() -> void:
		_add_path_row(id, path + ["rows"],
			{"type": "", "value": str(Array(items.get("rows", [])).size()),
				"text": "New Item"}))
	editor.row_removed.connect(func(index: int) -> void:
		_remove_path_row(id, path + ["rows"], index))
	editor.row_moved.connect(func(from: int, to: int) -> void:
		_move_path_row(id, path + ["rows"], from, to))


func _build_position_fields(id: int, heading: String, data: Dictionary, path: Array) -> void:
	MnuUiHelpersScript.add_muted(_box, heading)
	_build_optional_int(id, "Left", data, "has_left", "left", path, POS_MIN, POS_MAX)
	_build_optional_int(id, "Top", data, "has_top", "top", path, POS_MIN, POS_MAX)
	_build_optional_int(id, "Right", data, "has_right", "right", path, POS_MIN, POS_MAX)
	_build_optional_int(id, "Bottom", data, "has_bottom", "bottom", path, POS_MIN, POS_MAX)


func _build_list_box_section(id: int, authoring: Dictionary) -> void:
	var list_box: Dictionary = authoring.get("list_box", {})
	MnuUiHelpersScript.add_heading(_box, "Dropdown")
	var present := MnuUiHelpersScript.add_check_row(_box, "Authored LIST_BOX",
		bool(list_box.get("present", false)))
	present.toggled.connect(func(on: bool) -> void:
		_emit_patch_path(id, ["list_box", "present"], on))
	_build_position_fields(id, "Popup rectangle", list_box.get("position", {}),
		["list_box", "position"])
	var string_data: Dictionary = list_box.get("string", {})
	var string_present := MnuUiHelpersScript.add_check_row(_box, "Authored popup STRING",
		bool(string_data.get("present", false)))
	string_present.toggled.connect(func(on: bool) -> void:
		_emit_patch_path(id, ["list_box", "string", "present"], on))
	var string_type := MnuUiHelpersScript.add_option_row(_box, "Popup text type",
		String(string_data.get("type", "")), ["", "id"])
	_wire_patch_option(string_type, id, ["list_box", "string", "type"])
	var string_value := MnuUiHelpersScript.add_text_edit_row(_box, "Popup text",
		String(string_data.get("value", "")))
	_wire_patch_text(string_value, id, ["list_box", "string", "value"])
	var justify := MnuUiHelpersScript.add_option_row(_box, "Text horizontal",
		String(string_data.get("justify", "")), ["", "LEFT", "CENTER", "RIGHT"])
	_wire_patch_option(justify, id, ["list_box", "string", "justify"])
	var vjustify := MnuUiHelpersScript.add_option_row(_box, "Text vertical",
		String(string_data.get("vjustify", "")), ["", "TOP", "CENTER", "BOTTOM"])
	_wire_patch_option(vjustify, id, ["list_box", "string", "vjustify"])
	_build_optional_int(id, "Text edge", string_data, "has_edge", "edge",
		["list_box", "string"], 0, SIZE_MAX)
	_build_optional_int(id, "Minimum item height", list_box,
		"has_min_item_height", "min_item_height", ["list_box"], 0, SIZE_MAX)
	_build_optional_int(id, "Scrollbar edge padding", list_box,
		"has_sb_edge_pad", "sb_edge_pad", ["list_box"], 0, SIZE_MAX)
	MnuUiHelpersScript.add_muted(_box, "Popup appearances")
	_build_appearance_rows_editor(id, ["list_box", "appearances"],
		Array(list_box.get("appearances", [])))
	var list_items: Dictionary = list_box.get("items", {})
	_build_items_block(id, "Dropdown items", ["list_box", "items"], list_items)
	var scrollbar: Dictionary = list_box.get("scrollbar", {})
	_build_scrollbar_block(id, "Dropdown scrollbar",
		["list_box", "scrollbar"], scrollbar)


func _build_direct_scrollbar_section(id: int, authoring: Dictionary) -> void:
	_build_scrollbar_block(id, "Scrollbar", ["scrollbar"],
		authoring.get("scrollbar", {}))


func _build_scrollbar_block(id: int, heading: String, path: Array,
		scrollbar: Dictionary) -> void:
	MnuUiHelpersScript.add_heading(_box, heading)
	var present := MnuUiHelpersScript.add_check_row(_box, "Authored SCROLLBAR",
		bool(scrollbar.get("present", false)))
	present.toggled.connect(func(on: bool) -> void:
		_emit_patch_path(id, path + ["present"], on))
	_build_position_fields(id, "Scrollbar rectangle",
		scrollbar.get("position", {}), path + ["position"])
	for part in ["track", "shuttle", "scrollup", "scrolldown"]:
		MnuUiHelpersScript.add_muted(_box, String(part).capitalize() + " appearances")
		_build_appearance_rows_editor(id, path + [part],
			Array(scrollbar.get(part, [])))
	MnuUiHelpersScript.add_muted(_box, "Scrollbar sounds")
	_build_nested_sound_rows_editor(id, path + ["sounds"],
		Array(scrollbar.get("sounds", [])))


func _build_nested_sound_rows_editor(id: int, path: Array, rows: Array) -> void:
	var editor = MnuListEditorScript.new()
	editor.configure([
		{"key": "state", "label": "State", "kind": "enum",
			"options": ["", "mousein", "mouseout", "selected"]},
		{"key": "trigger", "label": "Trigger", "kind": "text"},
		{"key": "file", "label": "Profile", "kind": "asset", "asset_kind": "sound"},
	])
	editor.set_reference_services(_ref_services)
	_box.add_child(editor)
	editor.set_rows(rows)
	editor.row_field_changed.connect(func(index: int, key: String, value: Variant) -> void:
		_set_path_row_field(id, path, index, key, value))
	editor.row_added.connect(func() -> void:
		_add_path_row(id, path,
			{"state": "selected", "trigger": "CLICK_VALUE", "file": "menu.lwf"}))
	editor.row_removed.connect(func(index: int) -> void:
		_remove_path_row(id, path, index))
	editor.row_moved.connect(func(from: int, to: int) -> void:
		_move_path_row(id, path, from, to))


func _build_spin_section(id: int, authoring: Dictionary) -> void:
	MnuUiHelpersScript.add_heading(_box, "Spin arrows")
	for row in [["Up arrow", "spinup"], ["Down arrow", "spindown"]]:
		var key := String(row[1])
		var data: Dictionary = authoring.get(key, {})
		var present := MnuUiHelpersScript.add_check_row(_box, String(row[0]),
			bool(data.get("present", false)))
		present.toggled.connect(func(on: bool) -> void:
			_emit_patch_path(id, [key, "present"], on))
		_build_position_fields(id, String(row[0]) + " rectangle",
			data.get("position", {}), [key, "position"])
		_build_appearance_rows_editor(id, [key, "appearances"],
			Array(data.get("appearances", [])))


# Table COLUMN authoring: column count + spacing (scalar edits), the per-column
# HEADER definitions, and the value->image SUBST cells (both list editors). Headers
# and substitutions are keyed by their `column` attribute, so reorder is disabled.
func _build_table_section(id: int, authoring: Dictionary) -> void:
	MnuUiHelpersScript.add_heading(_box, "Table columns")
	var table: Dictionary = authoring.get("table", {})
	_build_optional_int(id, "Minimum item height", table,
		"has_min_item_height", "min_item_height", ["table"], 0, SIZE_MAX)
	var multiselect := MnuUiHelpersScript.add_check_row(_box, "Multiple selection",
		bool(table.get("multiselect", false)))
	multiselect.toggled.connect(func(on: bool) -> void:
		_emit_patch_path(id, ["table", "multiselect"], on))
	_build_patch_color_row(id, "Outline color",
		String(table.get("outline_color", "")), ["table", "outline_color"])
	_build_patch_color_row(id, "Selection color",
		String(table.get("selection_color", "")), ["table", "selection_color"])
	var scrollbar: Dictionary = table.get("scrollbar", {})
	_build_scrollbar_block(id, "Table scrollbar",
		["table", "scrollbar"], scrollbar)
	_build_optional_int(id, "Column count", table, "has_count", "count",
		["table"], 0, 64)
	_build_optional_int(id, "Column spacing", table, "has_spacing", "spacing",
		["table"], 0, 256)

	MnuUiHelpersScript.add_muted(_box, "Headers")
	var editor = MnuListEditorScript.new()
	editor.configure([
		{"key": "has_column", "label": "Use col", "kind": "bool"},
		{"key": "column", "label": "Col", "kind": "int", "min": 0, "max": 63},
		{"key": "text", "label": "Title", "kind": "text"},
		{"key": "has_width", "label": "Use width", "kind": "bool"},
		{"key": "width", "label": "W", "kind": "int", "min": 0, "max": 4096},
		{"key": "justify", "label": "Justify", "kind": "enum", "options": ["", "LEFT", "CENTER", "RIGHT"]},
		{"key": "vjustify", "label": "Vertical", "kind": "enum", "options": ["", "TOP", "CENTER", "BOTTOM"]},
		{"key": "type", "label": "Text type", "kind": "enum", "options": ["", "id"]},
		{"key": "sort", "label": "Sort", "kind": "text"},
	], false)
	_box.add_child(editor)
	editor.set_rows(_document.get_table_headers(id))
	editor.row_field_changed.connect(func(index: int, key: String, value: Variant) -> void:
		_emit({"id": id, "op": "header_field", "index": index, "key": key, "value": value}))
	editor.row_added.connect(func() -> void:
		_emit({"id": id, "op": "header_add",
			"row": {"column": _document.get_table_headers(id).size(), "text": "Column",
				"has_column": true, "has_width": true, "width": 80,
				"justify": "LEFT", "vjustify": "", "type": "", "sort": ""}}))
	editor.row_removed.connect(func(index: int) -> void:
		_emit({"id": id, "op": "header_remove", "index": index}))

	MnuUiHelpersScript.add_muted(_box, "Bodies")
	var bodies = MnuListEditorScript.new()
	bodies.configure([
		{"key": "has_column", "label": "Use col", "kind": "bool"},
		{"key": "column", "label": "Col", "kind": "int", "min": 0, "max": 63},
		{"key": "justify", "label": "Justify", "kind": "enum", "options": ["", "LEFT", "CENTER", "RIGHT"]},
		{"key": "vjustify", "label": "Vertical", "kind": "enum", "options": ["", "TOP", "CENTER", "BOTTOM"]},
		{"key": "bitmap_draw", "label": "Bitmap", "kind": "bool"},
		{"key": "scale_bitmap", "label": "Scale", "kind": "bool"},
		{"key": "custom_draw", "label": "Game-drawn", "kind": "bool"},
		{"key": "bitmap_flags", "label": "Bitmap flags", "kind": "text"},
	], false)
	_box.add_child(bodies)
	bodies.set_rows(_document.get_table_bodies(id))
	bodies.row_field_changed.connect(func(index: int, key: String, value: Variant) -> void:
		_emit({"id": id, "op": "body_field", "index": index, "key": key, "value": value}))
	bodies.row_added.connect(func() -> void:
		_emit({"id": id, "op": "body_add", "row": {
			"has_column": true, "column": _document.get_table_bodies(id).size(),
			"justify": "LEFT", "vjustify": "", "bitmap_draw": false,
			"scale_bitmap": false, "custom_draw": false, "bitmap_flags": "",
		}}))
	bodies.row_removed.connect(func(index: int) -> void:
		_emit({"id": id, "op": "body_remove", "index": index}))

	# Value->image SUBST cells: when column `column` holds `value`, the cell renders
	# image `file` (the Img flag = the FILE attribute). Keyed by column/value rather
	# than order, so reorder is disabled, mirroring headers.
	MnuUiHelpersScript.add_muted(_box, "Substitutions")
	var subst = MnuListEditorScript.new()
	subst.configure([
		{"key": "has_column", "label": "Use col", "kind": "bool"},
		{"key": "column", "label": "Col", "kind": "int", "min": 0, "max": 63},
		{"key": "value", "label": "Value", "kind": "text"},
		{"key": "is_file", "label": "Img", "kind": "bool"},
		{"key": "file", "label": "File", "kind": "asset", "asset_kind": "texture"},
	], false)
	subst.set_reference_services(_ref_services)
	_box.add_child(subst)
	subst.set_rows(_document.get_table_substs(id))
	subst.row_field_changed.connect(func(index: int, key: String, value: Variant) -> void:
		_emit({"id": id, "op": "subst_field", "index": index, "key": key, "value": value}))
	subst.row_added.connect(func() -> void:
		_emit({"id": id, "op": "subst_add",
			"row": {"has_column": true, "column": 0, "value": "",
				"is_file": true, "file": ""}}))
	subst.row_removed.connect(func(index: int) -> void:
		_emit({"id": id, "op": "subst_remove", "index": index}))


# Commit a LineEdit on Enter and on blur. The edit's "value" is the field text at
# commit time; the editor no-ops if it equals the document. A focus_exited that
# fires while the field is being rebuilt or torn down (no longer in the tree, or
# already freed) is skipped: the captured control is mid-teardown and the row
# already reflects the document.
# --- Stylesheet awareness ---------------------------------------------------

# Stylesheet variables whose value parses as `value_type` ("color"/"font"/
# "image"/"text"), as {name, value} rows for the "%" dropdown.
func _style_vars_of_type(value_type: String) -> Array:
	if _stylesheet == null:
		return []
	var out: Array = []
	for entry_value in _stylesheet.get_entries():
		var entry := entry_value as Dictionary
		var value := String(entry.get("value", ""))
		if MnsVariableTableScript.infer_type(value) == value_type:
			out.append({"name": String(entry.get("name", "")), "value": value})
	return out


# The variable name when `raw` is a whole-field %NAME% token, else "".
func _style_token_name(raw: String) -> String:
	var token := raw.strip_edges()
	if token.length() < 3 or not token.begins_with("%") or not token.ends_with("%"):
		return ""
	return token.substr(1, token.length() - 2)


# A %VAR% resolves through the stylesheet (when loaded); literals pass through.
func _resolve_style_token(raw: String) -> String:
	if _stylesheet != null and not _style_token_name(raw).is_empty():
		return String(_stylesheet.substitute(raw.strip_edges()))
	return raw


# Append the "%" variable dropdown to `edit`'s row: picking a variable writes
# the %NAME% token (preserved on save - the document keeps raw tokens, ADR
# 0005) and commits through the normal edit path. No stylesheet or no
# type-matching variables -> no affordance.
func _append_style_var_menu(edit: LineEdit, value_type: String, base: Dictionary, swatch: ColorRect = null) -> void:
	var row := edit.get_parent() as HBoxContainer
	if row == null:
		return
	MnuUiHelpersScript.add_var_menu_button(row, _style_vars_of_type(value_type), func(name: String) -> void:
		if not is_instance_valid(edit) or not edit.is_inside_tree():
			return
		var token := "%" + name + "%"
		edit.text = token
		edit.tooltip_text = token
		# Programmatic .text writes do not emit text_changed; refresh the
		# swatch by hand so the picked color previews immediately.
		if swatch != null and is_instance_valid(swatch):
			MnuUiHelpersScript.refresh_swatch(swatch, token, _stylesheet)
		var e := base.duplicate()
		e["value"] = token
		_emit(e))


# When the current value IS a %VAR% token, offer the jump into Menu Styles.
func _append_style_jump(edit: LineEdit, raw: String) -> void:
	var name := _style_token_name(raw)
	if name.is_empty():
		return
	var row := edit.get_parent() as HBoxContainer
	if row == null:
		return
	var jump := Button.new()
	jump.text = "Edit style"
	jump.tooltip_text = "Edit %s in the Menu Styles workspace" % name
	jump.pressed.connect(func() -> void:
		style_jump_requested.emit(name))
	row.add_child(jump)


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
func _wire_rect(id: int, sx: SpinBox, sy: SpinBox, sw: SpinBox, sh: SpinBox,
		auto_width: CheckBox, auto_height: CheckBox) -> void:
	var commit := func(_v: float) -> void:
		if not (is_instance_valid(sx) and is_instance_valid(sy) and is_instance_valid(sw)
				and is_instance_valid(sh) and is_instance_valid(auto_width)
				and is_instance_valid(auto_height)):
			return
		_emit({"target": "widget", "id": id, "prop": "rect",
			"value": Rect2(sx.value, sy.value,
				-1 if auto_width.button_pressed else sw.value,
				-1 if auto_height.button_pressed else sh.value)})
	sx.value_changed.connect(commit)
	sy.value_changed.connect(commit)
	sw.value_changed.connect(commit)
	sh.value_changed.connect(commit)
	auto_width.toggled.connect(func(_on: bool) -> void: commit.call(0.0))
	auto_height.toggled.connect(func(_on: bool) -> void: commit.call(0.0))
