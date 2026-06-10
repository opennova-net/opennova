extends GutTest

# Stage 3 of the blueprint editor: editing on the section logic graph. Each
# TOP-LEVEL statement node grows a ✎/✕/↑/↓ cluster; the toolbar grows a ＋Add
# palette; a track dropped on the canvas adds a play. All of these emit the SAME
# add/replace/delete/reorder/add-play intents the inspector emits, routed through
# the shared MusForms (so the lines are byte-identical) into the document's
# parity-gated, undoable write path. Topology + intent payloads are asserted on
# synthetic AST sections; the document round-trip (intent -> mutate -> undo) is
# exercised end-to-end against the real jo_gamemus pair.

const MusicSectionLogicGraph = preload("res://modtools/music/ui/section_logic_graph.gd")
const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const MusForms = preload("res://modtools/music/mus_forms.gd")
const MusStmtText = preload("res://modtools/music/mus_stmt_text.gd")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const PAIR_BANK := "user://music_le_pair.sbf"
const PAIR_SCRIPT := "user://music_le_pair.bin"


func after_each() -> void:
	_rm(PAIR_BANK)
	_rm(PAIR_SCRIPT)


# --- helpers -------------------------------------------------------------

func _vars() -> Array:
	var out := []
	for i in range(17):
		out.append({"token": "Var%02d" % i, "label": "Var%02d" % i})
	return out


# An editable graph for a synthetic section (configure BEFORE show, so the tool
# clusters build in editable mode).
func _egraph(section: Dictionary, bank_names: Array = []) -> GraphEdit:
	var g = MusicSectionLogicGraph.new()
	g.size = Vector2(960, 720)
	add_child_autofree(g)
	g.configure_authoring(PackedStringArray(["S", "A", "B", "Next"]), _vars(), null, bank_names, true)
	g.show_section(section, bank_names)
	return g


func _nodes(g) -> Array:
	var out := []
	for c in g.get_children():
		if c is GraphNode:
			out.append(c)
	return out


func _descendants(node: Node, out: Array) -> void:
	for c in node.get_children():
		out.append(c)
		_descendants(c, out)


func _node_titled(g, needle: String) -> GraphNode:
	for n in _nodes(g):
		if String(n.title).contains(needle):
			return n
	return null


func _button_in(node: Node, text: String) -> Button:
	var all := []
	_descendants(node, all)
	for c in all:
		if c is Button and String((c as Button).text) == text:
			return c
	return null


func _button_containing(node: Node, text: String) -> Button:
	var all := []
	_descendants(node, all)
	for c in all:
		if c is Button and String((c as Button).text).contains(text):
			return c
	return null


func _button_starting(node: Node, prefix: String) -> Button:
	var all := []
	_descendants(node, all)
	for c in all:
		if c is Button and String((c as Button).text).begins_with(prefix):
			return c
	return null


func _nodes_titled(g, needle: String) -> Array:
	var out := []
	for n in _nodes(g):
		if String(n.title).contains(needle):
			out.append(n)
	out.sort_custom(func(a, b): return a.position_offset.x < b.position_offset.x)
	return out


func _node_with_ordinal(g, ordinal: int) -> GraphNode:
	for n in _nodes(g):
		if int(n.get_meta("ordinal", -1)) == ordinal:
			return n
	return null


# --- per-node tool clusters ---------------------------------------------

func test_top_level_statement_gets_edit_delete_tools():
	var g := _egraph({
		"index": 5, "name": "S", "statements": [
			{"kind": "play", "code_offset": 0, "track": 0, "text": "play sound_0"},
			{"kind": "transition", "code_offset": 2, "target_name": "A", "target_section": 1, "text": "enter A"},
		],
	})
	await get_tree().process_frame
	var play := _node_titled(g, "Play")
	assert_not_null(play, "play node present")
	assert_not_null(_button_in(play, "✎"), "a top-level play has an Edit tool")
	assert_not_null(_button_in(play, "✕"), "a top-level play has a Delete tool")
	assert_not_null(_button_in(play, "↑"), "a top-level play has a Move-up tool")
	assert_not_null(_button_in(play, "↓"), "a top-level play has a Move-down tool")


func test_read_only_graph_has_no_tools():
	# Without configure_authoring (editable defaults false) the graph stays read-only.
	var g = MusicSectionLogicGraph.new()
	g.size = Vector2(800, 600)
	add_child_autofree(g)
	g.show_section({"index": 0, "name": "S", "statements": [
		{"kind": "play", "code_offset": 0, "track": 0, "text": "play sound_0"}]}, [])
	await get_tree().process_frame
	var play := _node_titled(g, "Play")
	assert_not_null(play, "play node present")
	assert_null(_button_in(play, "✕"), "a read-only graph node carries no edit tools")


func test_folded_run_has_no_tools():
	# 5 identical plays fold to one ×5 node, which is intentionally not individually
	# editable (which of the 5 would an edit touch?). It stays read-only on the graph.
	var stmts := []
	for i in range(5):
		stmts.append({"kind": "play", "code_offset": i * 2, "track": 0, "text": "play sound_0"})
	var g := _egraph({"index": 0, "name": "S", "statements": stmts})
	await get_tree().process_frame
	var play := _node_titled(g, "Play")
	assert_not_null(play, "the folded play node is present")
	assert_true(String((play.get_child(0) as Label).text).contains("×5"), "node is the folded ×5 run")
	assert_null(_button_in(play, "✕"), "a folded ×N node carries no edit tools")


func test_flat_if_body_statement_is_editable_in_place():
	# A leaf statement inside a flat if's then/else is now individually editable on
	# the graph (Phase B): it carries its own ✎/✕/↑/↓; deleting it regenerates the
	# whole if and emits a replace of the if's row (not a top-level delete).
	var g := _egraph({
		"index": 0, "name": "S", "statements": [
			{"kind": "if", "code_offset": 0, "expr": "(Var01 == 0)", "else_present": false,
				"then": [{"kind": "transition", "code_offset": 4, "target_name": "A", "target_section": 1, "text": "enter A"}],
				"else": []},
		],
	})
	await get_tree().process_frame
	var enter := _node_titled(g, "Go to state")
	assert_not_null(enter, "nested enter node present")
	assert_not_null(_button_in(enter, "✕"), "a flat if-body leaf now carries a delete tool")
	assert_not_null(_button_in(enter, "✎"), "a flat if-body leaf now carries an edit tool")


# --- tool buttons emit the right intents --------------------------------

func test_delete_button_emits_intent():
	var g := _egraph({
		"index": 5, "name": "S", "statements": [
			{"kind": "play", "code_offset": 0, "track": 0, "text": "play sound_0"},
		],
	})
	await get_tree().process_frame
	var captured := []
	g.delete_statement_requested.connect(func(si, o): captured.append([si, o]))
	_button_in(_node_titled(g, "Play"), "✕").pressed.emit()
	assert_eq(captured, [[5, 0]], "✕ emits delete_statement_requested(section_index, ordinal)")


func test_reorder_buttons_emit_direction_from_middle_statement():
	var g := _egraph({
		"index": 2, "name": "S", "statements": [
			{"kind": "play", "code_offset": 0, "track": 0, "text": "play sound_0"},
			{"kind": "assign", "code_offset": 2, "text": "Var01 = 1", "var_name": "Var01", "has_call": false},
			{"kind": "play", "code_offset": 4, "track": 1, "text": "play sound_1"},
		],
	})
	await get_tree().process_frame
	var moves := []
	g.reorder_statement_requested.connect(func(si, o, d): moves.append([si, o, d]))
	var middle := _node_with_ordinal(g, 1)
	assert_not_null(middle, "middle statement node present")
	if middle == null:
		return
	_button_in(middle, "↑").pressed.emit()
	_button_in(middle, "↓").pressed.emit()
	assert_eq(moves, [[2, 1, -1], [2, 1, 1]], "middle ↑/↓ emit reorder with -1 / +1")


func test_reorder_boundary_buttons_disable_with_reason():
	var g := _egraph({
		"index": 6, "name": "S", "statements": [
			{"kind": "play", "code_offset": 0, "track": 0, "text": "play sound_0"},
			{"kind": "assign", "code_offset": 2, "text": "Var01 = 1", "var_name": "Var01", "has_call": false},
			{"kind": "play", "code_offset": 4, "track": 1, "text": "play sound_1"},
		],
	})
	await get_tree().process_frame
	var first := _node_with_ordinal(g, 0)
	var middle := _node_with_ordinal(g, 1)
	var last := _node_with_ordinal(g, 2)
	assert_not_null(first, "first statement node present")
	assert_not_null(middle, "middle statement node present")
	assert_not_null(last, "last statement node present")
	if first == null or middle == null or last == null:
		return
	var first_up := _button_in(first, "↑")
	var middle_up := _button_in(middle, "↑")
	var middle_down := _button_in(middle, "↓")
	var last_down := _button_in(last, "↓")
	assert_true(first_up.disabled, "first statement cannot move farther up")
	assert_eq(first_up.tooltip_text, "Already the first statement")
	assert_false(middle_up.disabled, "middle statement can move up")
	assert_false(middle_down.disabled, "middle statement can move down")
	assert_true(last_down.disabled, "last statement cannot move farther down")
	assert_eq(last_down.tooltip_text, "Already the last statement")


func test_flat_if_branch_reorder_boundaries_disable_with_reason():
	var g := _egraph({
		"index": 8, "name": "S", "statements": [
			{"kind": "if", "code_offset": 0, "expr": "(Var01 == 0)", "else_present": false,
				"then": [
					{"kind": "play", "code_offset": 2, "track": 0, "text": "play sound_0"},
					{"kind": "assign", "code_offset": 4, "text": "Var01 = 1", "var_name": "Var01", "has_call": false},
					{"kind": "transition", "code_offset": 6, "target_name": "A", "target_section": 1, "text": "enter A"},
				],
				"else": []},
		],
	})
	await get_tree().process_frame
	var first := _node_titled(g, "Play")
	var middle := _node_titled(g, "Set")
	var last := _node_titled(g, "Go to state")
	assert_not_null(first, "first branch statement node present")
	assert_not_null(middle, "middle branch statement node present")
	assert_not_null(last, "last branch statement node present")
	if first == null or middle == null or last == null:
		return
	var first_up := _button_in(first, "↑")
	var middle_up := _button_in(middle, "↑")
	var middle_down := _button_in(middle, "↓")
	var last_down := _button_in(last, "↓")
	assert_true(first_up.disabled, "first branch statement cannot move farther up")
	assert_eq(first_up.tooltip_text, "Already the first branch statement")
	assert_false(middle_up.disabled, "middle branch statement can move up")
	assert_false(middle_down.disabled, "middle branch statement can move down")
	assert_true(last_down.disabled, "last branch statement cannot move farther down")
	assert_eq(last_down.tooltip_text, "Already the last branch statement")


func test_flat_if_header_edits_condition_inline():
	# A flat if's ✎ edits its CONDITION inline on the node (an embedded expression
	# row + Apply/Cancel); its body statements are edited on their own nodes.
	var g := _egraph({
		"index": 7, "name": "Branchy", "statements": [
			{"kind": "if", "code_offset": 0, "expr": "(Var01 == 0)", "else_present": false,
				"then": [{"kind": "transition", "code_offset": 4, "target_name": "A", "target_section": 1, "text": "enter A"}], "else": []},
		],
	})
	await get_tree().process_frame
	var if_node := _node_titled(g, "If")
	_button_in(if_node, "✎").pressed.emit()
	assert_null(_first_dialog(g), "no modal opens for a condition edit")
	assert_not_null(_button_in(if_node, "✓ Apply"), "the if node grew an inline Apply")
	assert_not_null(_button_in(if_node, "✕ Cancel"), "and an inline Cancel")


# --- if regeneration: the whole block is rebuilt from its (edited) body texts ----

func test_if_body_replace_regenerates_block():
	var g := _egraph({"index": 5, "name": "S", "statements": [
		{"kind": "if", "expr": "(Var01 != 0)", "else_present": true,
			"then": [{"kind": "transition", "target_name": "A", "text": "enter A"}],
			"else": [{"kind": "transition", "target_name": "B", "text": "enter B"}]}]})
	await get_tree().process_frame
	var captured := []
	g.replace_statement_requested.connect(func(si, o, l): captured.append([si, o, Array(l)]))
	var if_dict := {"expr": "(Var01 != 0)", "else_present": true,
		"then": [{"text": "enter A"}], "else": [{"text": "enter B"}]}
	g._apply_branch_mutation(if_dict, 0, "then", 0, "replace", "enter Next")
	assert_eq(captured.size(), 1, "one replace emitted")
	assert_eq(captured[0][0], 5, "section index comes from the shown section")
	assert_eq(captured[0][1], 0, "the if's ordinal")
	assert_eq(captured[0][2],
		["if ((Var01 != 0))", "{", "    enter Next", "}", "else", "{", "    enter B", "}"],
		"then[0] replaced, else preserved, canonical formatting")


func test_if_body_delete_and_append_regenerate():
	var g := _egraph({"index": 1, "name": "S", "statements": [
		{"kind": "if", "expr": "(x)", "else_present": false, "then": [], "else": []}]})
	await get_tree().process_frame
	var captured := []
	g.replace_statement_requested.connect(func(si, o, l): captured.append(Array(l)))
	# expr already carries its parens (binops self-parenthesize), so if_block wraps to
	# the decompiler's canonical double-paren form: if ((x)).
	var if_dict := {"expr": "(x)", "else_present": false,
		"then": [{"text": "play sound_0"}, {"text": "enter A"}], "else": []}
	g._apply_branch_mutation(if_dict, 0, "then", 0, "delete", "")
	assert_eq(captured[0], ["if ((x))", "{", "    enter A", "}"], "delete then[0] leaves then[1]")
	captured.clear()
	g._apply_branch_mutation(if_dict, 0, "then", -1, "append", "play sound_3")
	assert_eq(captured[0], ["if ((x))", "{", "    play sound_0", "    enter A", "    play sound_3", "}"],
		"append adds to the end of the branch")


func test_if_condition_edit_regenerates_keeping_body():
	var g := _egraph({"index": 2, "name": "S", "statements": [
		{"kind": "if", "expr": "(Var01 != 0)", "else_present": false,
			"then": [{"kind": "transition", "target_name": "A", "text": "enter A"}], "else": []}]})
	await get_tree().process_frame
	var captured := []
	g.replace_statement_requested.connect(func(si, o, l): captured.append(Array(l)))
	var if_dict := {"expr": "(Var01 != 0)", "else_present": false,
		"then": [{"text": "enter A"}], "else": []}
	g._emit_if_replace(if_dict, 0, "(Var05 > 3)", g._branch_texts(if_dict, "then"), g._branch_texts(if_dict, "else"))
	assert_eq(captured[0], ["if ((Var05 > 3))", "{", "    enter A", "}"],
		"new condition, body preserved, no else block")


func test_edit_if_body_target_through_document():
	# End-to-end against the shipped gamemus: retarget the Testmission if's then-branch
	# transition via the graph's branch mutation, routed through the document.
	var doc := _doc()
	var sn := _sname(doc)
	var tm := _section(doc, "Testmission")
	assert_false(tm.is_empty(), "Testmission present")
	var stmts: Array = tm.get("statements", [])
	var if_idx := -1
	for i in range(stmts.size()):
		if String(stmts[i].get("kind", "")) == "if":
			if_idx = i
	assert_gt(if_idx, -1, "Testmission has an if")
	var if_dict: Dictionary = stmts[if_idx]
	var g = MusicSectionLogicGraph.new()
	g.size = Vector2(960, 720)
	add_child_autofree(g)
	g.configure_authoring(doc.mus_script.get_section_names(sn), _vars(), doc.mus_script, [], true)
	g.show_section(tm, [])
	await get_tree().process_frame
	g.replace_statement_requested.connect(func(si, o, l): doc.replace_statement(si, o, l))
	g._apply_branch_mutation(if_dict, if_idx, "then", 0, "replace", "enter Win000")
	# Re-fetch and confirm the if's then-body now enters Win000.
	var if2 := {}
	for s in _section(doc, "Testmission").get("statements", []):
		if String(s.get("kind", "")) == "if":
			if2 = s
	assert_false(if2.is_empty(), "if still present after the edit")
	var then0: String = String((if2.get("then", []) as Array)[0].get("text", ""))
	assert_eq(then0, "enter Win000", "the if's then-branch transition now targets Win000")


# --- frame-setup (0x38) is hidden engine plumbing ------------------------

func test_frame_enter_is_hidden_and_offset_carries():
	# enter (0x38) is frame setup, not an authored statement: it renders NO node at
	# all. Its byte offset transfers to the first visible node (so the live
	# highlight resolves while the pc sits on the hidden op), ordinals stay raw AST
	# indices (the play is still ordinal 1), the first visible statement does not
	# offer ↑ (ordinal 0 is the immovable hidden row), and the host can read the
	# state's input count for its header.
	var g := _egraph({
		"index": 0, "name": "S", "statements": [
			{"kind": "frame_enter", "code_offset": 0, "locals_count": 2, "text": "enter X"},
			{"kind": "play", "code_offset": 2, "track": 0, "text": "play sound_0"},
			{"kind": "play", "code_offset": 4, "track": 1, "text": "play sound_1"},
		],
	})
	await get_tree().process_frame
	assert_null(_node_titled(g, "Frame setup"), "the 0x38 op renders no node")
	assert_eq(_nodes(g).size(), 2, "only the two plays render")
	var first := _node_with_ordinal(g, 1)
	assert_not_null(first, "the play after the hidden row keeps its raw ordinal (1)")
	assert_eq(g.section_inputs_count(), 2, "the hidden frame op surfaces as an input count")
	# Live highlight: a pc on the hidden op's offset lights the first visible node.
	g.set_active_offset(0)
	assert_eq(g._active_node, first, "the hidden offset carries onto the first visible node")
	# Reorder affordance: the first VISIBLE statement must not offer ↑ (it would
	# swap with the locked hidden row and the document would refuse).
	var up := _button_in(first, "↑")
	assert_not_null(up, "tool cluster present on the first visible statement")
	assert_true(up.disabled, "↑ disabled on the first movable statement")
	var second := _node_with_ordinal(g, 2)
	assert_false(_button_in(second, "↑").disabled, "the next statement can still move up")


func test_frame_enter_present_in_gamemus_and_uneditable():
	# End-to-end against the shipped gamemus: Begin's leaked 0x38 decodes as a
	# frame_enter (not a transition), and the document refuses to delete/replace it.
	var doc := _doc()
	var begin := _section(doc, "Begin")
	assert_false(begin.is_empty(), "Begin section present")
	var stmts: Array = begin.get("statements", [])
	var fe_ord := -1
	for i in range(stmts.size()):
		if String(stmts[i].get("kind", "")) == "frame_enter":
			fe_ord = i
	assert_gt(fe_ord, -1, "Begin's leaked 0x38 op decodes as a frame_enter, not a transition")
	var sidx := int(begin.get("index", -1))
	var before := _stmt_count(doc, "Begin")
	assert_false(doc.delete_statement(sidx, fe_ord), "the document refuses to delete a frame_enter row")
	assert_false(doc.replace_statement(sidx, fe_ord, PackedStringArray(["enter Win000"])),
		"the document refuses to replace a frame_enter row")
	assert_eq(_stmt_count(doc, "Begin"), before, "frame_enter rejections were no-ops")


# --- folded ×N runs can be unfolded to edit one member -------------------

func test_folded_run_unfolds_to_editable_members():
	# A folded ×N run carries an ⊞ unfold toggle; expanding it re-renders the run as
	# N individually-tooled member nodes (the only Inspector-era edit the graph lacked).
	var stmts := []
	for i in range(3):
		stmts.append({"kind": "play", "code_offset": i * 2, "track": 0, "text": "play sound_0"})
	var g := _egraph({"index": 9, "name": "S", "statements": stmts})
	await get_tree().process_frame
	var fold := _node_titled(g, "Play")
	assert_not_null(fold, "the folded run node is present")
	var unfold := _button_starting(fold, "⊞")
	assert_not_null(unfold, "a folded run offers an ⊞ unfold toggle")
	assert_null(_button_in(fold, "✕"), "the collapsed fold has no per-member tools")
	unfold.pressed.emit()
	await get_tree().process_frame
	var members := _nodes_titled(g, "Play")
	assert_eq(members.size(), 3, "unfold expands the run into 3 member nodes")
	var tooled := 0
	for n in members:
		if _button_in(n, "✕") != null:
			tooled += 1
	assert_eq(tooled, 3, "each unfolded member carries its own edit tools")
	# And a ⊟ fold toggle re-collapses.
	assert_not_null(_button_starting(members[0], "⊟"), "the first member offers a ⊟ fold toggle")


func test_unfolded_member_delete_targets_its_own_ordinal():
	# Unfolding ordinal-addresses each member: deleting the 2nd member emits ordinal 1.
	var stmts := []
	for i in range(3):
		stmts.append({"kind": "play", "code_offset": i * 2, "track": 0, "text": "play sound_0"})
	var g := _egraph({"index": 4, "name": "S", "statements": stmts})
	await get_tree().process_frame
	_button_starting(_node_titled(g, "Play"), "⊞").pressed.emit()
	await get_tree().process_frame
	var deletes := []
	g.delete_statement_requested.connect(func(si, o): deletes.append([si, o]))
	var members := _nodes_titled(g, "Play")
	assert_eq(members.size(), 3, "three member nodes after unfold")
	_button_in(members[1], "✕").pressed.emit()
	assert_eq(deletes, [[4, 1]], "deleting the 2nd unfolded member targets section 4, ordinal 1")


# --- the ＋Add palette ---------------------------------------------------

func test_add_palette_mounted_when_editable():
	var g := _egraph({"index": 0, "name": "S", "statements": []})
	await get_tree().process_frame
	assert_not_null(g._add_menu, "the ＋Add palette is mounted in editable mode")
	assert_true(g._add_menu.visible, "the palette is visible while editable")


func test_empty_blueprint_shows_first_action_hint():
	var g := _egraph({"index": 2, "name": "Quiet", "statements": []})
	await get_tree().process_frame
	var hint := _node_titled(g, "Empty state")
	assert_not_null(hint, "empty sections show a visible blueprint hint node")
	if hint == null:
		return
	var all := []
	_descendants(hint, all)
	var found_message := false
	var found_detail := false
	for c in all:
		if c is Label and String((c as Label).text).contains("No statements yet"):
			found_message = true
		if c is Label and String((c as Label).text).contains("Import tracks in the Tracks dock"):
			found_detail = true
	assert_true(found_message, "hint explains why the canvas is empty")
	assert_true(found_detail, "empty-bank hint tells the user to import tracks first")
	var add := _button_containing(hint, "Add statement")
	assert_not_null(add, "hint has a direct first-action button")
	assert_false(add.disabled, "first-action button is enabled while editable")
	assert_eq(add.tooltip_text, "Add the first statement to this state.")


func test_empty_blueprint_mentions_track_drag_when_tracks_exist():
	var g := _egraph({"index": 2, "name": "Quiet", "statements": []}, ["intro"])
	await get_tree().process_frame
	var hint := _node_titled(g, "Empty state")
	assert_not_null(hint, "empty sections show a visible blueprint hint node")
	if hint == null:
		return
	var all := []
	_descendants(hint, all)
	var found_detail := false
	for c in all:
		if c is Label and String((c as Label).text).contains("drag a track from the Tracks dock"):
			found_detail = true
	assert_true(found_detail, "track-backed hint mentions drag-to-add")


func test_read_only_empty_blueprint_hint_explains_blocked_action():
	var g = MusicSectionLogicGraph.new()
	g.size = Vector2(800, 600)
	add_child_autofree(g)
	g.configure_authoring(PackedStringArray(["S"]), _vars(), null, [], false, "Fix script errors first")
	g.show_section({"index": 0, "name": "S", "statements": []}, [])
	await get_tree().process_frame
	var hint := _node_titled(g, "Empty state")
	assert_not_null(hint, "read-only empty sections still show the hint")
	if hint == null:
		return
	var add := _button_containing(hint, "Add statement")
	assert_not_null(add, "read-only hint keeps the direct action visible")
	assert_true(add.disabled, "read-only hint disables the first action")
	assert_eq(add.tooltip_text, "Fix script errors first")


func test_empty_hint_add_button_routes_to_add_palette():
	var g := _egraph({"index": 4, "name": "S", "statements": []})
	await get_tree().process_frame
	var added := []
	g.add_statement_requested.connect(func(si, l): added.append([si, Array(l)]))
	var add := _button_containing(_node_titled(g, "Empty state"), "Add statement")
	assert_not_null(add, "hint add button present")
	if add == null:
		return
	add.pressed.emit()
	await get_tree().process_frame
	var pop := _first_popup(g)
	assert_not_null(pop, "hint add button opens the construct palette")
	if pop == null:
		return
	var ret_id := -1
	for i in range(MusForms.ADD_ITEMS.size()):
		if String(MusForms.ADD_ITEMS[i][1]) == "return":
			ret_id = i
	assert_gt(ret_id, -1, "the palette offers a return item")
	pop.id_pressed.emit(ret_id)
	assert_eq(added, [[4, ["return"]]], "hint palette uses the same add_statement path")


func test_add_palette_inputless_emits_directly():
	var g := _egraph({"index": 4, "name": "S", "statements": []})
	await get_tree().process_frame
	var added := []
	g.add_statement_requested.connect(func(si, l): added.append([si, Array(l)]))
	# Drive the "return" palette item (no form, inserts its canonical line directly).
	var ret_id := -1
	for i in range(MusForms.ADD_ITEMS.size()):
		if String(MusForms.ADD_ITEMS[i][1]) == "return":
			ret_id = i
	assert_gt(ret_id, -1, "the palette offers a return item")
	g._on_add_palette_id(ret_id)
	assert_eq(added, [[4, ["return"]]], "an inputless palette pick inserts its canonical line")


# --- track drop sink -----------------------------------------------------

func test_drop_sink_gated_by_editable():
	var g := _egraph({"index": 0, "name": "S", "statements": []})
	await get_tree().process_frame
	assert_true(g._can_drop_data(Vector2.ZERO, {"kind": "mus_track", "index": 0}),
		"editable graph accepts a track drop")
	assert_false(g._can_drop_data(Vector2.ZERO, {"kind": "nope"}), "rejects non-track payloads")
	# A read-only graph rejects drops.
	var ro = MusicSectionLogicGraph.new()
	add_child_autofree(ro)
	ro.show_section({"index": 0, "name": "S", "statements": []}, [])
	assert_false(ro._can_drop_data(Vector2.ZERO, {"kind": "mus_track", "index": 0}),
		"read-only graph rejects drops")


func test_drop_emits_add_play_for_shown_section():
	var g := _egraph({"index": 3, "name": "Win000", "statements": []})
	await get_tree().process_frame
	var got := {"section": "", "track": -1}
	g.add_play_requested.connect(func(s, t):
		got["section"] = String(s)
		got["track"] = t)
	g._drop_data(Vector2.ZERO, {"kind": "mus_track", "index": 5})
	assert_eq(String(got["section"]), "Win000", "drop targets the shown state")
	assert_eq(int(got["track"]), 5, "drop carries the dropped track index")


# --- the remaining dialog form (if creation) stays canonical ----------------
# (transition/assign/play/etc. now edit inline; music_inline_edit_test.gd
# covers their canonical lines and open-time list capture.)

func test_if_creation_form_emits_canonical_block():
	var doc := _doc()
	var forms = MusForms.new()
	forms.configure(PackedStringArray(["A", "B"]), _vars(), doc.mus_script, [])
	var host := Control.new()
	add_child_autofree(host)
	await get_tree().process_frame
	var captured := []
	forms.open_form(host, "if", {}, false, func(lines): captured.append(Array(lines)))
	var dlg := _first_dialog(host)
	assert_not_null(dlg, "if creation keeps a dialog (a valid line needs a condition first)")
	if dlg == null:
		return
	# Defaults: condition 0, then-action -> enter A (first section).
	dlg.confirmed.emit()
	assert_eq(captured, [["if (0)", "{", "    enter A", "}"]],
		"the if form emits the canonical block")


func _first_dialog(host: Node) -> AcceptDialog:
	for c in host.get_children():
		if c is AcceptDialog:
			return c
	return null


func _first_popup(host: Node) -> PopupMenu:
	for c in host.get_children():
		if c is PopupMenu:
			return c
	return null


func _first_option(root: Node) -> OptionButton:
	var all := []
	_descendants(root, all)
	for c in all:
		if c is OptionButton:
			return c
	return null


# --- end-to-end: a graph ✕ deletes through the document and undoes ---------

func test_graph_delete_through_document_and_undoes():
	var doc := _doc()
	assert_true(doc.can_author(), "gamemus is authorable")
	var name := _section_with_play(doc)
	assert_ne(name, "", "found a section with a play")
	var sidx := _section_index(doc, name)
	var base := _stmt_count(doc, name)
	# Add a guaranteed-singleton statement (a high-slot assign won't fold) so the
	# graph has one unambiguous top-level node to delete.
	assert_true(doc.insert_statement(sidx, MusStmtText.assign("Var15", "(Var15 + 1)")),
		"seed a unique assign to delete")
	assert_eq(_stmt_count(doc, name), base + 1, "the seed assign landed")

	var g = MusicSectionLogicGraph.new()
	g.size = Vector2(960, 720)
	add_child_autofree(g)
	var sn := StringName(doc.mus_script.get_default_script_name())
	g.configure_authoring(doc.mus_script.get_section_names(sn), _vars(), doc.mus_script, [], true)
	g.show_section(_section(doc, name), [])
	await get_tree().process_frame
	# Route the graph's delete intent into the document (as live_mode does).
	g.delete_statement_requested.connect(func(si, o): doc.delete_statement(si, o))

	var assign_node := _node_titled(g, "Set")
	assert_not_null(assign_node, "the seeded assign shows as a node")
	var del := _button_in(assign_node, "✕")
	assert_not_null(del, "the assign node has a Delete tool")
	del.pressed.emit()
	assert_eq(_stmt_count(doc, name), base, "the graph ✕ deleted the statement through the document")
	doc.undo()
	assert_eq(_stmt_count(doc, name), base + 1, "undo restores the deleted statement")


# --- document fixture helpers (mirrors music_authoring_test) --------------

func _doc() -> MusicEditorDocument:
	var doc = MusicEditorDocument.new()
	_copy(BANK_FIXTURE, PAIR_BANK)
	_copy(SCRIPT_FIXTURE, PAIR_SCRIPT)
	doc.open_pair(PAIR_BANK)
	return doc


func _sname(doc) -> StringName:
	return StringName(doc.mus_script.get_default_script_name())


func _ast(doc) -> Array:
	return doc.mus_script.get_program_ast(_sname(doc))


func _section(doc, name: String) -> Dictionary:
	for s in _ast(doc):
		if String(s.get("name", "")) == name:
			return s
	return {}


func _section_index(doc, name: String) -> int:
	return int(_section(doc, name).get("index", -1))


func _stmt_count(doc, name: String) -> int:
	return (_section(doc, name).get("statements", []) as Array).size()


func _section_with_play(doc) -> String:
	for s in _ast(doc):
		for st in s.get("statements", []):
			if String(st.get("kind", "")) == "play":
				return String(s.get("name", ""))
	return ""


func _copy(src_path: String, dst_path: String) -> void:
	var src := FileAccess.open(src_path, FileAccess.READ)
	assert_not_null(src, "fixture readable: %s" % src_path)
	if src == null:
		return
	var dst := FileAccess.open(dst_path, FileAccess.WRITE)
	if dst != null:
		dst.store_buffer(src.get_buffer(src.get_length()))
		dst.close()
	src.close()


func _rm(path: String) -> void:
	var abs := ProjectSettings.globalize_path(path)
	if FileAccess.file_exists(abs):
		DirAccess.remove_absolute(abs)
