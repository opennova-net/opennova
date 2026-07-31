extends ObjectListDetailInspector

## Part Anims (PANM) workflow: a left list of part-animation entries for the
## selected LOD plus a right detail dock with rotation / scale / translation
## channel cards for the selected entry.

# Part-animation drivers reuse the shared generator-style enum, but PANM is a
# consumer-specific subset: retail's sampler reads a register only for code 113.
# Codes 114..117 are ordinary wave lookups and are not present in the retail PANM
# corpus, so the editor does not mislabel or author them as register operations.
const MOTION_MODE_IDS := [0, 16, 17, 24, 32, 33, 50, 52, 53, 113]
const MOTION_MODE_KEYS := {
	0: "none",
	16: "slide",
	17: "slide_inverse",
	24: "set",
	32: "rotate_cw",
	33: "rotate_ccw",
	50: "sine_wave",
	52: "saw_wave",
	53: "inverse_saw_wave",
	113: "control_register",
}
const PART_ANIM_SCALE_STYLE_OPTIONS := [
	{"id": 1, "label": "Uniform"},
	{"id": 2, "label": "Per-axis"},
]
const PART_ANIM_TRANSLATION_AXIS_OPTIONS := [
	{"id": 1, "label": "X"},
	{"id": 2, "label": "Y"},
	{"id": 3, "label": "Z"},
]
const PANM_TRANSLATION_VALUE_MIN := -128.0
const PANM_TRANSLATION_VALUE_MAX := 127.99609375
# Motion-mode IDs that bind a control register (see _motion_mode_options()).
const CONTROL_REGISTER_MODE_ID := 113

var _part_anim_lod_index := 0
var _part_anim_selected_index := 0
var _part_anim_list: ItemList
var _part_anim_lod_spin: SpinBox
var _part_anim_duplicate_button: Button
var _part_anim_delete_button: Button
var _part_anim_inspector_summary: Label
var _part_anim_inspector_context: Label


func build_main(mount: Control) -> void:
	_build_part_anims_inspector(mount)


func build_detail(box: VBoxContainer) -> void:
	_build_part_anim_detail_dock(box)


func refresh() -> void:
	_refresh_part_anim_list(_part_anim_selected_index, false)


func _motion_mode_options() -> Array:
	return GeneratorStyleCatalog.options_for_ids(
			GeneratorStyleCatalog.CONSUMER_PANM, MOTION_MODE_IDS)


func _mode_name_for_id(mode_id: int) -> String:
	return String(MOTION_MODE_KEYS.get(mode_id, "none"))


func _mode_id_for_name(mode_name: String) -> int:
	for id in MOTION_MODE_KEYS:
		if String(MOTION_MODE_KEYS[id]) == mode_name:
			return int(id)
	return 0


func _axis_name_for_id(axis_id: int) -> String:
	if axis_id == 2:
		return "y"
	if axis_id == 3:
		return "z"
	return "x"


func _axis_id_for_name(axis_name: String) -> int:
	if axis_name == "y":
		return 2
	if axis_name == "z":
		return 3
	return 1


func _part_options_for_lod(lod_index: int) -> Array:
	var data: NovaObjectData = object_editor.object_data if object_editor else null
	var options := []
	var lod_info: Dictionary = data.get_render_lod_info(lod_index) if data != null else {}
	var part_count := maxi(1, int(lod_info.get("part_count", lod_info.get("render_object_count", 1))))
	for part_index in range(part_count):
		options.append({"id": part_index, "label": "Part %02d" % part_index})
	return options


func _part_anim_entries() -> Array:
	var data: NovaObjectData = object_editor.object_data if object_editor else null
	if data == null:
		return []
	return data.get_part_anim_editor_entries(_part_anim_lod_index)


func _refresh_part_anim_list(preferred_index: int = -1, rebuild_detail: bool = true) -> void:
	var data: NovaObjectData = object_editor.object_data if object_editor else null
	if data == null:
		return
	var summary: Dictionary = data.get_summary()
	var lod_count := maxi(1, int(summary.get("lod_count", 1)))
	_part_anim_lod_index = clampi(_part_anim_lod_index, 0, lod_count - 1)
	if _part_anim_lod_spin != null:
		_part_anim_lod_spin.max_value = lod_count - 1
		_set_spin(_part_anim_lod_spin, _part_anim_lod_index)

	var total_count := 0
	for lod_index in range(lod_count):
		total_count += data.get_part_anim_count(lod_index)
	if _part_anim_inspector_summary != null:
		_part_anim_inspector_summary.text = "%d part animations" % total_count
	var entries := _part_anim_entries()
	if preferred_index >= 0:
		_part_anim_selected_index = preferred_index
	_part_anim_selected_index = clampi(_part_anim_selected_index, 0, maxi(0, entries.size() - 1)) if not entries.is_empty() else -1
	if _part_anim_inspector_context != null:
		_part_anim_inspector_context.text = "LOD %d: %d entries" % [_part_anim_lod_index, entries.size()]
	if _part_anim_duplicate_button != null:
		_part_anim_duplicate_button.disabled = _part_anim_selected_index < 0
	if _part_anim_delete_button != null:
		_part_anim_delete_button.disabled = _part_anim_selected_index < 0
	if _part_anim_list != null:
		_part_anim_list.clear()
		for anim in entries:
			_part_anim_list.add_item(String((anim as Dictionary).get("summary", "Part animation")))
		if _part_anim_selected_index >= 0:
			_part_anim_list.select(_part_anim_selected_index)
	if rebuild_detail:
		_rebuild_detail_dock()


func _build_part_anims_inspector(mount: Control) -> void:
	var box := _make_inspector_box(mount)
	box.name = "PartAnimListPane"
	var data: NovaObjectData = object_editor.object_data if object_editor else null
	var summary: Dictionary = data.get_summary() if data != null else {}
	var lod_count := maxi(1, int(summary.get("lod_count", 1)))
	_part_anim_lod_index = clampi(_part_anim_lod_index, 0, lod_count - 1)

	_part_anim_inspector_summary = Label.new()
	_part_anim_inspector_summary.name = "PartAnimInspectorSummary"
	_part_anim_inspector_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_part_anim_inspector_summary)

	_part_anim_inspector_context = Label.new()
	_part_anim_inspector_context.name = "PartAnimInspectorContext"
	_part_anim_inspector_context.theme_type_variation = &"Muted"
	_part_anim_inspector_context.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_part_anim_inspector_context)

	_part_anim_lod_spin = _add_spin_row(box, "PartAnimLodIndex", "LOD", 0, lod_count - 1, 1)

	var actions := HBoxContainer.new()
	actions.name = "PartAnimActions"
	actions.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(actions)

	var add_button := Button.new()
	add_button.name = "PartAnimAddButton"
	add_button.text = "Add"
	actions.add_child(add_button)

	_part_anim_duplicate_button = Button.new()
	_part_anim_duplicate_button.name = "PartAnimDuplicateButton"
	_part_anim_duplicate_button.text = "Duplicate"
	actions.add_child(_part_anim_duplicate_button)

	_part_anim_delete_button = Button.new()
	_part_anim_delete_button.name = "PartAnimDeleteButton"
	_part_anim_delete_button.text = "Delete"
	actions.add_child(_part_anim_delete_button)

	_part_anim_list = ItemList.new()
	_part_anim_list.name = "PartAnimList"
	_part_anim_list.custom_minimum_size = Vector2(0, 200)
	_part_anim_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_part_anim_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_child(_part_anim_list)

	_part_anim_lod_spin.value_changed.connect(func(value: float) -> void:
		_part_anim_lod_index = int(value)
		_part_anim_selected_index = 0
		_refresh_part_anim_list(0, true)
	)
	_part_anim_list.item_selected.connect(func(index: int) -> void:
		var selection_changed := index != _part_anim_selected_index
		_part_anim_selected_index = index
		_refresh_part_anim_list(index, selection_changed)
	)
	add_button.pressed.connect(func() -> void:
		if data == null:
			return
		var target_index := 0
		var entries := _part_anim_entries()
		if _part_anim_selected_index >= 0 and _part_anim_selected_index < entries.size():
			target_index = int((entries[_part_anim_selected_index] as Dictionary).get("target_part", 0))
		var new_index := int(data.call("add_part_anim", _part_anim_lod_index, target_index))
		if new_index >= 0:
			_refresh_part_anim_list(new_index, true)
	)
	_part_anim_duplicate_button.pressed.connect(func() -> void:
		if data == null or _part_anim_selected_index < 0:
			return
		var new_index := int(data.call("duplicate_part_anim", _part_anim_lod_index, _part_anim_selected_index))
		if new_index >= 0:
			_refresh_part_anim_list(new_index, true)
	)
	_part_anim_delete_button.pressed.connect(func() -> void:
		if data == null or _part_anim_selected_index < 0:
			return
		var removed_index := _part_anim_selected_index
		if bool(data.call("delete_part_anim", _part_anim_lod_index, removed_index)):
			_refresh_part_anim_list(mini(removed_index, maxi(0, data.get_part_anim_count(_part_anim_lod_index) - 1)), true)
	)
	_refresh_part_anim_list(_part_anim_selected_index, true)


func _build_part_anim_detail_dock(box: VBoxContainer) -> void:
	var data: NovaObjectData = object_editor.object_data if object_editor else null
	_add_section_heading(box, "Part animation")

	var entries := _part_anim_entries()
	if data == null or entries.is_empty() or _part_anim_selected_index < 0:
		_add_empty_state(box, "Select or add a part animation.", "PartAnimDetailsEmpty")
		return

	_part_anim_selected_index = clampi(_part_anim_selected_index, 0, entries.size() - 1)
	var info: Dictionary = entries[_part_anim_selected_index]
	var supported := bool(info.get("supported", true))

	var selected_summary := Label.new()
	selected_summary.name = "PartAnimSelectedSummary"
	selected_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	selected_summary.theme_type_variation = &"Muted"
	selected_summary.text = String(info.get("summary", "Part animation"))
	box.add_child(selected_summary)

	var unsupported_notice := Label.new()
	unsupported_notice.name = "PartAnimUnsupportedNotice"
	unsupported_notice.visible = not supported
	unsupported_notice.theme_type_variation = &"Warn"
	unsupported_notice.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	unsupported_notice.text = String(info.get("unsupported_reason", "Unsupported PANM mode"))
	box.add_child(unsupported_notice)

	var part_section := VBoxContainer.new()
	part_section.name = "PartAnimTargetSection"
	part_section.add_theme_constant_override("separation", 6)
	part_section.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(part_section)

	var part_options := _part_options_for_lod(_part_anim_lod_index)
	var target_part := _add_detail_id_option_row(part_section, "PartAnimTargetPart", "Animated part", part_options)
	var parent_part := _add_detail_id_option_row(part_section, "PartAnimParentPart", "Moves relative to", part_options)
	# Read-only: the left-pane list selection already conveys which part this entry
	# animates, so these are shown for context but not user-editable.
	target_part.disabled = true
	parent_part.disabled = true
	_populate_id_option(target_part, part_options, int(info.get("target_part", 0)))
	_populate_id_option(parent_part, part_options, int(info.get("parent_part", 0)))

	var rotation: Dictionary = info.get("rotation", {})
	var rotation_x: Dictionary = rotation.get("x", {})
	var rotation_y: Dictionary = rotation.get("y", {})
	var rotation_z: Dictionary = rotation.get("z", {})
	var rotation_card := _build_channel_card(box, "PartAnimRotationCard")
	var rotation_enabled := CheckBox.new()
	rotation_enabled.name = "PartAnimRotationEnabled"
	rotation_enabled.text = "Rotation"
	rotation_enabled.button_pressed = bool(rotation.get("enabled", false))
	rotation_enabled.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rotation_card.add_child(rotation_enabled)
	var rotation_actions := HBoxContainer.new()
	rotation_actions.name = "PartAnimRotationActions"
	rotation_actions.add_theme_constant_override("separation", 6)
	rotation_actions.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rotation_card.add_child(rotation_actions)
	var add_rotation := Button.new()
	add_rotation.name = "PartAnimAddRotationButton"
	add_rotation.text = "Add"
	add_rotation.custom_minimum_size = Vector2(0, 32)
	add_rotation.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rotation_actions.add_child(add_rotation)
	var remove_rotation := Button.new()
	remove_rotation.name = "PartAnimRemoveRotationButton"
	remove_rotation.text = "Remove"
	remove_rotation.custom_minimum_size = Vector2(0, 32)
	remove_rotation.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rotation_actions.add_child(remove_rotation)
	var rotation_controls := VBoxContainer.new()
	rotation_controls.name = "PartAnimRotationControls"
	rotation_controls.add_theme_constant_override("separation", 4)
	rotation_card.add_child(rotation_controls)
	var rotation_mode := _add_detail_id_option_row(rotation_controls, "PartAnimRotationMode", "Driver", _motion_mode_options())
	var rotation_x_from := _add_detail_spin_row(rotation_controls, "PartAnimRotationXFrom", "Yaw start", -3600, 3600, 0.1)
	var rotation_x_to := _add_detail_spin_row(rotation_controls, "PartAnimRotationXTo", "Yaw end", -3600, 3600, 0.1)
	var rotation_y_from := _add_detail_spin_row(rotation_controls, "PartAnimRotationYFrom", "Pitch start", -3600, 3600, 0.1)
	var rotation_y_to := _add_detail_spin_row(rotation_controls, "PartAnimRotationYTo", "Pitch end", -3600, 3600, 0.1)
	var rotation_z_from := _add_detail_spin_row(rotation_controls, "PartAnimRotationZFrom", "Roll start", -3600, 3600, 0.1)
	var rotation_z_to := _add_detail_spin_row(rotation_controls, "PartAnimRotationZTo", "Roll end", -3600, 3600, 0.1)
	var rotation_speed := _add_detail_spin_row(rotation_controls, "PartAnimRotationSpeed", "Speed", -128, 128, 0.01)
	var reversed := CheckBox.new()
	reversed.name = "PartAnimRotationReversed"
	reversed.text = "Reverse rotation order"
	reversed.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rotation_controls.add_child(reversed)
	_populate_id_option(rotation_mode, _motion_mode_options(), _mode_id_for_name(String(rotation_x.get("mode", "none"))))
	_set_spin(rotation_x_from, float(rotation_x.get("from_value", 0.0)))
	_set_spin(rotation_x_to, float(rotation_x.get("to_value", 0.0)))
	_set_spin(rotation_y_from, float(rotation_y.get("from_value", 0.0)))
	_set_spin(rotation_y_to, float(rotation_y.get("to_value", 0.0)))
	_set_spin(rotation_z_from, float(rotation_z.get("from_value", 0.0)))
	_set_spin(rotation_z_to, float(rotation_z.get("to_value", 0.0)))
	_set_spin(rotation_speed, float(rotation_x.get("speed", 0.0)))
	reversed.button_pressed = bool(rotation.get("reversed", false))

	var scale: Dictionary = info.get("scale", {})
	var scale_x: Dictionary = scale.get("x", {})
	var scale_card := _build_channel_card(box, "PartAnimScaleCard")
	var scale_enabled := CheckBox.new()
	scale_enabled.name = "PartAnimScaleEnabled"
	scale_enabled.text = "Scale"
	scale_enabled.button_pressed = bool(scale.get("enabled", false))
	scale_enabled.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scale_card.add_child(scale_enabled)
	var scale_actions := HBoxContainer.new()
	scale_actions.name = "PartAnimScaleActions"
	scale_actions.add_theme_constant_override("separation", 6)
	scale_actions.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scale_card.add_child(scale_actions)
	var add_scale := Button.new()
	add_scale.name = "PartAnimAddScaleButton"
	add_scale.text = "Add"
	add_scale.custom_minimum_size = Vector2(0, 32)
	add_scale.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scale_actions.add_child(add_scale)
	var remove_scale := Button.new()
	remove_scale.name = "PartAnimRemoveScaleButton"
	remove_scale.text = "Remove"
	remove_scale.custom_minimum_size = Vector2(0, 32)
	remove_scale.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scale_actions.add_child(remove_scale)
	var scale_controls := VBoxContainer.new()
	scale_controls.name = "PartAnimScaleControls"
	scale_controls.add_theme_constant_override("separation", 4)
	scale_card.add_child(scale_controls)
	var scale_mode := _add_detail_id_option_row(scale_controls, "PartAnimScaleMode", "Style", PART_ANIM_SCALE_STYLE_OPTIONS)
	var scale_motion := _add_detail_id_option_row(scale_controls, "PartAnimScaleMotionMode", "Driver", _motion_mode_options())
	var scale_from := _add_detail_spin_row(scale_controls, "PartAnimScaleUniformFrom", "Start", -128, 128, 0.01)
	var scale_to := _add_detail_spin_row(scale_controls, "PartAnimScaleUniformTo", "End", -128, 128, 0.01)
	var scale_speed := _add_detail_spin_row(scale_controls, "PartAnimScaleSpeed", "Speed", -128, 128, 0.01)
	_populate_id_option(scale_mode, PART_ANIM_SCALE_STYLE_OPTIONS, 2 if String(scale.get("style", "uniform")) == "per_axis" else 1)
	_populate_id_option(scale_motion, _motion_mode_options(), _mode_id_for_name(String(scale_x.get("mode", "none"))))
	_set_spin(scale_from, float(scale_x.get("from_value", 1.0)))
	_set_spin(scale_to, float(scale_x.get("to_value", 1.0)))
	_set_spin(scale_speed, float(scale_x.get("speed", 0.0)))

	var translation: Dictionary = info.get("translation", {})
	var translation_track: Dictionary = translation.get("track", {})
	var translation_card := _build_channel_card(box, "PartAnimTranslationCard")
	var translation_enabled := CheckBox.new()
	translation_enabled.name = "PartAnimTranslationEnabled"
	translation_enabled.text = "Translation"
	translation_enabled.button_pressed = bool(translation.get("enabled", false))
	translation_enabled.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	translation_card.add_child(translation_enabled)
	var translation_actions := HBoxContainer.new()
	translation_actions.name = "PartAnimTranslationActions"
	translation_actions.add_theme_constant_override("separation", 6)
	translation_actions.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	translation_card.add_child(translation_actions)
	var add_translation := Button.new()
	add_translation.name = "PartAnimAddTranslationButton"
	add_translation.text = "Add"
	add_translation.custom_minimum_size = Vector2(0, 32)
	add_translation.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	translation_actions.add_child(add_translation)
	var remove_translation := Button.new()
	remove_translation.name = "PartAnimRemoveTranslationButton"
	remove_translation.text = "Remove"
	remove_translation.custom_minimum_size = Vector2(0, 32)
	remove_translation.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	translation_actions.add_child(remove_translation)
	var translation_controls := VBoxContainer.new()
	translation_controls.name = "PartAnimTranslationControls"
	translation_controls.add_theme_constant_override("separation", 4)
	translation_card.add_child(translation_controls)
	var translation_axis := _add_detail_id_option_row(translation_controls, "PartAnimTranslationAxis", "Direction", PART_ANIM_TRANSLATION_AXIS_OPTIONS)
	var translation_mode := _add_detail_id_option_row(translation_controls, "PartAnimTranslationMode", "Driver", _motion_mode_options())
	var translation_register = _add_detail_ctrl_reg_row(translation_controls, "PartAnimTranslationRegister", "Control register")
	var translation_from := _add_detail_spin_row(translation_controls, "PartAnimTranslationFrom", "Start", PANM_TRANSLATION_VALUE_MIN, PANM_TRANSLATION_VALUE_MAX, 0.01)
	var translation_to := _add_detail_spin_row(translation_controls, "PartAnimTranslationTo", "End", PANM_TRANSLATION_VALUE_MIN, PANM_TRANSLATION_VALUE_MAX, 0.01)
	var translation_speed := _add_detail_spin_row(translation_controls, "PartAnimTranslationSpeed", "Speed", -128, 128, 0.01)
	_populate_id_option(translation_axis, PART_ANIM_TRANSLATION_AXIS_OPTIONS, _axis_id_for_name(String(translation.get("axis", "x"))))
	_populate_id_option(translation_mode, _motion_mode_options(), _mode_id_for_name(String(translation_track.get("mode", "none"))))
	if translation_register != null:
		translation_register.setup(_control_registers(), int(translation_track.get("control_register", -1)))
	_set_spin(translation_from, float(translation_track.get("from_value", 0.0)))
	_set_spin(translation_to, float(translation_track.get("to_value", 0.0)))
	_set_spin(translation_speed, float(translation_track.get("speed", 0.0)))

	var update_visibility := func() -> void:
		rotation_controls.visible = rotation_enabled.button_pressed and supported
		add_rotation.visible = not rotation_enabled.button_pressed and supported
		remove_rotation.visible = rotation_enabled.button_pressed and supported
		scale_controls.visible = scale_enabled.button_pressed and supported
		add_scale.visible = not scale_enabled.button_pressed and supported
		remove_scale.visible = scale_enabled.button_pressed and supported
		translation_controls.visible = translation_enabled.button_pressed and supported
		add_translation.visible = not translation_enabled.button_pressed and supported
		remove_translation.visible = translation_enabled.button_pressed and supported
		if translation_register != null and translation_register.get_parent() != null:
			translation_register.get_parent().visible = translation_mode.get_selected_id() == CONTROL_REGISTER_MODE_ID
	update_visibility.call()

	var refresh_after_edit := func() -> void:
		_refresh_part_anim_list(_part_anim_selected_index, false)

	# These dropdowns are read-only (disabled) for the user, so item_selected is not
	# reachable by clicking; the handlers stay as the programmatic data-binding seam
	# (exercised by tests via item_selected.emit) and in case the controls are ever
	# re-enabled.
	target_part.item_selected.connect(func(_index: int) -> void:
		if data != null:
			data.set_part_anim_target(_part_anim_lod_index, _part_anim_selected_index, target_part.get_selected_id(), parent_part.get_selected_id())
			refresh_after_edit.call()
	)
	parent_part.item_selected.connect(func(_index: int) -> void:
		if data != null:
			data.set_part_anim_target(_part_anim_lod_index, _part_anim_selected_index, target_part.get_selected_id(), parent_part.get_selected_id())
			refresh_after_edit.call()
	)
	rotation_enabled.toggled.connect(func(value: bool) -> void:
		if data != null:
			data.set_part_anim_channel_enabled(_part_anim_lod_index, _part_anim_selected_index, "rotation", value)
			refresh_after_edit.call()
		update_visibility.call()
	)
	add_rotation.pressed.connect(func() -> void:
		rotation_enabled.button_pressed = true
		rotation_enabled.toggled.emit(true)
	)
	remove_rotation.pressed.connect(func() -> void:
		rotation_enabled.button_pressed = false
		rotation_enabled.toggled.emit(false)
	)
	rotation_mode.item_selected.connect(func(index: int) -> void:
		if data != null:
			data.set_part_anim_channel_mode(_part_anim_lod_index, _part_anim_selected_index, "rotation", "x", _mode_name_for_id(rotation_mode.get_item_id(index)), -1)
			refresh_after_edit.call()
	)
	var set_rotation_values := func(axis: String, from_control: SpinBox, to_control: SpinBox) -> void:
		if data != null:
			data.set_part_anim_channel_values(_part_anim_lod_index, _part_anim_selected_index, "rotation", axis, float(from_control.value), float(to_control.value), float(rotation_speed.value))
			refresh_after_edit.call()
	rotation_x_from.value_changed.connect(func(_value: float) -> void: set_rotation_values.call("x", rotation_x_from, rotation_x_to))
	rotation_x_to.value_changed.connect(func(_value: float) -> void: set_rotation_values.call("x", rotation_x_from, rotation_x_to))
	rotation_y_from.value_changed.connect(func(_value: float) -> void: set_rotation_values.call("y", rotation_y_from, rotation_y_to))
	rotation_y_to.value_changed.connect(func(_value: float) -> void: set_rotation_values.call("y", rotation_y_from, rotation_y_to))
	rotation_z_from.value_changed.connect(func(_value: float) -> void: set_rotation_values.call("z", rotation_z_from, rotation_z_to))
	rotation_z_to.value_changed.connect(func(_value: float) -> void: set_rotation_values.call("z", rotation_z_from, rotation_z_to))
	rotation_speed.value_changed.connect(func(_value: float) -> void:
		set_rotation_values.call("x", rotation_x_from, rotation_x_to)
		set_rotation_values.call("y", rotation_y_from, rotation_y_to)
		set_rotation_values.call("z", rotation_z_from, rotation_z_to)
	)
	reversed.toggled.connect(func(value: bool) -> void:
		if data != null:
			data.set_part_anim_rotation_reversed(_part_anim_lod_index, _part_anim_selected_index, value)
			refresh_after_edit.call()
	)
	scale_enabled.toggled.connect(func(value: bool) -> void:
		if data != null:
			data.set_part_anim_channel_enabled(_part_anim_lod_index, _part_anim_selected_index, "scale", value)
			refresh_after_edit.call()
		update_visibility.call()
	)
	add_scale.pressed.connect(func() -> void:
		scale_enabled.button_pressed = true
		scale_enabled.toggled.emit(true)
	)
	remove_scale.pressed.connect(func() -> void:
		scale_enabled.button_pressed = false
		scale_enabled.toggled.emit(false)
	)
	var set_scale_values := func(axis: String) -> void:
		if data != null:
			data.set_part_anim_channel_values(_part_anim_lod_index, _part_anim_selected_index, "scale", axis, float(scale_from.value), float(scale_to.value), float(scale_speed.value))
			refresh_after_edit.call()
	scale_mode.item_selected.connect(func(index: int) -> void:
		set_scale_values.call("per_axis" if scale_mode.get_item_id(index) == 2 else "uniform")
	)
	scale_motion.item_selected.connect(func(index: int) -> void:
		if data != null:
			data.set_part_anim_channel_mode(_part_anim_lod_index, _part_anim_selected_index, "scale", "uniform", _mode_name_for_id(scale_motion.get_item_id(index)), -1)
			refresh_after_edit.call()
	)
	scale_from.value_changed.connect(func(_value: float) -> void: set_scale_values.call("uniform"))
	scale_to.value_changed.connect(func(_value: float) -> void: set_scale_values.call("uniform"))
	scale_speed.value_changed.connect(func(_value: float) -> void: set_scale_values.call("uniform"))
	translation_enabled.toggled.connect(func(value: bool) -> void:
		if data != null:
			data.set_part_anim_channel_enabled(_part_anim_lod_index, _part_anim_selected_index, "translation", value)
			refresh_after_edit.call()
		update_visibility.call()
	)
	add_translation.pressed.connect(func() -> void:
		translation_enabled.button_pressed = true
		translation_enabled.toggled.emit(true)
	)
	remove_translation.pressed.connect(func() -> void:
		translation_enabled.button_pressed = false
		translation_enabled.toggled.emit(false)
	)
	var set_translation_values := func() -> void:
		if data != null:
			data.set_part_anim_channel_values(_part_anim_lod_index, _part_anim_selected_index, "translation", _axis_name_for_id(translation_axis.get_selected_id()), float(translation_from.value), float(translation_to.value), float(translation_speed.value))
			refresh_after_edit.call()
	translation_axis.item_selected.connect(func(_index: int) -> void: set_translation_values.call())
	translation_mode.item_selected.connect(func(index: int) -> void:
		if data != null:
			data.set_part_anim_channel_mode(_part_anim_lod_index, _part_anim_selected_index, "translation", _axis_name_for_id(translation_axis.get_selected_id()), _mode_name_for_id(translation_mode.get_item_id(index)), translation_register.selected if translation_register != null else -1)
			refresh_after_edit.call()
		update_visibility.call()
	)
	if translation_register != null:
		translation_register.register_selected.connect(func(reg: int) -> void:
			if data != null:
				data.set_part_anim_channel_mode(_part_anim_lod_index, _part_anim_selected_index, "translation", _axis_name_for_id(translation_axis.get_selected_id()), _mode_name_for_id(translation_mode.get_selected_id()), reg)
				refresh_after_edit.call()
		)
	translation_from.value_changed.connect(func(_value: float) -> void: set_translation_values.call())
	translation_to.value_changed.connect(func(_value: float) -> void: set_translation_values.call())
	translation_speed.value_changed.connect(func(_value: float) -> void: set_translation_values.call())
