class_name MusicStmtIfBlock
extends PanelContainer

# An if/else statement as a container block in the block-stack canvas: a
# condition header, then an indented "then" lane (and an "else" lane when
# present) of MusicStmtRow leaves. Whatever follows the block in the stack IS
# the "after" path, so depth reads as indentation instead of graph sprawl.
#
# The block carries the if's top-level ordinal; lane rows share it (nested
# statements are not separately addressable -- every lane mutation regenerates
# the WHOLE if from its body texts and replaces it as one row) and carry a
# "branch_key" meta ("ifOrdinal:branch:index") so an open edit can re-resolve
# after a re-render.
#
# A flat if (no nested if/switch/branch_comment in a body) is the editable
# case; a non-flat body renders read-only. No stock script has a non-flat if
# (pinned by music_corpus_census_test).

const MusDisplayNames = preload("res://modtools/music/mus_display_names.gd")
const StmtRowClass = preload("res://modtools/music/ui/stmt_row.gd")

var ordinal := -1
var flat := false

var _header: Label = null
var _then_lane: VBoxContainer = null
var _else_lane: VBoxContainer = null
var _host = null


func setup(stmt: Dictionary, p_ordinal: int, ctx: Dictionary, opts: Dictionary = {}) -> MusicStmtIfBlock:
	ordinal = p_ordinal
	_host = ctx.get("view")
	flat = _flat_editable(stmt)
	set_meta("ordinal", ordinal)
	set_meta("kind", "if")
	set_meta("run", 1)
	add_theme_stylebox_override("panel", StmtRowClass._row_style(MusDisplayNames.stmt_color("if")))
	mouse_filter = Control.MOUSE_FILTER_STOP

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 4)
	add_child(box)

	var head_row := HBoxContainer.new()
	head_row.add_theme_constant_override("separation", 8)
	box.add_child(head_row)
	var glyph := Label.new()
	glyph.text = "◇"
	glyph.add_theme_color_override("font_color", MusDisplayNames.stmt_color("if"))
	glyph.tooltip_text = MusDisplayNames.stmt_tooltip("if")
	glyph.custom_minimum_size = Vector2(22, 0)
	head_row.add_child(glyph)
	_header = Label.new()
	_header.text = "If  %s" % MusDisplayNames.pretty_expr(
		_unwrap_outer_parens(String(stmt.get("expr", ""))),
		ctx.get("display_vars", []),
		int(ctx.get("locals_base", MusDisplayNames.DEFAULT_LOCALS_BASE)),
		ctx.get("input_names", {}))
	_header.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_header.tooltip_text = String(stmt.get("text", ""))
	head_row.add_child(_header)

	var read_only := bool(opts.get("read_only", false)) or not flat
	_then_lane = _build_lane(box, "then", Color(0.6, 0.9, 0.6), stmt, ctx, read_only)
	if bool(stmt.get("else_present", false)):
		_else_lane = _build_lane(box, "else", Color(0.9, 0.6, 0.6), stmt, ctx, read_only)

	gui_input.connect(func(event: InputEvent):
		if event is InputEventMouseButton and event.pressed \
				and (event as InputEventMouseButton).button_index == MOUSE_BUTTON_LEFT:
			if _host != null:
				_host.notify_selected(ordinal))
	return self


# One indented branch lane: a thin colour rule, a tiny branch label, then the
# body statements as ordinary rows (sharing this block's ordinal).
func _build_lane(into: Container, branch: String, color: Color, stmt: Dictionary, ctx: Dictionary, read_only: bool) -> VBoxContainer:
	var lane_row := HBoxContainer.new()
	lane_row.add_theme_constant_override("separation", 8)
	into.add_child(lane_row)

	var indent := Control.new()
	indent.custom_minimum_size = Vector2(14, 0)
	lane_row.add_child(indent)
	var rule := ColorRect.new()
	rule.color = Color(color, 0.55)
	rule.custom_minimum_size = Vector2(2, 0)
	lane_row.add_child(rule)

	var lane := VBoxContainer.new()
	lane.add_theme_constant_override("separation", 3)
	lane.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	lane_row.add_child(lane)

	var tag := Label.new()
	tag.text = branch
	tag.add_theme_color_override("font_color", color)
	tag.add_theme_font_size_override("font_size", 11)
	lane.add_child(tag)

	var body: Array = stmt.get(branch, [])
	var idx := 0
	for st in body:
		var st_dict: Dictionary = st
		var k := String(st_dict.get("kind", ""))
		if k == "frame_enter":
			# Engine plumbing stays hidden inside bodies too; bank the offset so
			# the live glow resolves on the next rendered row.
			if ctx.has("pend"):
				(ctx["pend"] as Callable).call(int(st_dict.get("code_offset", -1)))
			idx += 1
			continue
		var row: Control
		if k == "if" or k == "switch":
			# A nested block in a body (never in stock): render its header as a
			# read-only annotation row rather than recursing -- regeneration
			# rebuilds bodies from leaf TEXT, so nesting stays uneditable.
			row = StmtRowClass.new().setup(
				{"kind": "branch_comment", "text": String(st_dict.get("text", "")),
					"code_offset": int(st_dict.get("code_offset", -1))},
				ordinal, ctx, {"read_only": true, "branch_key": "%d:%s:%d" % [ordinal, branch, idx]})
		else:
			row = StmtRowClass.new().setup(st_dict, ordinal, ctx,
				{"read_only": read_only, "branch_key": "%d:%s:%d" % [ordinal, branch, idx]})
		lane.add_child(row)
		if ctx.has("register"):
			(ctx["register"] as Callable).call(row, int(st_dict.get("code_offset", -1)))
		idx += 1
	return lane


# Lane rows (MusicStmtRow leaves) for tests and the view.
func then_rows() -> Array:
	return _lane_rows(_then_lane)


func else_rows() -> Array:
	return _lane_rows(_else_lane)


func has_else() -> bool:
	return _else_lane != null


func _lane_rows(lane: VBoxContainer) -> Array:
	var out: Array = []
	if lane == null:
		return out
	for c in lane.get_children():
		if c is StmtRowClass:
			out.append(c)
	return out


# A flat if has only leaf statements in its bodies. Only flat ifs are editable
# in place, because regeneration rebuilds the body from each statement's
# rendered TEXT -- a nested block's text is just its header line.
static func _flat_editable(stmt: Dictionary) -> bool:
	for branch in ["then", "else"]:
		for st in stmt.get(branch, []):
			var k := String((st as Dictionary).get("kind", ""))
			if k == "if" or k == "switch" or k == "branch_comment":
				return false
	return true


# Strip one fully-enclosing matched paren pair for display (binops
# self-parenthesise, so conditions arrive as "((a != b))").
static func _unwrap_outer_parens(expr: String) -> String:
	var t := expr.strip_edges()
	if t.length() < 2 or t[0] != "(" or t[t.length() - 1] != ")":
		return t
	var depth := 0
	for idx in range(t.length()):
		var ch := t[idx]
		if ch == "(":
			depth += 1
		elif ch == ")":
			depth -= 1
			if depth == 0:
				if idx == t.length() - 1:
					return t.substr(1, t.length() - 2).strip_edges()
				return t
	return t
