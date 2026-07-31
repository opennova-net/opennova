class_name MusicStmtSwitchBlock
extends PanelContainer

# A tablexec ("on (value) ...") statement as a container block: a "Choose by"
# header, one case row per table entry ("0 → Missionwin"), and a faint
# "otherwise" note for the out-of-range fall-through (the engine continues
# with whatever follows the block). This is where "on (l_32) enter A B C"
# reads as an indexed dispatch instead of a mnemonic.
#
# EDITING ON THE BLOCK (the old 64-row dialog is gone): the selector is a
# popover chip, the action is a dropdown, every case row retargets with its
# own picker and removes with ✕, and ＋ add case appends (capped at 64 -- the
# engine's table count is one byte). Every mutation re-serializes the WHOLE
# table as one canonical `on (...)` line via MusStmtText.switch_stmt and
# replaces this block's single ordinal -- one intent, one undo step.
#
# Case targets are sections (action go-to / jump-to) or tracks (action play).
# Switching to/from "play it" resets the targets to one default (the two
# target spaces don't map onto each other); enter<->goto keeps them.

const MusDisplayNames = preload("res://modtools/music/mus_display_names.gd")
const MusStmtText = preload("res://modtools/music/mus_stmt_text.gd")
const StmtRowClass = preload("res://modtools/music/ui/stmt_row.gd")
const IfBlockClass = preload("res://modtools/music/ui/stmt_if_block.gd")

const MAX_TARGETS := 64

var ordinal := -1
var action := "enter"

var _stmt: Dictionary = {}
var _ctx: Dictionary = {}
var _opts: Dictionary = {}
var _editable := false
var _cases: VBoxContainer = null
var _selector_chip: Button = null
var _action_opt: OptionButton = null
var _add_case: Button = null
var _snames_snapshot: PackedStringArray = PackedStringArray()
var _mount = null


func setup(stmt: Dictionary, p_ordinal: int, ctx: Dictionary, opts: Dictionary = {}) -> MusicStmtSwitchBlock:
	_stmt = stmt
	_ctx = ctx
	_opts = opts
	ordinal = p_ordinal
	_mount = ctx.get("view")
	action = String(stmt.get("action", "enter"))
	_editable = bool(ctx.get("editable", false)) and not bool(opts.get("read_only", false))
	set_meta("ordinal", ordinal)
	set_meta("kind", "switch")
	set_meta("run", 1)
	add_theme_stylebox_override("panel", StmtRowClass._row_style(MusDisplayNames.stmt_color("switch")))
	mouse_filter = Control.MOUSE_FILTER_STOP
	if ctx.has("forms"):
		_snames_snapshot = PackedStringArray(ctx.get("forms").section_names())

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
	var pretty_sel := MusDisplayNames.pretty_expr(
		IfBlockClass._unwrap_outer_parens(String(stmt.get("expr", ""))),
		ctx.get("display_vars", []),
		int(ctx.get("locals_base", MusDisplayNames.DEFAULT_LOCALS_BASE)),
		ctx.get("input_names", {}))
	if _editable:
		var word := Label.new()
		word.text = "Choose by"
		head_row.add_child(word)
		_selector_chip = Button.new()
		_selector_chip.text = pretty_sel
		_selector_chip.tooltip_text = "%s\nClick to edit the selector." % String(stmt.get("expr", ""))
		_selector_chip.focus_mode = Control.FOCUS_NONE
		_selector_chip.clip_text = true
		_selector_chip.pressed.connect(_open_selector_popover)
		head_row.add_child(_selector_chip)
		var arrow := Label.new()
		arrow.text = "→"
		head_row.add_child(arrow)
		_action_opt = OptionButton.new()
		_action_opt.add_item("go to it", 0)
		_action_opt.add_item("jump to it", 1)
		_action_opt.add_item("play it", 2)
		_action_opt.select(2 if action == "play" else (1 if action == "goto" else 0))
		_action_opt.focus_mode = Control.FOCUS_NONE
		_action_opt.get_popup().about_to_popup.connect(func():
			if _ctx.has("notify_edit_started"):
				(_ctx["notify_edit_started"] as Callable).call())
		_action_opt.item_selected.connect(_on_action_picked)
		head_row.add_child(_action_opt)
		var sp := Control.new()
		sp.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		head_row.add_child(sp)
		_build_block_tools(head_row)
	else:
		var head := Label.new()
		head.text = "Choose by  %s → %s" % [pretty_sel, _action_word()]
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
		_cases.add_child(_case_row(t, targets[t], targets.size()))

	var fallthrough := Label.new()
	fallthrough.text = "otherwise → continue below"
	fallthrough.tooltip_text = "A value past the last case falls through to the next step."
	fallthrough.add_theme_color_override("font_color", Color(0.6, 0.6, 0.65))
	fallthrough.add_theme_font_size_override("font_size", 11)
	_cases.add_child(fallthrough)

	if _editable:
		_add_case = Button.new()
		_add_case.text = "＋ add case"
		_add_case.flat = true
		_add_case.focus_mode = Control.FOCUS_NONE
		_add_case.disabled = targets.size() >= MAX_TARGETS
		_add_case.tooltip_text = "Add the next case." if targets.size() < MAX_TARGETS \
			else "The engine's dispatch table holds at most %d cases." % MAX_TARGETS
		_add_case.pressed.connect(_on_add_case)
		_cases.add_child(_add_case)

	gui_input.connect(func(event: InputEvent):
		if event is InputEventMouseButton and event.pressed \
				and (event as InputEventMouseButton).button_index == MOUSE_BUTTON_LEFT:
			if _mount != null:
				_mount.notify_selected(ordinal))
	return self


func _case_row(idx: int, target: Dictionary, case_count: int) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	row.set_meta("case_index", idx)
	var num := Label.new()
	num.text = "%d →" % idx
	num.add_theme_color_override("font_color", Color(0.7, 0.75, 0.8))
	row.add_child(num)
	var tname := String(target.get("name", ""))
	if _editable:
		var forms = _ctx.get("forms")
		var picker: OptionButton
		if action == "play":
			picker = forms.make_track_option(int(target.get("track", -1)))
		else:
			picker = forms.make_section_option(tname)
		picker.focus_mode = Control.FOCUS_NONE
		picker.get_popup().about_to_popup.connect(func():
			if _ctx.has("notify_edit_started"):
				(_ctx["notify_edit_started"] as Callable).call())
		picker.item_selected.connect(_on_case_retarget.bind(idx))
		row.add_child(picker)
		var sp := Control.new()
		sp.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(sp)
		if action != "play" and tname != "":
			row.add_child(_open_button(tname))
		var rm := Button.new()
		rm.text = "✕"
		rm.tooltip_text = "Remove this case (later cases shift down)" if case_count > 1 \
			else "A dispatch needs at least one case"
		rm.flat = true
		rm.focus_mode = Control.FOCUS_NONE
		rm.disabled = case_count <= 1
		rm.pressed.connect(_on_case_remove.bind(idx))
		row.add_child(rm)
	else:
		var what := Label.new()
		if action == "play":
			what.text = StmtRowClass.track_label(int(target.get("track", -1)), _ctx.get("bank_names", []))
		else:
			what.text = tname if tname != "" else "(unresolved)"
		what.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(what)
		if action != "play" and tname != "":
			row.add_child(_open_button(tname))
	return row


# --- mutations (always: rebuild the one canonical on-line, replace) ---------

# Current target TOKENS (section names / sound_N) from the freshest AST dict.
func _current_tokens(cur: Dictionary) -> PackedStringArray:
	var toks := PackedStringArray()
	var act := String(cur.get("action", "enter"))
	for t in cur.get("targets", []):
		if act == "play":
			toks.append("sound_%d" % int((t as Dictionary).get("track", 0)))
		else:
			toks.append(String((t as Dictionary).get("name", "")))
	return toks


func _selector_text(cur: Dictionary) -> String:
	# The AST's expr IS the tree-render form (no on-parens); switch_stmt
	# re-wraps it.
	return String(cur.get("expr", "Var00"))


func _emit_table(selector: String, act: String, toks: PackedStringArray) -> void:
	if toks.is_empty():
		return
	if _opts.has("on_lines"):
		(_opts["on_lines"] as Callable).call(MusStmtText.switch_stmt(selector, act, toks))


func _on_case_retarget(picked: int, idx: int) -> void:
	var cur := _current_dict()
	var toks := _current_tokens(cur)
	if idx < 0 or idx >= toks.size():
		return
	if action == "play":
		toks[idx] = "sound_%d" % picked
	else:
		if picked < 0 or picked >= _snames_snapshot.size():
			return
		toks[idx] = _snames_snapshot[picked]
	_emit_table(_selector_text(cur), action, toks)


func _on_case_remove(idx: int) -> void:
	var cur := _current_dict()
	var toks := _current_tokens(cur)
	if idx < 0 or idx >= toks.size() or toks.size() <= 1:
		return
	toks.remove_at(idx)
	_emit_table(_selector_text(cur), action, toks)


func _on_add_case() -> void:
	var cur := _current_dict()
	var toks := _current_tokens(cur)
	if toks.size() >= MAX_TARGETS:
		return
	if action == "play":
		toks.append("sound_0")
	else:
		if _snames_snapshot.is_empty():
			return
		toks.append(_snames_snapshot[0])
	_emit_table(_selector_text(cur), action, toks)


func _on_action_picked(idx: int) -> void:
	var new_act := "enter"
	if _action_opt.get_item_id(idx) == 1:
		new_act = "goto"
	elif _action_opt.get_item_id(idx) == 2:
		new_act = "play"
	if new_act == action:
		return
	var cur := _current_dict()
	var toks: PackedStringArray
	if (new_act == "play") != (action == "play"):
		# Sections and tracks don't map onto each other: reset to one default.
		if new_act == "play":
			toks = PackedStringArray(["sound_0"])
		else:
			if _snames_snapshot.is_empty():
				return
			toks = PackedStringArray([_snames_snapshot[0]])
	else:
		toks = _current_tokens(cur)
	_emit_table(_selector_text(cur), new_act, toks)


func _open_selector_popover() -> void:
	if not _ctx.has("open_expr"):
		return
	var seed = _stmt.get("expr_tree", {})
	if not (seed is Dictionary) or (seed as Dictionary).is_empty():
		seed = String(_stmt.get("expr", "Var00"))
	(_ctx["open_expr"] as Callable).call(self, "Choose by which value?", seed,
		{"ordinal": ordinal, "kind": "switch", "slot": "selector"}, _on_selector_applied)


func _on_selector_applied(text: String) -> void:
	var cur := _current_dict()
	_emit_table(text, String(cur.get("action", action)), _current_tokens(cur))


func _build_block_tools(into: Container) -> void:
	var del := Button.new()
	del.text = "✕"
	del.tooltip_text = "Delete this whole dispatch"
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
	if _opts.has("on_insert_above"):
		pop.add_separator()
		pop.add_item("Insert step above…", 2)
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
			if _opts.has("on_insert_above"):
				(_opts["on_insert_above"] as Callable).call()


# The freshest AST dict for this ordinal (the document may have changed since
# this block was built); falls back to the build-time dict.
func _current_dict() -> Dictionary:
	if _ctx.has("stmt_at"):
		var cur: Dictionary = (_ctx["stmt_at"] as Callable).call(ordinal)
		if not cur.is_empty() and String(cur.get("kind", "")) == "switch":
			return cur
	return _stmt


func _open_button(target: String) -> Button:
	var open := Button.new()
	open.text = "open ▸"
	open.tooltip_text = "Open %s" % target
	open.focus_mode = Control.FOCUS_NONE
	open.flat = true
	open.pressed.connect(func():
		if _mount != null:
			_mount.notify_open(StringName(target)))
	return open


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
