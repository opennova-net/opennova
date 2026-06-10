class_name MusicStmtRow
extends PanelContainer

# One statement of a state's program, as a sentence-style row in the
# block-stack canvas (MusicSectionProgramView). A row OWNS no document logic:
# it renders a leaf statement (play / go-to / set / function call / ...) and
# reports interactions back to the host view, which routes them through the
# parity-gated document intents. if/switch render as container blocks
# (MusicStmtIfBlock / MusicStmtSwitchBlock), not as this class.
#
# Anatomy (left to right): a kind-coloured glyph, the sentence ("Play
# <track>", "Go to <state>", "Set volume(200)"), then badges (×N for a folded
# run, "waits" for playw) and, for state references, an "open ▸" jump. The
# canonical engine text lives in the tooltip; everything the user READS is the
# MusDisplayNames prettified form.
#
# Meta contract (what the view and tests address rows by):
#   "ordinal"    raw AST index (top-level rows; lane rows carry their parent's)
#   "kind"       the AST kind string
#   "run"        >1 when this row stands for a folded run of identical rows
#   "branch_key" "ifOrdinal:branch:index" on rows inside an if lane

const MusDisplayNames = preload("res://modtools/music/mus_display_names.gd")

# Rows the user can never edit or move (annotations / engine artifacts).
const READ_ONLY_KINDS := ["branch_comment", "nop"]

var kind := ""
var ordinal := -1
var run := 1
var read_only := false

var _glyph: Label = null
var _sentence: Label = null
var _badges: HBoxContainer = null
var _open_btn: Button = null
var _host = null  # MusicSectionProgramView (duck-typed; lanes pass the view too)


# Build the row for one AST statement dict. `ctx` comes from the view:
#   { "view", "bank_names", "display_vars", "locals_base", "input_names",
#     "editable" }
# `opts`: { "run": int, "branch_key": String, "read_only": bool }
func setup(stmt: Dictionary, p_ordinal: int, ctx: Dictionary, opts: Dictionary = {}) -> MusicStmtRow:
	kind = String(stmt.get("kind", ""))
	ordinal = p_ordinal
	run = int(opts.get("run", 1))
	_host = ctx.get("view")
	read_only = bool(opts.get("read_only", false)) or kind in READ_ONLY_KINDS \
		or not bool(ctx.get("editable", false))
	set_meta("ordinal", ordinal)
	set_meta("kind", kind)
	set_meta("run", run)
	if String(opts.get("branch_key", "")) != "":
		set_meta("branch_key", String(opts.get("branch_key", "")))

	add_theme_stylebox_override("panel", _row_style(MusDisplayNames.stmt_color(kind)))
	mouse_filter = Control.MOUSE_FILTER_STOP

	var box := HBoxContainer.new()
	box.add_theme_constant_override("separation", 8)
	add_child(box)

	_glyph = Label.new()
	_glyph.text = _glyph_for(kind)
	_glyph.add_theme_color_override("font_color", MusDisplayNames.stmt_color(kind))
	_glyph.tooltip_text = MusDisplayNames.stmt_tooltip(kind)
	_glyph.custom_minimum_size = Vector2(22, 0)
	box.add_child(_glyph)

	_sentence = Label.new()
	_sentence.text = _sentence_for(stmt, ctx)
	_sentence.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_sentence.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	# Power users get the canonical engine line on hover; the row itself stays
	# in plain language.
	var canon := String(stmt.get("text", ""))
	if canon != "":
		_sentence.tooltip_text = canon
	if kind == "nop" or kind == "branch_comment":
		_sentence.add_theme_color_override("font_color", Color(0.55, 0.55, 0.58))
	box.add_child(_sentence)

	_badges = HBoxContainer.new()
	_badges.add_theme_constant_override("separation", 6)
	box.add_child(_badges)
	if run > 1:
		var xn := Label.new()
		xn.text = "×%d" % run
		xn.tooltip_text = "This step repeats %d times in a row." % run
		xn.add_theme_color_override("font_color", Color(0.85, 0.85, 0.6))
		_badges.add_child(xn)
	if kind == "play" and bool(stmt.get("wait", false)):
		var w := Label.new()
		w.text = "waits"
		w.tooltip_text = "The program waits for this track to finish before moving on."
		w.add_theme_color_override("font_color", Color(0.6, 0.65, 0.7))
		_badges.add_child(w)

	# Folded-run affordance: ⊞ expands the run into individually-addressable
	# rows, ⊟ collapses it back. Display-only (the view re-renders), so it is
	# offered in read-only mode too.
	var fold: Dictionary = opts.get("fold_toggle", {})
	if not fold.is_empty():
		var ft := Button.new()
		ft.flat = true
		ft.focus_mode = Control.FOCUS_NONE
		if bool(fold.get("expanded", false)):
			ft.text = "⊟ fold"
			ft.tooltip_text = "Collapse these %d identical steps back into one row." % int(fold.get("run", run))
		else:
			ft.text = "⊞ unfold"
			ft.tooltip_text = "Expand this run into %d individually-editable rows." % int(fold.get("run", run))
		ft.pressed.connect(func():
			if fold.has("on_toggle"):
				(fold["on_toggle"] as Callable).call())
		_badges.add_child(ft)

	var target := String(stmt.get("target_name", ""))
	if kind in ["transition", "goto", "call"] and target != "":
		_open_btn = Button.new()
		_open_btn.text = "open ▸"
		_open_btn.tooltip_text = "Open %s" % target
		_open_btn.focus_mode = Control.FOCUS_NONE
		_open_btn.flat = true
		_open_btn.pressed.connect(func():
			if _host != null:
				_host.notify_open(StringName(target)))
		box.add_child(_open_btn)

	gui_input.connect(_on_gui_input)
	return self


func _on_gui_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.pressed \
			and (event as InputEventMouseButton).button_index == MOUSE_BUTTON_LEFT:
		if _host != null:
			_host.notify_selected(ordinal)


# The one-line sentence for a leaf kind. Friendly names everywhere: bank entry
# names for tracks, prettified expression text (intrinsic labels, named
# variables / caller inputs) for everything expression-shaped.
func _sentence_for(stmt: Dictionary, ctx: Dictionary) -> String:
	var bank_names: Array = ctx.get("bank_names", [])
	var dvars: Array = ctx.get("display_vars", [])
	var base := int(ctx.get("locals_base", MusDisplayNames.DEFAULT_LOCALS_BASE))
	var input_names: Dictionary = ctx.get("input_names", {})
	match kind:
		"play":
			return "Play  %s" % track_label(int(stmt.get("track", -1)), bank_names)
		"transition":
			return "Go to  %s" % _target_label(stmt)
		"goto":
			return "Jump to  %s" % _target_label(stmt)
		"call":
			return "Run  %s, then come back" % _target_label(stmt)
		"return":
			return "Return to wherever this state was run from"
		"yield":
			return "Wait for the engine's next music tick"
		"nop":
			return "(does nothing)"
		"branch_comment":
			return MusDisplayNames.pretty_expr(String(stmt.get("text", "")), dvars, base, input_names)
		_:
			# assign / expr: the prettified canonical line reads as the sentence
			# ("Speed = (Speed + 1)", "Set volume(200)").
			return MusDisplayNames.pretty_expr(String(stmt.get("text", "")), dvars, base, input_names)


func _target_label(stmt: Dictionary) -> String:
	var t := String(stmt.get("target_name", ""))
	return t if t != "" else "(unresolved)"


static func track_label(track: int, bank_names: Array) -> String:
	if track >= 0 and track < bank_names.size() and String(bank_names[track]) != "":
		return String(bank_names[track])
	return "track %d" % track


func _glyph_for(k: String) -> String:
	# First rune of the registry title ("♪ Play track" -> "♪"); the row's
	# sentence supplies the words.
	var title := MusDisplayNames.stmt_title(k)
	return title.substr(0, 1) if title != "" else "•"


static func _row_style(kind_color: Color) -> StyleBoxFlat:
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.13, 0.14, 0.17)
	sb.border_color = kind_color
	sb.set_border_width_all(0)
	sb.border_width_left = 3
	sb.content_margin_left = 10.0
	sb.content_margin_right = 8.0
	sb.content_margin_top = 5.0
	sb.content_margin_bottom = 5.0
	sb.corner_radius_top_left = 3
	sb.corner_radius_top_right = 3
	sb.corner_radius_bottom_left = 3
	sb.corner_radius_bottom_right = 3
	return sb
