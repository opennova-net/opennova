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
# Cross-workspace jump intents (handled by the workspace adapter via the shell): edit
# the selected widget's string in Strings, its font in Fonts, or a cross-file
# screen action's target in Menus.
signal string_jump_requested(key: String)
signal font_jump_requested(font: String)
signal menu_jump_requested(file: String, screen: String)
# Edit a %VAR% the selected widget references, in the Menu Styles workspace.
signal style_jump_requested(variable: String)
# Audition a widget's sound: the workspace resolves (trigger -> set in the menu .lwf
# -> member -> .wav) and plays it, reusing the Sound workspace's preview player.
signal sound_preview_requested(trigger: String, file: String)

const MnuUiHelpersScript = preload("res://modtools/mnu/mnu_ui_helpers.gd")
const MnuListEditorScript = preload("res://modtools/mnu/mnu_list_editor.gd")
const MnuStringPickerScript = preload("res://modtools/mnu/mnu_string_picker.gd")
const MnsVariableTableScript = preload("res://modtools/mnu/mns_variable_table.gd")

# Position spins span negative coords (a widget can sit off the authoring board);
# sizes never do.
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
	authored_type.tooltip_text = "Raw MNU TYPE token; unknown host-owned widget types are preserved verbatim"
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
	_build_string_layout_section(id, authoring)
	_build_behavior_section(id, wtype, authoring)
	_build_hotkey_section(id, authoring)
	_build_frame_section(id, authoring)

	_build_color_section(id)
	_build_texture_section(id)
	_build_appearance_section(id, authoring)
	if wtype == NovaMnuDocument.TYPE_SCROLL:
		_build_scroll_parts_section(id, authoring)
	_build_flag_section(id)
	_build_action_section(id)
	_build_sound_section(id)

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


func _build_behavior_section(id: int, wtype: int, authoring: Dictionary) -> void:
	var behavior: Dictionary = authoring.get("behavior", {})
	var constraints: Dictionary = authoring.get("constraints", {})
	MnuUiHelpersScript.add_heading(_box, "Behavior")
	if wtype == NovaMnuDocument.TYPE_CHECKBOX:
		var as_button := MnuUiHelpersScript.add_check_row(_box, "Button presentation",
			bool(behavior.get("as_button", false)))
		as_button.tooltip_text = "Lay out this checkbox as a full-rect toggle button"
		as_button.toggled.connect(func(on: bool) -> void:
			_emit_patch_path(id, ["behavior", "as_button"], on))
	if wtype == NovaMnuDocument.TYPE_RADIO or wtype == NovaMnuDocument.TYPE_CHECKBOX \
			or wtype == NovaMnuDocument.TYPE_RADIOEDIT:
		_build_optional_int(id, "Group", behavior, "has_group", "group",
			["behavior"], 0, SIZE_MAX)

	_build_optional_int(id, "Form", behavior, "has_form", "form", ["behavior"], 0, SIZE_MAX)
	var global_var := MnuUiHelpersScript.add_check_row(_box, "Global variable",
		bool(behavior.get("global_var", false)))
	global_var.toggled.connect(func(on: bool) -> void:
		_emit_patch_path(id, ["behavior", "global_var"], on))

	var is_edit := wtype == NovaMnuDocument.TYPE_EDIT \
		or wtype == NovaMnuDocument.TYPE_MULTILINE_EDIT \
		or wtype == NovaMnuDocument.TYPE_RADIOEDIT
	if is_edit:
		var password := MnuUiHelpersScript.add_check_row(_box, "Password",
			bool(behavior.get("password", false)))
		password.toggled.connect(func(on: bool) -> void:
			_emit_patch_path(id, ["behavior", "password"], on))
		var number := MnuUiHelpersScript.add_check_row(_box, "Numbers only",
			bool(constraints.get("number", false)))
		number.toggled.connect(func(on: bool) -> void:
			_emit_patch_path(id, ["constraints", "number"], on))
		_build_optional_int(id, "Minimum", constraints, "has_minval", "minval",
			["constraints"], -2147483648, 2147483647)
		_build_optional_int(id, "Maximum", constraints, "has_maxval", "maxval",
			["constraints"], -2147483648, 2147483647)
		_build_optional_int(id, "Maximum characters", constraints, "has_maxchar", "maxchar",
			["constraints"], 0, SIZE_MAX)

	if wtype == NovaMnuDocument.TYPE_SCROLL or wtype == NovaMnuDocument.TYPE_MARQUEE:
		var orientation := MnuUiHelpersScript.add_option_row(_box, "Orientation",
			String(behavior.get("orientation", "")), ["", "HORIZONTAL", "VERTICAL"])
		_wire_patch_option(orientation, id, ["behavior", "orientation"])
	if wtype == NovaMnuDocument.TYPE_MARQUEE:
		_add_asset_row("Datasource", String(behavior.get("datasource", "")), "credits",
			func(value: String) -> void:
				_emit_patch_path(id, ["behavior", "datasource"], value))
	if wtype == NovaMnuDocument.TYPE_SCROLL:
		var scroll_size: Dictionary = authoring.get("scroll_size", {})
		_build_optional_int(id, "Scroll height", scroll_size, "has_height", "height",
			["scroll_size"], 0, SIZE_MAX)
		_build_optional_int(id, "Scroll width", scroll_size, "has_width", "width",
			["scroll_size"], 0, SIZE_MAX)

	var cursor: Dictionary = authoring.get("cursor", {})
	_add_asset_row("Pointer picture", String(cursor.get("file", "")), "texture",
		func(value: String) -> void:
			_emit_patch_path(id, ["cursor", "file"], value))
	var cursor_flags := MnuUiHelpersScript.add_text_edit_row(_box, "Pointer flags",
		String(cursor.get("flags", "")))
	_wire_patch_text(cursor_flags, id, ["cursor", "flags"])


func _top_rows(id: int, key: String) -> Array:
	var state: Dictionary = _document.get_widget_authoring_state(id)
	return Array(state.get(key, [])).duplicate(true)


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


func _set_top_row_field(id: int, key: String, index: int, field: String, value: Variant) -> void:
	var rows := _top_rows(id, key)
	if index < 0 or index >= rows.size():
		return
	var row: Dictionary = (rows[index] as Dictionary).duplicate()
	if row.get(field) == value:
		return
	row[field] = value
	rows[index] = row
	_emit_patch_path(id, [key], rows)


func _add_top_row(id: int, key: String, row: Dictionary) -> void:
	var rows := _top_rows(id, key)
	rows.append(row)
	_emit_patch_path(id, [key], rows)
	_rebuild()


func _remove_top_row(id: int, key: String, index: int) -> void:
	var rows := _top_rows(id, key)
	if index < 0 or index >= rows.size():
		return
	rows.remove_at(index)
	_emit_patch_path(id, [key], rows)
	_rebuild()


func _move_top_row(id: int, key: String, from: int, to: int) -> void:
	var rows := _top_rows(id, key)
	if from < 0 or from >= rows.size() or to < 0 or to >= rows.size():
		return
	var row = rows[from]
	rows.remove_at(from)
	rows.insert(to, row)
	_emit_patch_path(id, [key], rows)
	_rebuild()


func _build_hotkey_section(id: int, authoring: Dictionary) -> void:
	MnuUiHelpersScript.add_heading(_box, "Keyboard shortcuts")
	var editor = MnuListEditorScript.new()
	editor.configure([
		{"key": "value", "label": "Key", "kind": "text"},
		{"key": "virtual", "label": "Named key", "kind": "bool"},
	])
	_box.add_child(editor)
	editor.set_rows(Array(authoring.get("hotkeys", [])))
	editor.row_field_changed.connect(func(index: int, key: String, value: Variant) -> void:
		_set_top_row_field(id, "hotkeys", index, key, value))
	editor.row_added.connect(func() -> void:
		_add_top_row(id, "hotkeys", {"value": "VK_RETURN", "virtual": true}))
	editor.row_removed.connect(func(index: int) -> void:
		_remove_top_row(id, "hotkeys", index))
	editor.row_moved.connect(func(from: int, to: int) -> void:
		_move_top_row(id, "hotkeys", from, to))


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


# --- Actions (visual scripting) -------------------------------------------------

# <ACTION> rows are the MNU visual-scripting surface: a button/goto dispatches
# these navigation/window verbs through NovaMnuMenu. The editor treats the action
# list as one property so add/remove/field edits are undoable as a single row-list
# replacement, just like Sounds.
func _build_action_section(id: int) -> void:
	MnuUiHelpersScript.add_heading(_box, "Actions")
	var actions: Array = _document.get_widget_actions(id)
	if actions.is_empty():
		MnuUiHelpersScript.add_muted(_box, "No navigation actions.")
	for i in range(actions.size()):
		_build_action_row(id, actions[i], i, actions.size())
	var add_btn := Button.new()
	add_btn.text = "Add action"
	add_btn.tooltip_text = "Add a navigation/window action for this widget"
	add_btn.pressed.connect(func() -> void: _add_action(id))
	_box.add_child(add_btn)


func _build_action_row(id: int, action: Dictionary, index: int, count: int) -> void:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_theme_constant_override("separation", 4)
	_box.add_child(row)

	var type_value := String(action.get("type", "screen")).to_lower()
	var type_opt := _action_option([
		"screen", "window", "url", "form_post",
		"glb_load", "glb_loadandping", "glb_filter", "glb_filter_num",
		"glb_ping", "glb_join", "tab", "pop_screen", "appmsg",
		"lan_search", "lan_join", "mnx",
	], type_value)
	type_opt.tooltip_text = "TAB is the retail focus/capture action; visibility tabs use WINDOW actions"
	type_opt.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	type_opt.item_selected.connect(func(i: int) -> void:
		_set_action_field(id, index, "type", type_opt.get_item_text(i)))
	row.add_child(type_opt)

	var target_options := _action_target_options(id, action)
	if target_options.is_empty():
		var target_edit := LineEdit.new()
		target_edit.text = String(action.get("target", ""))
		target_edit.placeholder_text = "target"
		target_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		target_edit.text_submitted.connect(func(t: String) -> void: _set_action_field(id, index, "target", t))
		target_edit.focus_exited.connect(func() -> void: _set_action_field(id, index, "target", target_edit.text))
		row.add_child(target_edit)
	else:
		var target_opt := _action_option(target_options, String(action.get("target", "")))
		target_opt.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		target_opt.item_selected.connect(func(i: int) -> void:
			_set_action_field(id, index, "target", target_opt.get_item_text(i)))
		row.add_child(target_opt)

	var state_opt := _action_option(["", "SHOW", "HIDE", "ENABLE", "DISABLE"],
		String(action.get("state", "")).to_upper())
	state_opt.custom_minimum_size = Vector2(86, 0)
	state_opt.item_selected.connect(func(i: int) -> void:
		_set_action_field(id, index, "state", state_opt.get_item_text(i)))
	row.add_child(state_opt)

	var file_ref := ResourceRefWidget.new()
	file_ref.custom_minimum_size = Vector2(110, 0)
	file_ref.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	file_ref.set_value_from_path(func(path: String) -> String: return path.get_file())
	file_ref.configure("menu", "Target menu", _ref_services)
	file_ref.set_value(String(action.get("file", "")))
	file_ref.value_changed.connect(func(value: String) -> void:
		_set_action_field(id, index, "file", value))
	row.add_child(file_ref)

	if type_value == "screen" and not String(action.get("file", "")).is_empty():
		var jump := Button.new()
		jump.text = "Open menu"
		jump.tooltip_text = "Open this cross-menu action target in the Menus workspace"
		jump.pressed.connect(func() -> void:
			menu_jump_requested.emit(String(action.get("file", "")), String(action.get("target", ""))))
		row.add_child(jump)

	if type_value == "url":
		var external := CheckBox.new()
		external.text = "External browser"
		external.tooltip_text = "Preserve the URL action's EXTERNAL_BROWSER flag"
		external.button_pressed = bool(action.get("external_browser", false))
		external.toggled.connect(func(pressed: bool) -> void:
			_set_action_field(id, index, "external_browser", pressed))
		row.add_child(external)

	var up := Button.new()
	up.text = "▲"
	up.disabled = index == 0
	up.pressed.connect(func() -> void: _move_action(id, index, index - 1))
	row.add_child(up)
	var down := Button.new()
	down.text = "▼"
	down.disabled = index >= count - 1
	down.pressed.connect(func() -> void: _move_action(id, index, index + 1))
	row.add_child(down)
	var rm := Button.new()
	rm.text = "✕"
	rm.tooltip_text = "Remove this action"
	rm.pressed.connect(func() -> void: _remove_action(id, index))
	row.add_child(rm)

	var details := GridContainer.new()
	details.columns = 2
	details.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_box.add_child(details)
	for spec in [["Source", "source"], ["Field", "field"]]:
		var field_key := String(spec[1])
		details.add_child(MnuUiHelpersScript._key_label(String(spec[0])))
		var edit := LineEdit.new()
		edit.text = String(action.get(field_key, ""))
		edit.placeholder_text = "(host-owned)"
		edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		edit.text_submitted.connect(func(text: String) -> void:
			_set_action_field(id, index, field_key, text))
		edit.focus_exited.connect(func() -> void:
			_set_action_field(id, index, field_key, edit.text))
		details.add_child(edit)

	details.add_child(MnuUiHelpersScript._key_label("Test"))
	var test_opt := _action_option(["", "LT", "LE", "EQ", "GE", "GT"],
		String(action.get("test", "")).to_upper())
	test_opt.item_selected.connect(func(i: int) -> void:
		_set_action_field(id, index, "test", test_opt.get_item_text(i)))
	details.add_child(test_opt)

	details.add_child(MnuUiHelpersScript._key_label("Target form"))
	var form_row := HBoxContainer.new()
	var has_form := CheckBox.new()
	has_form.text = "Use"
	has_form.button_pressed = bool(action.get("has_target_form", false))
	var target_form := SpinBox.new()
	target_form.min_value = 0
	target_form.max_value = SIZE_MAX
	target_form.step = 1
	target_form.editable = has_form.button_pressed
	target_form.set_value_no_signal(int(action.get("target_form", 0)))
	has_form.toggled.connect(func(on: bool) -> void:
		target_form.editable = on
		_set_action_field(id, index, "has_target_form", on))
	target_form.value_changed.connect(func(value: float) -> void:
		_set_action_field(id, index, "target_form", int(value)))
	form_row.add_child(has_form)
	form_row.add_child(target_form)
	details.add_child(form_row)

	details.add_child(MnuUiHelpersScript._key_label("Flags"))
	var flags_row := HBoxContainer.new()
	var toggle := CheckBox.new()
	toggle.text = "Toggle"
	toggle.button_pressed = bool(action.get("toggle", false))
	toggle.toggled.connect(func(on: bool) -> void:
		_set_action_field(id, index, "toggle", on))
	flags_row.add_child(toggle)
	details.add_child(flags_row)


func _action_option(options: Array, value: String) -> OptionButton:
	var opt := OptionButton.new()
	var selected := -1
	for i in range(options.size()):
		var label := String(options[i])
		opt.add_item(label, i)
		if label == value:
			selected = i
	if selected < 0 and not value.is_empty():
		opt.add_item(value, options.size())
		selected = opt.item_count - 1
	if selected >= 0:
		opt.select(selected)
	return opt


func _action_target_options(id: int, action: Dictionary) -> Array:
	var type_value := String(action.get("type", "")).to_lower()
	if type_value == "screen" and String(action.get("file", "")).is_empty():
		return _screen_names()
	if type_value == "window":
		return _widget_names(id)
	return []


func _screen_names() -> Array:
	var out := []
	if _document == null:
		return out
	for sid in _document.get_screen_ids():
		var name := _document.get_screen_name(sid)
		if not name.is_empty() and not out.has(name):
			out.append(name)
	return out


func _widget_names(selected_id: int) -> Array:
	var out := []
	if _document == null:
		return out
	var owner := selected_id
	while owner >= 0 and _document.widget_exists(owner) and not _document.is_screen(owner):
		owner = _document.get_parent_id(owner)
	if owner >= 0 and _document.is_screen(owner):
		_collect_widget_names(_document.get_screen_root_id(owner), out, selected_id)
	return out


func _collect_widget_names(id: int, out: Array, selected_id: int) -> void:
	if _document == null or id < 0 or not _document.widget_exists(id):
		return
	if not _document.is_screen(id) and id != selected_id:
		var name := _document.get_widget_name(id)
		if not name.is_empty() and not out.has(name):
			out.append(name)
	for child in _document.get_child_ids(id):
		_collect_widget_names(child, out, selected_id)


func _set_action_field(id: int, index: int, key: String, value: Variant) -> void:
	var actions: Array = _document.get_widget_actions(id)
	if index < 0 or index >= actions.size():
		return
	var row: Dictionary = (actions[index] as Dictionary).duplicate()
	if row.get(key, "") == value:
		return
	row[key] = value
	actions[index] = row
	_emit({"target": "widget", "id": id, "prop": "actions", "value": actions})
	_rebuild()


func _add_action(id: int) -> void:
	var actions: Array = _document.get_widget_actions(id)
	actions.append({
		"type": "screen", "target": "", "state": "", "file": "",
		"source": "", "field": "", "test": "",
		"has_target_form": false, "target_form": 0,
		"toggle": false, "external_browser": false,
	})
	_emit({"target": "widget", "id": id, "prop": "actions", "value": actions})
	_rebuild()


func _remove_action(id: int, index: int) -> void:
	var actions: Array = _document.get_widget_actions(id)
	if index < 0 or index >= actions.size():
		return
	actions.remove_at(index)
	_emit({"target": "widget", "id": id, "prop": "actions", "value": actions})
	_rebuild()


func _move_action(id: int, from: int, to: int) -> void:
	var actions: Array = _document.get_widget_actions(id)
	if from < 0 or from >= actions.size() or to < 0 or to >= actions.size():
		return
	var row: Dictionary = actions[from]
	actions.remove_at(from)
	actions.insert(to, row)
	_emit({"target": "widget", "id": id, "prop": "actions", "value": actions})
	_rebuild()


# --- Sounds (hover / click) -----------------------------------------------------

# Each <SOUND> row names a .lwf profile (file) and a trigger that selects a set in
# it (MOUSE_OVER/CLICK_SELECT/...). Rows let the author retarget the trigger (from
# the profile's real set list when loaded, else free text), edit the .lwf file,
# preview, or remove; an "Add" button appends one. Every change replaces the whole
# list through the normal "sounds" prop, so the editor records one undo step.
func _build_sound_section(id: int) -> void:
	MnuUiHelpersScript.add_heading(_box, "Sounds")
	var sounds := _document.get_widget_sounds(id)
	if sounds.is_empty():
		MnuUiHelpersScript.add_muted(_box, "No interaction sounds.")
	for i in range(sounds.size()):
		_build_sound_row(id, sounds[i], i)
	var add_btn := Button.new()
	add_btn.text = "Add sound"
	add_btn.tooltip_text = "Add a hover/click sound played from the menu .lwf profile"
	add_btn.pressed.connect(func() -> void: _add_sound(id))
	_box.add_child(add_btn)


func _build_sound_row(id: int, snd: Dictionary, index: int) -> void:
	var trigger := String(snd.get("trigger", ""))
	var file := String(snd.get("file", ""))
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_theme_constant_override("separation", 6)
	_box.add_child(row)

	var state_opt := _action_option(["", "mousein", "mouseout", "selected"],
		String(snd.get("state", "")))
	state_opt.tooltip_text = "Widget state that enables this sound"
	state_opt.custom_minimum_size = Vector2(78, 0)
	state_opt.item_selected.connect(func(idx: int) -> void:
		_set_sound_field(id, index, "state", state_opt.get_item_text(idx)))
	row.add_child(state_opt)

	# Trigger: a dropdown over the profile's real set names when one is loaded,
	# else a free-text field (so triggers survive without a profile).
	if _sound_sets.size() > 0:
		var opt := OptionButton.new()
		opt.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		var matched := false
		for si in range(_sound_sets.size()):
			opt.add_item(_sound_sets[si], si)
			if _sound_sets[si].to_upper() == trigger.to_upper():
				opt.select(si)
				matched = true
		if not matched and not trigger.is_empty():
			opt.add_item(trigger, _sound_sets.size())  # preserve an off-profile trigger
			opt.select(opt.item_count - 1)
		opt.item_selected.connect(func(idx: int) -> void:
			_set_sound_field(id, index, "trigger", opt.get_item_text(idx)))
		row.add_child(opt)
	else:
		var trig_edit := LineEdit.new()
		trig_edit.text = trigger
		trig_edit.placeholder_text = "trigger (e.g. MOUSE_OVER)"
		trig_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		trig_edit.text_submitted.connect(func(t: String) -> void: _set_sound_field(id, index, "trigger", t))
		trig_edit.focus_exited.connect(func() -> void: _set_sound_field(id, index, "trigger", trig_edit.text))
		row.add_child(trig_edit)

	var file_ref := ResourceRefWidget.new()
	file_ref.custom_minimum_size = Vector2(120, 0)
	file_ref.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	file_ref.set_value_from_path(func(path: String) -> String: return path.get_file())
	file_ref.configure("sound", "Sound profile", _ref_services)
	file_ref.set_value(file)
	file_ref.value_changed.connect(func(value: String) -> void:
		_set_sound_field(id, index, "file", value))
	row.add_child(file_ref)

	var play := Button.new()
	play.text = "▶"
	play.tooltip_text = "Preview this sound"
	play.pressed.connect(func() -> void: sound_preview_requested.emit(trigger, file))
	row.add_child(play)

	var rm := Button.new()
	rm.text = "✕"
	rm.tooltip_text = "Remove this sound"
	rm.pressed.connect(func() -> void: _remove_sound(id, index))
	row.add_child(rm)


# Mutate one field of one sound row and commit the whole list. Re-reads from the
# document each time so concurrent edits compose; no-ops an unchanged value.
func _set_sound_field(id: int, index: int, key: String, value: String) -> void:
	var sounds := _document.get_widget_sounds(id)
	if index < 0 or index >= sounds.size():
		return
	var snd: Dictionary = (sounds[index] as Dictionary).duplicate()
	if String(snd.get(key, "")) == value:
		return
	snd[key] = value
	sounds[index] = snd
	_emit({"target": "widget", "id": id, "prop": "sounds", "value": sounds})
	_rebuild()


func _add_sound(id: int) -> void:
	var sounds := _document.get_widget_sounds(id)
	var trigger := String(_sound_sets[0]) if _sound_sets.size() > 0 else "MOUSE_OVER"
	# Default the state to match the trigger family (cosmetic for playback, but it
	# keeps the round-tripped <SOUND state=...> faithful to the shipped menus).
	var up := trigger.to_upper()
	var state := "selected" if up.contains("CLICK") or up.contains("SELECT") else "mousein"
	sounds.append({"state": state, "trigger": trigger, "file": "menu.lwf"})
	_emit({"target": "widget", "id": id, "prop": "sounds", "value": sounds})
	_rebuild()


func _remove_sound(id: int, index: int) -> void:
	var sounds := _document.get_widget_sounds(id)
	if index < 0 or index >= sounds.size():
		return
	sounds.remove_at(index)
	_emit({"target": "widget", "id": id, "prop": "sounds", "value": sounds})
	_rebuild()


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
		{"key": "custom_draw", "label": "Host draw", "kind": "bool"},
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
