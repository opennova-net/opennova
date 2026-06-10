class_name MusicStmtSwitchBlock
extends PanelContainer

# A tablexec ("on (value) ...") statement as a container block: a "Choose by"
# header, one case row per table entry ("0 → Missionwin"), and a faint
# "otherwise" note for the out-of-range fall-through (the engine continues
# with whatever follows the block). This is where "on (l_32) enter A B C"
# reads as an indexed dispatch instead of a mnemonic.
#
# Case targets are sections (action go-to / jump-to) or tracks (action play);
# the whole table re-serializes as ONE canonical `on (...)` line on any edit,
# so the block carries a single top-level ordinal like every other row.

const MusDisplayNames = preload("res://modtools/music/mus_display_names.gd")
const StmtRowClass = preload("res://modtools/music/ui/stmt_row.gd")
const IfBlockClass = preload("res://modtools/music/ui/stmt_if_block.gd")

var ordinal := -1
var action := "enter"

var _cases: VBoxContainer = null
var _host = null


func setup(stmt: Dictionary, p_ordinal: int, ctx: Dictionary, _opts: Dictionary = {}) -> MusicStmtSwitchBlock:
	ordinal = p_ordinal
	_host = ctx.get("view")
	action = String(stmt.get("action", "enter"))
	set_meta("ordinal", ordinal)
	set_meta("kind", "switch")
	set_meta("run", 1)
	add_theme_stylebox_override("panel", StmtRowClass._row_style(MusDisplayNames.stmt_color("switch")))
	mouse_filter = Control.MOUSE_FILTER_STOP

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 4)
	add_child(box)

	var head_row := HBoxContainer.new()
	head_row.add_theme_constant_override("separation", 8)
	box.add_child(head_row)
	var glyph := Label.new()
	glyph.text = "⋔"
	glyph.add_theme_color_override("font_color", MusDisplayNames.stmt_color("switch"))
	glyph.tooltip_text = MusDisplayNames.stmt_tooltip("switch")
	glyph.custom_minimum_size = Vector2(22, 0)
	head_row.add_child(glyph)
	var head := Label.new()
	head.text = "Choose by  %s → %s" % [
		MusDisplayNames.pretty_expr(
			IfBlockClass._unwrap_outer_parens(String(stmt.get("expr", ""))),
			ctx.get("display_vars", []),
			int(ctx.get("locals_base", MusDisplayNames.DEFAULT_LOCALS_BASE)),
			ctx.get("input_names", {})),
		_action_word()]
	head.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.tooltip_text = "The value picks a case: 0 picks the first, 1 the second, ...\n%s" \
		% String(stmt.get("text", ""))
	head_row.add_child(head)

	var lane_row := HBoxContainer.new()
	lane_row.add_theme_constant_override("separation", 8)
	box.add_child(lane_row)
	var indent := Control.new()
	indent.custom_minimum_size = Vector2(14, 0)
	lane_row.add_child(indent)
	var rule := ColorRect.new()
	rule.color = Color(MusDisplayNames.stmt_color("switch"), 0.55)
	rule.custom_minimum_size = Vector2(2, 0)
	lane_row.add_child(rule)
	_cases = VBoxContainer.new()
	_cases.add_theme_constant_override("separation", 2)
	_cases.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	lane_row.add_child(_cases)

	var targets: Array = stmt.get("targets", [])
	for t in range(targets.size()):
		_cases.add_child(_case_row(t, targets[t], ctx))

	var fallthrough := Label.new()
	fallthrough.text = "otherwise → continue below"
	fallthrough.tooltip_text = "A value past the last case falls through to the next step."
	fallthrough.add_theme_color_override("font_color", Color(0.6, 0.6, 0.65))
	fallthrough.add_theme_font_size_override("font_size", 11)
	_cases.add_child(fallthrough)

	gui_input.connect(func(event: InputEvent):
		if event is InputEventMouseButton and event.pressed \
				and (event as InputEventMouseButton).button_index == MOUSE_BUTTON_LEFT:
			if _host != null:
				_host.notify_selected(ordinal))
	return self


func _case_row(idx: int, target: Dictionary, ctx: Dictionary) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	row.set_meta("case_index", idx)
	var num := Label.new()
	num.text = "%d →" % idx
	num.add_theme_color_override("font_color", Color(0.7, 0.75, 0.8))
	row.add_child(num)
	var what := Label.new()
	if action == "play":
		what.text = StmtRowClass.track_label(int(target.get("track", -1)), ctx.get("bank_names", []))
	else:
		var tname := String(target.get("name", ""))
		what.text = tname if tname != "" else "(unresolved)"
	what.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(what)
	if action != "play" and String(target.get("name", "")) != "":
		var tname2 := String(target.get("name", ""))
		var open := Button.new()
		open.text = "open ▸"
		open.tooltip_text = "Open %s" % tname2
		open.focus_mode = Control.FOCUS_NONE
		open.flat = true
		open.pressed.connect(func():
			if _host != null:
				_host.notify_open(StringName(tname2)))
		row.add_child(open)
	return row


# Case rows (excluding the fall-through note), ordered, for tests and editing.
func case_rows() -> Array:
	var out: Array = []
	if _cases == null:
		return out
	for c in _cases.get_children():
		if c.has_meta("case_index"):
			out.append(c)
	return out


func _action_word() -> String:
	match action:
		"play":
			return "play"
		"goto":
			return "jump to"
		_:
			return "go to"
