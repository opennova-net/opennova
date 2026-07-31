extends Node

# Owns the runtime-interaction authoring surface for one widget. The property
# inspector supplies the document context once, then asks this module to append
# the interaction sections at the two established points in the form. All
# collection replacement, target discovery, and resource wiring stays behind
# this seam.

signal edit_requested(edit: Dictionary)
signal rebuild_requested
signal menu_jump_requested(file: String, screen: String)
signal sound_preview_requested(trigger: String, file: String)

const MnuUiHelpersScript = preload("res://modtools/mnu/mnu_ui_helpers.gd")
const MnuListEditorScript = preload("res://modtools/mnu/mnu_list_editor.gd")

const SIZE_MAX := 4096

var _document: NovaMnuDocument
var _id := -1
var _wtype := NovaMnuDocument.TYPE_UNKNOWN
var _authoring: Dictionary = {}
var _sound_sets: PackedStringArray = PackedStringArray()
var _ref_services: Dictionary = {}


func configure(document: NovaMnuDocument, id: int, wtype: int,
		authoring: Dictionary, sound_sets: PackedStringArray,
		reference_services: Dictionary) -> void:
	_document = document
	_id = id
	_wtype = wtype
	_authoring = authoring
	_sound_sets = sound_sets
	_ref_services = reference_services


# Behavior and shortcuts remain before the visual/frame sections in the parent
# inspector, matching the established form order.
func append_behavior_sections(box: VBoxContainer) -> void:
	_build_behavior_section(box)
	_build_hotkey_section(box)


# Flags, commands, and feedback remain after the visual sections.
func append_command_sections(box: VBoxContainer) -> void:
	_build_flag_section(box)
	_build_action_section(box)
	_build_sound_section(box)


func _emit(edit: Dictionary) -> void:
	edit_requested.emit(edit)


func _emit_patch_path(path: Array, value: Variant) -> void:
	_emit({"target": "widget", "id": _id, "prop": "patch",
		"value": _nested_patch(path, value)})


func _nested_patch(path: Array, value: Variant) -> Dictionary:
	var result: Variant = value
	for i in range(path.size() - 1, -1, -1):
		result = {String(path[i]): result}
	return result as Dictionary


func _wire_patch_text(edit: LineEdit, path: Array) -> void:
	var commit := func() -> void:
		if not is_instance_valid(edit) or not edit.is_inside_tree():
			return
		_emit_patch_path(path, edit.text)
	edit.text_submitted.connect(func(_text: String) -> void: commit.call())
	edit.focus_exited.connect(func() -> void: commit.call())


func _wire_patch_option(option: OptionButton, path: Array) -> void:
	option.item_selected.connect(func(index: int) -> void:
		_emit_patch_path(path, option.get_item_text(index)))


func _build_optional_int(box: VBoxContainer, label: String, state: Dictionary,
		has_key: String, value_key: String, path: Array,
		min_value: int, max_value: int) -> void:
	var enabled := MnuUiHelpersScript.add_check_row(box,
		"Use " + label.to_lower(), bool(state.get(has_key, false)))
	var spin := MnuUiHelpersScript.add_spin_row(box, label,
		int(state.get(value_key, 0)), min_value, max_value)
	spin.editable = enabled.button_pressed
	enabled.toggled.connect(func(on: bool) -> void:
		spin.editable = on
		_emit_patch_path(path + [has_key], on))
	spin.value_changed.connect(func(value: float) -> void:
		_emit_patch_path(path + [value_key], int(value)))


func _add_asset_row(box: VBoxContainer, label: String, value: String,
		kind: String, on_change: Callable) -> ResourceRefWidget:
	var row := MnuUiHelpersScript._row(box)
	row.add_child(MnuUiHelpersScript._key_label(label))
	var widget := ResourceRefWidget.new()
	widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	widget.set_value_from_path(func(path: String) -> String: return path.get_file())
	widget.configure(kind, label, _ref_services)
	widget.set_value(value)
	widget.value_changed.connect(func(next: String) -> void: on_change.call(next))
	row.add_child(widget)
	return widget


func _build_behavior_section(box: VBoxContainer) -> void:
	var behavior: Dictionary = _authoring.get("behavior", {})
	var constraints: Dictionary = _authoring.get("constraints", {})
	MnuUiHelpersScript.add_heading(box, "Behavior")
	if _wtype == NovaMnuDocument.TYPE_CHECKBOX:
		var as_button := MnuUiHelpersScript.add_check_row(box,
			"Button presentation", bool(behavior.get("as_button", false)))
		as_button.tooltip_text = "Lay out this checkbox as a full-rect toggle button"
		as_button.toggled.connect(func(on: bool) -> void:
			_emit_patch_path(["behavior", "as_button"], on))
	if _wtype == NovaMnuDocument.TYPE_RADIO \
			or _wtype == NovaMnuDocument.TYPE_CHECKBOX \
			or _wtype == NovaMnuDocument.TYPE_RADIOEDIT:
		_build_optional_int(box, "Group", behavior, "has_group", "group",
			["behavior"], 0, SIZE_MAX)

	_build_optional_int(box, "Form", behavior, "has_form", "form",
		["behavior"], 0, SIZE_MAX)
	var global_var := MnuUiHelpersScript.add_check_row(box, "Global variable",
		bool(behavior.get("global_var", false)))
	global_var.toggled.connect(func(on: bool) -> void:
		_emit_patch_path(["behavior", "global_var"], on))

	var is_edit := _wtype == NovaMnuDocument.TYPE_EDIT \
		or _wtype == NovaMnuDocument.TYPE_MULTILINE_EDIT \
		or _wtype == NovaMnuDocument.TYPE_RADIOEDIT
	if is_edit:
		var password := MnuUiHelpersScript.add_check_row(box, "Password",
			bool(behavior.get("password", false)))
		password.toggled.connect(func(on: bool) -> void:
			_emit_patch_path(["behavior", "password"], on))
		var number := MnuUiHelpersScript.add_check_row(box, "Numbers only",
			bool(constraints.get("number", false)))
		number.toggled.connect(func(on: bool) -> void:
			_emit_patch_path(["constraints", "number"], on))
		_build_optional_int(box, "Minimum", constraints, "has_minval", "minval",
			["constraints"], -2147483648, 2147483647)
		_build_optional_int(box, "Maximum", constraints, "has_maxval", "maxval",
			["constraints"], -2147483648, 2147483647)
		_build_optional_int(box, "Maximum characters", constraints,
			"has_maxchar", "maxchar", ["constraints"], 0, SIZE_MAX)

	if _wtype == NovaMnuDocument.TYPE_SCROLL \
			or _wtype == NovaMnuDocument.TYPE_MARQUEE:
		var orientation := MnuUiHelpersScript.add_option_row(box, "Orientation",
			String(behavior.get("orientation", "")),
			["", "HORIZONTAL", "VERTICAL"])
		_wire_patch_option(orientation, ["behavior", "orientation"])
	if _wtype == NovaMnuDocument.TYPE_MARQUEE:
		_add_asset_row(box, "Datasource",
			String(behavior.get("datasource", "")), "credits",
			func(value: String) -> void:
				_emit_patch_path(["behavior", "datasource"], value))
	if _wtype == NovaMnuDocument.TYPE_SCROLL:
		var scroll_size: Dictionary = _authoring.get("scroll_size", {})
		_build_optional_int(box, "Scroll height", scroll_size,
			"has_height", "height", ["scroll_size"], 0, SIZE_MAX)
		_build_optional_int(box, "Scroll width", scroll_size,
			"has_width", "width", ["scroll_size"], 0, SIZE_MAX)

	var cursor: Dictionary = _authoring.get("cursor", {})
	_add_asset_row(box, "Pointer picture", String(cursor.get("file", "")),
		"texture", func(value: String) -> void:
			_emit_patch_path(["cursor", "file"], value))
	var cursor_flags := MnuUiHelpersScript.add_text_edit_row(box,
		"Pointer flags", String(cursor.get("flags", "")))
	_wire_patch_text(cursor_flags, ["cursor", "flags"])


func _hotkeys() -> Array:
	return Array(_document.get_widget_authoring_state(_id).get(
		"hotkeys", [])).duplicate(true)


func _set_hotkey_field(index: int, key: String, value: Variant) -> void:
	var rows := _hotkeys()
	if index < 0 or index >= rows.size():
		return
	var row: Dictionary = (rows[index] as Dictionary).duplicate()
	if row.get(key) == value:
		return
	row[key] = value
	rows[index] = row
	_emit_patch_path(["hotkeys"], rows)


func _add_hotkey() -> void:
	var rows := _hotkeys()
	rows.append({"value": "VK_RETURN", "virtual": true})
	_emit_patch_path(["hotkeys"], rows)
	rebuild_requested.emit()


func _remove_hotkey(index: int) -> void:
	var rows := _hotkeys()
	if index < 0 or index >= rows.size():
		return
	rows.remove_at(index)
	_emit_patch_path(["hotkeys"], rows)
	rebuild_requested.emit()


func _move_hotkey(from: int, to: int) -> void:
	var rows := _hotkeys()
	if from < 0 or from >= rows.size() or to < 0 or to >= rows.size():
		return
	var row = rows[from]
	rows.remove_at(from)
	rows.insert(to, row)
	_emit_patch_path(["hotkeys"], rows)
	rebuild_requested.emit()


func _build_hotkey_section(box: VBoxContainer) -> void:
	MnuUiHelpersScript.add_heading(box, "Keyboard shortcuts")
	var editor = MnuListEditorScript.new()
	editor.configure([
		{"key": "value", "label": "Key", "kind": "text"},
		{"key": "virtual", "label": "Named key", "kind": "bool"},
	])
	box.add_child(editor)
	editor.set_rows(Array(_authoring.get("hotkeys", [])))
	editor.row_field_changed.connect(func(index: int, key: String,
			value: Variant) -> void:
		_set_hotkey_field(index, key, value))
	editor.row_added.connect(_add_hotkey)
	editor.row_removed.connect(_remove_hotkey)
	editor.row_moved.connect(_move_hotkey)


func _build_flag_section(box: VBoxContainer) -> void:
	var labels := _document.get_flag_labels()
	if labels.is_empty():
		return
	var flags := _document.get_widget_flags(_id)
	MnuUiHelpersScript.add_heading(box, "Flags")
	var checks: Array = []
	for i in range(labels.size()):
		var bit := 1 << i
		var check := MnuUiHelpersScript.add_check_row(
			box, String(labels[i]), (flags & bit) != 0)
		checks.append([check, bit])
	for entry in checks:
		var check_box: CheckBox = entry[0]
		check_box.toggled.connect(func(_pressed: bool) -> void:
			var mask := 0
			for candidate in checks:
				if is_instance_valid(candidate[0]) \
						and (candidate[0] as CheckBox).button_pressed:
					mask |= int(candidate[1])
			_emit({"target": "widget", "id": _id,
				"prop": "flags", "value": mask}))


func _build_action_section(box: VBoxContainer) -> void:
	MnuUiHelpersScript.add_heading(box, "Actions")
	var actions: Array = _document.get_widget_actions(_id)
	if actions.is_empty():
		MnuUiHelpersScript.add_muted(box, "No navigation actions.")
	for i in range(actions.size()):
		_build_action_row(box, actions[i], i, actions.size())
	var add_btn := Button.new()
	add_btn.text = "Add action"
	add_btn.tooltip_text = "Add a navigation/window action for this widget"
	add_btn.pressed.connect(_add_action)
	box.add_child(add_btn)


func _build_action_row(box: VBoxContainer, action: Dictionary,
		index: int, count: int) -> void:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_theme_constant_override("separation", 4)
	box.add_child(row)

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
		_set_action_field(index, "type", type_opt.get_item_text(i)))
	row.add_child(type_opt)

	var target_options := _action_target_options(action)
	if target_options.is_empty():
		var target_edit := LineEdit.new()
		target_edit.text = String(action.get("target", ""))
		target_edit.placeholder_text = "target"
		target_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		target_edit.text_submitted.connect(func(text: String) -> void:
			_set_action_field(index, "target", text))
		target_edit.focus_exited.connect(func() -> void:
			_set_action_field(index, "target", target_edit.text))
		row.add_child(target_edit)
	else:
		var target_opt := _action_option(
			target_options, String(action.get("target", "")))
		target_opt.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		target_opt.item_selected.connect(func(i: int) -> void:
			_set_action_field(index, "target", target_opt.get_item_text(i)))
		row.add_child(target_opt)

	var state_opt := _action_option(["", "SHOW", "HIDE", "ENABLE", "DISABLE"],
		String(action.get("state", "")).to_upper())
	state_opt.custom_minimum_size = Vector2(86, 0)
	state_opt.item_selected.connect(func(i: int) -> void:
		_set_action_field(index, "state", state_opt.get_item_text(i)))
	row.add_child(state_opt)

	var file_ref := ResourceRefWidget.new()
	file_ref.custom_minimum_size = Vector2(110, 0)
	file_ref.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	file_ref.set_value_from_path(func(path: String) -> String: return path.get_file())
	file_ref.configure("menu", "Target menu", _ref_services)
	file_ref.set_value(String(action.get("file", "")))
	file_ref.value_changed.connect(func(value: String) -> void:
		_set_action_field(index, "file", value))
	row.add_child(file_ref)

	if type_value == "screen" \
			and not String(action.get("file", "")).is_empty():
		var jump := Button.new()
		jump.text = "Open menu"
		jump.tooltip_text = "Open this cross-menu action target in the Menus workspace"
		jump.pressed.connect(func() -> void:
			menu_jump_requested.emit(String(action.get("file", "")),
				String(action.get("target", ""))))
		row.add_child(jump)

	if type_value == "url":
		var external := CheckBox.new()
		external.text = "External browser"
		external.tooltip_text = "Preserve the URL action's EXTERNAL_BROWSER flag"
		external.button_pressed = bool(action.get("external_browser", false))
		external.toggled.connect(func(pressed: bool) -> void:
			_set_action_field(index, "external_browser", pressed))
		row.add_child(external)

	var up := Button.new()
	up.text = "▲"
	up.disabled = index == 0
	up.pressed.connect(func() -> void: _move_action(index, index - 1))
	row.add_child(up)
	var down := Button.new()
	down.text = "▼"
	down.disabled = index >= count - 1
	down.pressed.connect(func() -> void: _move_action(index, index + 1))
	row.add_child(down)
	var remove := Button.new()
	remove.text = "✕"
	remove.tooltip_text = "Remove this action"
	remove.pressed.connect(func() -> void: _remove_action(index))
	row.add_child(remove)

	var details := GridContainer.new()
	details.columns = 2
	details.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(details)
	for spec in [["Source", "source"], ["Field", "field"]]:
		var field_key := String(spec[1])
		details.add_child(MnuUiHelpersScript._key_label(String(spec[0])))
		var edit := LineEdit.new()
		edit.text = String(action.get(field_key, ""))
		edit.placeholder_text = "(game-supplied)"
		edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		edit.text_submitted.connect(func(text: String) -> void:
			_set_action_field(index, field_key, text))
		edit.focus_exited.connect(func() -> void:
			_set_action_field(index, field_key, edit.text))
		details.add_child(edit)

	details.add_child(MnuUiHelpersScript._key_label("Test"))
	var test_opt := _action_option(["", "LT", "LE", "EQ", "GE", "GT"],
		String(action.get("test", "")).to_upper())
	test_opt.item_selected.connect(func(i: int) -> void:
		_set_action_field(index, "test", test_opt.get_item_text(i)))
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
		_set_action_field(index, "has_target_form", on))
	target_form.value_changed.connect(func(value: float) -> void:
		_set_action_field(index, "target_form", int(value)))
	form_row.add_child(has_form)
	form_row.add_child(target_form)
	details.add_child(form_row)

	details.add_child(MnuUiHelpersScript._key_label("Flags"))
	var flags_row := HBoxContainer.new()
	var toggle := CheckBox.new()
	toggle.text = "Toggle"
	toggle.button_pressed = bool(action.get("toggle", false))
	toggle.toggled.connect(func(on: bool) -> void:
		_set_action_field(index, "toggle", on))
	flags_row.add_child(toggle)
	details.add_child(flags_row)


func _action_option(options: Array, value: String) -> OptionButton:
	var option := OptionButton.new()
	var selected := -1
	for i in range(options.size()):
		var label := String(options[i])
		option.add_item(label, i)
		if label == value:
			selected = i
	if selected < 0 and not value.is_empty():
		option.add_item(value, options.size())
		selected = option.item_count - 1
	if selected >= 0:
		option.select(selected)
	return option


func _action_target_options(action: Dictionary) -> Array:
	var type_value := String(action.get("type", "")).to_lower()
	if type_value == "screen" and String(action.get("file", "")).is_empty():
		return _screen_names()
	if type_value == "window":
		return _widget_names()
	return []


func _screen_names() -> Array:
	var out := []
	if _document == null:
		return out
	for screen_id in _document.get_screen_ids():
		var name := _document.get_screen_name(screen_id)
		if not name.is_empty() and not out.has(name):
			out.append(name)
	return out


func _widget_names() -> Array:
	var out := []
	if _document == null:
		return out
	var owner := _id
	while owner >= 0 and _document.widget_exists(owner) \
			and not _document.is_screen(owner):
		owner = _document.get_parent_id(owner)
	if owner >= 0 and _document.is_screen(owner):
		_collect_widget_names(
			_document.get_screen_root_id(owner), out)
	return out


func _collect_widget_names(id: int, out: Array) -> void:
	if _document == null or id < 0 or not _document.widget_exists(id):
		return
	if not _document.is_screen(id) and id != _id:
		var name := _document.get_widget_name(id)
		if not name.is_empty() and not out.has(name):
			out.append(name)
	for child in _document.get_child_ids(id):
		_collect_widget_names(child, out)


func _set_action_field(index: int, key: String, value: Variant) -> void:
	var actions: Array = _document.get_widget_actions(_id)
	if index < 0 or index >= actions.size():
		return
	var row: Dictionary = (actions[index] as Dictionary).duplicate()
	if row.get(key, "") == value:
		return
	row[key] = value
	actions[index] = row
	_emit({"target": "widget", "id": _id,
		"prop": "actions", "value": actions})
	rebuild_requested.emit()


func _add_action() -> void:
	var actions: Array = _document.get_widget_actions(_id)
	actions.append({
		"type": "screen", "target": "", "state": "", "file": "",
		"source": "", "field": "", "test": "",
		"has_target_form": false, "target_form": 0,
		"toggle": false, "external_browser": false,
	})
	_emit({"target": "widget", "id": _id,
		"prop": "actions", "value": actions})
	rebuild_requested.emit()


func _remove_action(index: int) -> void:
	var actions: Array = _document.get_widget_actions(_id)
	if index < 0 or index >= actions.size():
		return
	actions.remove_at(index)
	_emit({"target": "widget", "id": _id,
		"prop": "actions", "value": actions})
	rebuild_requested.emit()


func _move_action(from: int, to: int) -> void:
	var actions: Array = _document.get_widget_actions(_id)
	if from < 0 or from >= actions.size() \
			or to < 0 or to >= actions.size():
		return
	var row: Dictionary = actions[from]
	actions.remove_at(from)
	actions.insert(to, row)
	_emit({"target": "widget", "id": _id,
		"prop": "actions", "value": actions})
	rebuild_requested.emit()


func _build_sound_section(box: VBoxContainer) -> void:
	MnuUiHelpersScript.add_heading(box, "Sounds")
	var sounds := _document.get_widget_sounds(_id)
	if sounds.is_empty():
		MnuUiHelpersScript.add_muted(box, "No interaction sounds.")
	for i in range(sounds.size()):
		_build_sound_row(box, sounds[i], i)
	var add_btn := Button.new()
	add_btn.text = "Add sound"
	add_btn.tooltip_text = "Add a hover/click sound played from the menu .lwf profile"
	add_btn.pressed.connect(_add_sound)
	box.add_child(add_btn)


func _build_sound_row(box: VBoxContainer, sound: Dictionary,
		index: int) -> void:
	var trigger := String(sound.get("trigger", ""))
	var file := String(sound.get("file", ""))
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_theme_constant_override("separation", 6)
	box.add_child(row)

	var state_opt := _action_option(["", "mousein", "mouseout", "selected"],
		String(sound.get("state", "")))
	state_opt.tooltip_text = "Widget state that enables this sound"
	state_opt.custom_minimum_size = Vector2(78, 0)
	state_opt.item_selected.connect(func(idx: int) -> void:
		_set_sound_field(index, "state", state_opt.get_item_text(idx)))
	row.add_child(state_opt)

	if _sound_sets.size() > 0:
		var option := OptionButton.new()
		option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		var matched := false
		for sound_index in range(_sound_sets.size()):
			option.add_item(_sound_sets[sound_index], sound_index)
			if _sound_sets[sound_index].to_upper() == trigger.to_upper():
				option.select(sound_index)
				matched = true
		if not matched and not trigger.is_empty():
			option.add_item(trigger, _sound_sets.size())
			option.select(option.item_count - 1)
		option.item_selected.connect(func(idx: int) -> void:
			_set_sound_field(index, "trigger", option.get_item_text(idx)))
		row.add_child(option)
	else:
		var trigger_edit := LineEdit.new()
		trigger_edit.text = trigger
		trigger_edit.placeholder_text = "trigger (e.g. MOUSE_OVER)"
		trigger_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		trigger_edit.text_submitted.connect(func(text: String) -> void:
			_set_sound_field(index, "trigger", text))
		trigger_edit.focus_exited.connect(func() -> void:
			_set_sound_field(index, "trigger", trigger_edit.text))
		row.add_child(trigger_edit)

	var file_ref := ResourceRefWidget.new()
	file_ref.custom_minimum_size = Vector2(120, 0)
	file_ref.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	file_ref.set_value_from_path(func(path: String) -> String: return path.get_file())
	file_ref.configure("sound", "Sound profile", _ref_services)
	file_ref.set_value(file)
	file_ref.value_changed.connect(func(value: String) -> void:
		_set_sound_field(index, "file", value))
	row.add_child(file_ref)

	var play := Button.new()
	play.text = "▶"
	play.tooltip_text = "Preview this sound"
	play.pressed.connect(func() -> void:
		sound_preview_requested.emit(trigger, file))
	row.add_child(play)

	var remove := Button.new()
	remove.text = "✕"
	remove.tooltip_text = "Remove this sound"
	remove.pressed.connect(func() -> void: _remove_sound(index))
	row.add_child(remove)


func _set_sound_field(index: int, key: String, value: String) -> void:
	var sounds := _document.get_widget_sounds(_id)
	if index < 0 or index >= sounds.size():
		return
	var sound: Dictionary = (sounds[index] as Dictionary).duplicate()
	if String(sound.get(key, "")) == value:
		return
	sound[key] = value
	sounds[index] = sound
	_emit({"target": "widget", "id": _id,
		"prop": "sounds", "value": sounds})
	rebuild_requested.emit()


func _add_sound() -> void:
	var sounds := _document.get_widget_sounds(_id)
	var trigger := String(_sound_sets[0]) \
		if _sound_sets.size() > 0 else "MOUSE_OVER"
	var upper := trigger.to_upper()
	var state := "selected" \
		if upper.contains("CLICK") or upper.contains("SELECT") else "mousein"
	sounds.append({
		"state": state,
		"trigger": trigger,
		"file": "menu.lwf",
	})
	_emit({"target": "widget", "id": _id,
		"prop": "sounds", "value": sounds})
	rebuild_requested.emit()


func _remove_sound(index: int) -> void:
	var sounds := _document.get_widget_sounds(_id)
	if index < 0 or index >= sounds.size():
		return
	sounds.remove_at(index)
	_emit({"target": "widget", "id": _id,
		"prop": "sounds", "value": sounds})
	rebuild_requested.emit()
