class_name MusicInspectorPanel
extends VBoxContainer

# Per-state inspector for the section map's right dock. Given one section from
# the structured program AST (NovaMusicScript.get_program_ast), it renders EVERY
# statement the state runs -- plays, transitions, assignments, intrinsic calls,
# inc/dec, if/else, on-switches, return/yield -- in source order, each with a
# distinct icon so "a var changed here" / "a function ran here" reads at a glance.
# This is the visual-first surface: the raw script is the last-resort escape hatch.
#
# Phase 2 (authoring): in editable mode every top-level statement gains a ✎/✕/↑/↓
# tool cluster, a "＋ Add" palette offers the full construct menu, and the header
# carries Rename / Delete-state. Each edit builds canonical names-less .mus lines
# (MusStmtText / MusExpr) and emits an intent the host routes to the document's
# parity-gated write path. Plays keep their drag/▶/✕ chip affordances.

const MusicTrackChipClass = preload("res://modtools/music/ui/track_chip.gd")
const MusStmtText = preload("res://modtools/music/mus_stmt_text.gd")
const MusExpr = preload("res://modtools/music/mus_expr.gd")
const ExprBuilderClass = preload("res://modtools/music/ui/expr_builder.gd")

signal preview_requested(track_index: int)
signal jump_requested(section_name: StringName)
signal advanced_requested(section_name: StringName)
signal add_play_requested(section_name: StringName, track_index: int)
signal remove_play_requested(section_name: StringName, track_index: int)
# Phase 2 authoring intents (host routes to the document):
signal add_statement_requested(section_index: int, lines: PackedStringArray)
signal replace_statement_requested(section_index: int, ordinal: int, lines: PackedStringArray)
signal delete_statement_requested(section_index: int, ordinal: int)
signal reorder_statement_requested(section_index: int, ordinal: int, direction: int)
signal rename_section_requested(old_name: StringName, new_name: StringName)
signal delete_section_requested(section_name: StringName)
signal author_failed(message: String)

# glyph + colour per statement kind. The assignment/inc-dec "var changed" family
# is warm; the call family ("a function ran") is purple; control flow is gold;
# transitions/switches (state moves) are blue/cyan; terminators are muted.
const _ICON := {
	"play": ["♪", Color(0.60, 0.90, 0.60)],
	"transition": ["→", Color(0.55, 0.80, 1.00)],
	"goto": ["↪", Color(0.55, 0.80, 1.00)],
	"call": ["ƒ", Color(0.80, 0.65, 1.00)],
	"return": ["⏎", Color(0.70, 0.70, 0.70)],
	"yield": ["⏸", Color(0.70, 0.70, 0.70)],
	"nop": ["·", Color(0.50, 0.50, 0.50)],
	"done": ["▪", Color(0.50, 0.50, 0.50)],
	"assign": ["✎", Color(1.00, 0.78, 0.40)],
	"incdec": ["±", Color(1.00, 0.78, 0.40)],
	"call_expr": ["ƒ", Color(0.80, 0.65, 1.00)],
	"plain_expr": ["·", Color(0.65, 0.65, 0.65)],
	"if": ["◇", Color(1.00, 0.85, 0.45)],
	"switch": ["⋔", Color(0.50, 0.85, 0.90)],
	"branch_comment": ["⌥", Color(0.55, 0.55, 0.55)],
}

const _ACTIVE_TINT := Color(0.55, 1.00, 0.55)

var _section_name: String = ""
var _section_index: int = -1
var _editable: bool = false
var _bank_names: Array = []
# Authoring context (set by the host via configure_authoring).
var _section_names: PackedStringArray = PackedStringArray()
var _var_list: Array = []        # [{token:String, label:String}]
var _mus = null                  # NovaMusicScript for expr validation
# Rows that carry a bytecode offset, for the live-execution highlight. Each entry
# is { "ctrl": Control, "offset": int }. Rebuilt on every show_section.
var _offset_rows: Array = []
var _active_ctrl: Control = null


func _ready() -> void:
	clear()


# Supply the authoring context the construct popups need: the section-name list
# (transition/switch targets + rename validation), the variable picker list, and
# the NovaMusicScript used to validate expressions. Safe to call repeatedly.
func configure_authoring(section_names: PackedStringArray, var_list: Array, mus = null) -> void:
	_section_names = section_names
	_var_list = var_list
	_mus = mus


func clear() -> void:
	_section_name = ""
	_section_index = -1
	_offset_rows = []
	_active_ctrl = null
	for c in get_children():
		if c is Window:
			continue  # don't free an open authoring/rename dialog on a background refresh
		c.queue_free()
	var hint := Label.new()
	hint.text = "Select a state on the map to inspect it."
	hint.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(hint)


# section: one AST section dict from NovaMusicScript.get_program_ast, with
# is_idle_loop merged in by the caller (the model carries that flag).
func show_section(section: Dictionary, bank_names: Array, came_from: String = "", editable: bool = false) -> void:
	_section_name = String(section.get("name", ""))
	_section_index = int(section.get("index", -1))
	_editable = editable
	_bank_names = bank_names
	_offset_rows = []
	_active_ctrl = null
	for c in get_children():
		if c is Window:
			continue  # don't free an open authoring/rename dialog on a background refresh
		c.queue_free()

	var header := HBoxContainer.new()
	var title := Label.new()
	var badges := ""
	if bool(section.get("is_entry", false)):
		badges += "   ★ start"
	if bool(section.get("is_idle_loop", false)):
		badges += "   ↻ idle"
	title.text = "%s   [state %d]%s" % [_section_name, _section_index, badges]
	title.add_theme_color_override("font_color", Color(0.85, 0.92, 1.0))
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(title)
	if _editable:
		var ren := _tool_button("✎", "Rename this state")
		ren.pressed.connect(_open_rename_dialog)
		header.add_child(ren)
		var del := _tool_button("✕", "Delete this state")
		del.pressed.connect(_open_delete_section_dialog)
		header.add_child(del)
	add_child(header)

	if came_from != "":
		var crumb := Label.new()
		crumb.text = "came from: %s" % came_from
		crumb.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
		add_child(crumb)

	var statements: Array = section.get("statements", [])
	if statements.is_empty():
		var none := Label.new()
		none.text = "  (this state runs nothing on its own)"
		none.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
		add_child(none)
	else:
		var list := VBoxContainer.new()
		list.add_theme_constant_override("separation", 1)
		add_child(list)
		for i in range(statements.size()):
			_render_top_statement(statements[i], list, i)

	if _editable:
		var add_btn := MenuButton.new()
		add_btn.text = "＋ Add"
		add_btn.tooltip_text = "Add a statement to this state."
		_populate_add_menu(add_btn.get_popup())
		add_child(add_btn)
		var drop_hint := Label.new()
		drop_hint.text = "＋ drag a track here to add a play"
		drop_hint.add_theme_color_override("font_color", Color(0.5, 0.7, 0.5))
		drop_hint.tooltip_text = "Drop a track from the Tracks dock to add a play to this state."
		add_child(drop_hint)

	var adv := Button.new()
	adv.text = "Show raw script for this state"
	adv.tooltip_text = "Open the Advanced drawer at this state's faithful decompiled text."
	adv.pressed.connect(func(): advanced_requested.emit(StringName(_section_name)))
	add_child(adv)


# Render one TOP-LEVEL statement (with its ordinal) plus, in editable mode, the
# ✎/✕/↑/↓ tool cluster. Nested if/switch bodies render read-only via
# _render_statements (edited through their parent's ✎).
func _render_top_statement(s: Dictionary, container: Node, ordinal: int) -> void:
	var kind := String(s.get("kind", ""))
	var row: Control = null
	match kind:
		"play":
			row = _render_play(s, container, 0)
		"if":
			row = _render_if(s, container, 0)
		"switch":
			row = _render_switch(s, container, 0)
		"transition", "goto", "call":
			row = _render_jumpable(s, container, 0)
		_:
			row = _render_simple(s, container, 0)
	if _editable and row is HBoxContainer:
		row.add_child(_row_tools(ordinal, kind, s))


# Recursively render a statement array into `container`, indenting nested
# if/else and switch bodies. Read-only (nested statements are edited via the
# enclosing block's ✎).
func _render_statements(stmts: Array, container: Node, indent: int) -> void:
	for s in stmts:
		var kind := String(s.get("kind", ""))
		match kind:
			"play":
				_render_play(s, container, indent)
			"if":
				_render_if(s, container, indent)
			"switch":
				_render_switch(s, container, indent)
			"transition", "goto", "call":
				_render_jumpable(s, container, indent)
			_:
				_render_simple(s, container, indent)


func _indent_spacer(indent: int) -> Control:
	var sp := Control.new()
	sp.custom_minimum_size = Vector2(float(indent) * 18.0, 0)
	return sp


# Register a row so set_active_offset can light it while the VM runs.
func _register_row(ctrl: Control, offset: int) -> void:
	_offset_rows.append({"ctrl": ctrl, "offset": offset})


func _glyph_label(kind_key: String) -> Label:
	var pair: Array = _ICON.get(kind_key, ["•", Color(0.7, 0.7, 0.7)])
	var g := Label.new()
	g.text = String(pair[0])
	g.add_theme_color_override("font_color", pair[1])
	g.custom_minimum_size = Vector2(16, 0)
	g.tooltip_text = kind_key
	return g


func _tool_button(glyph: String, tip: String) -> Button:
	var b := Button.new()
	b.text = glyph
	b.tooltip_text = tip
	b.flat = true
	b.focus_mode = Control.FOCUS_NONE
	return b


# The per-row ✎/✕/↑/↓ cluster. Plays get a ✎ that swaps the track (their chip
# already carries ▶/✕, so no duplicate delete). The structural `done` and the
# text-less `nop` carry no tools (neither is editable as a text line).
func _row_tools(ordinal: int, kind: String, stmt: Dictionary) -> Control:
	var box := HBoxContainer.new()
	box.add_theme_constant_override("separation", 0)
	if kind == "done" or kind == "nop":
		return box
	var edit := _tool_button("✎", "Edit")
	edit.pressed.connect(func(): _open_edit_form(kind, ordinal, stmt))
	box.add_child(edit)
	# A play's chip already has its own ✕; avoid a second delete control on it.
	if kind != "play":
		var del := _tool_button("✕", "Delete")
		del.pressed.connect(func(): delete_statement_requested.emit(_section_index, ordinal))
		box.add_child(del)
	var up := _tool_button("↑", "Move up")
	up.pressed.connect(func(): reorder_statement_requested.emit(_section_index, ordinal, -1))
	box.add_child(up)
	var down := _tool_button("↓", "Move down")
	down.pressed.connect(func(): reorder_statement_requested.emit(_section_index, ordinal, 1))
	box.add_child(down)
	return box


func _render_play(s: Dictionary, container: Node, indent: int) -> Control:
	var row := HBoxContainer.new()
	row.add_child(_indent_spacer(indent))
	row.add_child(_glyph_label("play"))
	var track: int = int(s.get("track", -1))
	var chip := MusicTrackChipClass.new()
	chip.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(chip)
	chip.setup(track, _track_name(track), bool(s.get("wait", false)), _editable)
	chip.preview_requested.connect(func(t): preview_requested.emit(t))
	chip.remove_requested.connect(func(t): remove_play_requested.emit(StringName(_section_name), t))
	container.add_child(row)
	_register_row(row, int(s.get("code_offset", -1)))
	return row


# transition / goto / call: an icon + the rendered line + a Go button that jumps
# the running VM to the target section.
func _render_jumpable(s: Dictionary, container: Node, indent: int) -> Control:
	var kind := String(s.get("kind", ""))
	var row := HBoxContainer.new()
	row.add_child(_indent_spacer(indent))
	row.add_child(_glyph_label(kind))
	var lbl := Label.new()
	lbl.text = String(s.get("text", ""))
	lbl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(lbl)
	var target := String(s.get("target_name", ""))
	if target != "":
		var go := Button.new()
		go.text = "Go"
		go.tooltip_text = "Jump the running script to %s" % target
		go.pressed.connect(func(): jump_requested.emit(StringName(target)))
		row.add_child(go)
	container.add_child(row)
	_register_row(row, int(s.get("code_offset", -1)))
	return row


# Simple read-only row (assign / incdec / expr / return / yield / nop / done /
# branch_comment). Assignments and intrinsic calls get their own icon so the
# "what changed / what ran" reads at a glance.
func _render_simple(s: Dictionary, container: Node, indent: int) -> Control:
	var kind := String(s.get("kind", ""))
	var key := kind
	if kind == "expr":
		key = "call_expr" if bool(s.get("has_call", false)) else "plain_expr"
	var row := HBoxContainer.new()
	row.add_child(_indent_spacer(indent))
	row.add_child(_glyph_label(key))
	var lbl := Label.new()
	lbl.text = String(s.get("text", ""))
	lbl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	if kind == "branch_comment" or kind == "nop" or kind == "done":
		lbl.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
	# Annotate any call with the intrinsic name so it's obvious a function ran.
	var has_call := bool(s.get("has_call", false))
	var call_name := String(s.get("call_name", ""))
	if has_call and call_name != "":
		lbl.tooltip_text = "calls %s" % call_name
	row.add_child(lbl)
	# An assignment whose RHS runs a function carries BOTH meanings: the ✎ mutation
	# icon AND a ƒ call badge, so "a var changed AND a function ran here" reads at a
	# glance (an expr-call already shows ƒ as its primary icon, so no badge there).
	if kind == "assign" and has_call:
		var badge := Label.new()
		badge.text = "ƒ"
		badge.add_theme_color_override("font_color", _ICON["call_expr"][1])
		badge.tooltip_text = ("calls %s" % call_name) if call_name != "" else "calls a function"
		row.add_child(badge)
	container.add_child(row)
	_register_row(row, int(s.get("code_offset", -1)))
	return row


func _render_if(s: Dictionary, container: Node, indent: int) -> Control:
	var head := HBoxContainer.new()
	head.add_child(_indent_spacer(indent))
	head.add_child(_glyph_label("if"))
	var lbl := Label.new()
	lbl.text = "if (%s)" % String(s.get("expr", ""))
	lbl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(lbl)
	container.add_child(head)
	_register_row(head, int(s.get("code_offset", -1)))
	_render_statements(s.get("then", []), container, indent + 1)
	if bool(s.get("else_present", false)):
		var elbl := Label.new()
		elbl.text = "%selse" % "  ".repeat(indent + 1)
		elbl.add_theme_color_override("font_color", _ICON["if"][1])
		container.add_child(elbl)
		_render_statements(s.get("else", []), container, indent + 1)
	return head


func _render_switch(s: Dictionary, container: Node, indent: int) -> Control:
	var head := HBoxContainer.new()
	head.add_child(_indent_spacer(indent))
	head.add_child(_glyph_label("switch"))
	var action := String(s.get("action", "enter"))
	var lbl := Label.new()
	lbl.text = "on (%s) → %s" % [String(s.get("expr", "")), action]
	lbl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(lbl)
	container.add_child(head)
	_register_row(head, int(s.get("code_offset", -1)))
	var targets: Array = s.get("targets", [])
	for t in targets:
		var trow := HBoxContainer.new()
		trow.add_child(_indent_spacer(indent + 1))
		if action == "play":
			var track: int = int(t.get("track", -1))
			var chip := MusicTrackChipClass.new()
			chip.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			trow.add_child(chip)
			chip.setup(track, _track_name(track), false, false)
			chip.preview_requested.connect(func(tk): preview_requested.emit(tk))
		else:
			var tname := String(t.get("name", ""))
			var tlbl := Label.new()
			tlbl.text = tname
			tlbl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			trow.add_child(tlbl)
			if int(t.get("section", -1)) >= 0 and tname != "":
				var go := Button.new()
				go.text = "Go"
				go.tooltip_text = "Jump the running script to %s" % tname
				go.pressed.connect(func(): jump_requested.emit(StringName(tname)))
				trow.add_child(go)
		container.add_child(trow)
	return head


func current_section() -> String:
	return _section_name


# Live-execution highlight: tint the row whose statement is at-or-just-before the
# VM pc (the greatest recorded offset <= pc), clearing any previous tint. pc < 0
# clears all. Called by live_mode while the VM runs.
func set_active_offset(pc: int) -> void:
	var best: Control = null
	var best_off := -1
	if pc >= 0:
		for r in _offset_rows:
			var off := int(r["offset"])
			if off >= 0 and off <= pc and off > best_off:
				best_off = off
				best = r["ctrl"]
	if best == _active_ctrl:
		return
	if _active_ctrl != null and is_instance_valid(_active_ctrl):
		_active_ctrl.modulate = Color(1, 1, 1)
	if best != null:
		best.modulate = _ACTIVE_TINT
	_active_ctrl = best


# Drop sink for the Tracks dock: accept a {kind:"mus_track", index} payload and
# ask to add a play to the shown state. Only while editable and a state is shown.
func _can_drop_data(_at_position: Vector2, data: Variant) -> bool:
	return _editable and _section_name != "" \
		and data is Dictionary and String((data as Dictionary).get("kind", "")) == "mus_track"


func _drop_data(_at_position: Vector2, data: Variant) -> void:
	if not (data is Dictionary):
		return
	var track := int((data as Dictionary).get("index", -1))
	if track >= 0:
		add_play_requested.emit(StringName(_section_name), track)


func _track_name(track: int) -> String:
	if track >= 0 and track < _bank_names.size() and String(_bank_names[track]) != "":
		return String(_bank_names[track])
	return "sound_%d" % track


# =====================================================================
# Authoring: the "＋ Add" palette + per-construct forms.
# =====================================================================

const _ADD_ITEMS := [
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
	# nop is intentionally NOT offered: the decompiler renders nop as nothing, so an
	# authored nop has no text line -- it would vanish on the next re-decompile and
	# couldn't be deleted/reordered. (A pre-existing top-level nop is shown read-only.)
]


func _populate_add_menu(popup: PopupMenu) -> void:
	popup.clear()
	for i in range(_ADD_ITEMS.size()):
		popup.add_item(String(_ADD_ITEMS[i][0]), i)
	if not popup.id_pressed.is_connected(_on_add_menu_id):
		popup.id_pressed.connect(_on_add_menu_id)


func _on_add_menu_id(id: int) -> void:
	if id < 0 or id >= _ADD_ITEMS.size():
		return
	var kind := String(_ADD_ITEMS[id][1])
	match kind:
		"return", "yield":
			# No inputs -- insert directly.
			add_statement_requested.emit(_section_index, _simple_lines(kind))
		_:
			_open_add_form(kind)


func _simple_lines(kind: String) -> PackedStringArray:
	match kind:
		"return":
			return MusStmtText.ret()
		"yield":
			return MusStmtText.yield_stmt()
	return PackedStringArray()


# Open an authoring form for a NEW statement (ordinal < 0) ...
func _open_add_form(kind: String) -> void:
	_open_form(kind, -1, {})


# ... or to EDIT an existing one (ordinal >= 0), pre-filled from its AST dict.
func _open_edit_form(kind: String, ordinal: int, stmt: Dictionary) -> void:
	# An existing if's then/else bodies can hold arbitrary statements the single-
	# action if-form can't represent, so a structured edit would silently truncate
	# them. Route if-editing to the raw-script drawer (lossless) instead; authoring
	# a NEW if still uses the structured form (no body to lose).
	if kind == "if":
		advanced_requested.emit(StringName(_section_name))
		return
	_open_form(kind, ordinal, stmt)


# Shared form builder. On confirm it generates canonical lines and emits either an
# add (ordinal < 0) or a replace (ordinal >= 0). Returns immediately; the dialog
# drives the rest.
func _open_form(kind: String, ordinal: int, prefill: Dictionary) -> void:
	# Capture the section index NOW: a background refresh (a live VM transition)
	# can re-point the panel at another state while the dialog is open, so reading
	# _section_index at confirm time could emit against the wrong section.
	var sidx := _section_index
	var dlg := ConfirmationDialog.new()
	dlg.title = ("Edit " if ordinal >= 0 else "Add ") + kind
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

	add_child(dlg)
	dlg.confirmed.connect(func():
		var lines: PackedStringArray = producer.call()
		if lines.is_empty():
			# Nothing buildable (e.g. no target picked / no track loaded): tell the
			# user instead of closing as if the edit applied.
			author_failed.emit("Fill in the fields first")
		elif ordinal >= 0:
			replace_statement_requested.emit(sidx, ordinal, lines)
		else:
			add_statement_requested.emit(sidx, lines)
		dlg.queue_free()
	)
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
	if picked < 0 and selected_offset >= 0 and selected_offset <= 60:
		var idx := selected_offset / 4
		if idx < _var_list.size():
			picked = idx
	if picked >= 0:
		ob.select(picked)
	return ob


func _var_token_at(ob: OptionButton) -> String:
	var idx := ob.selected
	if idx >= 0 and idx < _var_list.size():
		return String(_var_list[idx].get("token", "Var00"))
	return "Var00"


func _build_transition_form(box: VBoxContainer, kind: String, prefill: Dictionary) -> Callable:
	var lbl := Label.new()
	lbl.text = "Go to which state?"
	box.add_child(lbl)
	var target := String(prefill.get("target_name", ""))
	var ob := _section_option(target)
	box.add_child(ob)
	return func() -> PackedStringArray:
		if ob.selected < 0 or ob.selected >= _section_names.size():
			return PackedStringArray()
		var name := _section_names[ob.selected]
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
	return func() -> PackedStringArray:
		return MusStmtText.assign(_var_token_at(vob), eb.get_expression_text())


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
	return func() -> PackedStringArray:
		return MusStmtText.incdec(_var_token_at(vob), dir.get_selected_id() == 1)


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

	return func() -> PackedStringArray:
		var then_line := _action_line(then_action)
		if then_line == "":
			return PackedStringArray()
		var then_body := PackedStringArray([then_line])
		var else_body := PackedStringArray()
		var has_else := with_else.button_pressed
		if has_else:
			var el := _action_line(else_action)
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


func _action_line(picker: Array) -> String:
	var kind_opt: OptionButton = picker[1]
	if kind_opt.get_selected_id() == 1:
		var tob: OptionButton = picker[3]
		var t := tob.selected
		if t < 0:
			return ""
		return "play sound_%d" % t
	var sob: OptionButton = picker[2]
	if sob.selected < 0 or sob.selected >= _section_names.size():
		return ""
	return "enter %s" % _section_names[sob.selected]


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
				if sob.selected >= 0 and sob.selected < _section_names.size():
					toks.append(_section_names[sob.selected])
		if toks.is_empty():
			return PackedStringArray()
		return MusStmtText.switch_stmt(sel.get_expression_text(), act, toks)


func _label(text: String) -> Label:
	var l := Label.new()
	l.text = text
	l.add_theme_color_override("font_color", Color(0.75, 0.8, 0.9))
	return l


# --- section rename / delete dialogs ---

func _open_rename_dialog() -> void:
	var dlg := ConfirmationDialog.new()
	dlg.title = "Rename state"
	dlg.min_size = Vector2i(340, 120)
	var box := VBoxContainer.new()
	dlg.add_child(box)
	box.add_child(_label("New name for '%s':" % _section_name))
	var edit := LineEdit.new()
	edit.text = _section_name
	box.add_child(edit)
	add_child(dlg)
	var old_name := _section_name
	dlg.confirmed.connect(func():
		var nn := edit.text.strip_edges()
		if nn != "" and nn != old_name:
			rename_section_requested.emit(StringName(old_name), StringName(nn))
		dlg.queue_free())
	dlg.canceled.connect(func(): dlg.queue_free())
	dlg.close_requested.connect(func(): dlg.queue_free())
	dlg.popup_centered()
	edit.select_all()
	edit.grab_focus()


func _open_delete_section_dialog() -> void:
	var dlg := ConfirmationDialog.new()
	dlg.title = "Delete state"
	dlg.dialog_text = "Delete state '%s'?\nStates that other states point at can't be deleted until those links are retargeted." % _section_name
	var name := _section_name
	add_child(dlg)
	dlg.confirmed.connect(func():
		delete_section_requested.emit(StringName(name))
		dlg.queue_free())
	dlg.canceled.connect(func(): dlg.queue_free())
	dlg.close_requested.connect(func(): dlg.queue_free())
	dlg.popup_centered()
