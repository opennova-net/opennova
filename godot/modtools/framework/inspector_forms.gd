class_name InspectorForms
extends RefCounted

## Stateless UI builders shared by the workflow inspectors across workspaces
## (B7: the de-facto shared forms library, promoted out of object/ui/). Every
## function returns the control(s) it adds and never reads instance state, so
## callers own the data. The object-only ctrl-reg builders live in ObjectForms.


class IdOption:
	extends RefCounted

	var id: int
	var label: String

	func _init(p_id: int, p_label: String) -> void:
		id = p_id
		label = p_label


# Panel/detail container margin and inner card margin, in pixels.
const PANEL_MARGIN := 10
const CARD_MARGIN := 8

# Width (px) of the left-hand label column in row builders. Wide enough for the
# longest inspector labels (e.g. "Min combat range") so they no longer clip;
# clip_text + the tooltip stay as a safety net for anything longer.
const LABEL_COL_WIDTH := 120


static func add_spin_row(parent: Control, node_name: String, label_text: String, min_value: float, max_value: float, step: float) -> SpinBox:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.tooltip_text = label_text
	label.clip_text = true
	label.custom_minimum_size = Vector2(LABEL_COL_WIDTH, 0)
	row.add_child(label)
	var spin := SpinBox.new()
	if not node_name.is_empty():
		spin.name = node_name
	spin.min_value = min_value
	spin.max_value = max_value
	spin.step = step
	spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(spin)
	return spin


static func add_checkbox(parent: Control, node_name: String, text: String) -> CheckBox:
	var checkbox := CheckBox.new()
	if not node_name.is_empty():
		checkbox.name = node_name
	checkbox.text = text
	parent.add_child(checkbox)
	return checkbox


static func add_color_row(parent: Control, node_name: String, label_text: String) -> ColorPickerButton:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.tooltip_text = label_text
	label.clip_text = true
	label.custom_minimum_size = Vector2(LABEL_COL_WIDTH, 0)
	row.add_child(label)
	var picker := ColorPickerButton.new()
	if not node_name.is_empty():
		picker.name = node_name
	picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(picker)
	return picker


static func add_id_option_row(parent: Control, node_name: String, label_text: String, options: Array) -> OptionButton:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.tooltip_text = label_text
	label.clip_text = true
	label.custom_minimum_size = Vector2(LABEL_COL_WIDTH, 0)
	row.add_child(label)
	var option := OptionButton.new()
	if not node_name.is_empty():
		option.name = node_name
	option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(option)
	populate_id_option(option, options, 0)
	return option


static func add_detail_field(parent: Control, label_text: String) -> VBoxContainer:
	var row := VBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_theme_constant_override("separation", 3)
	parent.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.tooltip_text = label_text
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(label)
	return row


static func add_detail_spin_row(parent: Control, node_name: String, label_text: String, min_value: float, max_value: float, step: float) -> SpinBox:
	var row := add_detail_field(parent, label_text)
	var spin := SpinBox.new()
	spin.name = node_name
	spin.min_value = min_value
	spin.max_value = max_value
	spin.step = step
	spin.custom_minimum_size = Vector2(0, 32)
	spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(spin)
	return spin


static func add_detail_id_option_row(parent: Control, node_name: String, label_text: String, options: Array) -> OptionButton:
	var row := add_detail_field(parent, label_text)
	var option := OptionButton.new()
	option.name = node_name
	option.fit_to_longest_item = false
	option.custom_minimum_size = Vector2(0, 32)
	option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(option)
	populate_id_option(option, options, 0)
	return option


static func populate_id_option(option: OptionButton, options: Array, current_id: int) -> void:
	if option == null:
		return
	option.clear()
	var selected_index := 0
	var matched := false
	for item in options:
		var item_id := 0
		var item_label := ""
		if item is IdOption:
			var typed_item := item as IdOption
			item_id = typed_item.id
			item_label = typed_item.label
		elif item is Dictionary:
			item_id = int(item.get("id", 0))
			item_label = String(item.get("label", str(item_id)))
		else:
			continue
		option.add_item(item_label, item_id)
		var option_index := option.get_item_count() - 1
		if item_id == current_id:
			selected_index = option_index
			matched = true
	if not matched:
		option.add_item("Custom %d" % current_id, current_id)
		selected_index = option.get_item_count() - 1
	option.select(selected_index)


static func selected_option_id(option: OptionButton) -> int:
	if option == null or option.selected < 0 or option.selected >= option.get_item_count():
		return 0
	return option.get_item_id(option.selected)


static func set_spin(node, value: float) -> void:
	var spin := node as SpinBox
	if spin != null:
		spin.value = value


static func build_channel_card(parent: VBoxContainer, node_name: String) -> VBoxContainer:
	var card := PanelContainer.new()
	if not node_name.is_empty():
		card.name = node_name
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(card)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", CARD_MARGIN)
	margin.add_theme_constant_override("margin_top", CARD_MARGIN)
	margin.add_theme_constant_override("margin_right", CARD_MARGIN)
	margin.add_theme_constant_override("margin_bottom", CARD_MARGIN)
	card.add_child(margin)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.add_child(box)
	return box


static func make_inspector_box(mount: Control) -> VBoxContainer:
	return UiBox.make_inspector_box(mount)


static func add_section_heading(parent: Control, text: String) -> Label:
	var label := Label.new()
	label.theme_type_variation = &"Heading"
	label.text = text
	parent.add_child(label)
	return label


static func add_muted_label(parent: Control, text: String) -> Label:
	var label := Label.new()
	label.theme_type_variation = &"Muted"
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.text = text
	parent.add_child(label)
	return label


static func add_empty_state(parent: Control, text: String, node_name := "") -> Label:
	var label := add_muted_label(parent, text)
	if not node_name.is_empty():
		label.name = node_name
	return label


## Collapsible section: a flat toggle-header button + a content VBox that shows or
## hides with it. Returns the content VBox to add rows into. Used for Primary /
## Advanced grouping so dense inspectors stay scannable.
static func add_foldable_section(parent: Control, title: String, expanded := true) -> VBoxContainer:
	var header := Button.new()
	header.flat = true
	header.toggle_mode = true
	header.button_pressed = expanded
	header.alignment = HORIZONTAL_ALIGNMENT_LEFT
	header.focus_mode = Control.FOCUS_NONE
	header.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.text = ("  ▾  " if expanded else "  ▸  ") + title
	parent.add_child(header)

	var content := VBoxContainer.new()
	content.add_theme_constant_override("separation", 6)
	content.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	content.visible = expanded
	parent.add_child(content)

	header.toggled.connect(func(on: bool) -> void:
		content.visible = on
		header.text = ("  ▾  " if on else "  ▸  ") + title)
	return content
