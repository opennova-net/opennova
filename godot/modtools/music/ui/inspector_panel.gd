class_name MusicInspectorPanel
extends VBoxContainer

# Per-state inspector for the section map's right dock. Given one section from
# the structured program AST (NovaMusicScript.get_program_ast), it renders EVERY
# statement the state runs -- plays, transitions, assignments, intrinsic calls,
# inc/dec, if/else, on-switches, return/yield -- in source order, each with a
# distinct icon so "a var changed here" / "a function ran here" reads at a glance.
# This is the visual-first surface: the raw script is the last-resort escape hatch.
#
# Plays are interactive chips (preview / remove behind the parity gate) and a
# drop target adds a play; transitions and switch targets get Go buttons that
# jump the running VM. set_active_offset(pc) lights the live statement.

const MusicTrackChipClass = preload("res://modtools/music/ui/track_chip.gd")

signal preview_requested(track_index: int)
signal jump_requested(section_name: StringName)
signal advanced_requested(section_name: StringName)
signal add_play_requested(section_name: StringName, track_index: int)
signal remove_play_requested(section_name: StringName, track_index: int)

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
var _editable: bool = false
var _bank_names: Array = []
# Rows that carry a bytecode offset, for the live-execution highlight. Each entry
# is { "ctrl": Control, "offset": int }. Rebuilt on every show_section.
var _offset_rows: Array = []
var _active_ctrl: Control = null


func _ready() -> void:
	clear()


func clear() -> void:
	_section_name = ""
	_offset_rows = []
	_active_ctrl = null
	for c in get_children():
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
	_editable = editable
	_bank_names = bank_names
	_offset_rows = []
	_active_ctrl = null
	for c in get_children():
		c.queue_free()

	var header := Label.new()
	var badges := ""
	if bool(section.get("is_entry", false)):
		badges += "   ★ start"
	if bool(section.get("is_idle_loop", false)):
		badges += "   ↻ idle"
	header.text = "%s   [state %d]%s" % [_section_name, int(section.get("index", -1)), badges]
	header.add_theme_color_override("font_color", Color(0.85, 0.92, 1.0))
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
		_render_statements(statements, list, 0)

	if _editable:
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


# Recursively render a statement array into `container`, indenting nested
# if/else and switch bodies. Each row records its bytecode offset for the live
# highlight.
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


func _render_play(s: Dictionary, container: Node, indent: int) -> void:
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


# transition / goto / call: an icon + the rendered line + a Go button that jumps
# the running VM to the target section.
func _render_jumpable(s: Dictionary, container: Node, indent: int) -> void:
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


# Simple read-only row (assign / incdec / expr / return / yield / nop / done /
# branch_comment). Assignments and intrinsic calls get their own icon so the
# "what changed / what ran" reads at a glance.
func _render_simple(s: Dictionary, container: Node, indent: int) -> void:
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


func _render_if(s: Dictionary, container: Node, indent: int) -> void:
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


func _render_switch(s: Dictionary, container: Node, indent: int) -> void:
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
