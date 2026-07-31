class_name MusicInputsCard
extends PanelContainer

# The "Inputs from caller" header card at the top of a state's program: shown
# when the state's frame setup banks caller-pushed values BEFORE its first
# statement (a callable state). One row per input slot, in plain language --
# the hidden engine mechanics (the 0x38 frame op, l_N byte offsets) live in
# tooltips. Engine-dispatch payloads (a frame op in a section's tail, like
# gamemus Begin's) are explained by the dispatch divider instead, not here.
#
# Editable mode renders each name as a LineEdit: committing one reports
# through on_rename (the mount persists it in the .music_profile.json sidecar;
# display-only -- the script keeps its l_N tokens).

const MusDisplayNames = preload("res://modtools/music/mus_display_names.gd")

var _rows: VBoxContainer = null
var _on_rename: Callable = Callable()


# count: how many values the caller hands this state. names: {0-based index ->
# label} from the profile sidecar. on_rename: Callable(index, label).
func setup(count: int, locals_base: int, names: Dictionary = {}, editable: bool = false, on_rename: Callable = Callable()) -> MusicInputsCard:
	_on_rename = on_rename if editable else Callable()
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.12, 0.15, 0.20)
	sb.border_color = Color(0.45, 0.60, 0.85)
	sb.set_border_width_all(1)
	sb.content_margin_left = 10.0
	sb.content_margin_right = 10.0
	sb.content_margin_top = 6.0
	sb.content_margin_bottom = 6.0
	sb.corner_radius_top_left = 3
	sb.corner_radius_top_right = 3
	sb.corner_radius_bottom_left = 3
	sb.corner_radius_bottom_right = 3
	add_theme_stylebox_override("panel", sb)
	set_meta("inputs_card", true)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 3)
	add_child(box)

	var title := Label.new()
	title.text = "⚙ Inputs from caller"
	title.tooltip_text = "Whoever runs this state hands it these values (the engine banks\nthem before the first step; slot l_%d holds the first)." % locals_base
	title.add_theme_color_override("font_color", Color(0.65, 0.78, 1.0))
	box.add_child(title)

	_rows = VBoxContainer.new()
	_rows.add_theme_constant_override("separation", 2)
	box.add_child(_rows)
	for k in range(count):
		_rows.add_child(_input_row(k, locals_base, names))
	return self


func _input_row(k: int, locals_base: int, names: Dictionary) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	row.set_meta("input_index", k)
	var num := Label.new()
	num.text = "%d." % (k + 1)
	num.add_theme_color_override("font_color", Color(0.6, 0.65, 0.72))
	row.add_child(num)
	var default_label := MusDisplayNames.input_label_for_offset(locals_base + 4 * k, locals_base)
	var tip := "Engine slot %s" % MusDisplayNames.input_token(k, locals_base)
	if _on_rename.is_valid():
		var initial := String(names.get(k, ""))
		var edit := LineEdit.new()
		edit.text = initial
		edit.placeholder_text = default_label
		edit.tooltip_text = tip + "\nName what the caller hands here (display-only)."
		edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		edit.text_submitted.connect(func(t: String):
			if t != initial:
				_on_rename.call(k, t))
		edit.focus_exited.connect(func():
			if edit.text != initial:
				_on_rename.call(k, edit.text))
		row.add_child(edit)
	else:
		var name := Label.new()
		name.text = String(names.get(k, default_label))
		name.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		name.tooltip_text = tip
		row.add_child(name)
	return row


func input_rows() -> Array:
	var out: Array = []
	if _rows == null:
		return out
	for c in _rows.get_children():
		if c.has_meta("input_index"):
			out.append(c)
	return out
