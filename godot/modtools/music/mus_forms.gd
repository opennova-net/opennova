extends RefCounted

# Shared MUS authoring forms: the "＋ Add" construct palette and the per-kind
# dialog builders used by the logic-graph blueprint (the sole authoring surface).
# Every producer routes through MusStmtText / MusExpr / the structured ExprBuilder
# so the emitted .mus lines are canonical. The graph instances one of these,
# configure()s it with the section/var/bank context, then open_form(); on OK the
# dialog hands the produced lines to the on_lines callback, which the host routes to
# the document's parity-gated add/replace write path. The forms NEVER hand-assemble
# bytecode -- they only emit text the compile gate already accepts.
#
# No class_name (preload as a const) to avoid GDScript class-registration ordering
# surprises, matching mus_stmt_text.gd / mus_expr.gd.

const MusStmtText = preload("res://modtools/music/mus_stmt_text.gd")
const ExprBuilderClass = preload("res://modtools/music/ui/expr_builder.gd")

# The "＋ Add" palette: [label, kind]. Kept verbatim from the inspector so both
# surfaces offer the identical menu. nop is intentionally absent -- the decompiler
# renders nop as nothing, so an authored nop has no text line and would vanish on
# the next re-decompile (it couldn't then be deleted/reordered).
const ADD_ITEMS := [
	["→  enter a state", "transition"],
	["↪  goto a label", "goto"],
	["ƒ  call a state", "call"],
	["⋔  on (selector) …", "switch"],
	["◇  if / else", "if"],
	["✎  set a variable", "assign"],
	["±  increment / decrement", "incdec"],
	["ƒ  call a method", "expr"],
	["⏎  return", "return"],
	["⏸  yield", "yield"],
]

var _section_names: PackedStringArray = PackedStringArray()
var _var_list: Array = []        # [{token:String, label:String}]
var _mus = null                  # NovaMusicScript for expr validation
var _bank_names: Array = []


# Supply the context every form needs: the section-name list (transition/switch
# targets), the variable picker list, the NovaMusicScript used to validate
# expressions, and the bank track names (for play / switch-play pickers). Safe to
# call repeatedly; the host refreshes it before each open_form so a live edit sees
# the current section/var/bank state.
func configure(section_names: PackedStringArray, var_list: Array, mus, bank_names: Array) -> void:
	_section_names = section_names
	_var_list = var_list
	_mus = mus
	_bank_names = bank_names


# True for the kinds that take no inputs (return/yield): the host inserts their
# canonical lines directly, no dialog.
func is_inputless(kind: String) -> bool:
	return kind == "return" or kind == "yield"


# Canonical lines for the inputless kinds; empty for anything that needs a form.
func simple_lines(kind: String) -> PackedStringArray:
	match kind:
		"return":
			return MusStmtText.ret()
		"yield":
			return MusStmtText.yield_stmt()
	return PackedStringArray()


# Build + show a ConfirmationDialog parented under `host`. On confirm it generates
# the canonical lines from the form and hands them to on_lines.call(lines) -- an
# EMPTY array means the user left a required field blank (the host surfaces that
# instead of pretending the edit applied). The dialog frees itself on close.
# is_edit only changes the title ("Edit" vs "Add"); the add-vs-replace routing is
# the host's decision (it owns the section index / ordinal).
func open_form(host: Node, kind: String, prefill: Dictionary, is_edit: bool, on_lines: Callable) -> void:
	var dlg := ConfirmationDialog.new()
	dlg.title = ("Edit " if is_edit else "Add ") + kind
	dlg.min_size = Vector2i(380, 140)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	dlg.add_child(box)
	# state captured per-kind, read in the confirmed handler via a producer Callable
	var producer: Callable = Callable()

	match kind:
		"transition", "goto", "call":
			producer = _build_transition_form(box, kind, prefill)
		"play":
			producer = _build_play_form(box, prefill)
		"assign":
			producer = _build_assign_form(box, prefill)
		"incdec":
			producer = _build_incdec_form(box, prefill)
		"expr":
			producer = _build_expr_form(box, prefill)
		"if":
			producer = _build_if_form(box, prefill)
		"switch":
			producer = _build_switch_form(box, prefill)
		_:
			producer = func() -> PackedStringArray: return PackedStringArray()

	host.add_child(dlg)
	dlg.confirmed.connect(func():
		on_lines.call(producer.call())
		dlg.queue_free())
	dlg.canceled.connect(func(): dlg.queue_free())
	dlg.close_requested.connect(func(): dlg.queue_free())
	dlg.popup_centered()


# Play track-swap form: a single track picker seeded from the row's current track.
func _build_play_form(box: VBoxContainer, prefill: Dictionary) -> Callable:
	box.add_child(_label("Play which track?"))
	var tob := OptionButton.new()
	for i in range(_bank_names.size()):
		tob.add_item("%d: %s" % [i, _track_name(i)])
	if _bank_names.is_empty():
		# No bank loaded: still let the user pick a slot index by number.
		for i in range(8):
			tob.add_item("sound_%d" % i)
	var cur := int(prefill.get("track", -1))
	if cur >= 0 and cur < tob.item_count:
		tob.select(cur)
	box.add_child(tob)
	return func() -> PackedStringArray:
		if tob.selected < 0:
			return PackedStringArray()
		return MusStmtText.play(tob.selected)


# --- per-construct form builders (return a producer -> PackedStringArray) ---

func _section_option(selected_name: String) -> OptionButton:
	var ob := OptionButton.new()
	for i in range(_section_names.size()):
		ob.add_item(_section_names[i])
		if _section_names[i] == selected_name:
			ob.select(ob.item_count - 1)
	return ob


func _var_option(selected_token: String, selected_offset: int = -1) -> OptionButton:
	var ob := OptionButton.new()
	var picked := -1
	for i in range(_var_list.size()):
		ob.add_item(String(_var_list[i].get("label", _var_list[i].get("token", "Var00"))))
		if String(_var_list[i].get("token", "")) == selected_token:
			picked = i
	if _var_list.is_empty():
		ob.add_item("Var00")
	# Fall back to byte-offset matching when the resolved name didn't match a token:
	# a script with editor-named globals reports its custom name (e.g. "Intensity"),
	# not "VarNN", so a name-only match would silently default to Var00 on edit.
	# 64 is the last 4-byte-aligned global offset (Var16, MUS_GLOBALS_BYTES 68).
	if picked < 0 and selected_offset >= 0 and selected_offset <= 64:
		var idx := selected_offset / 4
		if idx < _var_list.size():
			picked = idx
	if picked >= 0:
		ob.select(picked)
	return ob


# Resolve the picked variable's token against the list the form OPENED with
# (passed in, not read from the instance) so a background configure() can't shift
# the index->token mapping out from under an open dialog.
func _var_token_at(ob: OptionButton, var_list: Array) -> String:
	var idx := ob.selected
	if idx >= 0 and idx < var_list.size():
		return String(var_list[idx].get("token", "Var00"))
	return "Var00"


func _build_transition_form(box: VBoxContainer, kind: String, prefill: Dictionary) -> Callable:
	var lbl := Label.new()
	lbl.text = "Go to which state?"
	box.add_child(lbl)
	var target := String(prefill.get("target_name", ""))
	var ob := _section_option(target)
	box.add_child(ob)
	# Snapshot the section list at open time: the picker items were built from it,
	# so the producer must resolve the picked index against the SAME list even if a
	# background configure() (a document.changed mid-dialog) rebinds _section_names.
	# Mirrors the host capturing the section index before open_form.
	var snames := _section_names
	return func() -> PackedStringArray:
		if ob.selected < 0 or ob.selected >= snames.size():
			return PackedStringArray()
		var name := snames[ob.selected]
		match kind:
			"goto":
				return MusStmtText.goto_section(name)
			"call":
				return MusStmtText.call_section(name)
			_:
				return MusStmtText.enter(name)


func _build_assign_form(box: VBoxContainer, prefill: Dictionary) -> Callable:
	var row := HBoxContainer.new()
	row.add_child(Label.new())
	row.get_child(0).text = "Set"
	var vob := _var_option(String(prefill.get("var_name", "")), int(prefill.get("var_offset", -1)))
	row.add_child(vob)
	var eq := Label.new()
	eq.text = "="
	row.add_child(eq)
	box.add_child(row)
	var eb := ExprBuilderClass.new()
	eb.setup(_var_list, _mus)
	box.add_child(eb)
	if prefill.has("rhs"):
		eb.set_expression_text(String(prefill.get("rhs", "")))
	var vlist := _var_list
	return func() -> PackedStringArray:
		return MusStmtText.assign(_var_token_at(vob, vlist), eb.get_expression_text())


func _build_incdec_form(box: VBoxContainer, prefill: Dictionary) -> Callable:
	var row := HBoxContainer.new()
	var vob := _var_option(String(prefill.get("var_name", "")), int(prefill.get("var_offset", -1)))
	row.add_child(vob)
	var dir := OptionButton.new()
	dir.add_item("++  (increment)", 1)
	dir.add_item("--  (decrement)", 0)
	dir.select(0 if bool(prefill.get("is_inc", true)) else 1)
	row.add_child(dir)
	box.add_child(row)
	var vlist := _var_list
	return func() -> PackedStringArray:
		return MusStmtText.incdec(_var_token_at(vob, vlist), dir.get_selected_id() == 1)


func _build_expr_form(box: VBoxContainer, prefill: Dictionary) -> Callable:
	var lbl := Label.new()
	lbl.text = "Run an expression (e.g. a function call):"
	box.add_child(lbl)
	var eb := ExprBuilderClass.new()
	eb.setup(_var_list, _mus)
	box.add_child(eb)
	if prefill.has("expr"):
		eb.set_expression_text(String(prefill.get("expr", "")))
	return func() -> PackedStringArray:
		return MusStmtText.expr_stmt(eb.get_expression_text())


# if / else: condition + a single then-action and optional else-action (enter a
# state or play a track). Richer multi-statement branches are a follow-up; this
# covers the dominant conditional-transition / conditional-play case.
func _build_if_form(box: VBoxContainer, prefill: Dictionary) -> Callable:
	box.add_child(_label("When this is true:"))
	var cond := ExprBuilderClass.new()
	cond.setup(_var_list, _mus)
	box.add_child(cond)
	if prefill.has("expr"):
		cond.set_expression_text(String(prefill.get("expr", "")))

	box.add_child(_label("then:"))
	var then_action := _action_picker()
	box.add_child(then_action[0])
	var with_else := CheckBox.new()
	with_else.text = "otherwise (else):"
	with_else.button_pressed = bool(prefill.get("else_present", false))
	box.add_child(with_else)
	var else_action := _action_picker()
	box.add_child(else_action[0])

	var snames := _section_names
	return func() -> PackedStringArray:
		var then_line := _action_line(then_action, snames)
		if then_line == "":
			return PackedStringArray()
		var then_body := PackedStringArray([then_line])
		var else_body := PackedStringArray()
		var has_else := with_else.button_pressed
		if has_else:
			var el := _action_line(else_action, snames)
			if el == "":
				has_else = false
			else:
				else_body = PackedStringArray([el])
		return MusStmtText.if_block(cond.get_expression_text(), then_body, has_else, else_body)


# A small [enter state | play track] action picker; returns [container, kind_opt,
# section_opt, track_opt] so _action_line can read it.
func _action_picker() -> Array:
	var row := HBoxContainer.new()
	var kind_opt := OptionButton.new()
	kind_opt.add_item("enter a state", 0)
	kind_opt.add_item("play a track", 1)
	row.add_child(kind_opt)
	var sob := _section_option("")
	row.add_child(sob)
	var tob := OptionButton.new()
	for i in range(_bank_names.size()):
		tob.add_item("%d: %s" % [i, _track_name(i)])
	tob.visible = false
	row.add_child(tob)
	kind_opt.item_selected.connect(func(idx):
		sob.visible = (idx == 0)
		tob.visible = (idx == 1))
	return [row, kind_opt, sob, tob]


func _action_line(picker: Array, section_names: PackedStringArray) -> String:
	var kind_opt: OptionButton = picker[1]
	if kind_opt.get_selected_id() == 1:
		var tob: OptionButton = picker[3]
		var t := tob.selected
		if t < 0:
			return ""
		return "play sound_%d" % t
	var sob: OptionButton = picker[2]
	if sob.selected < 0 or sob.selected >= section_names.size():
		return ""
	return "enter %s" % section_names[sob.selected]


# on (selector) <action> <targets...>. Action enter/goto -> section targets;
# play -> track targets. A simple add/remove target list.
func _build_switch_form(box: VBoxContainer, prefill: Dictionary) -> Callable:
	box.add_child(_label("Choose based on:"))
	var sel := ExprBuilderClass.new()
	sel.setup(_var_list, _mus)
	box.add_child(sel)
	if prefill.has("expr"):
		sel.set_expression_text(String(prefill.get("expr", "")))

	var arow := HBoxContainer.new()
	arow.add_child(_label("action:"))
	var action := OptionButton.new()
	action.add_item("enter", 0)
	action.add_item("goto", 1)
	action.add_item("play", 2)
	var pa := String(prefill.get("action", "enter"))
	action.select(2 if pa == "play" else (1 if pa == "goto" else 0))
	arow.add_child(action)
	box.add_child(arow)

	var targets_box := VBoxContainer.new()
	box.add_child(targets_box)
	var add_target := Button.new()
	add_target.text = "＋ add target"
	box.add_child(add_target)

	var rows: Array = []  # each: [container, section_opt, track_opt]
	var add_row := func(sec_name: String, track: int) -> void:
		var r := HBoxContainer.new()
		var sob := _section_option(sec_name)
		r.add_child(sob)
		var tob := OptionButton.new()
		for i in range(_bank_names.size()):
			tob.add_item("%d: %s" % [i, _track_name(i)])
		if track >= 0 and track < tob.item_count:
			tob.select(track)
		r.add_child(tob)
		var is_play: bool = action.get_selected_id() == 2
		sob.visible = not is_play
		tob.visible = is_play
		var rm := _tool_button("✕", "Remove target")
		r.add_child(rm)
		targets_box.add_child(r)
		var entry := [r, sob, tob]
		rows.append(entry)
		rm.pressed.connect(func():
			rows.erase(entry)
			r.queue_free())
	# Seed from prefill targets, else one empty row.
	var pretargets: Array = prefill.get("targets", [])
	if pretargets.is_empty():
		add_row.call("", -1)
	else:
		for t in pretargets:
			add_row.call(String(t.get("name", "")), int(t.get("track", -1)))
	add_target.pressed.connect(func(): add_row.call("", -1))
	action.item_selected.connect(func(idx):
		var is_play: bool = idx == 2
		for e in rows:
			(e[1] as OptionButton).visible = not is_play
			(e[2] as OptionButton).visible = is_play)

	var snames := _section_names
	return func() -> PackedStringArray:
		var act := "enter"
		if action.get_selected_id() == 1:
			act = "goto"
		elif action.get_selected_id() == 2:
			act = "play"
		var toks := PackedStringArray()
		for e in rows:
			if act == "play":
				var tob: OptionButton = e[2]
				if tob.selected >= 0:
					toks.append("sound_%d" % tob.selected)
			else:
				var sob: OptionButton = e[1]
				if sob.selected >= 0 and sob.selected < snames.size():
					toks.append(snames[sob.selected])
		if toks.is_empty():
			return PackedStringArray()
		return MusStmtText.switch_stmt(sel.get_expression_text(), act, toks)


func _label(text: String) -> Label:
	var l := Label.new()
	l.text = text
	l.add_theme_color_override("font_color", Color(0.75, 0.8, 0.9))
	return l


func _tool_button(glyph: String, tip: String) -> Button:
	var b := Button.new()
	b.text = glyph
	b.tooltip_text = tip
	b.flat = true
	b.focus_mode = Control.FOCUS_NONE
	return b


func _track_name(track: int) -> String:
	if track >= 0 and track < _bank_names.size() and String(_bank_names[track]) != "":
		return String(_bank_names[track])
	return "sound_%d" % track
