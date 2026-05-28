extends ListDetailInspector

## Lights workflow: a left list of object lights plus a right detail dock for
## the selected light's color, falloff, animation, and output flags.

# Light "flags" bitfield.
const LIGHT_FLAG_DISABLE_CORONA := 0x01
const LIGHT_FLAG_DISABLE_TERRAIN := 0x02
const LIGHT_FLAG_DISABLE_OBJECTS := 0x04


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
	var syncing := {"value": false}

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

	var style := _add_detail_spin_row(detail_box, "LightStyle", "Style", 0, 255, 1)
	var phase := _add_detail_spin_row(detail_box, "LightPhase", "Phase", 0, 255, 1)
	var rate := _add_detail_spin_row(detail_box, "LightRate", "Rate", 0, ObjectEditorWorkspace.U16_VALUE_MAX, 1)

	_add_section_heading(detail_box, "Output")

	var positive_flags_row := VBoxContainer.new()
	positive_flags_row.name = "LightPositiveFlags"
	positive_flags_row.add_theme_constant_override("separation", 2)
	positive_flags_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	detail_box.add_child(positive_flags_row)
	var draw_corona := CheckBox.new()
	draw_corona.name = "LightDrawCorona"
	draw_corona.text = "Draw corona"
	positive_flags_row.add_child(draw_corona)
	var light_terrain := CheckBox.new()
	light_terrain.name = "LightTerrain"
	light_terrain.text = "Light terrain"
	positive_flags_row.add_child(light_terrain)
	var light_objects := CheckBox.new()
	light_objects.name = "LightObjects"
	light_objects.text = "Light objects"
	positive_flags_row.add_child(light_objects)

	var flags_row := HBoxContainer.new()
	flags_row.name = "LightDisableRawRow"
	flags_row.visible = false
	flags_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	detail_box.add_child(flags_row)
	var disable_corona := CheckBox.new()
	disable_corona.name = "LightDisableCorona"
	disable_corona.text = "No corona"
	flags_row.add_child(disable_corona)
	var disable_terrain := CheckBox.new()
	disable_terrain.name = "LightDisableTerrain"
	disable_terrain.text = "No terrain"
	flags_row.add_child(disable_terrain)
	var disable_objects := CheckBox.new()
	disable_objects.name = "LightDisableObjects"
	disable_objects.text = "No objects"
	flags_row.add_child(disable_objects)

	var light_info := func(index: int) -> Dictionary:
		if object_editor == null or object_editor.object_data == null:
			return {}
		if object_editor.object_data.has_method("get_light_info"):
			return object_editor.object_data.get_light_info(index)
		return lights[index] if index >= 0 and index < lights.size() else {}

	var set_field := func(key: String, value: Variant) -> void:
		if syncing["value"] or selected["index"] < 0:
			return
		_selected_index = int(selected["index"])
		if object_editor.object_data.has_method("set_light_field"):
			object_editor.object_data.set_light_field(selected["index"], key, value)
		elif key == "color_start" or key == "color_end":
			object_editor.object_data.set_light_colors(selected["index"], start_color.color, end_color.color)
		_refresh_list()

	var sync := func(index: int) -> void:
		syncing["value"] = true
		selected["index"] = index
		if index < 0 or index >= lights.size():
			syncing["value"] = false
			return
		var info: Dictionary = light_info.call(index)
		start_color.color = info.get("color_start", Color.WHITE)
		end_color.color = info.get("color_end", Color.WHITE)
		attenuation_start.value = float(info.get("atten_start", info.get("attenuation_start", 0.0)))
		attenuation_end.value = float(info.get("atten_end", info.get("attenuation_end", 0.0)))
		falloff.value = float(info.get("falloff_deg", info.get("falloff", 0.0)))
		style.value = int(info.get("colorgen_style", info.get("style", 0)))
		phase.value = int(info.get("colorgen_phase", info.get("phase", 0)))
		rate.value = int(info.get("colorgen_rate", info.get("rate", 0)))
		var corona_disabled := bool(info.get("disable_corona", (int(info.get("flags", 0)) & LIGHT_FLAG_DISABLE_CORONA) != 0))
		var terrain_disabled := bool(info.get("disable_lightterrain", (int(info.get("flags", 0)) & LIGHT_FLAG_DISABLE_TERRAIN) != 0))
		var objects_disabled := bool(info.get("disable_lightobjects", (int(info.get("flags", 0)) & LIGHT_FLAG_DISABLE_OBJECTS) != 0))
		draw_corona.button_pressed = not corona_disabled
		light_terrain.button_pressed = not terrain_disabled
		light_objects.button_pressed = not objects_disabled
		disable_corona.button_pressed = corona_disabled
		disable_terrain.button_pressed = terrain_disabled
		disable_objects.button_pressed = objects_disabled
		syncing["value"] = false

	start_color.color_changed.connect(func(color: Color) -> void: set_field.call("color_start", color))
	end_color.color_changed.connect(func(color: Color) -> void: set_field.call("color_end", color))
	attenuation_start.value_changed.connect(func(value: float) -> void: set_field.call("atten_start", value))
	attenuation_end.value_changed.connect(func(value: float) -> void: set_field.call("atten_end", value))
	falloff.value_changed.connect(func(value: float) -> void: set_field.call("falloff_deg", value))
	style.value_changed.connect(func(value: float) -> void: set_field.call("colorgen_style", int(value)))
	phase.value_changed.connect(func(value: float) -> void: set_field.call("colorgen_phase", int(value)))
	rate.value_changed.connect(func(value: float) -> void: set_field.call("colorgen_rate", int(value)))
	draw_corona.toggled.connect(func(value: bool) -> void: set_field.call("disable_corona", not value))
	light_terrain.toggled.connect(func(value: bool) -> void: set_field.call("disable_lightterrain", not value))
	light_objects.toggled.connect(func(value: bool) -> void: set_field.call("disable_lightobjects", not value))
	disable_corona.toggled.connect(func(value: bool) -> void: set_field.call("disable_corona", value))
	disable_terrain.toggled.connect(func(value: bool) -> void: set_field.call("disable_lightterrain", value))
	disable_objects.toggled.connect(func(value: bool) -> void: set_field.call("disable_lightobjects", value))
	sync.call(_selected_index)
