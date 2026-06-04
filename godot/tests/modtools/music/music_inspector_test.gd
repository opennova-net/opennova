extends GutTest

# The per-state inspector (right dock) and live-lighting on the section map.

const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const LiveModeScene = preload("res://modtools/music/ui/live_mode.tscn")
const MusicInspectorPanel = preload("res://modtools/music/ui/inspector_panel.gd")
const MusicSectionGraph = preload("res://modtools/music/music_section_graph.gd")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const PAIR_BANK := "user://music_insp_pair.sbf"
const PAIR_SCRIPT := "user://music_insp_pair.bin"


func after_each() -> void:
	_rm(PAIR_BANK)
	_rm(PAIR_SCRIPT)


func _make_live() -> Control:
	var doc = MusicEditorDocument.new()
	_copy(BANK_FIXTURE, PAIR_BANK)
	_copy(SCRIPT_FIXTURE, PAIR_SCRIPT)
	doc.open_pair(PAIR_BANK)
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	return lm


func _nodes(map) -> Array:
	var out := []
	for c in map.get_children():
		if c is GraphNode:
			out.append(c)
	return out


func test_inspector_populates_on_node_select():
	var lm := _make_live()
	await get_tree().process_frame
	var nodes := _nodes(lm.get_node("%SectionMap"))
	assert_gt(nodes.size(), 0, "map has nodes")
	var sec := String(nodes[0].get_meta("section", ""))
	lm._on_map_node_selected(nodes[0])
	assert_eq(lm._inspector_panel.current_section(), sec, "inspector shows the selected state")
	assert_gt(lm._inspector_panel.get_child_count(), 1, "inspector built detail rows, not just the empty hint")


func test_manual_select_pins_then_start_rearms_follow():
	var lm := _make_live()
	await get_tree().process_frame
	var nodes := _nodes(lm.get_node("%SectionMap"))
	lm._on_map_node_selected(nodes[0])
	assert_false(lm._follow_live, "manual node click pins the inspector (auto-follow off)")
	lm.get_node("%StartButton").pressed.emit()
	await get_tree().create_timer(0.1).timeout
	assert_true(lm._follow_live, "Start re-arms auto-follow")


func test_active_node_glows_while_running():
	var lm := _make_live()
	await get_tree().process_frame
	lm.get_node("%StartButton").pressed.emit()
	await get_tree().create_timer(0.1).timeout
	assert_eq(lm._director.vm_state(), 1, "RUNNING after Start")
	var cur := String(lm._current_section)
	assert_ne(cur, "", "a section is current while running")
	var active: Node = null
	for n in _nodes(lm.get_node("%SectionMap")):
		if String(n.get_meta("section", "")) == cur:
			active = n
			break
	assert_not_null(active, "the current section has a node")
	assert_true(active.modulate.is_equal_approx(Color(0.6, 1.0, 0.6)),
		"active node is tinted green (lit)")


func test_advanced_toggle_shows_drawer():
	var lm := _make_live()
	await get_tree().process_frame
	var drawer = lm.get_node("%AdvancedDrawer")
	var toggle: Button = lm.get_node("%AdvancedToggle")
	assert_false(drawer.visible, "raw-script drawer hidden by default")
	toggle.button_pressed = true
	assert_true(drawer.visible, "advanced toggle reveals the raw-script drawer")
	toggle.button_pressed = false
	assert_false(drawer.visible, "toggling off hides it")


func test_edge_glyph_table():
	assert_eq(MusicSectionGraph.edge_glyph(0), "→", "transition glyph")
	assert_eq(MusicSectionGraph.edge_glyph(1), "⇒", "switch glyph")
	assert_eq(MusicSectionGraph.edge_glyph(2), "↪", "branch glyph")


# Collect every HBoxContainer row in the inspector's statement list (recursing
# into nested if/switch blocks).
func _all_rows(node: Node, out: Array) -> void:
	for c in node.get_children():
		if c is HBoxContainer:
			out.append(c)
		_all_rows(c, out)


func test_inspector_renders_statement_tree_with_go():
	var panel = MusicInspectorPanel.new()
	add_child_autofree(panel)
	await get_tree().process_frame
	# An AST-shaped section (the form NovaMusicScript.get_program_ast returns).
	var sec := {
		"name": "Begin", "index": 0, "is_entry": true, "is_idle_loop": false, "code_offset": 0,
		"statements": [
			{"kind": "play", "code_offset": 2, "byte_size": 2, "text": "play sound_2", "track": 2, "wait": false},
			{"kind": "assign", "code_offset": 4, "byte_size": 4, "text": "Var07 = 1", "var_name": "Var07", "rhs": "1", "has_call": false, "call_name": ""},
			{"kind": "expr", "code_offset": 8, "byte_size": 4, "text": "SV(200)", "expr": "SV(200)", "has_call": true, "call_name": "GSV"},
			{"kind": "transition", "code_offset": 12, "byte_size": 2, "text": "enter Win000", "target_section": 1, "target_name": "Win000"},
			{"kind": "if", "code_offset": 16, "byte_size": 10, "expr": "(Var01 != 0)", "else_present": true,
				"then": [{"kind": "transition", "code_offset": 22, "byte_size": 2, "text": "enter A", "target_section": 2, "target_name": "A"}],
				"else": [{"kind": "transition", "code_offset": 28, "byte_size": 2, "text": "enter B", "target_section": 3, "target_name": "B"}]},
		],
	}
	var captured := []
	panel.jump_requested.connect(func(n): captured.append(String(n)))
	panel.show_section(sec, ["s0", "Win000", "A", "B"], "", false)

	var rows := []
	_all_rows(panel, rows)
	# Glyphs present: play ♪, assign ✎, call ƒ, transition →, if ◇.
	var glyphs := {}
	for r in rows:
		for c in r.get_children():
			if c is Label and String(c.text).length() <= 2:
				glyphs[String(c.text)] = true
	for g in ["♪", "✎", "ƒ", "→", "◇"]:
		assert_true(glyphs.has(g), "statement glyph '%s' rendered" % g)

	# A play row carries an interactive track chip.
	var has_chip := false
	for r in rows:
		for c in r.get_children():
			if c is MusicTrackChip:
				has_chip = true
	assert_true(has_chip, "play statement renders a track chip")

	# The transition's Go button jumps to the target section.
	var go: Button = null
	for r in rows:
		for c in r.get_children():
			if c is Button and (c as Button).text == "Go":
				go = c
				break
		if go != null:
			break
	assert_not_null(go, "a transition row has a Go button")
	if go != null:
		go.pressed.emit()
		assert_eq(captured.size(), 1, "Go emits jump_requested once")


func test_assign_with_call_shows_function_badge():
	# An assignment whose RHS runs a function must read as BOTH a var-mutation and
	# a function-call at a glance (the ✎ icon plus a ƒ badge), so "what changed /
	# what ran" is never hidden in the text alone.
	var panel = MusicInspectorPanel.new()
	add_child_autofree(panel)
	await get_tree().process_frame
	var sec := {
		"name": "S", "index": 0, "is_entry": true, "code_offset": 0,
		"statements": [
			{"kind": "assign", "code_offset": 0, "byte_size": 4, "text": "Var07 = GSV(200)", "var_name": "Var07", "rhs": "GSV(200)", "has_call": true, "call_name": "GSV"},
			{"kind": "assign", "code_offset": 4, "byte_size": 4, "text": "Var08 = 1", "var_name": "Var08", "rhs": "1", "has_call": false, "call_name": ""},
		],
	}
	panel.show_section(sec, [], "", false)
	var rows := []
	_all_rows(panel, rows)
	var badges := 0
	for r in rows:
		for c in r.get_children():
			if c is Label and String(c.text) == "ƒ":
				badges += 1
	assert_eq(badges, 1, "only the call-bearing assignment shows a ƒ badge")


func test_live_highlight_lights_statement_at_pc():
	var panel = MusicInspectorPanel.new()
	add_child_autofree(panel)
	await get_tree().process_frame
	var sec := {
		"name": "S", "index": 0, "is_entry": true, "code_offset": 0,
		"statements": [
			{"kind": "play", "code_offset": 0, "byte_size": 2, "text": "play sound_0", "track": 0, "wait": false},
			{"kind": "play", "code_offset": 2, "byte_size": 2, "text": "play sound_1", "track": 1, "wait": false},
			{"kind": "transition", "code_offset": 4, "byte_size": 2, "text": "enter S", "target_section": 0, "target_name": "S"},
		],
	}
	panel.show_section(sec, [], "", false)
	# pc=3 is inside the second play (offset 2): it must be the lit row.
	panel.set_active_offset(3)
	var rows := []
	_all_rows(panel, rows)
	var lit := []
	for r in rows:
		if r.modulate.is_equal_approx(Color(0.55, 1.0, 0.55)):
			lit.append(r)
	assert_eq(lit.size(), 1, "exactly one row is lit")
	# Moving the pc clears the old highlight and lights the new row.
	panel.set_active_offset(-1)
	var still_lit := 0
	for r in rows:
		if r.modulate.is_equal_approx(Color(0.55, 1.0, 0.55)):
			still_lit += 1
	assert_eq(still_lit, 0, "pc<0 clears the highlight")


func test_document_add_section():
	var doc = MusicEditorDocument.new()
	_copy(BANK_FIXTURE, PAIR_BANK)
	_copy(SCRIPT_FIXTURE, PAIR_SCRIPT)
	doc.open_pair(PAIR_BANK)
	var sname := StringName(doc.mus_script.get_default_script_name())
	var before: int = doc.mus_script.get_section_names(sname).size()
	assert_true(doc.add_section(StringName("Brandnew")), "add_section succeeds on a compilable script")
	var names := Array(doc.mus_script.get_section_names(sname))
	assert_eq(names.size(), before + 1, "exactly one section added")
	assert_true(names.has("Brandnew"), "the new section carries its name")
	# Bad names + duplicates are rejected without changing the script.
	assert_false(doc.add_section(StringName("Brandnew")), "duplicate name rejected")
	assert_false(doc.add_section(StringName("has space")), "non-identifier name rejected")
	assert_false(doc.add_section(StringName("")), "empty name rejected")
	# Compiler keywords are rejected up front (they would fail the recompile).
	assert_false(doc.add_section(StringName("play")), "reserved keyword 'play' rejected")
	assert_false(doc.add_section(StringName("done")), "reserved keyword 'done' rejected")
	assert_false(doc.add_section(StringName("section")), "reserved keyword 'section' rejected")
	assert_eq(doc.mus_script.get_section_names(sname).size(), before + 1, "rejections changed nothing")
	# The new section is empty (no plays, no outgoing edges) in the model.
	var model: Array = MusicSectionGraph.build(doc.mus_script, sname)
	for sec in model:
		if String(sec.get("name", "")) == "Brandnew":
			assert_eq((sec.get("plays", []) as Array).size(), 0, "new state plays nothing")
	# Undo removes it.
	doc.undo()
	assert_eq(doc.mus_script.get_section_names(sname).size(), before, "undo removes the added section")


func test_add_state_button_adds_a_node():
	var lm := _make_live()
	await get_tree().process_frame
	var before := _nodes(lm.get_node("%SectionMap")).size()
	assert_not_null(lm._add_state_btn, "the Add State button is mounted")
	lm._on_add_state()
	await get_tree().process_frame
	var after := _nodes(lm.get_node("%SectionMap")).size()
	assert_eq(after, before + 1, "Add State adds one node to the map")
	assert_eq(lm._inspector_panel.current_section(), "State_1", "the new state opens in the inspector")


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
