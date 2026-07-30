extends ObjectListDetailInspector

## Lights workflow: a left list of object lights plus a right detail dock for
## the selected light's color, falloff, animation, and output flags.

# Light "flags" bitfield.
const LIGHT_FLAG_DISABLE_CORONA := 0x01
const LIGHT_FLAG_DISABLE_TERRAIN := 0x02
const LIGHT_FLAG_DISABLE_OBJECTS := 0x04

# Light color uses the RGB generator dispatch. Styles 113 and 114 read CTRL;
# 115..117 remain low-nibble waveform lookups.
# [orig: RgbGen_EvaluateColor @ 0x5B23D0]


func _lights() -> Array:
	return object_editor.object_data.get_lights() if object_editor and object_editor.object_data else []


func _list_items() -> Array:
	return _lights()


func _list_item_text(item, _index: int) -> String:
	return "%02d  part %d" % [int(item.get("index", 0)), int(item.get("part_index", 0))]


func _list_summary_text(count: int) -> String:
	return "%d lights" % count


func _list_node_name() -> StringName:
	return &"ObjectLightsList"


func _list_panel_node_name() -> StringName:
	return &"LightListPanel"


func _detail_panel_node_name() -> StringName:
	return &"LightDetailPanel"


func _empty_detail_text() -> String:
	return "Select a light."


func _empty_detail_node_name() -> StringName:
	return &"LightDetailEmpty"


func _build_detail_fields(detail_box: VBoxContainer, _index: int) -> void:
	var lights := _lights()
	var selected := {"index": _selected_index}
	var binder := FieldBinder.new()

	_add_section_heading(detail_box, "Color")

	var start_color_row := _add_detail_field(detail_box, "Start color")
	var start_color := ColorPickerButton.new()
	start_color.name = "LightStartColor"
	start_color.custom_minimum_size = Vector2(0, 34)
	start_color.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	start_color_row.add_child(start_color)

	var end_color_row := _add_detail_field(detail_box, "End color")
	var end_color := ColorPickerButton.new()
	end_color.name = "LightEndColor"
	end_color.custom_minimum_size = Vector2(0, 34)
	end_color.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	end_color_row.add_child(end_color)

	_add_section_heading(detail_box, "Falloff")

	var attenuation_start := _add_detail_spin_row(detail_box, "LightAttenuationStart", "Atten start", 0, 100000, 0.1)
	var attenuation_end := _add_detail_spin_row(detail_box, "LightAttenuationEnd", "Atten end", 0, 100000, 0.1)
	var falloff := _add_detail_spin_row(detail_box, "LightFalloff", "Falloff", 0, 255, 1)

	_add_section_heading(detail_box, "Animation")

	var style := _add_detail_id_option_row(detail_box, "LightStyle", "Style", GeneratorStyleCatalog.options_for_consumer(GeneratorStyleCatalog.CONSUMER_LIGHT))
	var phase := _add_detail_spin_row(detail_box, "LightPhase", "Phase", 0, 255, 1)
	var control_reference = _add_detail_ctrl_reg_row(
			detail_box, "LightControlRegister", "Control reference")
	var rate := _add_detail_spin_row(detail_box, "LightRate", "Rate", 0, ObjectEditorWorkspace.U16_VALUE_MAX, 1)
	var sync_parameter_visibility := func(style_id: int) -> void:
		# The loader resolves every style > 0x70 through the model-local CTRL
		# table. Waveform fallbacks consume the resolved ordinal as phase, so
		# the authored field remains a reference even though no CTRL value is
		# read. [orig: loader fixup sub_5B4640 @ 0x5B4640]
		var is_reference := GeneratorStyleCatalog.parameter_is_ctrl_reference(style_id)
		(phase.get_parent() as Control).visible = not is_reference
		if control_reference != null and control_reference.get_parent() != null:
			(control_reference.get_parent() as Control).visible = is_reference

	_add_section_heading(detail_box, "Output")

	var positive_flags_row := VBoxContainer.new()
	positive_flags_row.name = "LightPositiveFlags"
	positive_flags_row.add_theme_constant_override("separation", 2)
	positive_flags_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	detail_box.add_child(positive_flags_row)
	var draw_corona := _add_checkbox(positive_flags_row, "LightDrawCorona", "Draw corona")
	var light_terrain := _add_checkbox(positive_flags_row, "LightTerrain", "Light terrain")
	var light_objects := _add_checkbox(positive_flags_row, "LightObjects", "Light objects")

	var flags_row := HBoxContainer.new()
	flags_row.name = "LightDisableRawRow"
	flags_row.visible = false
	flags_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	detail_box.add_child(flags_row)
	var disable_corona := _add_checkbox(flags_row, "LightDisableCorona", "No corona")
	var disable_terrain := _add_checkbox(flags_row, "LightDisableTerrain", "No terrain")
	var disable_objects := _add_checkbox(flags_row, "LightDisableObjects", "No objects")

	var light_info := func(index: int) -> Dictionary:
		if object_editor == null or object_editor.object_data == null:
			return {}
		return object_editor.object_data.get_light_info(index)

	var set_field := func(key: String, value: Variant) -> void:
		if int(selected["index"]) < 0:
			return
		_selected_index = int(selected["index"])
		object_editor.object_data.set_light_field(int(selected["index"]), key, value)
		_refresh_list()

	var corona_disabled := func(info: Dictionary) -> bool:
		return bool(info.get("disable_corona", (int(info.get("flags", 0)) & LIGHT_FLAG_DISABLE_CORONA) != 0))
	var terrain_disabled := func(info: Dictionary) -> bool:
		return bool(info.get("disable_lightterrain", (int(info.get("flags", 0)) & LIGHT_FLAG_DISABLE_TERRAIN) != 0))
	var objects_disabled := func(info: Dictionary) -> bool:
		return bool(info.get("disable_lightobjects", (int(info.get("flags", 0)) & LIGHT_FLAG_DISABLE_OBJECTS) != 0))

	binder.bind_color(start_color,
		func(info): return info.get("color_start", Color.WHITE),
		func(value): set_field.call("color_start", value))
	binder.bind_color(end_color,
		func(info): return info.get("color_end", Color.WHITE),
		func(value): set_field.call("color_end", value))
	binder.bind_spin(attenuation_start,
		func(info): return float(info.get("atten_start", info.get("attenuation_start", 0.0))),
		func(value): set_field.call("atten_start", value))
	binder.bind_spin(attenuation_end,
		func(info): return float(info.get("atten_end", info.get("attenuation_end", 0.0))),
		func(value): set_field.call("atten_end", value))
	binder.bind_spin(falloff,
		func(info): return float(info.get("falloff_deg", info.get("falloff", 0.0))),
		func(value): set_field.call("falloff_deg", value))
	# Style is a labeled dropdown rather than a raw number, so it is wired manually
	# (FieldBinder has no option helper) and populated in `sync` below. OptionButton
	# does not emit item_selected on programmatic select(), so no guard is needed.
	style.item_selected.connect(func(index: int) -> void:
		var style_id := style.get_item_id(index)
		set_field.call("colorgen_style", style_id)
		sync_parameter_visibility.call(style_id))
	binder.bind_spin(phase,
		func(info): return float(int(info.get("colorgen_phase", info.get("phase", 0)))),
		func(value): set_field.call("colorgen_phase", int(value)))
	if control_reference != null:
		control_reference.register_selected.connect(func(reg: int) -> void:
			set_field.call("colorgen_phase", maxi(0, reg)))
	binder.bind_spin(rate,
		func(info): return float(int(info.get("colorgen_rate", info.get("rate", 0)))),
		func(value): set_field.call("colorgen_rate", int(value)))
	binder.bind_checkbox(draw_corona,
		func(info): return not corona_disabled.call(info),
		func(value): set_field.call("disable_corona", not value))
	binder.bind_checkbox(light_terrain,
		func(info): return not terrain_disabled.call(info),
		func(value): set_field.call("disable_lightterrain", not value))
	binder.bind_checkbox(light_objects,
		func(info): return not objects_disabled.call(info),
		func(value): set_field.call("disable_lightobjects", not value))
	binder.bind_checkbox(disable_corona,
		func(info): return corona_disabled.call(info),
		func(value): set_field.call("disable_corona", value))
	binder.bind_checkbox(disable_terrain,
		func(info): return terrain_disabled.call(info),
		func(value): set_field.call("disable_lightterrain", value))
	binder.bind_checkbox(disable_objects,
		func(info): return objects_disabled.call(info),
		func(value): set_field.call("disable_lightobjects", value))

	var sync := func(index: int) -> void:
		selected["index"] = index
		if index < 0 or index >= lights.size():
			return
		var info: Dictionary = light_info.call(index)
		binder.sync_from(info)
		var style_id := int(info.get("colorgen_style", info.get("style", 0)))
		var parameter := int(info.get("colorgen_phase", info.get("phase", 0)))
		_populate_id_option(style, GeneratorStyleCatalog.options_for_consumer(GeneratorStyleCatalog.CONSUMER_LIGHT), style_id)
		if control_reference != null:
			control_reference.setup(_control_registers(), parameter)
		sync_parameter_visibility.call(style_id)

	sync.call(_selected_index)
