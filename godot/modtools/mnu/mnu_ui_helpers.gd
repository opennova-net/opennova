class_name MnuUiHelpers
extends RefCounted

## Stateless UI builders for the Menus workspace, kept local so modtools/mnu/ stays
## self-contained (mirrors the object workspace's object_ui_helpers.gd). Every
## function returns the control it adds and never reads instance state. The M6
## inspector is read-only, so these build label/value rows; M7 swaps in editable
## rows.

const PANEL_MARGIN := 10


static func make_inspector_box(host: Control) -> VBoxContainer:
	var margin := MarginContainer.new()
	for side in ["margin_left", "margin_top", "margin_right", "margin_bottom"]:
		margin.add_theme_constant_override(side, PANEL_MARGIN)
	margin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.size_flags_vertical = Control.SIZE_EXPAND_FILL
	host.add_child(margin)

	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	margin.add_child(scroll)

	var box := VBoxContainer.new()
	box.name = "Box"
	box.add_theme_constant_override("separation", 8)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(box)
	return box


static func add_heading(parent: Control, text: String) -> Label:
	var label := Label.new()
	label.theme_type_variation = &"Heading"
	label.clip_text = true
	label.text = text
	parent.add_child(label)
	return label


static func add_muted(parent: Control, text: String) -> Label:
	var label := Label.new()
	label.theme_type_variation = &"Muted"
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.text = text
	parent.add_child(label)
	return label


# A read-only "key: value" row. The value clips and carries a tooltip so long
# paths/refs stay readable in the narrow inspector column.
static func add_kv_row(parent: Control, key: String, value: String) -> Label:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_theme_constant_override("separation", 6)
	parent.add_child(row)

	var key_label := Label.new()
	key_label.theme_type_variation = &"Muted"
	key_label.text = key
	key_label.clip_text = true
	key_label.custom_minimum_size = Vector2(104, 0)
	key_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	row.add_child(key_label)

	var value_label := Label.new()
	value_label.text = value if not value.is_empty() else "(none)"
	value_label.tooltip_text = value
	value_label.clip_text = true
	value_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	value_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	row.add_child(value_label)
	return value_label


# A "key: [swatch] raw" row for an MNU color. A literal hex value (RRGGBB) shows
# a filled swatch; a %VAR% stylesheet reference shows an outlined swatch (the
# resolved color is the stylesheet's job, not the inspector's), so the raw token
# always survives a round-trip.
static func add_color_row(parent: Control, key: String, raw: String) -> void:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_theme_constant_override("separation", 6)
	parent.add_child(row)

	var key_label := Label.new()
	key_label.theme_type_variation = &"Muted"
	key_label.text = key
	key_label.clip_text = true
	key_label.custom_minimum_size = Vector2(104, 0)
	key_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	row.add_child(key_label)

	var swatch := ColorRect.new()
	swatch.custom_minimum_size = Vector2(16, 16)
	var parsed = color_from_mnu(raw)
	if parsed != null:
		swatch.color = parsed
	else:
		# Unresolved (%VAR% or empty): show transparent so it reads as "not a literal".
		swatch.color = Color(0, 0, 0, 0)
	row.add_child(swatch)

	var value_label := Label.new()
	value_label.text = raw if not raw.is_empty() else "(none)"
	value_label.tooltip_text = raw
	value_label.clip_text = true
	value_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	value_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	row.add_child(value_label)


# Parse an MNU color token into a Color, or null when it is a %VAR% reference,
# empty, or not valid hex (so the caller can render it as "unresolved").
static func color_from_mnu(raw: String):
	var token := raw.strip_edges()
	if token.is_empty() or token.begins_with("%"):
		return null
	if not token.begins_with("#"):
		token = "#" + token
	if not Color.html_is_valid(token):
		return null
	return Color.html(token)
