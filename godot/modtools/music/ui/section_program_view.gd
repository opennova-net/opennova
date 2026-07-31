class_name MusicSectionProgramView
extends Control

# Level-2 of the music workspace: one state's program as a vertical BLOCK
# STACK -- a scrollable, top-to-bottom list of sentence-style statement rows.
# Sequential statements read in execution order; if/switch render as container
# blocks with indented lanes (MusicStmtIfBlock / MusicStmtSwitchBlock), so
# depth is indentation instead of graph sprawl. This replaced the old GraphEdit
# node-per-statement chain: the state-machine MAP keeps its graph (that
# metaphor fits BETWEEN states); inside a state, programs are lists.
#
# Built purely from NovaMusicScript.get_program_ast(section). The view never
# writes bytecode; it emits add/replace/delete/reorder/move intents the mount
# (live_mode) hands to the document's parity-gated, undoable write path.
#
# Engine artifacts the stack hides or explains instead of rendering raw:
#  - frame-setup ops (0x38) never render. A LEADING run is the state's caller
#    inputs -> the "Inputs from caller" card on top. One in the dispatch tail
#    (gamemus Begin) is the engine-event payload -> explained by the divider.
#    Either way its byte offset carries to the next rendered row so the live
#    glow resolves while the pc sits on it, and its ordinal still counts (the
#    write path addresses raw AST indices; hiding never renumbers).
#  - the section-closing `done` never renders; its offset attaches to the row
#    before it so the last step stays lit when the program ends.
#  - top-level statements after the first flow-leaving statement (the leaked
#    main-loop tail in gamemus Begin) sit behind a "⚡ Engine events" divider,
#    read-only: the game dispatches straight to them, they never run in the
#    state's own flow.
#
# Runs of identical simple statements (21x the same play) fold into one row
# with a ×N badge; ⊞ expands them into individually-addressable rows
# (display-only, persists across re-renders, resets on section change).

signal statement_selected(section_index: int, ordinal: int)
signal open_section_requested(section_name: StringName)
# Authoring intents (mount routes them to the document's parity-gated write path):
signal add_statement_requested(section_index: int, lines: PackedStringArray)
signal replace_statement_requested(section_index: int, ordinal: int, lines: PackedStringArray)
signal delete_statement_requested(section_index: int, ordinal: int)
signal reorder_statement_requested(section_index: int, ordinal: int, direction: int)
signal insert_statement_at_requested(section_index: int, before_ordinal: int, lines: PackedStringArray)
signal move_statement_requested(section_index: int, ordinal: int, before_ordinal: int)
signal set_run_count_requested(section_index: int, start_ordinal: int, old_count: int, new_count: int)
signal add_play_requested(section_name: StringName, track: int)
signal input_renamed(section_name: String, input_index: int, label: String)
signal author_failed(message: String)
# An edit control just opened: the mount pins follow-live so the VM can't yank
# the canvas (and the edit with it) out from under the user.
signal inline_edit_started

const MusForms = preload("res://modtools/music/mus_forms.gd")
const MusStmtText = preload("res://modtools/music/mus_stmt_text.gd")
const MusDisplayNames = preload("res://modtools/music/mus_display_names.gd")
const StmtRowClass = preload("res://modtools/music/ui/stmt_row.gd")
const IfBlockClass = preload("res://modtools/music/ui/stmt_if_block.gd")
const SwitchBlockClass = preload("res://modtools/music/ui/stmt_switch_block.gd")
const InputsCardClass = preload("res://modtools/music/ui/inputs_card.gd")
const ExprPopoverClass = preload("res://modtools/music/ui/expr_popover.gd")

const _ACTIVE_TINT := Color(0.55, 1.00, 0.55)
# Kinds that may fold when identical+consecutive at the top level.
const _FOLDABLE := ["play", "assign", "incdec", "expr"]
# Flow leaves the state here: anything after the first of these (or the
# section-closing done) is the engine-dispatch tail.
const _FLOW_LEAVES := ["transition", "goto", "return"]

var _section_index: int = -1
var _section_name: String = ""
var _section_dict: Dictionary = {}
var _bank_names: Array = []
# Live-glow registry: [{ "row": Control, "offset": int }].
var _offset_rows: Array = []
var _active_row: Control = null
# Byte offsets of hidden rows awaiting the next rendered row.
var _pending_offsets: Array = []
# Set of run-start ordinals the user expanded; carries across re-renders,
# reset on section change.
var _unfolded: Dictionary = {}
# Caller-input bookkeeping: leading frame ops feed the card, tail ones the
# divider; the total stays available to the mount (and the Input pickers).
var _section_inputs: int = 0
var _leading_inputs: int = 0
var _tail_inputs: int = 0
var _locals_base: int = MusDisplayNames.DEFAULT_LOCALS_BASE

# Authoring context (set by the mount via configure_authoring).
var _editable: bool = false
var _forms = MusForms.new()
var _section_names: PackedStringArray = PackedStringArray()
var _var_list: Array = []
var _mus = null
var _authoring_blocked_reason: String = ""
var _profile_path: String = ""
var _inputs_by_section: Dictionary = {}
# A ＋Add just inserted this default line: when the post-add re-render shows
# the section again, auto-open editing on the new row. {kind, line} or {}.
var _pending_add_edit: Dictionary = {}

var _toolbar: HBoxContainer = null
var _add_menu: MenuButton = null
var _scroll: ScrollContainer = null
var _stack: VBoxContainer = null
# The one expression popover (one edit at a time; opening elsewhere
# re-targets it). Lives on the VIEW so a row rebuild can't eat its state.
var _popover: MusicExprPopover = null
var _popover_apply: Callable = Callable()


func _ready() -> void:
	var root := VBoxContainer.new()
	root.set_anchors_preset(Control.PRESET_FULL_RECT)
	root.add_theme_constant_override("separation", 4)
	add_child(root)

	_toolbar = HBoxContainer.new()
	root.add_child(_toolbar)
	_add_menu = MenuButton.new()
	_add_menu.text = "＋ Add step"
	_add_menu.tooltip_text = "Add a step to this state."
	_add_menu.focus_mode = Control.FOCUS_NONE
	_add_menu.flat = false
	var pop := _add_menu.get_popup()
	for i in range(MusForms.ADD_ITEMS.size()):
		pop.add_item(String(MusForms.ADD_ITEMS[i][0]), i)
	pop.id_pressed.connect(_on_add_palette_id)
	_toolbar.add_child(_add_menu)
	# configure_authoring may have run before this node entered the tree.
	_add_menu.disabled = not _editable
	if not _editable:
		_add_menu.tooltip_text = _read_only_add_tooltip()

	_scroll = ScrollContainer.new()
	_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	root.add_child(_scroll)
	_stack = VBoxContainer.new()
	_stack.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_stack.add_theme_constant_override("separation", 4)
	_scroll.add_child(_stack)

	# The popover floats over the stack (sibling of the layout root, added
	# after it so it draws on top); it repositions itself on scroll.
	_popover = ExprPopoverClass.new()
	add_child(_popover)
	_popover.applied.connect(_on_popover_applied)
	_scroll.get_v_scroll_bar().value_changed.connect(func(_v): _popover.reposition())


# Supply the context rows and the palette need + flip editing on/off. The mount
# calls this on each drill-in and on every document.changed refresh, BEFORE
# show_section. profile_path / inputs_by_section feed the input-name sidecar
# and the call-site "hands it" chips.
func configure_authoring(section_names: PackedStringArray, var_list: Array, mus, bank_names: Array, editable: bool, blocked_reason: String = "", profile_path: String = "", inputs_by_section: Dictionary = {}) -> void:
	_section_names = section_names
	_var_list = var_list
	_mus = mus
	_bank_names = bank_names
	_editable = editable
	_authoring_blocked_reason = blocked_reason
	_profile_path = profile_path
	_inputs_by_section = inputs_by_section
	_locals_base = MusDisplayNames.DEFAULT_LOCALS_BASE
	if _mus != null and _mus.has_method("get_locals_frame_offset") \
			and _mus.has_method("get_default_script_name"):
		_locals_base = int(_mus.get_locals_frame_offset(_mus.get_default_script_name()))
	_forms.configure(section_names, var_list, mus, bank_names)
	if _add_menu != null:
		_add_menu.disabled = not editable
		_add_menu.tooltip_text = "Add a step to this state." if editable else _read_only_add_tooltip()


func _read_only_add_tooltip() -> String:
	var reason := _authoring_blocked_reason.strip_edges()
	if reason != "":
		return reason
	return "Authoring is unavailable for this script."


# Render one AST section dict (NovaMusicScript.get_program_ast element).
func show_section(section: Dictionary, bank_names: Array) -> void:
	var new_index := int(section.get("index", -1))
	if new_index != _section_index:
		_unfolded.clear()  # expanded folds don't carry to a different state
		_pending_add_edit = {}
	var same_section := new_index == _section_index
	_bank_names = bank_names
	_section_index = new_index
	_section_dict = section
	_section_name = String(section.get("name", ""))
	_offset_rows = []
	_active_row = null
	_pending_offsets = []
	# Keep the reader's place: a re-render of the SAME section (edit, undo,
	# fold toggle) restores the scroll; a different section starts at the top.
	var keep_scroll := _scroll.scroll_vertical if (same_section and _scroll != null) else 0
	for c in _stack.get_children():
		_stack.remove_child(c)
		c.queue_free()

	var stmts: Array = section.get("statements", [])
	var authored_end := _authored_end(stmts)
	_section_inputs = 0
	_leading_inputs = 0
	_tail_inputs = 0
	var leading := true
	for i in range(stmts.size()):
		var s: Dictionary = stmts[i]
		if String(s.get("kind", "")) == "frame_enter":
			var lc := int(s.get("locals_count", 0))
			_section_inputs += lc
			if leading:
				_leading_inputs += lc
			elif i > authored_end:
				_tail_inputs += lc
		else:
			leading = false

	if _leading_inputs > 0:
		_stack.add_child(InputsCardClass.new().setup(_leading_inputs, _locals_base,
			_input_names(), _editable, _on_card_input_renamed))

	if _is_empty_section_body(stmts):
		_stack.add_child(_build_empty_hint())
	else:
		_build_rows(stmts, authored_end)

	# An open popover outlives the rebuild (it is NOT a row child): re-anchor
	# it to the surviving row, or close it with a notice when the statement is
	# gone (an external change -- undo, another edit -- ate it).
	_resolve_popover_after_render()
	_consume_pending_add_edit(stmts)
	if keep_scroll > 0:
		# The stack lays out next frame; restore once sizes exist.
		_restore_scroll_to = keep_scroll
		call_deferred("_apply_restored_scroll")


# The index of the first top-level statement after which flow has left the
# state (or the section close, whichever comes first).
func _authored_end(stmts: Array) -> int:
	for i in range(stmts.size()):
		var k := String((stmts[i] as Dictionary).get("kind", ""))
		if k in _FLOW_LEAVES or k == "done":
			return i
	return stmts.size()


func _build_rows(stmts: Array, authored_end: int) -> void:
	var ctx := _row_ctx()
	var divider_added := false
	var i := 0
	while i < stmts.size():
		var s: Dictionary = stmts[i]
		var kind := String(s.get("kind", ""))
		# Hidden rows: bank the offset, keep counting i (ordinals are raw AST
		# indices -- hiding never renumbers).
		if kind == "frame_enter":
			_pending_offsets.append(int(s.get("code_offset", -1)))
			i += 1
			continue
		if kind == "done":
			_attach_offset_to_last_row(int(s.get("code_offset", -1)))
			i += 1
			continue
		if i > authored_end and not divider_added:
			_stack.add_child(_build_dispatch_divider())
			divider_added = true
		var in_tail := divider_added

		var run := 1
		if kind in _FOLDABLE:
			while i + run < stmts.size() and _same_simple(stmts[i + run], s):
				run += 1
		if run > 1 and _unfolded.has(i):
			# Expanded run: every member is its own row with its own ordinal;
			# the first carries the ⊟ collapse toggle.
			for k in range(run):
				var opts := _leaf_opts(stmts, i + k, in_tail)
				if k == 0:
					opts["fold_toggle"] = _fold_toggle(i, true, run)
				var member: Dictionary = stmts[i + k]
				var mrow := StmtRowClass.new().setup(member, i + k, ctx, opts)
				_stack.add_child(mrow)
				_register(mrow, int(member.get("code_offset", -1)))
			i += run
			continue
		var row: Control
		match kind:
			"if":
				row = IfBlockClass.new().setup(s, i, ctx, _leaf_opts(stmts, i, in_tail))
			"switch":
				row = SwitchBlockClass.new().setup(s, i, ctx, _leaf_opts(stmts, i, in_tail))
			_:
				var opts2 := _leaf_opts(stmts, i, in_tail)
				opts2["run"] = run
				if run > 1:
					opts2["fold_toggle"] = _fold_toggle(i, false, run)
					if _editable and not in_tail:
						opts2["on_run_count"] = _request_run_count.bind(i, run)
				row = StmtRowClass.new().setup(s, i, ctx, opts2)
		_stack.add_child(row)
		_register(row, int(s.get("code_offset", -1)))
		i += run if run > 1 else 1


# The intent callbacks one top-level row gets. A folded run only deletes /
# moves as individual members after unfolding, so run handling stays in the
# build loop; everything here addresses ONE raw ordinal.
func _leaf_opts(stmts: Array, i: int, in_tail: bool) -> Dictionary:
	var opts := {"read_only": in_tail}
	if in_tail or not _editable:
		return opts
	opts["on_lines"] = _request_replace.bind(i)
	opts["on_delete"] = _request_delete.bind(i)
	opts["on_move"] = _request_move.bind(i)
	opts["can_up"] = _can_swap(stmts, i, -1)
	opts["can_down"] = _can_swap(stmts, i, 1)
	opts["on_insert_above"] = _open_gap_palette.bind(i)
	opts["on_drop_move"] = _request_move_before.bind(i)
	opts["on_drop_track"] = _request_track_above.bind(i)
	return opts


func _request_move_before(from_ordinal: int, before_ordinal: int) -> void:
	move_statement_requested.emit(_section_index, from_ordinal, before_ordinal)


func _request_track_above(track: int, before_ordinal: int) -> void:
	if track < 0:
		return
	insert_statement_at_requested.emit(_section_index, before_ordinal, MusStmtText.play(track))


func _request_run_count(new_count: int, start_ordinal: int, old_count: int) -> void:
	set_run_count_requested.emit(_section_index, start_ordinal, old_count, new_count)


func _request_replace(lines: PackedStringArray, ordinal: int) -> void:
	replace_statement_requested.emit(_section_index, ordinal, lines)


func _request_delete(ordinal: int) -> void:
	delete_statement_requested.emit(_section_index, ordinal)


func _request_move(dir: int, ordinal: int) -> void:
	reorder_statement_requested.emit(_section_index, ordinal, dir)


# Whether the row at i can swap with its neighbour: skip the zero-width nops
# the document also steps over, refuse the locked rows (the section-closing
# done, hidden frame setup) -- which is exactly what keeps a row from moving
# across the dispatch boundary. Per-ROW, not a single first-movable index:
# gamemus Begin's frame op sits MID-section.
func _can_swap(stmts: Array, i: int, dir: int) -> bool:
	var j := i + dir
	while j >= 0 and j < stmts.size() \
			and String((stmts[j] as Dictionary).get("kind", "")) == "nop":
		j += dir
	if j < 0 or j >= stmts.size():
		return false
	var k := String((stmts[j] as Dictionary).get("kind", ""))
	return k != "done" and k != "frame_enter"


func _row_ctx() -> Dictionary:
	return {
		"view": self,
		"bank_names": _bank_names,
		"display_vars": _display_var_list(),
		"locals_base": _locals_base,
		"input_names": _input_names(),
		"editable": _editable,
		"register": Callable(self, "_register"),
		"pend": Callable(self, "_pend_offset"),
		"forms": _forms,
		"open_expr": Callable(self, "_open_expr_popover"),
		"notify_edit_started": Callable(self, "notify_edit_started"),
		"stmt_at": Callable(self, "_stmt_at"),
	}


# The freshest AST dict for a top-level ordinal: blocks resolve through this
# at mutation time so a delayed apply (popover) never resurrects stale bodies.
# _section_dict is replaced by every show_section, which the mount re-runs on
# every document change.
func _stmt_at(ordinal: int) -> Dictionary:
	var stmts: Array = _section_dict.get("statements", [])
	if ordinal >= 0 and ordinal < stmts.size():
		return stmts[ordinal]
	return {}


# A picker/popover just opened: the mount pins follow-live.
func notify_edit_started() -> void:
	inline_edit_started.emit()


# The Inputs card committed a rename: the mount persists it (sidecar) and
# re-populates, so the new name reaches the pickers and sentences too.
func _on_card_input_renamed(input_index: int, label: String) -> void:
	input_renamed.emit(_section_name, input_index, label)


# --- the expression popover -------------------------------------------------

func _open_expr_popover(anchor: Control, title: String, seed, key: Dictionary, on_apply: Callable) -> void:
	notify_edit_started()
	_popover_apply = on_apply
	_popover.open_for(anchor, title, seed, _display_var_list(), _mus, key)


func _on_popover_applied(text: String) -> void:
	if _popover_apply.is_valid():
		_popover_apply.call(text)
	_popover_apply = Callable()


# After a rebuild: keep an open popover alive by re-anchoring it to the row
# that now renders its statement; close-with-notice when that statement is
# gone. Apply/Cancel/takeover never get here with an open popover, so a close
# here is always an EXTERNAL change eating the edit.
func _resolve_popover_after_render() -> void:
	if _popover == null or not _popover.is_open():
		return
	var key := _popover.key()
	var anchor := _find_row_by_key(key)
	if anchor != null:
		_popover.reanchor(anchor)
		# The rebuild freed the row the apply callback was bound to; rebind it
		# to the surviving statement's NEW row so Apply still lands.
		var rebound := _apply_callable_for(anchor, key)
		if rebound.is_valid():
			_popover_apply = rebound
	else:
		_popover.cancel()
		_popover_apply = Callable()
		author_failed.emit("That change closed your open edit — reopen it to continue.")


# The apply route for a popover key against a (fresh) row instance: the same
# method a chip click on that row would have bound.
func _apply_callable_for(row: Control, key: Dictionary) -> Callable:
	var slot := String(key.get("slot", ""))
	match slot:
		"cond":
			if row is IfBlockClass:
				return Callable(row, "_on_condition_applied")
		"selector":
			if row is SwitchBlockClass:
				return Callable(row, "_on_selector_applied")
		"rhs", "expr":
			if row is StmtRowClass:
				return Callable(row, "_on_expr_applied").bind(slot)
	return Callable()


# Locate the row/block for a popover key ({ordinal, kind, branch_key?}).
func _find_row_by_key(key: Dictionary) -> Control:
	var want_branch := String(key.get("branch_key", ""))
	var want_ordinal := int(key.get("ordinal", -1))
	var want_kind := String(key.get("kind", ""))
	for r in top_level_rows():
		if want_branch != "":
			if r is IfBlockClass:
				for lane_row in (r as MusicStmtIfBlock).then_rows() + (r as MusicStmtIfBlock).else_rows():
					if lane_row.has_meta("branch_key") and String(lane_row.get_meta("branch_key")) == want_branch:
						return lane_row
			continue
		if int(r.get_meta("ordinal")) == want_ordinal and String(r.get_meta("kind")) == want_kind:
			return r
	return null


# Profile-named caller inputs for the SHOWN section ({0-based index -> label}).
func _input_names() -> Dictionary:
	if _profile_path == "" or _section_name == "" or _mus == null \
			or not _mus.has_method("get_default_script_name"):
		return {}
	return MusInputNames.labels_for(String(_mus.get_default_script_name()), _section_name, _profile_path)


# The "hands it: …" chip text for a call row's target, "" when the target
# takes no inputs. The VALUES aren't statically recoverable (the engine pops
# them at runtime), so the chip names the slots the callee declares.
func callee_inputs_text(target: String) -> String:
	var count := int(_inputs_by_section.get(target, 0))
	if count <= 0:
		return ""
	var names := {}
	if _profile_path != "" and _mus != null and _mus.has_method("get_default_script_name"):
		names = MusInputNames.labels_for(String(_mus.get_default_script_name()), target, _profile_path)
	var slots := []
	for k in range(count):
		slots.append(String(names.get(k, "Input %d" % (k + 1))))
	return "hands it: %s" % ", ".join(slots)


func _fold_toggle(start_ordinal: int, expanded: bool, run: int) -> Dictionary:
	return {"expanded": expanded, "run": run, "on_toggle": _toggle_fold.bind(start_ordinal)}


func _toggle_fold(start_ordinal: int) -> void:
	if _unfolded.has(start_ordinal):
		_unfolded.erase(start_ordinal)
	else:
		_unfolded[start_ordinal] = true
	_rerender()


# Re-render the current section in place (fold toggle, input rename).
func _rerender() -> void:
	if not _section_dict.is_empty():
		show_section(_section_dict, _bank_names)


func _same_simple(a: Dictionary, b: Dictionary) -> bool:
	return String(a.get("kind", "")) == String(b.get("kind", "")) \
		and String(a.get("text", "")) == String(b.get("text", ""))


# Treat a section as empty when nothing AUTHORED remains: hidden frame-setup
# rows and the closing done don't count (a frame+done body is a callable that
# does nothing yet -- show the add-your-first-step hint).
func _is_empty_section_body(stmts: Array) -> bool:
	for s in stmts:
		if not (s is Dictionary):
			return false
		var k := String((s as Dictionary).get("kind", ""))
		if k != "frame_enter" and k != "done":
			return false
	return true


func _build_empty_hint() -> Control:
	var card := PanelContainer.new()
	card.set_meta("empty_hint", true)
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.12, 0.13, 0.16)
	sb.set_border_width_all(1)
	sb.border_color = Color(0.3, 0.32, 0.38)
	sb.content_margin_left = 14.0
	sb.content_margin_right = 14.0
	sb.content_margin_top = 10.0
	sb.content_margin_bottom = 10.0
	card.add_theme_stylebox_override("panel", sb)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 4)
	card.add_child(box)
	var head := Label.new()
	head.text = "No steps yet"
	head.add_theme_color_override("font_color", Color(0.85, 0.9, 1.0))
	box.add_child(head)
	var detail := Label.new()
	detail.text = "Import tracks in the Tracks dock, or use Add step for control flow." \
		if _bank_names.is_empty() else "Add the first step or drag a track from the Tracks dock."
	detail.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	detail.add_theme_color_override("font_color", Color(0.65, 0.68, 0.74))
	box.add_child(detail)
	var add := Button.new()
	add.text = "＋ Add step"
	add.focus_mode = Control.FOCUS_NONE
	add.disabled = not _editable
	add.tooltip_text = "Add the first step to this state." if _editable else _read_only_add_tooltip()
	add.pressed.connect(func():
		if _add_menu != null:
			_add_menu.get_popup().popup(Rect2i(Vector2i(add.get_global_rect().position + Vector2(0, add.size.y)), Vector2i.ZERO)))
	box.add_child(add)
	return card


# The band between the state's own flow and the leaked main-loop tail the
# game dispatches straight into (gamemus Begin). The tail's frame op is the
# engine handing values -- explain it here, in words, instead of rendering
# the raw op.
func _build_dispatch_divider() -> Control:
	var band := PanelContainer.new()
	band.set_meta("dispatch_divider", true)
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.16, 0.14, 0.10)
	sb.border_color = Color(0.85, 0.70, 0.30)
	sb.set_border_width_all(0)
	sb.border_width_left = 3
	sb.content_margin_left = 10.0
	sb.content_margin_right = 8.0
	sb.content_margin_top = 5.0
	sb.content_margin_bottom = 5.0
	band.add_theme_stylebox_override("panel", sb)
	var box := VBoxContainer.new()
	band.add_child(box)
	var head := Label.new()
	head.text = "⚡ Engine events"
	head.add_theme_color_override("font_color", Color(0.95, 0.82, 0.45))
	box.add_child(head)
	var detail := Label.new()
	var txt := "The game jumps straight to these when it signals music events; they don't run after the steps above."
	if _tail_inputs > 0:
		var names := _input_names()
		var slots := []
		for k in range(_tail_inputs):
			slots.append(String(names.get(k, "Input %d" % (k + 1))))
		txt += " It hands this state: %s." % ", ".join(slots)
	detail.text = txt
	detail.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	detail.add_theme_color_override("font_color", Color(0.75, 0.68, 0.5))
	detail.tooltip_text = "Engine slot %s holds the first value (frame op 0x38 banks them)." % MusDisplayNames.input_token(0, _locals_base) \
		if _tail_inputs > 0 else "Decompiled tail after the state's last own step."
	box.add_child(detail)
	return band


# --- live glow -------------------------------------------------------------

func _register(row: Control, offset: int) -> void:
	# Adopt any hidden offsets banked since the last rendered row, so the glow
	# lights this row while the pc sits on them.
	for off in _pending_offsets:
		if int(off) >= 0:
			_offset_rows.append({"row": row, "offset": int(off)})
	_pending_offsets.clear()
	if offset >= 0:
		_offset_rows.append({"row": row, "offset": offset})


func _pend_offset(offset: int) -> void:
	_pending_offsets.append(offset)


# The hidden section-closing done: keep the LAST step lit while the program
# sits at its end (attach to the previous row; a leading done can't happen).
func _attach_offset_to_last_row(offset: int) -> void:
	if offset < 0:
		return
	if _offset_rows.is_empty():
		_pending_offsets.append(offset)
		return
	var last_row: Control = _offset_rows[-1]["row"]
	_offset_rows.append({"row": last_row, "offset": offset})


# Light the row whose statement is at-or-just-before the VM pc (greatest
# recorded offset <= pc), clearing the previous. pc < 0 clears. Called every
# process frame while the VM runs: modulate-only with an early-out.
func set_active_offset(pc: int) -> void:
	var best: Control = null
	var best_off := -1
	if pc >= 0:
		for r in _offset_rows:
			var off := int(r["offset"])
			if off >= 0 and off <= pc and off > best_off:
				best_off = off
				best = r["row"]
	if best == _active_row:
		return
	if _active_row != null and is_instance_valid(_active_row):
		_active_row.modulate = Color(1, 1, 1)
	if best != null:
		best.modulate = _ACTIVE_TINT
	_active_row = best


# Bring the live row into view (the mount calls this while follow-live is on).
func scroll_to_active() -> void:
	if _active_row != null and is_instance_valid(_active_row) and _scroll != null:
		_scroll.ensure_control_visible(_active_row)


var _restore_scroll_to := 0

func _apply_restored_scroll() -> void:
	if _restore_scroll_to > 0 and _scroll != null:
		_scroll.scroll_vertical = _restore_scroll_to
	_restore_scroll_to = 0


# --- mount surface ----------------------------------------------------------


# How many values a caller (or the engine) hands the shown state -- the hidden
# frame ops' locals counts. The Inputs card explains the leading ones; the
# dispatch divider the tail ones.
func section_inputs_count() -> int:
	return _section_inputs


# The picker/display variable list for the SHOWN section: the global VarXX
# list plus one row per caller input, so input slots read by name in
# expressions AND are pickable in editors. Tokens stay canonical (l_N).
func _display_var_list() -> Array:
	if _section_inputs <= 0:
		return _var_list
	var names := _input_names()
	var out := _var_list.duplicate()
	for k in range(_section_inputs):
		var token := MusDisplayNames.input_token(k, _locals_base)
		var label := String(names.get(k, "Input %d" % (k + 1)))
		out.append({"token": token, "label": "%s (%s)" % [label, token]})
	return out


# Child rows report through these (rows stay dumb; the view owns signals).
func notify_open(section_name: StringName) -> void:
	open_section_requested.emit(section_name)


func notify_selected(ordinal: int) -> void:
	statement_selected.emit(_section_index, ordinal)


# --- add palette -----------------------------------------------------------

# The toolbar ＋ appends at the canonical anchor (before the first
# terminator); a gap ⋮ "Insert step above" targets one row's gap.
func _on_add_palette_id(id: int) -> void:
	_add_kind_at(id, -1)


func _open_gap_palette(before_ordinal: int) -> void:
	if not _editable:
		return
	var pop := PopupMenu.new()
	for i in range(MusForms.ADD_ITEMS.size()):
		pop.add_item(String(MusForms.ADD_ITEMS[i][0]), i)
	add_child(pop)
	pop.id_pressed.connect(func(id: int): _add_kind_at(id, before_ordinal))
	pop.popup_hide.connect(pop.queue_free)
	pop.position = Vector2i(get_global_mouse_position())
	pop.reset_size()
	pop.popup()


# Insert the picked kind's canonical default lines (no creation dialogs: even
# if/switch land as editable defaults) and auto-open editing on the new row
# after the re-render. before_ordinal -1 = the canonical append anchor.
func _add_kind_at(id: int, before_ordinal: int) -> void:
	if id < 0 or id >= MusForms.ADD_ITEMS.size():
		return
	var kind := String(MusForms.ADD_ITEMS[id][1])
	var lines: PackedStringArray
	if _forms.is_inputless(kind):
		lines = _forms.simple_lines(kind)
	else:
		_forms.configure(_section_names, _var_list, _mus, _bank_names)
		lines = _forms.default_lines(kind)
	if lines.is_empty():
		author_failed.emit("Fill in the fields first")
		return
	if not _forms.is_inputless(kind):
		_pending_add_edit = {"kind": kind, "line": String(lines[0])}
	if before_ordinal < 0:
		add_statement_requested.emit(_section_index, lines)
	else:
		insert_statement_at_requested.emit(_section_index, before_ordinal, lines)


# After a ＋Add inserted its default and the document re-render brought us
# back, find the new statement (last match of the inserted canonical text)
# and put the user mid-edit: expression-shaped kinds open their popover; the
# picker kinds mark the row (and Stage-8 scrolls to it). Skips silently when
# the row folded into a ×N run (ambiguous) or the add was rejected.
func _consume_pending_add_edit(stmts: Array) -> void:
	if _pending_add_edit.is_empty() or not _editable:
		return
	var want_kind := String(_pending_add_edit.get("kind", ""))
	var want_line := String(_pending_add_edit.get("line", ""))
	_pending_add_edit = {}
	for i in range(stmts.size() - 1, -1, -1):
		var st: Dictionary = stmts[i]
		if String(st.get("kind", "")) != want_kind:
			continue
		if String(st.get("text", "")) != want_line:
			continue
		var row := _find_row_by_key({"ordinal": i, "kind": want_kind})
		if row == null:
			return  # folded into a run: nothing unambiguous to open
		row.set_meta("auto_opened", true)
		# Expression-shaped kinds land mid-edit through the row's OWN chip
		# wiring (same apply path a click would take); picker kinds just mark
		# the row -- their dropdowns are one click away and a popup stealing
		# the pointer would be worse than none.
		match want_kind:
			"assign", "expr":
				if row is StmtRowClass and (row as MusicStmtRow)._expr_chip != null:
					(row as MusicStmtRow)._expr_chip.pressed.emit()
			"if":
				if row is IfBlockClass and (row as MusicStmtIfBlock)._cond_chip != null:
					(row as MusicStmtIfBlock)._open_condition_popover()
		return


# --- Tracks-dock drag-drop ---------------------------------------------------

# A track dragged from the Tracks dock drops anywhere on the view and lands a
# play at the canonical anchor (Stage-8 carets make the gap positional).
func _can_drop_data(_pos: Vector2, data) -> bool:
	return _editable and data is Dictionary \
		and String((data as Dictionary).get("kind", "")) == "mus_track"


func _drop_data(_pos: Vector2, data) -> void:
	if not _can_drop_data(_pos, data):
		return
	add_play_requested.emit(StringName(_section_name), int((data as Dictionary).get("index", -1)))


# --- introspection (tests + mount) -------------------------------------------

# The stack's statement rows/blocks in program order (cards/dividers excluded).
func top_level_rows() -> Array:
	var out: Array = []
	for c in _stack.get_children():
		if c.has_meta("ordinal"):
			out.append(c)
	return out


func inputs_card() -> Control:
	for c in _stack.get_children():
		if c.has_meta("inputs_card"):
			return c
	return null


func dispatch_divider() -> Control:
	for c in _stack.get_children():
		if c.has_meta("dispatch_divider"):
			return c
	return null


func empty_hint() -> Control:
	for c in _stack.get_children():
		if c.has_meta("empty_hint"):
			return c
	return null


func add_menu_button() -> MenuButton:
	return _add_menu
