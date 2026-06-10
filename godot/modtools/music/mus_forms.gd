extends RefCounted

# Shared MUS authoring context: the "＋ Add" construct palette metadata, the
# picker builders (section / variable / track) every editing surface shares,
# and the two remaining DIALOG forms -- new-if creation (condition + first
# action) and the switch target table (up to 64 rows; it doesn't fit a node).
# Everything else edits INLINE on the blueprint node (section_logic_graph's
# _begin_inline_edit), which routes through the same canonical producers.
#
# Every producer routes through MusStmtText / MusExpr / the structured ExprRow
# so the emitted .mus lines are canonical. The forms NEVER hand-assemble
# bytecode -- they only emit text the compile gate already accepts.
#
# No class_name (preload as a const) to avoid GDScript class-registration
# ordering surprises, matching mus_stmt_text.gd / mus_expr.gd.

const MusStmtText = preload("res://modtools/music/mus_stmt_text.gd")
const ExprRowClass = preload("res://modtools/music/ui/expr_row.gd")

# The "＋ Add" palette: [label, kind]. nop is intentionally absent -- the
# decompiler renders nop as nothing, so an authored nop has no text line and
# would vanish on the next re-decompile (it couldn't then be deleted/reordered).
const ADD_ITEMS := [
	["→  go to a state", "transition"],
	["↪  jump to a label", "goto"],
	["ƒ  run a state and return", "call"],
	["⋔  choose by value …", "switch"],
	["◇  if / else", "if"],
	["✎  set a variable", "assign"],
	["±  adjust a variable (+1 / -1)", "incdec"],
	["ƒ  do an action (volume, flags, …)", "expr"],
	["⏎  return to caller", "return"],
	["⏸  wait", "yield"],
]

var _section_names: PackedStringArray = PackedStringArray()
var _var_list: Array = []        # [{token:String, label:String}]
var _mus = null                  # NovaMusicScript for expr validation
var _bank_names: Array = []


# Supply the context every form/picker needs: the section-name list, the
# variable picker list, the NovaMusicScript used to validate expressions, and
# the bank track names. Safe to call repeatedly; hosts refresh it on every
# document change so pickers see current state.
func configure(section_names: PackedStringArray, var_list: Array, mus, bank_names: Array) -> void:
	_section_names = section_names
	_var_list = var_list
	_mus = mus
	_bank_names = bank_names


func section_names() -> PackedStringArray:
	return _section_names


func var_list() -> Array:
	return _var_list


func mus_script():
	return _mus


# True for the kinds that take no inputs (return/yield): the host inserts their
# canonical lines directly, no editor.
func is_inputless(kind: String) -> bool:
	return kind == "return" or kind == "yield"


# Canonical lines for the inputless kinds; empty for anything that needs input.
func simple_lines(kind: String) -> PackedStringArray:
	match kind:
		"return":
			return MusStmtText.ret()
		"yield":
			return MusStmtText.yield_stmt()
	return PackedStringArray()


# A sensible, always-compilable default line for the inline-edited kinds, so
# ＋Add can insert immediately and open the new node's editor (no dialog hop).
# Empty when the kind has no inline default (if/switch keep their dialogs).
func default_lines(kind: String) -> PackedStringArray:
	match kind:
		"play":
			return MusStmtText.play(0)
		"assign":
			return MusStmtText.assign("Var00", "0")
		"incdec":
			return MusStmtText.incdec("Var00", true)
		"expr":
			# A debug echo: visible in the event log, no effect on the music.
			return MusStmtText.expr_stmt("Echo(0)")
		"transition":
			if _section_names.is_empty():
				return PackedStringArray()
			return MusStmtText.enter(_section_names[0])
		"goto":
			if _section_names.is_empty():
				return PackedStringArray()
			return MusStmtText.goto_section(_section_names[0])
		"call":
			if _section_names.is_empty():
				return PackedStringArray()
			return MusStmtText.call_section(_section_names[0])
	return PackedStringArray()


# ---- shared picker builders (used by the dialogs AND the inline editors) ----

func make_section_option(selected_name: String) -> OptionButton:
	var ob := OptionButton.new()
	for i in range(_section_names.size()):
		ob.add_item(_section_names[i])
		if _section_names[i] == selected_name:
			ob.select(ob.item_count - 1)
	return ob


func make_var_option(selected_token: String, selected_offset: int = -1) -> OptionButton:
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


func make_track_option(selected_track: int) -> OptionButton:
	var ob := OptionButton.new()
	# Cap at 256: the play opcode operand is a single byte, so tracks >= 256 are
	# unplayable by the original engine and must not be offered.
	for i in range(mini(_bank_names.size(), 256)):
		ob.add_item("%d: %s" % [i, _track_name(i)])
	if _bank_names.is_empty():
		# No bank loaded: still let the user pick a slot index by number.
		for i in range(8):
			ob.add_item("sound_%d" % i)
	if selected_track >= 0 and selected_track < ob.item_count:
		ob.select(selected_track)
	return ob


# Resolve the picked variable's token against the list the editor OPENED with
# (passed in, not read from the instance) so a background configure() can't
# shift the index->token mapping out from under an open editor.
func var_token_at(ob: OptionButton, var_list_snapshot: Array) -> String:
	var idx := ob.selected
	if idx >= 0 and idx < var_list_snapshot.size():
		return String(var_list_snapshot[idx].get("token", "Var00"))
	return "Var00"


# ---- the two remaining dialog forms (if creation, switch table) ------------

# Build + show a ConfirmationDialog parented under `host`. On confirm it
# generates the canonical lines and hands them to on_lines.call(lines) -- an
# EMPTY array means a required field was left blank (the host surfaces that
# instead of pretending the edit applied). The dialog frees itself on close.
func open_form(host: Node, kind: String, prefill: Dictionary, is_edit: bool, on_lines: Callable) -> void:
	var dlg := ConfirmationDialog.new()
	dlg.title = ("Edit " if is_edit else "Add ") + ("choose by value" if kind == "switch" else kind)
	dlg.min_size = Vector2i(380, 140)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	dlg.add_child(box)
	var producer: Callable = Callable()
	match kind:
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


# if / else creation: condition + a single then-action and optional else-action
# (enter a state or play a track). The created block's body statements are then
# edited inline on the graph, statement by statement.
func _build_if_form(box: VBoxContainer, prefill: Dictionary) -> Callable:
	box.add_child(_label("When this is true:"))
	var cond := ExprRowClass.new()
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
		return MusStmtText.if_block(cond.get_expr_text(), then_body, has_else, else_body)


# A small [enter state | play track] action picker; returns [container, kind_opt,
# section_opt, track_opt] so _action_line can read it.
func _action_picker() -> Array:
	var row := HBoxContainer.new()
	var kind_opt := OptionButton.new()
	kind_opt.add_item("go to a state", 0)
	kind_opt.add_item("play a track", 1)
	row.add_child(kind_opt)
	var sob := make_section_option("")
	row.add_child(sob)
	var tob := make_track_option(-1)
	tob.visible = false
	row.add_child(tob)
	kind_opt.item_selected.connect(func(idx):
		sob.visible = (idx == 0)
		tob.visible = (idx == 1))
	return [row, kind_opt, sob, tob]


func _action_line(picker: Array, section_names_snapshot: PackedStringArray) -> String:
	var kind_opt: OptionButton = picker[1]
	if kind_opt.get_selected_id() == 1:
		var tob: OptionButton = picker[3]
		var t := tob.selected
		if t < 0:
			return ""
		return "play sound_%d" % t
	var sob: OptionButton = picker[2]
	if sob.selected < 0 or sob.selected >= section_names_snapshot.size():
		return ""
	return "enter %s" % section_names_snapshot[sob.selected]


# on (selector) <action> <targets...>. Action enter/goto -> section targets;
# play -> track targets. A simple add/remove target list.
func _build_switch_form(box: VBoxContainer, prefill: Dictionary) -> Callable:
	box.add_child(_label("Choose based on:"))
	var sel := ExprRowClass.new()
	sel.setup(_var_list, _mus)
	box.add_child(sel)
	if prefill.has("expr_tree"):
		sel.set_expr(prefill.get("expr_tree"))
	elif prefill.has("expr"):
		sel.set_expression_text(String(prefill.get("expr", "")))

	var arow := HBoxContainer.new()
	arow.add_child(_label("the value picks one of these, then:"))
	var action := OptionButton.new()
	action.add_item("go to it", 0)
	action.add_item("jump to it", 1)
	action.add_item("play it", 2)
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
		var sob := make_section_option(sec_name)
		r.add_child(sob)
		var tob := make_track_option(track)
		r.add_child(tob)
		var is_play: bool = action.get_selected_id() == 2
		sob.visible = not is_play
		tob.visible = is_play
		var rm := _tool_button("✕", "Remove target")
		r.add_child(rm)
		targets_box.add_child(r)
		var entry := [r, sob, tob]
		rows.append(entry)
		# The tablexec count + skip_size are single bytes; the compiler rejects a
		# table over 64 targets. Stop offering more rows so the user can't build one.
		add_target.disabled = rows.size() >= 64
		rm.pressed.connect(func():
			rows.erase(entry)
			add_target.disabled = rows.size() >= 64
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
		return MusStmtText.switch_stmt(sel.get_expr_text(), act, toks)


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
