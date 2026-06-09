extends GutTest

# Stage 2 of the blueprint editor: the per-section logic graph (statements as
# exec-flow nodes). Topology is asserted on synthetic AST sections for precision,
# plus a smoke build from the real jo_gamemus AST so the bridge shape is exercised.

const MusicSectionLogicGraph = preload("res://modtools/music/ui/section_logic_graph.gd")
const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const PAIR_BANK := "user://music_lg_pair.sbf"
const PAIR_SCRIPT := "user://music_lg_pair.bin"


func after_each() -> void:
	_rm(PAIR_BANK)
	_rm(PAIR_SCRIPT)


func _graph() -> GraphEdit:
	var g = MusicSectionLogicGraph.new()
	g.size = Vector2(800, 600)
	add_child_autofree(g)
	return g


func _nodes(g) -> Array:
	var out := []
	for c in g.get_children():
		if c is GraphNode:
			out.append(c)
	return out


func test_simple_chain_wires_sequentially():
	var g := _graph()
	await get_tree().process_frame
	var sec := {
		"index": 0, "name": "S",
		"statements": [
			{"kind": "play", "code_offset": 2, "track": 1, "text": "play sound_1"},
			{"kind": "assign", "code_offset": 4, "text": "Var07 = 1", "var_name": "Var07", "has_call": false},
			{"kind": "transition", "code_offset": 6, "target_name": "Next", "target_section": 1, "text": "enter Next"},
		],
	}
	g.show_section(sec, ["s0", "combat"])
	await get_tree().process_frame
	assert_eq(_nodes(g).size(), 3, "one node per statement")
	# play -> assign -> enter; the terminal enter has no outgoing wire.
	assert_eq(g.get_connection_list().size(), 2, "two exec wires for a 3-node chain")


func test_if_else_branches_and_rejoin():
	var g := _graph()
	await get_tree().process_frame
	var sec := {
		"index": 0, "name": "S",
		"statements": [
			{"kind": "play", "code_offset": 0, "track": 0, "text": "play sound_0"},
			{"kind": "if", "code_offset": 2, "expr": "(Var14 == 0)", "else_present": true,
				"then": [{"kind": "assign", "code_offset": 4, "text": "Var14 = 1", "var_name": "Var14", "has_call": false}],
				"else": [{"kind": "transition", "code_offset": 6, "target_name": "Loop", "target_section": 1, "text": "enter Loop"}]},
			{"kind": "transition", "code_offset": 8, "target_name": "Check", "target_section": 2, "text": "enter Check"},
		],
	}
	g.show_section(sec, [])
	await get_tree().process_frame
	# play, if, then-assign, else-enter, after-enter = 5 nodes.
	assert_eq(_nodes(g).size(), 5, "if/else expands to a node per branch statement")
	# wires: play->if, if.then->assign, if.else->elseEnter, assign->afterEnter, if.after->afterEnter
	assert_eq(g.get_connection_list().size(), 5, "branches wired + then-tail rejoins after")


func test_switch_has_one_pin_per_target_and_default():
	var g := _graph()
	await get_tree().process_frame
	# The "Main Main Main" jump table: 3 indexed pins, all to Main.
	var sec := {
		"index": 0, "name": "Checkstate",
		"statements": [
			{"kind": "switch", "code_offset": 0, "expr": "Var00", "action": "enter",
				"targets": [{"name": "Main", "section": 1, "track": -1},
							{"name": "Main", "section": 1, "track": -1},
							{"name": "Main", "section": 1, "track": -1}]},
			{"kind": "transition", "code_offset": 8, "target_name": "Main", "target_section": 1, "text": "enter Main"},
		],
	}
	g.show_section(sec, [])
	await get_tree().process_frame
	# switch node + 3 target to-state nodes + the default-after enter = 5 nodes.
	assert_eq(_nodes(g).size(), 5, "switch + 3 target nodes + the fall-through enter")
	# 3 target wires + 1 default->after enter.
	assert_eq(g.get_connection_list().size(), 4, "one wire per target + default fall-through")
	# Pins are index-labelled (0 -> Main ...), so the repetition reads as a table.
	var labelled := 0
	for n in _nodes(g):
		for c in n.get_children():
			if c is Label and String(c.text).begins_with("0 → Main"):
				labelled += 1
	assert_gt(labelled, 0, "switch target pins are index-labelled (0 -> Main)")


func test_identical_run_folds():
	var g := _graph()
	await get_tree().process_frame
	var stmts := []
	for i in range(5):
		stmts.append({"kind": "play", "code_offset": i * 2, "track": 0, "text": "play sound_0"})
	stmts.append({"kind": "return", "code_offset": 20})
	g.show_section({"index": 0, "name": "S", "statements": stmts}, [])
	await get_tree().process_frame
	# 5 identical plays fold to 1 node + the return = 2 nodes.
	assert_eq(_nodes(g).size(), 2, "5 identical plays fold into one ×5 node")
	var found_fold := false
	for n in _nodes(g):
		for c in n.get_children():
			if c is Label and String(c.text).contains("×5"):
				found_fold = true
	assert_true(found_fold, "the folded node is badged ×5")


func test_node_carries_ordinal_and_selection_emits():
	var g := _graph()
	await get_tree().process_frame
	var sec := {
		"index": 3, "name": "S",
		"statements": [
			{"kind": "play", "code_offset": 0, "track": 0, "text": "play sound_0"},
			{"kind": "transition", "code_offset": 2, "target_name": "X", "target_section": 1, "text": "enter X"},
		],
	}
	g.show_section(sec, [])
	await get_tree().process_frame
	var captured := []
	g.statement_selected.connect(func(sidx, ord): captured.append([sidx, ord]))
	var first: GraphNode = _nodes(g)[0]
	assert_eq(int(first.get_meta("ordinal")), 0, "first node carries ordinal 0")
	g.node_selected.emit(first)
	assert_eq(captured.size(), 1, "selecting a node emits statement_selected once")
	assert_eq(captured[0], [3, 0], "selection carries (section_index, ordinal)")


func test_live_highlight_lights_node_at_pc():
	var g := _graph()
	await get_tree().process_frame
	var sec := {
		"index": 0, "name": "S",
		"statements": [
			{"kind": "play", "code_offset": 10, "track": 0, "text": "play sound_0"},
			{"kind": "play", "code_offset": 20, "track": 1, "text": "play sound_1"},
		],
	}
	g.show_section(sec, [])
	await get_tree().process_frame
	g.set_active_offset(22)  # at/after the second play
	var lit := 0
	for n in _nodes(g):
		if n.modulate.is_equal_approx(Color(0.55, 1.0, 0.55)):
			lit += 1
	assert_eq(lit, 1, "exactly one node lit at the pc")


func test_builds_from_real_gamescript_ast():
	var doc = MusicEditorDocument.new()
	_copy(BANK_FIXTURE, PAIR_BANK)
	_copy(SCRIPT_FIXTURE, PAIR_SCRIPT)
	doc.open_pair(PAIR_BANK)
	var sn := StringName(doc.mus_script.get_default_script_name())
	var ast: Array = doc.mus_script.get_program_ast(sn)
	assert_gt(ast.size(), 0, "gamescript AST present")
	var g := _graph()
	await get_tree().process_frame
	# Build every section's graph; none should crash and each non-empty section
	# yields at least one node.
	for sec in ast:
		g.show_section(sec, [])
		await get_tree().process_frame
		if (sec.get("statements", []) as Array).size() > 0:
			assert_gt(_nodes(g).size(), 0, "section %s yields nodes" % String(sec.get("name", "")))


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
