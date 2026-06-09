class_name MusicSectionLogicGraph
extends GraphEdit

# Level-2 of the blueprint editor: one section's program logic as an execution-
# flow node graph (UE-Blueprint style). Each statement is a node wired by exec
# pins; `if` exposes then/else/after pins, `switch` one pin per target, and
# enter/goto/call render as clickable "to-state" nodes that drill back to the map.
# Built purely from NovaMusicScript.get_program_ast(section) -- no new C++. This is
# now the SOLE per-statement authoring surface (the old right-dock inspector is gone).
#
# Editing: when configure_authoring() turns the graph editable, every TOP-LEVEL
# statement node grows a ✎/✕/↑/↓ cluster and the toolbar gains a "＋ Add" palette,
# all routed through the shared MusForms so the emitted lines are canonical. The
# graph never writes bytecode; it emits add/replace/delete/reorder intents the host
# (live_mode) hands to the document's parity-gated, undoable write path. A folded ×N
# run can be expanded in place (⊞ unfold) to edit a single member; a flat if's bodies
# are edited statement-by-statement here, while a non-flat if (a nested if/switch in a
# body) stays a read-only annotation -- none occurs in stock and the forms can't author
# one, so no edit path is lost (there is no raw-script editor; it was removed).
# The frame-setup op (0x38) is NOT rendered at all: it is engine plumbing (it
# reserves the call-argument slots a `callvl` caller pushed), not a statement the
# author placed, so the graph hides it. Its byte offset is carried onto the next
# rendered node (live highlight still resolves at section entry), its ordinal
# still counts (the write path addresses raw AST indices), and the section's
# input count surfaces via section_inputs_count() for the host's header.
#
# Layout is deterministic (left-to-right flow by a monotonic column, branch depth
# down the Y axis) so it never reshuffles and tests can assert topology. Runs of
# identical simple statements (e.g. 21x the same play) fold into one xN node --
# folding is suppressed for nothing; the tool cluster is withheld from a collapsed
# fold but restored on each member once unfolded.

signal statement_selected(section_index: int, ordinal: int)
signal open_section_requested(section_name: StringName)
# Authoring intents (host routes them to the document's parity-gated write path):
signal add_statement_requested(section_index: int, lines: PackedStringArray)
signal replace_statement_requested(section_index: int, ordinal: int, lines: PackedStringArray)
signal delete_statement_requested(section_index: int, ordinal: int)
signal reorder_statement_requested(section_index: int, ordinal: int, direction: int)
signal add_play_requested(section_name: StringName, track: int)
signal author_failed(message: String)

const MusForms = preload("res://modtools/music/mus_forms.gd")
const MusStmtText = preload("res://modtools/music/mus_stmt_text.gd")
const MusDisplayNames = preload("res://modtools/music/mus_display_names.gd")

const COL_W := 250.0
const ROW_H := 130.0

const _EXEC_PIN := Color(0.85, 0.85, 0.85)   # neutral exec wire
const _STATE_PIN := Color(0.55, 0.80, 1.00)  # blue: links to another state
const _IN_PIN := Color(0.55, 0.60, 0.70)
const _ACTIVE_TINT := Color(0.55, 1.00, 0.55)

# Kinds that carry no body of their own and may fold when identical+consecutive.
const _FOLDABLE := ["play", "assign", "incdec", "expr"]
# Kinds that end the exec chain (flow leaves the section / returns to caller).
const _TERMINAL := ["transition", "goto", "return", "done"]

var _section_index: int = -1
var _section_name: String = ""
var _bank_names: Array = []
var _node_seq: int = 0
# Live-highlight registry: [{ "node": GraphNode, "offset": int }].
var _offset_nodes: Array = []
var _active_node: GraphNode = null
# Last-rendered section dict, so a fold/unfold toggle can re-render in place.
var _section_dict: Dictionary = {}
# Set of run-start ordinals the user has expanded (folded ×N -> individual rows).
# Carries across a re-render; reset when a different section is shown.
var _unfolded: Dictionary = {}
# Byte offsets of hidden frame-setup (0x38) rows awaiting the next rendered node,
# so set_active_offset still resolves while the VM pc sits on the hidden op.
var _pending_offsets: Array = []
# Sum of the hidden frame ops' locals counts: how many inputs a `callvl` caller
# hands this state. Surfaced by the host's header, not by a node.
var _section_inputs: int = 0
# Ordinal of the first statement the user may actually move/edit (a hidden
# leading frame op still owns ordinal 0, and the document refuses to touch it).
var _first_movable_ordinal: int = 0

# Stage 3 authoring context (set by the host via configure_authoring).
var _editable: bool = false
var _forms = MusForms.new()
var _section_names: PackedStringArray = PackedStringArray()
var _var_list: Array = []        # [{token:String, label:String}]
var _mus = null                  # NovaMusicScript for expr validation
var _add_menu: MenuButton = null
var _authoring_blocked_reason: String = ""
# Items shown in the per-branch ＋Add popup (MusForms.ADD_ITEMS minus if/switch, so
# bodies stay flat); set when the popup opens, read by _on_branch_add_id.
var _branch_add_items: Array = []


func _ready() -> void:
	# Read-only in Stage 2: no user-drawn connections, but keep pan/zoom/minimap.
	right_disconnects = false
	show_grid = true
	grid_pattern = 1
	minimap_enabled = true
	node_selected.connect(_on_node_selected)


# Render one AST section dict (NovaMusicScript.get_program_ast element).
func show_section(section: Dictionary, bank_names: Array) -> void:
	var new_index := int(section.get("index", -1))
	if new_index != _section_index:
		_unfolded.clear()   # a different section -- expanded folds don't carry over
	_bank_names = bank_names
	_section_index = new_index
	_section_dict = section
	_section_name = String(section.get("name", ""))
	_node_seq = 0
	_offset_nodes = []
	_active_node = null
	_pending_offsets = []
	clear_connections()
	for c in get_children():
		if c is GraphNode:
			remove_child(c)
			c.queue_free()
	var stmts: Array = section.get("statements", [])
	_section_inputs = 0
	_first_movable_ordinal = 0
	for i in range(stmts.size()):
		if String((stmts[i] as Dictionary).get("kind", "")) == "frame_enter":
			_section_inputs += int((stmts[i] as Dictionary).get("locals_count", 0))
			if _first_movable_ordinal == i:
				_first_movable_ordinal = i + 1
	if _is_empty_section_body(stmts):
		_build_empty_hint()
	else:
		_build_seq(stmts, 0, -1)
	_fit_to_view()


func current_section_index() -> int:
	return _section_index


# How many inputs the shown state takes from a caller (the hidden frame-setup
# ops' locals counts). The host surfaces this in its header; 0 for most states.
func section_inputs_count() -> int:
	return _section_inputs


# --- build ---------------------------------------------------------------

# Build a statement sequence at branch depth `depth`, each top-level statement
# tagged with its ordinal (parent_ordinal for nested bodies, so a nested edit can
# route to the owning block). Returns { entry:[node,port], exits:[[node,port]..] }.
func _build_seq(stmts: Array, depth: int, parent_ordinal: int) -> Dictionary:
	var entry: Array = []
	var prev_exits: Array = []
	# Top-level statements own a distinct ordinal; nested (if/switch body) ones
	# share their parent's, so only the top level is individually editable.
	var top := parent_ordinal < 0
	var i := 0
	while i < stmts.size():
		var s: Dictionary = stmts[i]
		var kind := String(s.get("kind", ""))
		# Frame setup (0x38) is engine plumbing, not an authored statement: skip the
		# node entirely but bank its byte offset for the next rendered node (so the
		# live highlight resolves while the pc sits on it) and keep counting i -- the
		# write path addresses raw AST ordinals, so hiding must not renumber.
		if kind == "frame_enter":
			_pending_offsets.append(int(s.get("code_offset", -1)))
			i += 1
			continue
		var ordinal := parent_ordinal if parent_ordinal >= 0 else i
		var run := 1
		if kind in _FOLDABLE:
			while i + run < stmts.size() and _same_simple(stmts[i + run], s):
				run += 1
		# An expanded run (the user pressed ⊞ unfold): render each member as its own
		# node -- every folded member is a distinct top-level statement addressable by
		# its own ordinal, individually editable when the script is editable -- with a
		# ⊟ fold toggle on the first. Below handles run==1 and collapsed runs. Unfolding
		# is display-only, so a read-only script can still expand a run to inspect it.
		if top and run > 1 and _unfolded.has(ordinal):
			for k in range(run):
				var member: Dictionary = stmts[i + k]
				var b := _build_simple(member, depth, ordinal + k, 1)
				if _editable:
					_attach_tools(b["node"], member, ordinal + k, stmts.size())
				if k == 0:
					_attach_fold_toggle(b["node"], ordinal, true, run)
				if entry.is_empty():
					entry = b["entry"]
				for e in prev_exits:
					_wire(e, b["entry"])
				prev_exits = b["exits"]
			i += run
			continue
		var built: Dictionary
		if kind in _FOLDABLE:
			built = _build_simple(s, depth, ordinal, run)
			# A collapsed run of >1 gets a ⊞ unfold toggle so its members can be seen
			# (and, when editable, individually edited) one at a time. The toggle is
			# display-only, so it is offered in read-only mode too.
			if top and run > 1:
				_attach_fold_toggle(built["node"], ordinal, false, run)
			i += run
		else:
			built = _build_stmt(s, depth, ordinal)
			i += 1
		# Edit only unambiguous, individually-addressable statements: a top-level
		# node that isn't a folded ×N run. Folds + nested bodies stay read-only on
		# the graph.
		if _editable and top and run == 1 and built.has("node"):
			_attach_tools(built["node"], s, ordinal, stmts.size())
		if entry.is_empty():
			entry = built["entry"]
		for e in prev_exits:
			_wire(e, built["entry"])
		prev_exits = built["exits"]
	return {"entry": entry, "exits": prev_exits}


# Re-render the current section in place (after a fold/unfold toggle). _unfolded
# persists across this (show_section only clears it on a section change).
func _rerender() -> void:
	if not _section_dict.is_empty():
		show_section(_section_dict, _bank_names)


# Treat a section as empty when nothing AUTHORED remains: hidden frame-setup
# rows and the trailing done don't count (a frame+done body is a callable that
# does nothing yet -- show the add-your-first-statement hint, not a lone Done).
func _is_empty_section_body(stmts: Array) -> bool:
	var authored := 0
	for s in stmts:
		if not (s is Dictionary):
			return false
		var k := String((s as Dictionary).get("kind", ""))
		if k == "frame_enter" or k == "done":
			continue
		authored += 1
	return authored == 0


# The ⊞ unfold / ⊟ fold affordance on a folded run's node. Toggles the run-start
# ordinal in _unfolded and re-renders. Appended as a trailing (slot-less) row so it
# never disturbs the node's exec pin.
func _attach_fold_toggle(gn: GraphNode, start_ordinal: int, expanded: bool, run: int) -> void:
	var b := Button.new()
	b.flat = true
	b.focus_mode = Control.FOCUS_NONE
	if expanded:
		b.text = "⊟ fold"
		b.tooltip_text = "Collapse these %d identical statements back into one row." % run
	else:
		b.text = "⊞ unfold ×%d" % run
		b.tooltip_text = "Expand this run into %d individually-editable rows." % run
	b.pressed.connect(func():
		if _unfolded.has(start_ordinal):
			_unfolded.erase(start_ordinal)
		else:
			_unfolded[start_ordinal] = true
		_rerender())
	gn.add_child(b)


func _build_empty_hint() -> void:
	var gn := GraphNode.new()
	gn.title = "Empty state"
	gn.name = "N_%d" % _node_seq
	gn.custom_minimum_size = Vector2(280, 0)
	gn.add_theme_font_size_override("title_font_size", 14)
	gn.position_offset = Vector2.ZERO
	_node_seq += 1
	add_child(gn)

	var body := Label.new()
	body.text = "No statements yet"
	body.add_theme_color_override("font_color", Color(0.85, 0.9, 1.0))
	gn.add_child(body)

	var detail := Label.new()
	detail.text = _empty_hint_detail_text()
	detail.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	detail.add_theme_color_override("font_color", Color(0.65, 0.68, 0.74))
	gn.add_child(detail)

	var add := Button.new()
	add.text = "ï¼‹ Add statement"
	add.focus_mode = Control.FOCUS_NONE
	add.disabled = not _editable
	add.tooltip_text = "Add the first statement to this state." if _editable else _read_only_add_tooltip()
	add.pressed.connect(func():
		_open_add_popup_at(Vector2i(add.get_global_rect().position + Vector2(0, add.size.y))))
	gn.add_child(add)


func _empty_hint_detail_text() -> String:
	if _bank_names.is_empty():
		return "Import tracks in the Tracks dock, or use Add statement for control flow."
	return "Add the first statement or drag a track from the Tracks dock."


func _build_stmt(s: Dictionary, depth: int, ordinal: int) -> Dictionary:
	var kind := String(s.get("kind", ""))
	match kind:
		"if":
			return _build_if(s, depth, ordinal)
		"switch":
			return _build_switch(s, depth, ordinal)
		"transition", "goto", "call":
			return _build_jump(s, depth, ordinal)
		_:
			return _build_simple(s, depth, ordinal, 1)


# A single-row node (play/assign/incdec/expr/return/yield/nop/done/branch_comment).
# run>1 folds that many identical consecutive statements into one xN node.
func _build_simple(s: Dictionary, depth: int, ordinal: int, run: int) -> Dictionary:
	var kind := String(s.get("kind", ""))
	var gn := _new_node(kind, ordinal, depth)
	var body := Label.new()
	body.text = _body_text(s, kind)
	if run > 1:
		body.text += "   ×%d" % run
	gn.add_child(body)
	if bool(s.get("has_call", false)):
		var b := Label.new()
		b.text = "ƒ %s" % MusDisplayNames.pretty_expr(String(s.get("call_name", "calls a function")), _var_list)
		b.add_theme_color_override("font_color", MusDisplayNames.stmt_color("call"))
		gn.add_child(b)
	_register(gn, int(s.get("code_offset", -1)))
	var terminal := kind in _TERMINAL
	gn.set_slot(0, true, 0, _IN_PIN, not terminal, 0, _EXEC_PIN)
	var exits: Array = [] if terminal else [[gn.name, 0]]
	return {"entry": [gn.name, 0], "exits": exits, "node": gn}


# enter/goto/call -> a clickable "to-state" node that drills into the target.
func _build_jump(s: Dictionary, depth: int, ordinal: int) -> Dictionary:
	var kind := String(s.get("kind", ""))
	var gn := _new_node(kind, ordinal, depth)
	var target := String(s.get("target_name", ""))
	var row := HBoxContainer.new()
	var lbl := Label.new()
	lbl.text = target if target != "" else "(unresolved)"
	lbl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(lbl)
	if target != "":
		var open := Button.new()
		open.text = "open ▸"
		open.tooltip_text = "Open %s's blueprint" % target
		open.focus_mode = Control.FOCUS_NONE
		open.pressed.connect(func(): open_section_requested.emit(StringName(target)))
		row.add_child(open)
	gn.add_child(row)
	_register(gn, int(s.get("code_offset", -1)))
	# call returns (flow continues); enter/goto leave the section (terminal).
	var terminal := kind != "call"
	gn.set_slot(0, true, 0, _IN_PIN, not terminal, 0, _STATE_PIN)
	var exits: Array = [] if terminal else [[gn.name, 0]]
	return {"entry": [gn.name, 0], "exits": exits, "node": gn}


# if/else: in pin on the condition row, then/else/after out pins, branch bodies
# built at depth+1 and rejoined to whatever follows (their exits + the after pin).
func _build_if(s: Dictionary, depth: int, ordinal: int) -> Dictionary:
	var gn := _new_node("if", ordinal, depth)
	# Flat ifs (no nested if/switch in a body) are editable in place: the header ✎
	# edits the condition, each then/else label carries a ＋ to add to that branch,
	# and each body node carries its own ✎/✕/↑/↓. Every mutation regenerates the whole
	# if from its bodies and replaces it as one row. (No stock if is non-flat.)
	var flat := _if_flat_editable(s)
	var cond := Label.new()
	cond.text = "if (%s)" % MusDisplayNames.pretty_expr(
		_unwrap_outer_parens(String(s.get("expr", ""))), _var_list)
	gn.add_child(cond)
	gn.add_child(_branch_header_row("✓ then ▸", Color(0.6, 0.9, 0.6), s, ordinal, "then", flat))
	var else_present := bool(s.get("else_present", false))
	var else_row := -1
	if else_present:
		gn.add_child(_branch_header_row("✗ else ▸", Color(0.9, 0.6, 0.6), s, ordinal, "else", flat))
		else_row = gn.get_child_count() - 1
	var after_lbl := Label.new()
	after_lbl.text = "▾ after"
	after_lbl.add_theme_color_override("font_color", Color(0.7, 0.7, 0.7))
	gn.add_child(after_lbl)
	var after_row := gn.get_child_count() - 1
	_register(gn, int(s.get("code_offset", -1)))
	# Slots: row0 in-only; then row out (port 0); else row out (port 1 if present);
	# after row out (next port). Output port index = order of enabled outputs.
	gn.set_slot(0, true, 0, _IN_PIN, false, 0, _EXEC_PIN)
	gn.set_slot(1, false, 0, _IN_PIN, true, 0, _EXEC_PIN)
	var then_port := 0
	var after_port := 1
	if else_present:
		gn.set_slot(else_row, false, 0, _IN_PIN, true, 0, _EXEC_PIN)
		after_port = 2
	gn.set_slot(after_row, false, 0, _IN_PIN, true, 0, _EXEC_PIN)

	var exits: Array = []
	var then_seq := _build_branch(s, ordinal, "then", depth + 1)
	if not then_seq["entry"].is_empty():
		_wire([gn.name, then_port], then_seq["entry"])
		exits.append_array(then_seq["exits"])
	else:
		exits.append([gn.name, then_port])
	if else_present:
		var else_seq := _build_branch(s, ordinal, "else", depth + 1)
		if not else_seq["entry"].is_empty():
			_wire([gn.name, 1], else_seq["entry"])
			exits.append_array(else_seq["exits"])
		else:
			exits.append([gn.name, 1])
	exits.append([gn.name, after_port])
	return {"entry": [gn.name, 0], "exits": exits, "node": gn}


# --- in-place if-body editing (regenerate the whole if, replace as one row) ---

# A flat if has only leaf statements in its bodies (no nested if/switch/branch_comment).
# Only flat ifs are editable in place, because regeneration rebuilds the body from each
# statement's rendered TEXT -- a nested block's text is just its header, so a non-flat
# body would be truncated. No stock script has a non-flat if.
func _if_flat_editable(s: Dictionary) -> bool:
	if not _editable:
		return false
	for branch in ["then", "else"]:
		for st in s.get(branch, []):
			var k := String(st.get("kind", ""))
			if k == "if" or k == "switch" or k == "branch_comment":
				return false
	return true


# A then/else header label, plus a ＋ (add to this branch) when the if is editable.
func _branch_header_row(label_text: String, color: Color, if_dict: Dictionary, if_ordinal: int, branch: String, flat: bool) -> Control:
	var row := HBoxContainer.new()
	var lbl := Label.new()
	lbl.text = label_text
	lbl.add_theme_color_override("font_color", color)
	lbl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(lbl)
	if flat:
		var add := _tool_btn("＋", "Add a statement to this branch")
		add.pressed.connect(func(): _open_branch_add(if_dict, if_ordinal, branch))
		row.add_child(add)
	return row


# Build one branch's body nodes, wired in sequence; each leaf gets branch-aware tools
# (when the if is flat-editable). Mirrors _build_seq but addresses by (branch, index)
# into the owning if rather than by a top-level ordinal.
func _build_branch(if_dict: Dictionary, if_ordinal: int, branch: String, depth: int) -> Dictionary:
	var stmts: Array = if_dict.get(branch, [])
	var entry: Array = []
	var prev_exits: Array = []
	var flat := _if_flat_editable(if_dict)
	for i in range(stmts.size()):
		var st: Dictionary = stmts[i]
		if String(st.get("kind", "")) == "frame_enter":
			# Engine plumbing stays hidden inside bodies too (never occurs in
			# stock, but a body row must not render as an editable statement).
			_pending_offsets.append(int(st.get("code_offset", -1)))
			continue
		var built := _build_stmt(st, depth, if_ordinal)
		if flat and built.has("node"):
			_attach_branch_tools(built["node"], if_dict, if_ordinal, branch, i, st, stmts.size())
		if entry.is_empty():
			entry = built["entry"]
		for e in prev_exits:
			_wire(e, built["entry"])
		prev_exits = built["exits"]
	return {"entry": entry, "exits": prev_exits}


func _attach_branch_tools(gn: GraphNode, if_dict: Dictionary, if_ordinal: int, branch: String, idx: int, st: Dictionary, statement_count: int) -> void:
	var kind := String(st.get("kind", ""))
	var box := HBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	if kind != "return" and kind != "yield" and kind != "done" and kind != "nop":
		var edit := _tool_btn("✎", "Edit")
		edit.pressed.connect(func(): _open_branch_edit(if_dict, if_ordinal, branch, idx, st))
		box.add_child(edit)
	var del := _tool_btn("✕", "Delete")
	del.pressed.connect(func(): _apply_branch_mutation(if_dict, if_ordinal, branch, idx, "delete", ""))
	box.add_child(del)
	var can_move_up := idx > 0
	var up := _tool_btn("↑", "Move up" if can_move_up else "Already the first branch statement")
	up.disabled = not can_move_up
	if can_move_up:
		up.pressed.connect(func(): _apply_branch_mutation(if_dict, if_ordinal, branch, idx, "up", ""))
	box.add_child(up)
	var can_move_down := idx < statement_count - 1
	var down := _tool_btn("↓", "Move down" if can_move_down else "Already the last branch statement")
	down.disabled = not can_move_down
	if can_move_down:
		down.pressed.connect(func(): _apply_branch_mutation(if_dict, if_ordinal, branch, idx, "down", ""))
	box.add_child(down)
	for b in box.get_children():
		(b as Control).custom_minimum_size = Vector2(26, 0)
	gn.add_child(box)


func _open_branch_edit(if_dict: Dictionary, if_ordinal: int, branch: String, idx: int, st: Dictionary) -> void:
	var kind := String(st.get("kind", ""))
	_forms.configure(_section_names, _var_list, _mus, _bank_names)
	_forms.open_form(self, kind, st, true, func(lines: PackedStringArray):
		if lines.is_empty():
			author_failed.emit("Fill in the fields first")
		else:
			_apply_branch_mutation(if_dict, if_ordinal, branch, idx, "replace", String(lines[0])))


func _open_branch_add(if_dict: Dictionary, if_ordinal: int, branch: String) -> void:
	var pop := PopupMenu.new()
	_branch_add_items = []
	for item in MusForms.ADD_ITEMS:
		var k := String(item[1])
		if k == "if" or k == "switch":
			continue  # bodies stay flat (no nested blocks)
		_branch_add_items.append(item)
	for i in range(_branch_add_items.size()):
		pop.add_item(String(_branch_add_items[i][0]), i)
	add_child(pop)
	pop.id_pressed.connect(_on_branch_add_id.bind(if_dict, if_ordinal, branch))
	pop.popup_hide.connect(pop.queue_free)
	pop.position = Vector2i(get_global_mouse_position())
	pop.reset_size()
	pop.popup()


func _on_branch_add_id(id: int, if_dict: Dictionary, if_ordinal: int, branch: String) -> void:
	if id < 0 or id >= _branch_add_items.size():
		return
	var kind := String(_branch_add_items[id][1])
	if _forms.is_inputless(kind):
		var l := _forms.simple_lines(kind)
		if not l.is_empty():
			_apply_branch_mutation(if_dict, if_ordinal, branch, -1, "append", String(l[0]))
		return
	_forms.configure(_section_names, _var_list, _mus, _bank_names)
	_forms.open_form(self, kind, {}, false, func(lines: PackedStringArray):
		if not lines.is_empty():
			_apply_branch_mutation(if_dict, if_ordinal, branch, -1, "append", String(lines[0])))


# Apply a mutation to one branch's flat text list, then regenerate + replace the whole
# if. idx<0 for append. Reads the CURRENT body texts from the AST dict (names-less,
# recompilable) so unedited statements survive verbatim.
func _apply_branch_mutation(if_dict: Dictionary, if_ordinal: int, branch: String, idx: int, op: String, new_line: String) -> void:
	var then_t := _branch_texts(if_dict, "then")
	var else_t := _branch_texts(if_dict, "else")
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
	_emit_if_replace(if_dict, if_ordinal, String(if_dict.get("expr", "")), then_t, else_t)


func _branch_texts(if_dict: Dictionary, branch: String) -> Array:
	var out := []
	for st in if_dict.get(branch, []):
		out.append(String(st.get("text", "")))
	return out


func _emit_if_replace(if_dict: Dictionary, if_ordinal: int, cond: String, then_texts: Array, else_texts: Array) -> void:
	var with_else := bool(if_dict.get("else_present", false))
	var lines := MusStmtText.if_block(cond, PackedStringArray(then_texts), with_else, PackedStringArray(else_texts))
	replace_statement_requested.emit(_section_index, if_ordinal, lines)


# on (selector) action t0 t1 ... : in pin on the selector row, one out pin per
# target (each wired to a to-state / play node), plus a "default" out pin = the
# out-of-range fall-through. This is where the "Main Main Main" jump table reads
# as labelled indexed pins (0 -> Main, 1 -> Main, ...).
func _build_switch(s: Dictionary, depth: int, ordinal: int) -> Dictionary:
	var gn := _new_node("switch", ordinal, depth)
	var action := String(s.get("action", "enter"))
	var head := Label.new()
	var action_word := "go to"
	if action == "play":
		action_word = "play"
	elif action == "goto":
		action_word = "jump to"
	head.text = "by (%s) → %s" % [MusDisplayNames.pretty_expr(
		_unwrap_outer_parens(String(s.get("expr", ""))), _var_list), action_word]
	head.tooltip_text = "The value picks the target: 0 picks the first, 1 the second, ..."
	gn.add_child(head)
	var targets: Array = s.get("targets", [])
	gn.set_slot(0, true, 0, _IN_PIN, false, 0, _EXEC_PIN)
	# One labelled pin row per target.
	var target_rows: Array = []
	for t in range(targets.size()):
		var tlbl := Label.new()
		var tname := String(targets[t].get("name", ""))
		if action == "play":
			tname = _track_name(int(targets[t].get("track", -1)))
		tlbl.text = "%d → %s" % [t, tname]
		gn.add_child(tlbl)
		var row := gn.get_child_count() - 1
		gn.set_slot(row, false, 0, _IN_PIN, true, 0, _STATE_PIN)
		target_rows.append(row)
	var def_lbl := Label.new()
	def_lbl.text = "▾ default"
	def_lbl.add_theme_color_override("font_color", Color(0.7, 0.7, 0.7))
	gn.add_child(def_lbl)
	gn.set_slot(gn.get_child_count() - 1, false, 0, _IN_PIN, true, 0, _EXEC_PIN)
	_register(gn, int(s.get("code_offset", -1)))
	# Wire each target pin to a terminal to-state node.
	for t in range(targets.size()):
		var tname := String(targets[t].get("name", ""))
		if action != "play" and tname != "":
			var tn := _new_node("transition", ordinal, depth + 1)
			var trow := HBoxContainer.new()
			var l := Label.new()
			l.text = tname
			l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			trow.add_child(l)
			var open := Button.new()
			open.text = "open ▸"
			open.focus_mode = Control.FOCUS_NONE
			open.pressed.connect(func(): open_section_requested.emit(StringName(tname)))
			trow.add_child(open)
			tn.add_child(trow)
			tn.set_slot(0, true, 0, _IN_PIN, false, 0, _STATE_PIN)
			_wire([gn.name, t], [tn.name, 0])
	# default pin = the last output port (index == targets.size()).
	return {"entry": [gn.name, 0], "exits": [[gn.name, targets.size()]], "node": gn}


# --- node + wiring helpers ----------------------------------------------

func _new_node(kind: String, ordinal: int, depth: int) -> GraphNode:
	var gn := GraphNode.new()
	gn.title = MusDisplayNames.stmt_title(kind)
	gn.tooltip_text = MusDisplayNames.stmt_tooltip(kind)
	gn.name = "N_%d" % _node_seq
	gn.custom_minimum_size = Vector2(200, 0)
	gn.add_theme_font_size_override("title_font_size", 14)
	gn.set_meta("ordinal", ordinal)
	gn.set_meta("section_index", _section_index)
	# Column flows left-to-right by creation order; depth pushes branches down.
	gn.position_offset = Vector2(_node_seq * COL_W, depth * ROW_H)
	_node_seq += 1
	add_child(gn)
	return gn


func _wire(from_pin: Array, to_pin: Array) -> void:
	if from_pin.is_empty() or to_pin.is_empty():
		return
	connect_node(String(from_pin[0]), int(from_pin[1]), String(to_pin[0]), int(to_pin[1]))


func _register(gn: GraphNode, offset: int) -> void:
	# Adopt any hidden frame-setup offsets banked since the last node, so the
	# live highlight lights this (first visible) node while the pc is on them.
	for off in _pending_offsets:
		if int(off) >= 0:
			_offset_nodes.append({"node": gn, "offset": int(off)})
	_pending_offsets.clear()
	if offset >= 0:
		_offset_nodes.append({"node": gn, "offset": offset})


func _body_text(s: Dictionary, kind: String) -> String:
	match kind:
		"play":
			return _track_name(int(s.get("track", -1)))
		"return", "yield", "nop", "done":
			return MusDisplayNames.stmt_title(kind)
		_:
			# Display-only prettify: friendly function names + variable names. The
			# canonical text stays in the AST dict for the write path.
			return MusDisplayNames.pretty_expr(String(s.get("text", "")), _var_list)


func _track_name(track: int) -> String:
	if track >= 0 and track < _bank_names.size() and String(_bank_names[track]) != "":
		return String(_bank_names[track])
	return "sound_%d" % track


func _same_simple(a: Dictionary, b: Dictionary) -> bool:
	return String(a.get("kind", "")) == String(b.get("kind", "")) \
		and String(a.get("text", "")) == String(b.get("text", ""))


# Strip one fully-enclosing matched paren pair for display (the decompiler wraps
# selectors once + binops self-parenthesise). Display only; mirrors the inspector.
func _unwrap_outer_parens(expr: String) -> String:
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


# --- selection + live highlight -----------------------------------------

func _on_node_selected(node: Node) -> void:
	if node is GraphNode and (node as GraphNode).has_meta("ordinal"):
		statement_selected.emit(_section_index, int((node as GraphNode).get_meta("ordinal")))


# Light the node whose statement is at-or-just-before the VM pc (greatest recorded
# offset <= pc), clearing the previous. pc < 0 clears. Modulate-only (no rebuild).
func set_active_offset(pc: int) -> void:
	var best: GraphNode = null
	var best_off := -1
	if pc >= 0:
		for r in _offset_nodes:
			var off := int(r["offset"])
			if off >= 0 and off <= pc and off > best_off:
				best_off = off
				best = r["node"]
	if best == _active_node:
		return
	if _active_node != null and is_instance_valid(_active_node):
		_active_node.modulate = Color(1, 1, 1)
	if best != null:
		best.modulate = _ACTIVE_TINT
	_active_node = best


# Zoom + center to fit all nodes. Synchronous: positions are deterministic and a
# not-yet-laid-out node (size 0) is estimated from COL_W/ROW_H, so we never await
# a frame (a deferred await could resume on a freed graph). Skips while the
# GraphEdit itself has no size yet; the next show_section fits once it's laid out.
func _fit_to_view() -> void:
	if size.x <= 0.0 or size.y <= 0.0:
		return
	var first := true
	var mn := Vector2.ZERO
	var mx := Vector2.ZERO
	for c in get_children():
		if not (c is GraphNode):
			continue
		var gn: GraphNode = c
		var p := gn.position_offset
		var sz := gn.size
		if sz.x <= 0.0:
			sz = Vector2(COL_W, ROW_H)
		if first:
			mn = p
			mx = p + sz
			first = false
		else:
			mn = Vector2(minf(mn.x, p.x), minf(mn.y, p.y))
			mx = Vector2(maxf(mx.x, p.x + sz.x), maxf(mx.y, p.y + sz.y))
	if first:
		return
	var content := mx - mn
	if content.x <= 0.0 or content.y <= 0.0:
		return
	var margin := 80.0
	var z := minf((size.x - margin) / content.x, (size.y - margin) / content.y)
	z = clampf(z, 0.2, 1.0)
	zoom = z
	scroll_offset = (mn + mx) * 0.5 * z - size * 0.5


# --- Stage 3: authoring -------------------------------------------------

# Supply the context the construct forms need + flip editing on/off. The host calls
# this on each drill-in (and on every document.changed refresh) so the palette /
# forms see the current section list, variable list, bank names, and whether the
# script presently compiles (editable). MUST run before show_section, since the
# per-node tool clusters are built with this mode.
func configure_authoring(section_names: PackedStringArray, var_list: Array, mus, bank_names: Array, editable: bool, blocked_reason: String = "") -> void:
	_section_names = section_names
	_var_list = var_list
	_mus = mus
	_bank_names = bank_names
	_editable = editable
	_authoring_blocked_reason = blocked_reason
	_forms.configure(section_names, var_list, mus, bank_names)
	_ensure_add_menu()
	if _add_menu != null:
		_add_menu.visible = true
		_add_menu.disabled = not editable
		_add_menu.tooltip_text = "Add a statement to this state." if editable else _read_only_add_tooltip()


func _read_only_add_tooltip() -> String:
	var reason := _authoring_blocked_reason.strip_edges()
	if reason != "":
		return reason
	return "Authoring is unavailable for this script."


# Mount the "＋ Add" palette into the GraphEdit's built-in toolbar (top-right),
# once. Items mirror the inspector palette (MusForms.ADD_ITEMS) so both add the
# same constructs.
func _ensure_add_menu() -> void:
	if _add_menu != null:
		return
	_add_menu = MenuButton.new()
	_add_menu.text = "＋ Add"
	_add_menu.tooltip_text = "Add a statement to this state."
	_add_menu.focus_mode = Control.FOCUS_NONE
	var pop := _add_menu.get_popup()
	for i in range(MusForms.ADD_ITEMS.size()):
		pop.add_item(String(MusForms.ADD_ITEMS[i][0]), i)
	pop.id_pressed.connect(_on_add_palette_id)
	get_menu_hbox().add_child(_add_menu)


func _on_add_palette_id(id: int) -> void:
	if id < 0 or id >= MusForms.ADD_ITEMS.size():
		return
	var kind := String(MusForms.ADD_ITEMS[id][1])
	if _forms.is_inputless(kind):
		add_statement_requested.emit(_section_index, _forms.simple_lines(kind))
		return
	var sidx := _section_index
	_forms.configure(_section_names, _var_list, _mus, _bank_names)
	_forms.open_form(self, kind, {}, false, func(lines: PackedStringArray):
		if lines.is_empty():
			author_failed.emit("Fill in the fields first")
		else:
			add_statement_requested.emit(sidx, lines))


# The per-node ✎/✕/↑/↓ cluster, appended as a (slot-less) trailing row so it never
# disturbs the exec/branch pin ports built above it. done/nop carry no tools;
# return/yield carry no ✎ (nothing to edit); a branch_comment (an undetected branch)
# and a non-flat if are read-only annotations with no ✎ either. The ✕/↑/↓ still
# delete/move the whole statement. A flat if's ✎ edits its condition; everything
# else opens its structured form as a replace.
func _open_add_popup_at(pos: Vector2i) -> void:
	if not _editable:
		return
	var pop := PopupMenu.new()
	for i in range(MusForms.ADD_ITEMS.size()):
		pop.add_item(String(MusForms.ADD_ITEMS[i][0]), i)
	add_child(pop)
	pop.id_pressed.connect(_on_add_palette_id)
	pop.popup_hide.connect(pop.queue_free)
	pop.position = pos
	pop.reset_size()
	pop.popup()


func _attach_tools(gn: GraphNode, s: Dictionary, ordinal: int, statement_count: int) -> void:
	var kind := String(s.get("kind", ""))
	if kind == "done" or kind == "nop":
		return
	var box := HBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	var has_edit := kind != "return" and kind != "yield" and kind != "branch_comment"
	if kind == "if" and not _if_flat_editable(s):
		has_edit = false
	if has_edit:
		var edit := _tool_btn("✎", "Edit")
		edit.pressed.connect(func(): _open_edit(kind, ordinal, s))
		box.add_child(edit)
	var del := _tool_btn("✕", "Delete")
	del.pressed.connect(func(): delete_statement_requested.emit(_section_index, ordinal))
	box.add_child(del)
	# First MOVABLE ordinal, not 0: a hidden leading frame-setup row still owns
	# ordinal 0 and the document refuses to swap with it -- without this guard the
	# first visible statement would offer ↑ and fail with an unexplained flash.
	var can_move_up := ordinal > _first_movable_ordinal
	var up := _tool_btn("↑", "Move up" if can_move_up else "Already the first statement")
	up.disabled = not can_move_up
	if can_move_up:
		up.pressed.connect(func(): reorder_statement_requested.emit(_section_index, ordinal, -1))
	box.add_child(up)
	var can_move_down := ordinal < statement_count - 1
	var down := _tool_btn("↓", "Move down" if can_move_down else "Already the last statement")
	down.disabled = not can_move_down
	if can_move_down:
		down.pressed.connect(func(): reorder_statement_requested.emit(_section_index, ordinal, 1))
	box.add_child(down)
	for b in box.get_children():
		(b as Control).custom_minimum_size = Vector2(26, 0)
	gn.add_child(box)


func _open_edit(kind: String, ordinal: int, s: Dictionary) -> void:
	# A flat if's ✎ edits just its CONDITION in place; its body statements are edited
	# on their own nodes. The whole if is regenerated from its bodies + the new
	# condition and replaced as one row. (Non-flat ifs / branch_comments get no ✎, so
	# _open_edit is only ever reached for a flat if or a form-backed leaf kind.)
	if kind == "if":
		_forms.configure(_section_names, _var_list, _mus, _bank_names)
		_forms.open_form(self, "condition", s, true, func(lines: PackedStringArray):
			if lines.is_empty():
				author_failed.emit("Enter a condition")
			else:
				_emit_if_replace(s, ordinal, String(lines[0]),
					_branch_texts(s, "then"), _branch_texts(s, "else")))
		return
	var sidx := _section_index
	_forms.configure(_section_names, _var_list, _mus, _bank_names)
	_forms.open_form(self, kind, s, true, func(lines: PackedStringArray):
		if lines.is_empty():
			author_failed.emit("Fill in the fields first")
		else:
			replace_statement_requested.emit(sidx, ordinal, lines))


func _tool_btn(glyph: String, tip: String) -> Button:
	var b := Button.new()
	b.text = glyph
	b.tooltip_text = tip
	b.flat = true
	b.focus_mode = Control.FOCUS_NONE
	return b


# Drop sink for the Tracks dock: a {kind:"mus_track", index} payload adds a play to
# the shown state (only while editable). Mirrors the inspector's drop sink so a
# track dragged onto the blueprint canvas behaves like one dropped on the state.
# (Real drops land on empty canvas; dropping onto a node falls through to the node.)
func _can_drop_data(_at_position: Vector2, data: Variant) -> bool:
	return _editable and _section_name != "" \
		and data is Dictionary and String((data as Dictionary).get("kind", "")) == "mus_track"


func _drop_data(_at_position: Vector2, data: Variant) -> void:
	if not (data is Dictionary):
		return
	var track := int((data as Dictionary).get("index", -1))
	if track >= 0:
		add_play_requested.emit(StringName(_section_name), track)
