extends WorkflowInspector

## Preview workflow: object summary, playback controls, the export-chunk mask,
## and per-control-register sliders. The export mask itself is coordinator
## state (used by begin_export); this inspector only builds its checkboxes.


func build_main(host: Control) -> void:
	var box := _make_inspector_box(host)
	var summary := object_editor.object_data.get_summary() if object_editor and object_editor.object_data else {}
	for key in ["source_kind", "lod_count", "material_count", "light_count", "userpoint_count"]:
		var row := Label.new()
		row.text = "%s: %s" % [String(key).capitalize(), str(summary.get(key, ""))]
		row.clip_text = true
		box.add_child(row)

	var playback := HBoxContainer.new()
	playback.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(playback)

	var play := Button.new()
	play.name = "PreviewPlayButton"
	play.toggle_mode = true
	play.button_pressed = _preview == null or _preview.is_playing()
	play.text = "Pause" if play.button_pressed else "Play"
	playback.add_child(play)

	var reset := Button.new()
	reset.name = "PreviewResetButton"
	reset.text = "Reset"
	playback.add_child(reset)

	var wire := CheckBox.new()
	wire.name = "PreviewWireCheck"
	wire.text = "Wire"
	wire.button_pressed = _preview != null and _preview.is_wireframe()
	playback.add_child(wire)

	play.toggled.connect(func(pressed: bool) -> void:
		if _preview != null:
			_preview.set_playing(pressed)
		play.text = "Pause" if pressed else "Play"
	)
	reset.pressed.connect(func() -> void:
		if _preview != null:
			_preview.reset_animation_time()
	)
	wire.toggled.connect(func(pressed: bool) -> void:
		if _preview != null:
			_preview.set_wireframe(pressed)
	)

	_build_export_mask_controls(box)

	var ctrl_regs: Array = object_editor.object_data.get_control_registers() if object_editor and object_editor.object_data and object_editor.object_data.has_method("get_control_registers") else []
	if not ctrl_regs.is_empty() and _preview != null:
		var ctrl_label := Label.new()
		ctrl_label.text = "Control registers"
		box.add_child(ctrl_label)
		for reg in ctrl_regs:
			var reg_name := String((reg as Dictionary).get("name", ""))
			if reg_name.is_empty():
				continue
			var row := HBoxContainer.new()
			row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			box.add_child(row)
			var label := Label.new()
			label.text = reg_name
			label.custom_minimum_size = Vector2(90, 0)
			row.add_child(label)
			var slider := HSlider.new()
			slider.name = "ControlRegisterSlider_%s" % reg_name.replace(" ", "_")
			slider.min_value = 0
			slider.max_value = ObjectEditorWorkspace.U16_VALUE_MAX
			slider.step = 1
			slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			row.add_child(slider)
			slider.value_changed.connect(func(value: float, name: String = reg_name) -> void:
				if _preview != null:
					_preview.set_ctrl_value(name, int(value))
			)


func _build_export_mask_controls(box: VBoxContainer) -> void:
	if object_editor == null or object_editor.object_data == null or not object_editor.object_data.can_export_3di():
		return
	_ws._sync_export_update_mask_from_dirty()
	var label := Label.new()
	label.text = "Export chunks"
	box.add_child(label)

	var row := HBoxContainer.new()
	row.name = "ObjectExportMaskControls"
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(row)

	_add_export_mask_check(row, "ObjectExportMtrlCheck", "MTRL", ObjectEditorWorkspace.OED_UPDATE_MTRL)
	_add_export_mask_check(row, "ObjectExportLghtCheck", "LGHT", ObjectEditorWorkspace.OED_UPDATE_LGHT)
	_add_export_mask_check(row, "ObjectExportPanmCheck", "PANM", ObjectEditorWorkspace.OED_UPDATE_PANM)


func _add_export_mask_check(parent: HBoxContainer, node_name: String, label: String, bit: int) -> CheckBox:
	var check := CheckBox.new()
	check.name = node_name
	check.text = label
	check.button_pressed = (_ws._export_update_mask & bit) != 0
	parent.add_child(check)
	check.toggled.connect(func(pressed: bool) -> void:
		if pressed:
			_ws._export_update_mask |= bit
		else:
			_ws._export_update_mask &= ~bit
	)
	return check
