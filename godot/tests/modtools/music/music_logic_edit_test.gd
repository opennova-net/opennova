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


func test_nested_branch_statement_has_no_tools():
	# A statement inside an if's then/else shares the if's ordinal, so it isn't
	# individually editable on the graph (edit the whole if via its ✎ -> raw).
	var g := _egraph({
		"index": 0, "name": "S", "statements": [
			{"kind": "if", "code_offset": 0, "expr": "(Var01 == 0)", "else_present": false,
				"then": [{"kind": "transition", "code_offset": 4, "target_name": "A", "target_section": 1, "text": "enter A"}],
				"else": []},
		],
	})
	await get_tree().process_frame
	# The nested "enter A" to-state node must NOT carry a delete tool.
	var enter := _node_titled(g, "Enter")
	assert_not_null(enter, "nested enter node present")
	assert_null(_button_in(enter, "✕"), "a nested (branch-body) node carries no tools")


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


func test_reorder_buttons_emit_direction():
	var g := _egraph({
		"index": 2, "name": "S", "statements": [
			{"kind": "play", "code_offset": 0, "track": 0, "text": "play sound_0"},
			{"kind": "play", "code_offset": 2, "track": 1, "text": "play sound_1"},
		],
	})
	await get_tree().process_frame
	var moves := []
	g.reorder_statement_requested.connect(func(si, o, d): moves.append([si, o, d]))
	var first: GraphNode = _nodes(g)[0]
	_button_in(first, "↑").pressed.emit()
	_button_in(first, "↓").pressed.emit()
	assert_eq(moves, [[2, 0, -1], [2, 0, 1]], "↑/↓ emit reorder with -1 / +1")


func test_if_edit_routes_to_raw_drawer():
	# An existing if can hold nested bodies the single-action form can't represent,
	# so its ✎ routes to the raw-script drawer instead of a lossy structured edit.
	var g := _egraph({
		"index": 7, "name": "Branchy", "statements": [
			{"kind": "if", "code_offset": 0, "expr": "(Var01 == 0)", "else_present": false,
				"then": [{"kind": "return", "code_offset": 4, "text": "return"}], "else": []},
		],
	})
	await get_tree().process_frame
	var raw := []
	g.open_raw_requested.connect(func(n): raw.append(String(n)))
	var replaced := []
	g.replace_statement_requested.connect(func(si, o, l): replaced.append([si, o]))
	_button_in(_node_titled(g, "If"), "✎").pressed.emit()
	assert_eq(raw, ["Branchy"], "editing an if opens the raw drawer for its section")
	assert_eq(replaced.size(), 0, "an if edit does NOT emit a (lossy) structured replace")


# --- the ＋Add palette ---------------------------------------------------

func test_add_palette_mounted_when_editable():
	var g := _egraph({"index": 0, "name": "S", "statements": []})
	await get_tree().process_frame
	assert_not_null(g._add_menu, "the ＋Add palette is mounted in editable mode")
	assert_true(g._add_menu.visible, "the palette is visible while editable")


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


# --- shared forms produce byte-identical canonical lines (guards the lift) ---

func test_forms_emit_canonical_transition_line():
	var doc := _doc()
	var forms = MusForms.new()
	forms.configure(PackedStringArray(["A", "B"]), _vars(), doc.mus_script, [])
	var host := Control.new()
	add_child_autofree(host)
	await get_tree().process_frame
	var captured := []
	forms.open_form(host, "transition", {}, false, func(lines): captured.append(Array(lines)))
	var dlg := _first_dialog(host)
	assert_not_null(dlg, "transition form opened a dialog")
	var ob := _first_option(dlg)
	assert_not_null(ob, "transition form has a section picker")
	ob.select(1)  # -> "B"
	dlg.confirmed.emit()
	assert_eq(captured, [["enter B"]], "the shared form emits the canonical 'enter B' line")


func test_forms_emit_canonical_assign_line():
	var doc := _doc()
	var forms = MusForms.new()
	forms.configure(PackedStringArray([]), _vars(), doc.mus_script, [])
	var host := Control.new()
	add_child_autofree(host)
	await get_tree().process_frame
	var captured := []
	forms.open_form(host, "assign", {}, false, func(lines): captured.append(Array(lines)))
	var dlg := _first_dialog(host)
	assert_not_null(dlg, "assign form opened a dialog")
	# Defaults: var picker -> Var00, expression -> the literal 0.
	dlg.confirmed.emit()
	assert_eq(captured, [["Var00 = 0"]], "the shared form emits the canonical 'Var00 = 0' line")


func test_form_resolves_against_list_captured_at_open():
	# A form's output must resolve against the section list that was current when it
	# OPENED, even if configure() runs again (a background document.changed) before
	# the user confirms -- otherwise the picked index would resolve against a
	# different list (silent wrong-target). Guards the producer-time context capture.
	var forms = MusForms.new()
	forms.configure(PackedStringArray(["A", "B", "C"]), _vars(), null, [])
	var host := Control.new()
	add_child_autofree(host)
	await get_tree().process_frame
	var captured := []
	forms.open_form(host, "transition", {}, false, func(lines): captured.append(Array(lines)))
	var dlg := _first_dialog(host)
	var ob := _first_option(dlg)
	ob.select(2)  # -> "C" in the list the dialog opened with
	# A background reconfigure swaps the section list out from under the open dialog.
	forms.configure(PackedStringArray(["X", "Y"]), _vars(), null, [])
	dlg.confirmed.emit()
	assert_eq(captured, [["enter C"]], "producer resolves the index against the open-time list, not the reconfigured one")


func _first_dialog(host: Node) -> AcceptDialog:
	for c in host.get_children():
		if c is AcceptDialog:
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
