class_name MusicStmtIfBlock
extends PanelContainer

# An if/else statement as a container block in the block-stack canvas: a
# condition header, then an indented "then" lane (and an "else" lane when
# present) of MusicStmtRow leaves. Whatever follows the block in the stack IS
# the "after" path, so depth reads as indentation instead of graph sprawl.
#
# EDITING: the condition is a chip that opens the expression popover; every
# lane row carries the same live controls as a top-level row; each lane has a
# ＋ add menu (simple kinds only -- bodies stay flat); the header's ⋮ adds or
# removes the else branch and moves/deletes the whole block. EVERY mutation
# regenerates the whole if from the CURRENT body texts via
# MusStmtText.if_block and replaces it as one row (one ordinal, one undo
# step) -- nested statements are not separately addressable in the document.
#
# Lane rows carry a "branch_key" meta ("ifOrdinal:branch:index") so an open
# popover can re-resolve after a re-render. A non-flat if (a nested
# if/switch/branch_comment in a body) renders read-only; no stock script has
# one (pinned by music_corpus_census_test).

const MusDisplayNames = preload("res://modtools/music/mus_display_names.gd")
const MusStmtText = preload("res://modtools/music/mus_stmt_text.gd")
const MusForms = preload("res://modtools/music/mus_forms.gd")
const StmtRowClass = preload("res://modtools/music/ui/stmt_row.gd")

var ordinal := -1
var flat := false

var _stmt: Dictionary = {}
var _ctx: Dictionary = {}
var _opts: Dictionary = {}
var _editable := false
var _header: Label = null
var _cond_chip: Button = null
var _then_lane: VBoxContainer = null
var _else_lane: VBoxContainer = null
var _mount = null
# Items shown in a lane's ＋ menu (ADD_ITEMS minus the block kinds).
var _lane_add_items: Array = []


func setup(stmt: Dictionary, p_ordinal: int, ctx: Dictionary, opts: Dictionary = {}) -> MusicStmtIfBlock:
	_stmt = stmt
	_ctx = ctx
	_opts = opts
	ordinal = p_ordinal
	_mount = ctx.get("view")
	flat = _flat_editable(stmt)
	_editable = bool(ctx.get("editable", false)) and not bool(opts.get("read_only", false)) and flat
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
	var word := Label.new()
	word.text = "If"
	head_row.add_child(word)
	var pretty_cond := MusDisplayNames.pretty_expr(
		_unwrap_outer_parens(String(stmt.get("expr", ""))),
		ctx.get("display_vars", []),
		int(ctx.get("locals_base", MusDisplayNames.DEFAULT_LOCALS_BASE)),
		ctx.get("input_names", {}))
	if _editable:
		_cond_chip = Button.new()
		_cond_chip.text = pretty_cond
		_cond_chip.tooltip_text = "%s\nClick to edit the condition." % String(stmt.get("expr", ""))
		_cond_chip.focus_mode = Control.FOCUS_NONE
		_cond_chip.clip_text = true
		_cond_chip.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_cond_chip.pressed.connect(_open_condition_popover)
		head_row.add_child(_cond_chip)
	else:
		_header = Label.new()
		_header.text = pretty_cond
		_header.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_header.tooltip_text = String(stmt.get("text", ""))
		head_row.add_child(_header)
	if _editable or (bool(ctx.get("editable", false)) and not bool(opts.get("read_only", false))):
		_build_block_tools(head_row)

	var lanes_read_only := not _editable
	_then_lane = _build_lane(box, "then", Color(0.6, 0.9, 0.6), stmt, ctx, lanes_read_only)
	if bool(stmt.get("else_present", false)):
		_else_lane = _build_lane(box, "else", Color(0.9, 0.6, 0.6), stmt, ctx, lanes_read_only)

	gui_input.connect(func(event: InputEvent):
		if event is InputEventMouseButton and event.pressed \
				and (event as InputEventMouseButton).button_index == MOUSE_BUTTON_LEFT:
			if _mount != null:
				_mount.notify_selected(ordinal))
	return self


# One indented branch lane: a thin colour rule, a tiny branch label, the body
# statements as ordinary rows (sharing this block's ordinal), and -- when
# editable -- a ＋ add menu that appends to THIS branch.
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
	var body_len := body.size()
	for st in body:
		var st_dict: Dictionary = st
		var k := String(st_dict.get("kind", ""))
		if k == "frame_enter":
			# Engine plumbing stays hidden inside bodies too; bank the offset so
			# the live glow resolves on the next rendered row.
			if _ctx.has("pend"):
				(_ctx["pend"] as Callable).call(int(st_dict.get("code_offset", -1)))
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
			row = StmtRowClass.new().setup(st_dict, ordinal, ctx, _lane_row_opts(branch, idx, body_len, read_only))
		lane.add_child(row)
		if ctx.has("register"):
			(ctx["register"] as Callable).call(row, int(st_dict.get("code_offset", -1)))
		idx += 1

	if not read_only:
		var add := MenuButton.new()
		add.text = "＋ add here"
		add.tooltip_text = "Add a step to this branch."
		add.flat = true
		add.focus_mode = Control.FOCUS_NONE
		var pop := add.get_popup()
		_lane_add_items = []
		for item in MusForms.ADD_ITEMS:
			var kk := String(item[1])
			if kk == "if" or kk == "switch":
				continue  # bodies stay flat (no nested blocks)
			_lane_add_items.append(item)
		for i in range(_lane_add_items.size()):
			pop.add_item(String(_lane_add_items[i][0]), i)
		pop.id_pressed.connect(_on_lane_add_id.bind(branch))
		lane.add_child(add)
	return lane


func _lane_row_opts(branch: String, idx: int, body_len: int, read_only: bool) -> Dictionary:
	return {
		"read_only": read_only,
		"branch_key": "%d:%s:%d" % [ordinal, branch, idx],
		"on_lines": _replace_in_branch.bind(branch, idx),
		"on_delete": _branch_mutate.bind(branch, idx, "delete", ""),
		"on_move": _move_in_branch.bind(branch, idx),
		"can_up": idx > 0,
		"can_down": idx < body_len - 1,
		"on_drop_branch": _drop_branch_before.bind(branch, idx),
	}


# A lane sibling dropped ON a row lands above it: reorder the branch's text
# list, then the usual whole-if regeneration.
func _drop_branch_before(from_key: String, branch: String, before_idx: int) -> void:
	var parts := from_key.rsplit(":", true, 1)
	if parts.size() != 2 or not parts[1].is_valid_int():
		return
	var from_idx := int(parts[1])
	if from_idx == before_idx:
		return
	var cur := _current_dict()
	var then_t := _branch_texts(cur, "then")
	var else_t := _branch_texts(cur, "else")
	var target: Array = then_t if branch == "then" else else_t
	if from_idx < 0 or from_idx >= target.size() or before_idx < 0 or before_idx > target.size():
		return
	var line: String = target[from_idx]
	target.remove_at(from_idx)
	target.insert(before_idx - 1 if from_idx < before_idx else before_idx, line)
	_regen(String(cur.get("expr", "")), then_t, bool(cur.get("else_present", false)), else_t)


func _replace_in_branch(lines: PackedStringArray, branch: String, idx: int) -> void:
	if not lines.is_empty():
		_branch_mutate(branch, idx, "replace", String(lines[0]))


func _move_in_branch(dir: int, branch: String, idx: int) -> void:
	_branch_mutate(branch, idx, "up" if dir < 0 else "down", "")


func _on_lane_add_id(id: int, branch: String) -> void:
	if id < 0 or id >= _lane_add_items.size():
		return
	var kk := String(_lane_add_items[id][1])
	var forms = _ctx.get("forms")
	var lines: PackedStringArray
	if forms.is_inputless(kk):
		lines = forms.simple_lines(kk)
	else:
		lines = forms.default_lines(kk)
	if not lines.is_empty():
		_branch_mutate(branch, -1, "append", String(lines[0]))


# Apply one mutation to a branch's flat text list, then regenerate + replace
# the whole if. idx<0 for append. Reads the CURRENT body texts from the live
# AST (via the view), so unedited statements survive verbatim even if this
# block instance is stale.
func _branch_mutate(branch: String, idx: int, op: String, new_line: String) -> void:
	var cur := _current_dict()
	var then_t := _branch_texts(cur, "then")
	var else_t := _branch_texts(cur, "else")
	var target: Array = then_t if branch == "then" else else_t
	match op:
		"delete":
			if idx >= 0 and idx < target.size():
				target.remove_at(idx)
		"replace":
			if idx >= 0 and idx < target.size():
				target[idx] = new_line
		"append":
			target.append(new_line)
		"up":
			if idx > 0 and idx < target.size():
				var t: String = target[idx]
				target[idx] = target[idx - 1]
				target[idx - 1] = t
		"down":
			if idx >= 0 and idx < target.size() - 1:
				var t: String = target[idx]
				target[idx] = target[idx + 1]
				target[idx + 1] = t
	_regen(String(cur.get("expr", "")), then_t, bool(cur.get("else_present", false)), else_t)


func _open_condition_popover() -> void:
	if not _ctx.has("open_expr"):
		return
	var seed = _stmt.get("expr_tree", {})
	if not (seed is Dictionary) or (seed as Dictionary).is_empty():
		seed = String(_stmt.get("expr", "0"))
	(_ctx["open_expr"] as Callable).call(self, "When is this true?", seed,
		{"ordinal": ordinal, "kind": "if", "slot": "cond"}, _on_condition_applied)


func _on_condition_applied(text: String) -> void:
	var cur := _current_dict()
	_regen(text, _branch_texts(cur, "then"), bool(cur.get("else_present", false)), _branch_texts(cur, "else"))


# Header ⋮ : whole-block move/delete + add/remove the else branch.
func _build_block_tools(into: Container) -> void:
	var del := Button.new()
	del.text = "✕"
	del.tooltip_text = "Delete this whole if block"
	del.flat = true
	del.focus_mode = Control.FOCUS_NONE
	del.pressed.connect(func():
		if _opts.has("on_delete"):
			(_opts["on_delete"] as Callable).call())
	into.add_child(del)
	var menu := MenuButton.new()
	menu.text = "⋮"
	menu.flat = true
	menu.focus_mode = Control.FOCUS_NONE
	var pop := menu.get_popup()
	pop.add_item("Move up", 0)
	pop.add_item("Move down", 1)
	pop.set_item_disabled(0, not bool(_opts.get("can_up", false)))
	pop.set_item_disabled(1, not bool(_opts.get("can_down", false)))
	if _editable:
		pop.add_separator()
		if bool(_stmt.get("else_present", false)):
			pop.add_item("Remove the otherwise (else) branch", 3)
		else:
			pop.add_item("Add an otherwise (else) branch", 2)
	if _opts.has("on_insert_above"):
		pop.add_separator()
		pop.add_item("Insert step above…", 4)
	pop.id_pressed.connect(_on_block_menu_id)
	into.add_child(menu)


func _on_block_menu_id(id: int) -> void:
	match id:
		0:
			if _opts.has("on_move"):
				(_opts["on_move"] as Callable).call(-1)
		1:
			if _opts.has("on_move"):
				(_opts["on_move"] as Callable).call(1)
		2:
			_add_else()
		3:
			_remove_else()
		4:
			if _opts.has("on_insert_above"):
				(_opts["on_insert_above"] as Callable).call()


# Adding the else seeds it with a default step: an EMPTY else block would not
# survive the decompiler's if detection, so it never exists on disk.
func _add_else() -> void:
	var cur := _current_dict()
	var forms = _ctx.get("forms")
	var default_line := "Echo(0)"
	var defaults: PackedStringArray = forms.default_lines("transition")
	if not defaults.is_empty():
		default_line = String(defaults[0])
	_regen(String(cur.get("expr", "")), _branch_texts(cur, "then"), true, [default_line])


func _remove_else() -> void:
	var cur := _current_dict()
	_regen(String(cur.get("expr", "")), _branch_texts(cur, "then"), false, [])


func _regen(cond: String, then_texts: Array, with_else: bool, else_texts: Array) -> void:
	var lines := MusStmtText.if_block(cond, PackedStringArray(then_texts), with_else, PackedStringArray(else_texts))
	if _opts.has("on_lines"):
		(_opts["on_lines"] as Callable).call(lines)


# The freshest AST dict for this ordinal: re-read through the view (the
# document may have changed since this block was built -- a popover apply
# must not resurrect stale bodies). Falls back to the build-time dict.
func _current_dict() -> Dictionary:
	if _ctx.has("stmt_at"):
		var cur: Dictionary = (_ctx["stmt_at"] as Callable).call(ordinal)
		if not cur.is_empty() and String(cur.get("kind", "")) == "if":
			return cur
	return _stmt


func _branch_texts(if_dict: Dictionary, branch: String) -> Array:
	var out := []
	for st in if_dict.get(branch, []):
		var k := String((st as Dictionary).get("kind", ""))
		if k == "frame_enter":
			continue  # hidden plumbing has no authored line
		out.append(String((st as Dictionary).get("text", "")))
	return out


# Lane rows (MusicStmtRow leaves) for tests and the view.
func then_rows() -> Array:
	return _lane_rows(_then_lane)


func else_rows() -> Array:
	return _lane_rows(_else_lane)


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
# self-parenthesise, so conditions read as "(a != b)" raw).
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
