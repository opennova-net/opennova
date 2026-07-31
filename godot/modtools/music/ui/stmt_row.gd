class_name MusicStmtRow
extends PanelContainer

# One statement of a state's program, as a sentence-style row in the
# block-stack canvas (MusicSectionProgramView). A row OWNS no document logic:
# it renders a leaf statement (play / go-to / set / function call / ...) and
# reports interactions back through callbacks the builder supplied, which
# route into the parity-gated document intents. if/switch render as container
# blocks (MusicStmtIfBlock / MusicStmtSwitchBlock), not as this class.
#
# EDITING IS THE ROW: when the script is editable, the sentence's values are
# live controls -- the track / target-state / variable dropdowns commit the
# canonical replacement line the moment a pick lands (one intent = one undo
# step), and expression values are chips that open the anchored popover
# (MusicExprPopover) with room to build. There is no edit mode to enter and
# no Apply for atomic picks; expressions keep an explicit Apply in the
# popover because they need validation.
#
# Anatomy: kind-coloured glyph · the sentence (labels + value controls) ·
# badges (×N folded run, "waits") · open ▸ for state references · a trailing
# ✕ / ⋮ tool cluster (delete, move, insert above). The canonical engine text
# lives in tooltips; everything the user READS is the MusDisplayNames
# prettified form.
#
# Meta contract (what the view and tests address rows by):
#   "ordinal"    raw AST index (top-level rows; lane rows carry their parent's)
#   "kind"       the AST kind string
#   "run"        >1 when this row stands for a folded run of identical rows
#   "branch_key" "ifOrdinal:branch:index" on rows inside an if lane

const MusDisplayNames = preload("res://modtools/music/mus_display_names.gd")
const MusStmtText = preload("res://modtools/music/mus_stmt_text.gd")

# Rows the user can never edit or move (annotations / engine artifacts).
const READ_ONLY_KINDS := ["branch_comment", "nop"]
# Rows whose CONTENT has nothing to edit (the row still deletes/moves).
const NO_CONTENT_EDIT := ["return", "yield"]

var kind := ""
var ordinal := -1
var run := 1
var read_only := false

var _stmt: Dictionary = {}
var _ctx: Dictionary = {}
var _opts: Dictionary = {}
var _glyph: Label = null
var _sentence: Label = null      # read-only rendering only
var _badges: HBoxContainer = null
var _box: HBoxContainer = null
var _mount = null                 # MusicSectionProgramView
# Live value controls (editable rendering), per kind:
var _track_opt: OptionButton = null
var _section_opt: OptionButton = null
var _var_opt: OptionButton = null
var _dir_opt: OptionButton = null
var _expr_chip: Button = null
var _snames_snapshot: PackedStringArray = PackedStringArray()
var _vars_snapshot: Array = []


# Build the row for one AST statement dict. `ctx` comes from the view:
#   { "view", "bank_names", "display_vars", "locals_base", "input_names",
#     "editable", "forms", "open_expr", "notify_edit_started" }
# `opts`: { "run", "branch_key", "read_only", "fold_toggle",
#   "on_lines": Callable(PackedStringArray)   commit a canonical replacement
#   "on_delete": Callable()                   remove this statement
#   "on_move": Callable(dir: int)             move within its container
#   "can_up" / "can_down": bool
#   "on_insert_above": Callable()             top-level gap insert }
func setup(stmt: Dictionary, p_ordinal: int, ctx: Dictionary, opts: Dictionary = {}) -> MusicStmtRow:
	_stmt = stmt
	_ctx = ctx
	_opts = opts
	kind = String(stmt.get("kind", ""))
	ordinal = p_ordinal
	run = int(opts.get("run", 1))
	_mount = ctx.get("view")
	read_only = bool(opts.get("read_only", false)) or kind in READ_ONLY_KINDS \
		or not bool(ctx.get("editable", false))
	set_meta("ordinal", ordinal)
	set_meta("kind", kind)
	set_meta("run", run)
	if String(opts.get("branch_key", "")) != "":
		set_meta("branch_key", String(opts.get("branch_key", "")))

	add_theme_stylebox_override("panel", _row_style(MusDisplayNames.stmt_color(kind)))
	mouse_filter = Control.MOUSE_FILTER_STOP

	_box = HBoxContainer.new()
	_box.add_theme_constant_override("separation", 8)
	add_child(_box)

	_glyph = Label.new()
	_glyph.text = _glyph_for(kind)
	_glyph.add_theme_color_override("font_color", MusDisplayNames.stmt_color(kind))
	_glyph.tooltip_text = MusDisplayNames.stmt_tooltip(kind)
	_glyph.custom_minimum_size = Vector2(22, 0)
	_box.add_child(_glyph)

	# A folded ×N run edits per-member after unfolding; the collapsed row is
	# read-only except for its count badge (the view owns that affordance).
	var content_editable := not read_only and run == 1 \
		and kind not in NO_CONTENT_EDIT and _content_supported(stmt, ctx)
	if content_editable:
		_build_live_sentence(stmt, ctx)
	else:
		_build_static_sentence(stmt, ctx)

	_badges = HBoxContainer.new()
	_badges.add_theme_constant_override("separation", 6)
	_box.add_child(_badges)
	# A folded run with a resize route gets the ×N badge as a count stepper:
	# one document splice turns ×20 into ×3 (or ×24) without unfolding.
	var has_stepper := run > 1 and opts.has("on_run_count") and not read_only
	if has_stepper:
		_build_run_stepper()
	elif run > 1:
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
	if kind in ["transition", "goto", "call"] and target != "" and _section_opt == null:
		_box.add_child(_open_button(target))

	# A collapsed ×N row carries no tools: deleting/moving it would act on ONE
	# member of the run, which reads as acting on all N. Unfold first; each
	# member then carries its own cluster (old-canvas parity).
	if not read_only and kind not in READ_ONLY_KINDS and run == 1:
		_build_tools()

	gui_input.connect(_on_gui_input)
	return self


# --- read-only rendering -----------------------------------------------------

func _build_static_sentence(stmt: Dictionary, ctx: Dictionary) -> void:
	_sentence = Label.new()
	_sentence.text = _sentence_for(stmt, ctx)
	_sentence.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_sentence.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	var canon := String(stmt.get("text", ""))
	if canon != "":
		_sentence.tooltip_text = canon
	if kind == "nop" or kind == "branch_comment":
		_sentence.add_theme_color_override("font_color", Color(0.55, 0.55, 0.58))
	_box.add_child(_sentence)


# The one-line sentence for a leaf kind. Friendly names everywhere: bank entry
# names for tracks, prettified expression text (intrinsic labels, named
# variables / caller inputs) for everything expression-shaped.
func _sentence_for(stmt: Dictionary, ctx: Dictionary) -> String:
	var bank_names: Array = ctx.get("bank_names", [])
	match kind:
		"play":
			return "Play  %s" % track_label(int(stmt.get("track", -1)), bank_names)
		"transition":
			return "Go to  %s" % _target_label(stmt)
		"goto":
			return "Jump to  %s" % _target_label(stmt)
		"call":
			var hand := _callee_hand_text(stmt)
			var base := "Run  %s, then come back" % _target_label(stmt)
			return base if hand == "" else "%s  (%s)" % [base, hand]
		"return":
			return "Return to wherever this state was run from"
		"yield":
			return "Wait for the engine's next music tick"
		"nop":
			return "(does nothing)"
		_:
			# assign / expr / branch_comment: the prettified canonical line
			# reads as the sentence ("Speed = (Speed + 1)", "Set volume(200)").
			return _pretty(String(stmt.get("text", "")), ctx)


# --- live (editable) rendering ------------------------------------------------

# An assign/incdec to a local slot the picker can't represent (an off-grid
# l_N) must not be retargeted blind; render it read-only instead.
func _content_supported(stmt: Dictionary, ctx: Dictionary) -> bool:
	if (kind == "assign" or kind == "incdec") and bool(stmt.get("is_local", false)):
		var token := String(stmt.get("var_name", ""))
		for v in ctx.get("display_vars", []):
			if String(v.get("token", "")) == token:
				return true
		return false
	return true


func _build_live_sentence(stmt: Dictionary, ctx: Dictionary) -> void:
	var forms = ctx.get("forms")
	_snames_snapshot = PackedStringArray(forms.section_names())
	_vars_snapshot = (ctx.get("display_vars", []) as Array).duplicate()
	match kind:
		"play":
			_box.add_child(_word("Play"))
			_track_opt = forms.make_track_option(int(stmt.get("track", -1)))
			_wire_picker(_track_opt, func(idx: int): _commit(MusStmtText.play(idx)))
			_box.add_child(_track_opt)
			_spacer()
		"transition", "goto", "call":
			_box.add_child(_word({"transition": "Go to", "goto": "Jump to", "call": "Run"}[kind]))
			_section_opt = forms.make_section_option(String(stmt.get("target_name", "")))
			_wire_picker(_section_opt, _commit_target)
			_box.add_child(_section_opt)
			if kind == "call":
				_box.add_child(_word(", then come back"))
				var hand := _callee_hand_label(stmt)
				if hand != null:
					_box.add_child(hand)
			var target := String(stmt.get("target_name", ""))
			if target != "":
				_box.add_child(_open_button(target))
			_spacer()
		"assign":
			_box.add_child(_word("Set"))
			_var_opt = forms.make_var_option(String(stmt.get("var_name", "")), int(stmt.get("var_offset", -1)))
			_wire_picker(_var_opt, func(_idx: int): _commit_assign(String(stmt.get("rhs", "0"))))
			_box.add_child(_var_opt)
			_box.add_child(_word("="))
			_expr_chip = _chip(_pretty(String(stmt.get("rhs", "")), ctx), String(stmt.get("rhs", "")))
			_expr_chip.pressed.connect(func(): _open_expr_popover("Set the value", stmt.get("rhs_tree", {}), String(stmt.get("rhs", "0")), "rhs"))
			_box.add_child(_expr_chip)
			_spacer()
		"incdec":
			_var_opt = forms.make_var_option(String(stmt.get("var_name", "")), int(stmt.get("var_offset", -1)))
			_wire_picker(_var_opt, func(_idx: int): _commit_incdec())
			_box.add_child(_var_opt)
			_dir_opt = OptionButton.new()
			_dir_opt.add_item("+1", 1)
			_dir_opt.add_item("-1", 0)
			_dir_opt.select(0 if bool(stmt.get("is_inc", true)) else 1)
			_wire_picker(_dir_opt, func(_idx: int): _commit_incdec())
			_box.add_child(_dir_opt)
			_spacer()
		"expr":
			_expr_chip = _chip(_pretty(String(stmt.get("expr", "")), ctx), String(stmt.get("expr", "")))
			_expr_chip.pressed.connect(func(): _open_expr_popover("Edit the action", stmt.get("expr_tree", {}), String(stmt.get("expr", "0")), "expr"))
			_expr_chip.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			_box.add_child(_expr_chip)
		_:
			_build_static_sentence(_stmt, ctx)


func _commit(lines: PackedStringArray) -> void:
	if _opts.has("on_lines"):
		(_opts["on_lines"] as Callable).call(lines)


func _commit_target(idx: int) -> void:
	if idx < 0 or idx >= _snames_snapshot.size():
		return
	var name := _snames_snapshot[idx]
	match kind:
		"transition":
			_commit(MusStmtText.enter(name))
		"goto":
			_commit(MusStmtText.goto_section(name))
		"call":
			_commit(MusStmtText.call_section(name))


func _commit_assign(rhs: String) -> void:
	_commit(MusStmtText.assign(_picked_var_token(), rhs))


func _commit_incdec() -> void:
	var is_inc: bool = _dir_opt == null or _dir_opt.get_selected_id() == 1
	_commit(MusStmtText.incdec(_picked_var_token(), is_inc))


func _picked_var_token() -> String:
	var forms = _ctx.get("forms")
	if _var_opt != null and forms != null:
		return forms.var_token_at(_var_opt, _vars_snapshot)
	return "Var00"


func _open_expr_popover(title: String, tree, fallback_text: String, slot: String) -> void:
	if not _ctx.has("open_expr"):
		return
	var seed = tree if (tree is Dictionary and not (tree as Dictionary).is_empty()) else fallback_text
	var key := {"ordinal": ordinal, "kind": kind, "slot": slot}
	if has_meta("branch_key"):
		key["branch_key"] = get_meta("branch_key")
	(_ctx["open_expr"] as Callable).call(self, title, seed, key, _on_expr_applied.bind(slot))


func _on_expr_applied(text: String, slot: String) -> void:
	match slot:
		"rhs":
			_commit_assign(text)
		"expr":
			_commit(MusStmtText.expr_stmt(text))


# Dropdown plumbing shared by every picker: opening one pins follow-live (the
# mount must not re-render the row under an open popup), picking commits.
func _wire_picker(ob: OptionButton, on_pick: Callable) -> void:
	ob.focus_mode = Control.FOCUS_NONE
	ob.get_popup().about_to_popup.connect(func():
		if _ctx.has("notify_edit_started"):
			(_ctx["notify_edit_started"] as Callable).call())
	ob.item_selected.connect(on_pick)


# --- tools ---------------------------------------------------------------

func _build_tools() -> void:
	var del := Button.new()
	del.text = "✕"
	del.tooltip_text = "Delete this step"
	del.flat = true
	del.focus_mode = Control.FOCUS_NONE
	del.pressed.connect(func():
		if _opts.has("on_delete"):
			(_opts["on_delete"] as Callable).call())
	_box.add_child(del)

	var menu := MenuButton.new()
	menu.text = "⋮"
	menu.tooltip_text = "Move / insert"
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
	pop.id_pressed.connect(_on_tool_menu_id)
	_box.add_child(menu)


func _on_tool_menu_id(id: int) -> void:
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


# --- folded-run count stepper ----------------------------------------------

var _run_spin: SpinBox = null

func _build_run_stepper() -> void:
	var xn := Button.new()
	xn.text = "×%d" % run
	xn.flat = true
	xn.focus_mode = Control.FOCUS_NONE
	xn.tooltip_text = "This step repeats %d times in a row. Click to change the count." % run
	_badges.add_child(xn)
	var stepper := HBoxContainer.new()
	stepper.visible = false
	_run_spin = SpinBox.new()
	_run_spin.min_value = 1
	_run_spin.max_value = 99
	_run_spin.value = run
	_run_spin.custom_minimum_size = Vector2(64, 0)
	stepper.add_child(_run_spin)
	var ok := Button.new()
	ok.text = "✓"
	ok.tooltip_text = "Apply the new repeat count (one undo step)."
	ok.focus_mode = Control.FOCUS_NONE
	ok.pressed.connect(func():
		stepper.visible = false
		xn.visible = true
		var n := int(_run_spin.value)
		if n != run and _opts.has("on_run_count"):
			(_opts["on_run_count"] as Callable).call(n))
	stepper.add_child(ok)
	var cancel := Button.new()
	cancel.text = "✕"
	cancel.tooltip_text = "Keep ×%d." % run
	cancel.focus_mode = Control.FOCUS_NONE
	cancel.pressed.connect(func():
		_run_spin.value = run
		stepper.visible = false
		xn.visible = true)
	stepper.add_child(cancel)
	_badges.add_child(stepper)
	xn.pressed.connect(func():
		xn.visible = false
		stepper.visible = true
		if _ctx.has("notify_edit_started"):
			(_ctx["notify_edit_started"] as Callable).call())


# --- drag to reorder + positional drops --------------------------------------

# The drop-target accent (a brighter top edge = "lands above this row").
var _drop_mark := false

func _get_drag_data(_pos: Vector2):
	# Folded runs move as members after unfolding; read-only rows don't move.
	if read_only or run > 1 or not _opts.has("on_move"):
		return null
	var data := {}
	if has_meta("branch_key"):
		data = {"kind": "mus_branch_stmt", "branch_key": String(get_meta("branch_key"))}
	else:
		data = {"kind": "mus_stmt", "ordinal": ordinal}
	# Tests drive this directly without a live GUI drag; the preview only
	# exists for real pointer drags.
	if get_viewport() != null and get_viewport().gui_is_dragging():
		var prev := Label.new()
		prev.text = "⠿ %s" % (_sentence.text if _sentence != null else MusDisplayNames.stmt_title(kind))
		set_drag_preview(prev)
	return data


# Dropping ON a row lands ABOVE it: a moved row, a track from the dock, or a
# lane sibling. Same-lane only for branch rows; the dispatch tail accepts
# nothing (read_only covers it).
func _can_drop_data(_pos: Vector2, data) -> bool:
	var ok := _drop_kind_ok(data)
	_set_drop_mark(ok)
	return ok


func _drop_kind_ok(data) -> bool:
	if read_only or not (data is Dictionary):
		return false
	var k := String((data as Dictionary).get("kind", ""))
	match k:
		"mus_track":
			return not has_meta("branch_key") and _opts.has("on_drop_track")
		"mus_stmt":
			return not has_meta("branch_key") and _opts.has("on_drop_move") \
				and int((data as Dictionary).get("ordinal", -1)) != ordinal
		"mus_branch_stmt":
			if not has_meta("branch_key") or not _opts.has("on_drop_branch"):
				return false
			var mine := String(get_meta("branch_key"))
			var theirs := String((data as Dictionary).get("branch_key", ""))
			# Same if + same branch ("ord:branch:idx" minus the index), not self.
			return mine != theirs \
				and mine.rsplit(":", true, 1)[0] == theirs.rsplit(":", true, 1)[0]
	return false


func _drop_data(_pos: Vector2, data) -> void:
	_set_drop_mark(false)
	if not _drop_kind_ok(data):
		return
	var d: Dictionary = data
	match String(d.get("kind", "")):
		"mus_track":
			(_opts["on_drop_track"] as Callable).call(int(d.get("index", -1)))
		"mus_stmt":
			(_opts["on_drop_move"] as Callable).call(int(d.get("ordinal", -1)))
		"mus_branch_stmt":
			(_opts["on_drop_branch"] as Callable).call(String(d.get("branch_key", "")))


func _notification(what: int) -> void:
	if what == NOTIFICATION_DRAG_END:
		_set_drop_mark(false)


func _set_drop_mark(on: bool) -> void:
	if on == _drop_mark:
		return
	_drop_mark = on
	var sb := _row_style(MusDisplayNames.stmt_color(kind))
	if on:
		sb.border_width_top = 2
		sb.border_color = Color(0.55, 0.8, 1.0)
	add_theme_stylebox_override("panel", sb)


# --- shared bits ----------------------------------------------------------

func _on_gui_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.pressed \
			and (event as InputEventMouseButton).button_index == MOUSE_BUTTON_LEFT:
		if _mount != null:
			_mount.notify_selected(ordinal)


func _pretty(text: String, ctx: Dictionary) -> String:
	return MusDisplayNames.pretty_expr(text,
		ctx.get("display_vars", []),
		int(ctx.get("locals_base", MusDisplayNames.DEFAULT_LOCALS_BASE)),
		ctx.get("input_names", {}))


func _target_label(stmt: Dictionary) -> String:
	var t := String(stmt.get("target_name", ""))
	return t if t != "" else "(unresolved)"


# "hands it: <input names>" for a call to a state that takes inputs; "" else.
func _callee_hand_text(stmt: Dictionary) -> String:
	if kind != "call" or _mount == null or not _mount.has_method("callee_inputs_text"):
		return ""
	return String(_mount.callee_inputs_text(String(stmt.get("target_name", ""))))


func _callee_hand_label(stmt: Dictionary) -> Label:
	var text := _callee_hand_text(stmt)
	if text == "":
		return null
	var l := Label.new()
	l.text = text
	l.add_theme_color_override("font_color", Color(0.62, 0.72, 0.9))
	l.add_theme_font_size_override("font_size", 11)
	l.tooltip_text = "The values themselves are pushed at runtime; these are the slots %s declares." \
		% String(stmt.get("target_name", ""))
	return l


static func track_label(track: int, bank_names: Array) -> String:
	if track >= 0 and track < bank_names.size() and String(bank_names[track]) != "":
		return String(bank_names[track])
	return "track %d" % track


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


func _word(text: String) -> Label:
	var l := Label.new()
	l.text = text
	return l


# A value chip: reads as the pretty sentence fragment, opens the popover.
func _chip(pretty_text: String, canonical: String) -> Button:
	var b := Button.new()
	b.text = pretty_text if pretty_text != "" else "…"
	b.tooltip_text = "%s\nClick to edit." % canonical
	b.focus_mode = Control.FOCUS_NONE
	b.clip_text = true
	return b


func _spacer() -> void:
	var sp := Control.new()
	sp.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_box.add_child(sp)


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
