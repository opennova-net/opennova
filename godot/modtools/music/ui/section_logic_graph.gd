class_name MusicSectionLogicGraph
extends GraphEdit

# Level-2 of the blueprint editor: one section's program logic as an execution-
# flow node graph (UE-Blueprint style). Each statement is a node wired by exec
# pins; `if` exposes then/else/after pins, `switch` one pin per target, and
# enter/goto/call render as clickable "to-state" nodes that drill back to the map.
# Built purely from NovaMusicScript.get_program_ast(section) -- no new C++. The
# graph is read-only here (Stage 2); editing intents land in Stage 3.
#
# Layout is deterministic (left-to-right flow by a monotonic column, branch depth
# down the Y axis) so it never reshuffles and tests can assert topology. Runs of
# identical simple statements (e.g. 21x the same play) fold into one xN node.

signal statement_selected(section_index: int, ordinal: int)
signal open_section_requested(section_name: StringName)

const COL_W := 250.0
const ROW_H := 130.0

# glyph + accent colour per statement kind (mirrors the inspector's _ICON family
# so the two surfaces read the same).
const _KIND_STYLE := {
	"play": ["♪ Play", Color(0.60, 0.90, 0.60)],
	"transition": ["→ Enter", Color(0.55, 0.80, 1.00)],
	"goto": ["↪ Goto", Color(0.55, 0.80, 1.00)],
	"call": ["ƒ Call", Color(0.80, 0.65, 1.00)],
	"return": ["⏎ Return", Color(0.70, 0.70, 0.70)],
	"yield": ["⏸ Yield", Color(0.70, 0.70, 0.70)],
	"nop": ["· Nop", Color(0.50, 0.50, 0.50)],
	"done": ["▪ Done", Color(0.50, 0.50, 0.50)],
	"assign": ["✎ Set", Color(1.00, 0.78, 0.40)],
	"incdec": ["± Var", Color(1.00, 0.78, 0.40)],
	"expr": ["ƒ Run", Color(0.80, 0.65, 1.00)],
	"if": ["◇ If", Color(1.00, 0.85, 0.45)],
	"switch": ["⋔ On", Color(0.50, 0.85, 0.90)],
	"branch_comment": ["⌥ Branch", Color(0.55, 0.55, 0.55)],
}

const _EXEC_PIN := Color(0.85, 0.85, 0.85)   # neutral exec wire
const _STATE_PIN := Color(0.55, 0.80, 1.00)  # blue: links to another state
const _IN_PIN := Color(0.55, 0.60, 0.70)
const _ACTIVE_TINT := Color(0.55, 1.00, 0.55)

# Kinds that carry no body of their own and may fold when identical+consecutive.
const _FOLDABLE := ["play", "assign", "incdec", "expr"]
# Kinds that end the exec chain (flow leaves the section / returns to caller).
const _TERMINAL := ["transition", "goto", "return", "done"]

var _section_index: int = -1
var _bank_names: Array = []
var _node_seq: int = 0
# Live-highlight registry: [{ "node": GraphNode, "offset": int }].
var _offset_nodes: Array = []
var _active_node: GraphNode = null


func _ready() -> void:
	# Read-only in Stage 2: no user-drawn connections, but keep pan/zoom/minimap.
	right_disconnects = false
	show_grid = true
	grid_pattern = 1
	minimap_enabled = true
	node_selected.connect(_on_node_selected)


# Render one AST section dict (NovaMusicScript.get_program_ast element).
func show_section(section: Dictionary, bank_names: Array) -> void:
	_bank_names = bank_names
	_section_index = int(section.get("index", -1))
	_node_seq = 0
	_offset_nodes = []
	_active_node = null
	clear_connections()
	for c in get_children():
		if c is GraphNode:
			remove_child(c)
			c.queue_free()
	var stmts: Array = section.get("statements", [])
	_build_seq(stmts, 0, -1)
	_fit_to_view()


func current_section_index() -> int:
	return _section_index


# --- build ---------------------------------------------------------------

# Build a statement sequence at branch depth `depth`, each top-level statement
# tagged with its ordinal (parent_ordinal for nested bodies, so a nested edit can
# route to the owning block). Returns { entry:[node,port], exits:[[node,port]..] }.
func _build_seq(stmts: Array, depth: int, parent_ordinal: int) -> Dictionary:
	var entry: Array = []
	var prev_exits: Array = []
	var i := 0
	while i < stmts.size():
		var s: Dictionary = stmts[i]
		var kind := String(s.get("kind", ""))
		var ordinal := parent_ordinal if parent_ordinal >= 0 else i
		var built: Dictionary
		if kind in _FOLDABLE:
			var run := 1
			while i + run < stmts.size() and _same_simple(stmts[i + run], s):
				run += 1
			built = _build_simple(s, depth, ordinal, run)
			i += run
		else:
			built = _build_stmt(s, depth, ordinal)
			i += 1
		if entry.is_empty():
			entry = built["entry"]
		for e in prev_exits:
			_wire(e, built["entry"])
		prev_exits = built["exits"]
	return {"entry": entry, "exits": prev_exits}


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
		b.text = "ƒ %s" % String(s.get("call_name", "calls a function"))
		b.add_theme_color_override("font_color", _KIND_STYLE["call"][1])
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
	var cond := Label.new()
	cond.text = "if (%s)" % _unwrap_outer_parens(String(s.get("expr", "")))
	gn.add_child(cond)
	var then_lbl := Label.new()
	then_lbl.text = "✓ then ▸"
	then_lbl.add_theme_color_override("font_color", Color(0.6, 0.9, 0.6))
	gn.add_child(then_lbl)
	var else_present := bool(s.get("else_present", false))
	var else_row := -1
	if else_present:
		var else_lbl := Label.new()
		else_lbl.text = "✗ else ▸"
		else_lbl.add_theme_color_override("font_color", Color(0.9, 0.6, 0.6))
		gn.add_child(else_lbl)
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
	var then_seq := _build_seq(s.get("then", []), depth + 1, ordinal)
	if not then_seq["entry"].is_empty():
		_wire([gn.name, then_port], then_seq["entry"])
		exits.append_array(then_seq["exits"])
	else:
		exits.append([gn.name, then_port])
	if else_present:
		var else_seq := _build_seq(s.get("else", []), depth + 1, ordinal)
		if not else_seq["entry"].is_empty():
			_wire([gn.name, 1], else_seq["entry"])
			exits.append_array(else_seq["exits"])
		else:
			exits.append([gn.name, 1])
	exits.append([gn.name, after_port])
	return {"entry": [gn.name, 0], "exits": exits, "node": gn}


# on (selector) action t0 t1 ... : in pin on the selector row, one out pin per
# target (each wired to a to-state / play node), plus a "default" out pin = the
# out-of-range fall-through. This is where the "Main Main Main" jump table reads
# as labelled indexed pins (0 -> Main, 1 -> Main, ...).
func _build_switch(s: Dictionary, depth: int, ordinal: int) -> Dictionary:
	var gn := _new_node("switch", ordinal, depth)
	var action := String(s.get("action", "enter"))
	var head := Label.new()
	head.text = "on (%s) → %s" % [_unwrap_outer_parens(String(s.get("expr", ""))), action]
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
	var style: Array = _KIND_STYLE.get(kind, ["•", Color(0.7, 0.7, 0.7)])
	var gn := GraphNode.new()
	gn.title = String(style[0])
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
	if offset >= 0:
		_offset_nodes.append({"node": gn, "offset": offset})


func _body_text(s: Dictionary, kind: String) -> String:
	match kind:
		"play":
			return _track_name(int(s.get("track", -1)))
		"return", "yield", "nop", "done":
			return String(_KIND_STYLE.get(kind, ["", Color()])[0])
		_:
			return String(s.get("text", ""))


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
