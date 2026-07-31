class_name ScatterInspector
extends TerrainInspector

## Foliage workflow: a list of foliage defs (capped at FOLIAGE_DEFS_LIMIT) with a
## detail panel for the selected def's graphic, colour rules, and flags, plus the
## shared brush controls. Code-first; built into the inspector mount by the
## terrain workspace.

const VegPickerScene = preload("res://modtools/terrain/ui/widgets/veg_picker.tscn")
const FOLIAGE_DEFS_LIMIT := 4

var _selected_index: int = -1
var _list: ItemList
var _count_label: Label
var _add_button: Button
var _remove_button: Button
var _detail_box: VBoxContainer
var _detail_empty: Label
var _graphic_button: Button
var _graphic_name_label: Label
var _color_lower: OptionButton
var _color_upper: OptionButton
var _shadow_toggle: CheckBox
var _force_on_toggle: CheckBox
var _graphic_preview: VegPreview
var _picker: VegPicker


func set_editor(value: TerrainEditor) -> void:
	super.set_editor(value)
	if terrain_editor != null and terrain_editor.current_tool != TerrainEditor.Tool.FOLIAGE_PAINT:
		terrain_editor.set_tool(TerrainEditor.Tool.FOLIAGE_PAINT)


func build_main(mount: Control) -> void:
	var box := _make_inspector_box(mount)
	_root = box
	_add_section_heading(box, "Foliage")

	var header := HBoxContainer.new()
	header.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(header)
	var types_label := Label.new()
	types_label.text = "Foliage types"
	types_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(types_label)
	_count_label = Label.new()
	_count_label.theme_type_variation = &"Muted"
	header.add_child(_count_label)

	_list = ItemList.new()
	_list.name = "FoliageList"
	_list.custom_minimum_size = Vector2(0, 120)
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(_list)
	_list.item_selected.connect(_on_list_selected)

	var actions := HBoxContainer.new()
	actions.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(actions)
	_add_button = Button.new()
	_add_button.name = "AddButton"
	_add_button.text = "Add"
	_add_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	actions.add_child(_add_button)
	_add_button.pressed.connect(_on_add_pressed)
	_remove_button = Button.new()
	_remove_button.name = "RemoveButton"
	_remove_button.text = "Remove"
	_remove_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	actions.add_child(_remove_button)
	_remove_button.pressed.connect(_on_remove_pressed)

	_detail_empty = _add_muted_label(box, "Select a foliage type to edit it.")

	_detail_box = VBoxContainer.new()
	_detail_box.name = "DetailBox"
	_detail_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_detail_box.add_theme_constant_override("separation", 6)
	box.add_child(_detail_box)

	var graphic_label := Label.new()
	graphic_label.text = "Graphic"
	_detail_box.add_child(graphic_label)
	_graphic_button = Button.new()
	_graphic_button.name = "GraphicButton"
	_graphic_button.custom_minimum_size = Vector2(0, 180)
	_graphic_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_detail_box.add_child(_graphic_button)
	_graphic_button.pressed.connect(_on_graphic_button_pressed)
	_build_graphic_preview()
	_graphic_name_label = _add_muted_label(_detail_box, "")

	_color_lower = _add_color_option(_detail_box, "Ground level colour", "ColorLowerOption")
	_color_lower.item_selected.connect(_on_color_lower_selected)
	_color_upper = _add_color_option(_detail_box, "Top colour", "ColorUpperOption")
	_color_upper.item_selected.connect(_on_color_upper_selected)

	var toggles := HBoxContainer.new()
	toggles.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_detail_box.add_child(toggles)
	_shadow_toggle = CheckBox.new()
	_shadow_toggle.name = "ShadowToggle"
	_shadow_toggle.text = "Shadow"
	toggles.add_child(_shadow_toggle)
	_shadow_toggle.toggled.connect(_on_shadow_toggled)
	_force_on_toggle = CheckBox.new()
	_force_on_toggle.name = "ForceOnToggle"
	_force_on_toggle.text = "Force on"
	toggles.add_child(_force_on_toggle)
	_force_on_toggle.toggled.connect(_on_force_on_toggled)

	box.add_child(HSeparator.new())
	_attach_brush_controls(box)

	_populate_color_options(_color_lower)
	_populate_color_options(_color_upper)
	refresh()


func refresh() -> void:
	if not _ui_alive() or terrain_editor == null:
		return
	_syncing = true
	_sync_brush_values()
	var defs := terrain_editor.get_foliage_defs()
	_selected_index = clampi(terrain_editor.get_selected_foliage_def_index(), -1, defs.size() - 1)
	_refresh_list(defs)
	_refresh_detail(defs)
	_syncing = false


func _add_color_option(parent: Control, label_text: String, node_name: String) -> OptionButton:
	var label := Label.new()
	label.text = label_text
	parent.add_child(label)
	var option := OptionButton.new()
	option.name = node_name
	option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(option)
	return option


func _build_graphic_preview() -> void:
	_graphic_preview = VegPreview.new()
	_graphic_preview.anchor_right = 1.0
	_graphic_preview.anchor_bottom = 1.0
	_graphic_preview.offset_left = 4
	_graphic_preview.offset_top = 4
	_graphic_preview.offset_right = -4
	_graphic_preview.offset_bottom = -4
	_graphic_preview.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_graphic_button.add_child(_graphic_preview)


func _refresh_list(defs: Array) -> void:
	_count_label.text = "%d / %d" % [defs.size(), FOLIAGE_DEFS_LIMIT]
	_add_button.disabled = defs.size() >= FOLIAGE_DEFS_LIMIT
	var want_selected := clampi(_selected_index, -1, defs.size() - 1)
	_list.clear()
	for i in defs.size():
		var def = defs[i]
		var graphic := String(def.graphic) if def and def.graphic else "(unnamed)"
		_list.add_item("%d - %s" % [i + 1, graphic])
	if want_selected >= 0 and want_selected < defs.size():
		_list.select(want_selected)
		_selected_index = want_selected
	else:
		_selected_index = -1
	_remove_button.disabled = _selected_index < 0


func _refresh_detail(defs: Array) -> void:
	var has_selected := _selected_index >= 0 and _selected_index < defs.size()
	_detail_empty.visible = not has_selected
	_detail_box.visible = has_selected
	if not has_selected:
		return
	var def = defs[_selected_index]
	var graphic := String(def.graphic) if def and def.graphic else ""
	_graphic_preview.set_resource_root(_resource_root())
	_graphic_preview.set_graphic(graphic)
	_graphic_name_label.text = graphic if not graphic.is_empty() else "(none)"
	_match_color_option(_color_lower, int(def.color_lower))
	_match_color_option(_color_upper, int(def.color_upper))
	_shadow_toggle.set_pressed_no_signal(bool(def.shadow))
	_force_on_toggle.set_pressed_no_signal(bool(def.force_on))


func _populate_color_options(option: OptionButton) -> void:
	option.clear()
	option.add_item("Match ground")
	option.set_item_metadata(0, NovaTerrainFoliageDef.COLOR_MATCH_GROUND)
	option.add_item("Blend 50%")
	option.set_item_metadata(1, NovaTerrainFoliageDef.COLOR_BLEND_50)
	option.add_item("Retain full color")
	option.set_item_metadata(2, NovaTerrainFoliageDef.COLOR_RETAIN_FULL)


func _match_color_option(option: OptionButton, value: int) -> void:
	for i in option.item_count:
		if int(option.get_item_metadata(i)) == value:
			option.select(i)
			return
	option.select(0)


func _on_list_selected(idx: int) -> void:
	if _syncing or terrain_editor == null:
		return
	terrain_editor.set_selected_foliage_def_index(idx)
	refresh()


func _on_add_pressed() -> void:
	if terrain_editor == null:
		return
	terrain_editor.add_foliage_def()
	refresh()


func _on_remove_pressed() -> void:
	if terrain_editor == null or _selected_index < 0:
		return
	terrain_editor.remove_foliage_def(_selected_index)
	refresh()


func _on_graphic_button_pressed() -> void:
	if terrain_editor == null or _selected_index < 0:
		return
	_ensure_picker()
	_picker.refresh()
	_picker.open_picker()


func _ensure_picker() -> void:
	if _picker != null and is_instance_valid(_picker):
		_picker.set_resource_root(_resource_root())
		return
	_picker = VegPickerScene.instantiate()
	_root.add_child(_picker)
	_picker.set_resource_root(_resource_root())
	_picker.graphic_selected.connect(_on_picker_graphic_selected)


func _on_picker_graphic_selected(basename: String) -> void:
	_commit_graphic(basename)


func _commit_graphic(text: String) -> void:
	if terrain_editor == null or _selected_index < 0 or _syncing:
		return
	terrain_editor.set_foliage_def_field(_selected_index, "graphic", text)


func _resource_root() -> NovaResourceRoot:
	if terrain_editor != null and terrain_editor.has_method("get_resource_root"):
		return terrain_editor.get_resource_root()
	return null


func _on_color_lower_selected(idx: int) -> void:
	if terrain_editor == null or _selected_index < 0 or _syncing:
		return
	terrain_editor.set_foliage_def_field(_selected_index, "color_lower", int(_color_lower.get_item_metadata(idx)))


func _on_color_upper_selected(idx: int) -> void:
	if terrain_editor == null or _selected_index < 0 or _syncing:
		return
	terrain_editor.set_foliage_def_field(_selected_index, "color_upper", int(_color_upper.get_item_metadata(idx)))


func _on_shadow_toggled(pressed: bool) -> void:
	if terrain_editor == null or _selected_index < 0 or _syncing:
		return
	terrain_editor.set_foliage_def_field(_selected_index, "shadow", pressed)


func _on_force_on_toggled(pressed: bool) -> void:
	if terrain_editor == null or _selected_index < 0 or _syncing:
		return
	terrain_editor.set_foliage_def_field(_selected_index, "force_on", pressed)

