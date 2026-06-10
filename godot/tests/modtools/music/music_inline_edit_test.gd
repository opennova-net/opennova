extends GutTest

# Inline node editing on the blueprint (section_logic_graph._begin_inline_edit):
# ✎ swaps the node's body row for kind-specific controls at the SAME child
# index (pins never move) plus an Apply/Cancel row; commits flow through the
# existing parity-gated intents; one node edits at a time; any re-render
# cancels. ＋Add inserts a canonical default line and auto-opens the new node's
# editor. No modal dialogs anywhere in these flows.

const MusicSectionLogicGraph = preload("res://modtools/music/ui/section_logic_graph.gd")
const MusForms = preload("res://modtools/music/mus_forms.gd")


func _vars() -> Array:
	var out := []
	for i in range(17):
		out.append({"token": "Var%02d" % i, "label": "Var%02d" % i})
	return out


func _egraph(section: Dictionary, bank_names: Array = []) -> GraphEdit:
	var g = MusicSectionLogicGraph.new()
	g.size = Vector2(960, 720)
	add_child_autofree(g)
	g.configure_authoring(PackedStringArray(["S", "A", "B"]), _vars(), null, bank_names, true)
	g.show_section(section, bank_names)
	return g


func _nodes(g) -> Array:
	var out := []
	for c in g.get_children():
		if c is GraphNode:
			out.append(c)
	return out


func _node_titled(g, needle: String) -> GraphNode:
	for n in _nodes(g):
		if String(n.title).contains(needle):
			return n
	return null


func _descendants(node: Node, out: Array) -> void:
	for c in node.get_children():
		out.append(c)
		_descendants(c, out)


func _button_in(node: Node, text: String) -> Button:
	var all := []
	_descendants(node, all)
	for c in all:
		if c is Button and String((c as Button).text) == text:
			return c
	return null


func _options_in(node: Node) -> Array:
	var all := []
	_descendants(node, all)
	var out := []
	for c in all:
		if c is OptionButton:
			out.append(c)
	return out


func _first_dialog(host: Node) -> AcceptDialog:
	for c in host.get_children():
		if c is AcceptDialog:
			return c
	return null


func _nodes_in_edit(g) -> Array:
	var out := []
	for n in _nodes(g):
		if _button_in(n, "✓ Apply") != null:
			out.append(n)
	return out


# --- per-kind inline editors ----------------------------------------------

func test_play_edit_inline_track_picker_applies_canonical_line():
	var g := _egraph({"index": 5, "name": "S", "statements": [
		{"kind": "play", "code_offset": 0, "track": 0, "text": "play sound_0"},
	]}, ["intro", "combat", "calm"])
	await get_tree().process_frame
	var node := _node_titled(g, "Play")
	var replaces := []
	g.replace_statement_requested.connect(func(si, o, l): replaces.append([si, o, Array(l)]))
	_button_in(node, "✎").pressed.emit()
	assert_null(_first_dialog(g), "no modal for a play edit")
	var pickers := _options_in(node)
	assert_gt(pickers.size(), 0, "track picker embedded in the node")
	(pickers[0] as OptionButton).select(1)   # -> combat
	_button_in(node, "✓ Apply").pressed.emit()
	assert_eq(replaces, [[5, 0, ["play sound_1"]]],
		"Apply emits the canonical replace intent through the existing path")


func test_transition_edit_inline_resolves_against_open_time_list():
	# The section picker snapshots the list it opened with: a background
	# configure_authoring (document.changed mid-edit) must not shift the pick.
	var g := _egraph({"index": 2, "name": "S", "statements": [
		{"kind": "transition", "code_offset": 0, "target_name": "A", "target_section": 1, "text": "enter A"},
	]})
	await get_tree().process_frame
	var node := _node_titled(g, "Go to state")
	var replaces := []
	g.replace_statement_requested.connect(func(_si, _o, l): replaces.append(Array(l)))
	_button_in(node, "✎").pressed.emit()
	var ob: OptionButton = _options_in(node)[0]
	ob.select(2)   # -> "B" in the open-time list
	g._section_names = PackedStringArray(["X", "Y"])   # background reconfigure
	_button_in(node, "✓ Apply").pressed.emit()
	assert_eq(replaces, [["enter B"]],
		"the pick resolves against the open-time list, not the reconfigured one")


func test_assign_edit_inline_var_picker_and_expression_row():
	var g := _egraph({"index": 1, "name": "S", "statements": [
		{"kind": "assign", "code_offset": 0, "text": "Var01 = (Var01 + 1)",
			"var_name": "Var01", "var_offset": 4, "is_local": false,
			"rhs": "(Var01 + 1)", "has_call": false},
	]})
	await get_tree().process_frame
	var node := _node_titled(g, "Set variable")
	var replaces := []
	g.replace_statement_requested.connect(func(_si, _o, l): replaces.append(Array(l)))
	_button_in(node, "✎").pressed.emit()
	var vob: OptionButton = _options_in(node)[0]
	assert_eq(vob.selected, 1, "var picker seeded to Var01")
	vob.select(2)   # retarget to Var02
	_button_in(node, "✓ Apply").pressed.emit()
	assert_eq(replaces, [["Var02 = (Var01 + 1)"]],
		"var retarget keeps the RHS text byte-stable")


func test_if_condition_inline_edit_regenerates_block():
	var g := _egraph({"index": 3, "name": "S", "statements": [
		{"kind": "if", "code_offset": 0, "expr": "(Var01 == 0)", "else_present": false,
			"then": [{"kind": "transition", "code_offset": 4, "target_name": "A", "target_section": 1, "text": "enter A"}],
			"else": []},
	]})
	await get_tree().process_frame
	var if_node := _node_titled(g, "If")
	var replaces := []
	g.replace_statement_requested.connect(func(_si, _o, l): replaces.append(Array(l)))
	_button_in(if_node, "✎").pressed.emit()
	# The inline expression row mounted in the node; type a new condition.
	var er = if_node.get_child(0).get_child(1)   # "if (" + ExprRow + ")"
	er.set_expression_text("(Var05 > 3)")
	_button_in(if_node, "✓ Apply").pressed.emit()
	assert_eq(replaces, [["if ((Var05 > 3))", "{", "    enter A", "}"]],
		"condition edit regenerates the whole if with the body preserved")


# --- edit-mode lifecycle -----------------------------------------------------

func test_cancel_restores_read_mode_without_intent():
	var g := _egraph({"index": 0, "name": "S", "statements": [
		{"kind": "play", "code_offset": 0, "track": 0, "text": "play sound_0"},
	]}, ["intro"])
	await get_tree().process_frame
	var emitted := []
	g.replace_statement_requested.connect(func(_si, _o, _l): emitted.append(1))
	var node := _node_titled(g, "Play")
	_button_in(node, "✎").pressed.emit()
	assert_eq(_nodes_in_edit(g).size(), 1, "node entered edit mode")
	_button_in(_nodes_in_edit(g)[0], "✕ Cancel").pressed.emit()
	assert_eq(_nodes_in_edit(g).size(), 0, "Cancel restored read mode")
	assert_eq(emitted, [], "no intent emitted")
	assert_not_null(_button_in(_node_titled(g, "Play"), "✎"), "tools restored after cancel")


func test_only_one_node_edits_at_a_time():
	var g := _egraph({"index": 0, "name": "S", "statements": [
		{"kind": "play", "code_offset": 0, "track": 0, "text": "play sound_0"},
		{"kind": "transition", "code_offset": 2, "target_name": "A", "target_section": 1, "text": "enter A"},
	]}, ["intro"])
	await get_tree().process_frame
	_button_in(_node_titled(g, "Play"), "✎").pressed.emit()
	assert_eq(_nodes_in_edit(g).size(), 1, "first node in edit mode")
	# Opening the second cancels the first and edits the second.
	_button_in(_node_titled(g, "Go to state"), "✎").pressed.emit()
	var editing := _nodes_in_edit(g)
	assert_eq(editing.size(), 1, "still exactly one node in edit mode")
	assert_string_contains(String((editing[0] as GraphNode).title), "Go to state",
		"the second node took over the edit")


func test_rerender_cancels_open_edit():
	var section := {"index": 0, "name": "S", "statements": [
		{"kind": "play", "code_offset": 0, "track": 0, "text": "play sound_0"},
	]}
	var g := _egraph(section, ["intro"])
	await get_tree().process_frame
	_button_in(_node_titled(g, "Play"), "✎").pressed.emit()
	assert_eq(_nodes_in_edit(g).size(), 1, "edit open")
	# An external re-render (undo / document change / follow-live) drops the edit.
	g.show_section(section, ["intro"])
	await get_tree().process_frame
	assert_eq(_nodes_in_edit(g).size(), 0, "re-render cancels the edit (cancel semantics)")


func test_inline_edit_started_signal_fires_for_follow_live_pinning():
	var g := _egraph({"index": 0, "name": "S", "statements": [
		{"kind": "play", "code_offset": 0, "track": 0, "text": "play sound_0"},
	]}, ["intro"])
	await get_tree().process_frame
	var fired := []
	g.inline_edit_started.connect(func(): fired.append(1))
	_button_in(_node_titled(g, "Play"), "✎").pressed.emit()
	assert_eq(fired, [1], "the host can pin follow-live when an edit opens")


# --- ＋Add inserts a default and auto-opens the editor ------------------------

func test_add_palette_inserts_default_line_no_dialog():
	var g := _egraph({"index": 4, "name": "S", "statements": []})
	await get_tree().process_frame
	var added := []
	g.add_statement_requested.connect(func(si, l): added.append([si, Array(l)]))
	var assign_id := -1
	for i in range(MusForms.ADD_ITEMS.size()):
		if String(MusForms.ADD_ITEMS[i][1]) == "assign":
			assign_id = i
	g._on_add_palette_id(assign_id)
	assert_null(_first_dialog(g), "no dialog for an inline-editable kind")
	assert_eq(added, [[4, ["Var00 = 0"]]], "the canonical default line was inserted")


func test_post_add_rerender_auto_opens_the_new_nodes_editor():
	var g := _egraph({"index": 4, "name": "S", "statements": []})
	await get_tree().process_frame
	var assign_id := -1
	for i in range(MusForms.ADD_ITEMS.size()):
		if String(MusForms.ADD_ITEMS[i][1]) == "assign":
			assign_id = i
	g._on_add_palette_id(assign_id)
	# Simulate the document round-trip: re-show the section with the new row.
	g.show_section({"index": 4, "name": "S", "statements": [
		{"kind": "assign", "code_offset": 0, "text": "Var00 = 0",
			"var_name": "Var00", "var_offset": 0, "is_local": false,
			"rhs": "0", "has_call": false},
		{"kind": "done", "code_offset": 4, "text": "done"},
	]}, [])
	await get_tree().process_frame
	var editing := _nodes_in_edit(g)
	assert_eq(editing.size(), 1, "the new statement opened straight into its inline editor")
	assert_string_contains(String((editing[0] as GraphNode).title), "Set variable",
		"and it is the freshly added assign")


func test_if_creation_still_uses_its_dialog():
	var g := _egraph({"index": 4, "name": "S", "statements": []})
	await get_tree().process_frame
	var if_id := -1
	for i in range(MusForms.ADD_ITEMS.size()):
		if String(MusForms.ADD_ITEMS[i][1]) == "if":
			if_id = i
	g._on_add_palette_id(if_id)
	assert_not_null(_first_dialog(g), "if creation needs its condition dialog")


func test_branch_add_appends_default_line():
	var g := _egraph({"index": 6, "name": "S", "statements": [
		{"kind": "if", "code_offset": 0, "expr": "(Var01 == 0)", "else_present": false,
			"then": [{"kind": "transition", "code_offset": 4, "target_name": "A", "target_section": 1, "text": "enter A"}],
			"else": []},
	]})
	await get_tree().process_frame
	var replaces := []
	g.replace_statement_requested.connect(func(_si, _o, l): replaces.append(Array(l)))
	var if_dict := {"expr": "(Var01 == 0)", "else_present": false,
		"then": [{"text": "enter A"}], "else": []}
	# Drive the branch-add handler the popup routes to (assign is item index in
	# the filtered branch list; resolve it like the popup does).
	g._branch_add_items = []
	for item in MusForms.ADD_ITEMS:
		var k := String(item[1])
		if k != "if" and k != "switch":
			g._branch_add_items.append(item)
	var assign_idx := -1
	for i in range(g._branch_add_items.size()):
		if String(g._branch_add_items[i][1]) == "assign":
			assign_idx = i
	g._on_branch_add_id(assign_idx, if_dict, 0, "then")
	assert_eq(replaces, [["if ((Var01 == 0))", "{", "    enter A", "    Var00 = 0", "}"]],
		"branch add appends the canonical default and regenerates the if")
