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
# Audition a widget's sound: the workspace resolves (trigger -> set in the menu .lwf
# -> member -> .wav) and plays it, reusing the Sound workspace's preview player.
signal sound_preview_requested(trigger: String, file: String)

const MnuUiHelpersScript = preload("res://modtools/mnu/mnu_ui_helpers.gd")
const MnuListEditorScript = preload("res://modtools/mnu/mnu_list_editor.gd")
const MnuStringPickerScript = preload("res://modtools/mnu/mnu_string_picker.gd")

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

	var font_edit := MnuUiHelpersScript.add_text_edit_row(_box, "Font", _document.get_widget_font(id))
	_wire_text(font_edit, {"target": "widget", "id": id, "prop": "font"})
	if not _document.get_widget_font(id).is_empty():
		var font_jump := Button.new()
		font_jump.text = "Open in Fonts"
		font_jump.tooltip_text = "Open this font in the Fonts workspace"
		font_jump.pressed.connect(func() -> void: font_jump_requested.emit(font_edit.text))
		_box.add_child(font_jump)

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
	_build_action_section(id)
	_build_sound_section(id)

	# M10: nested template authoring. Item rows for list-like widgets; column
	# header/body definitions for tables. These emit op-tagged edits that the
	# editor routes through the snapshot-undo path (the row set is a collection,
	# not a single scalar, so the whole-list state is the natural undo unit).
	if wtype == NovaMnuDocument.TYPE_LIST or wtype == NovaMnuDocument.TYPE_MULTI \
			or wtype == NovaMnuDocument.TYPE_SPINLIST or wtype == NovaMnuDocument.TYPE_COMBO:
		_build_item_section(id)
	elif wtype == NovaMnuDocument.TYPE_TABLE:
		_build_table_section(id)


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
	var type_opt := _action_option(["screen", "window", "pop_screen", "quit", "url"], type_value)
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

	var state_opt := _action_option(["", "SHOW", "HIDE", "TOGGLE"], String(action.get("state", "")).to_upper())
	state_opt.disabled = type_value != "window"
	state_opt.custom_minimum_size = Vector2(86, 0)
	state_opt.item_selected.connect(func(i: int) -> void:
		_set_action_field(id, index, "state", state_opt.get_item_text(i)))
	row.add_child(state_opt)

	var file_edit := LineEdit.new()
	file_edit.text = String(action.get("file", ""))
	file_edit.placeholder_text = "file"
	file_edit.custom_minimum_size = Vector2(86, 0)
	file_edit.text_submitted.connect(func(t: String) -> void: _set_action_field(id, index, "file", t))
	file_edit.focus_exited.connect(func() -> void: _set_action_field(id, index, "file", file_edit.text))
	row.add_child(file_edit)

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
	for sid in _document.get_screen_ids():
		_collect_widget_names(_document.get_screen_root_id(sid), out, selected_id)
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
	actions.append({"type": "screen", "target": "", "state": "", "file": "", "external_browser": false})
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

	var file_edit := LineEdit.new()
	file_edit.text = file
	file_edit.placeholder_text = "menu.lwf"
	file_edit.custom_minimum_size = Vector2(96, 0)
	file_edit.text_submitted.connect(func(t: String) -> void: _set_sound_field(id, index, "file", t))
	file_edit.focus_exited.connect(func() -> void: _set_sound_field(id, index, "file", file_edit.text))
	row.add_child(file_edit)

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
