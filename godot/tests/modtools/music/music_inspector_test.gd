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


func test_inspector_renders_edges_with_glyphs():
	var panel = MusicInspectorPanel.new()
	add_child_autofree(panel)
	await get_tree().process_frame
	var sec := {
		"name": "Begin", "index": 0, "is_entry": true, "is_idle_loop": false,
		"edges": [
			{"to": 0, "to_name": "Begin", "kind": 0},        # self-loop -> excluded
			{"to": 1, "to_name": "Win000", "kind": 0},        # transition  →
			{"to": 2, "to_name": "Missionnull", "kind": 1},   # switch      ⇒
			{"to": 3, "to_name": "Loop", "kind": 2},          # branch      ↪
		],
		"plays": [],
	}
	var captured := []
	panel.jump_requested.connect(func(n): captured.append(String(n)))
	panel.show_section(sec, [], "", false)
	var rows := []
	for c in panel.get_children():
		if c is HBoxContainer:
			rows.append(c)
	assert_eq(rows.size(), 3, "self-loop excluded; 3 real edges rendered (plays empty so no chips)")
	var glyphs := {}
	for r in rows:
		glyphs[(r.get_child(0) as Label).text] = true
	assert_true(glyphs.has("→ Win000"), "transition row glyph")
	assert_true(glyphs.has("⇒ Missionnull"), "switch row glyph")
	assert_true(glyphs.has("↪ Loop"), "branch row glyph")
	(rows[0].get_child(1) as Button).pressed.emit()
	assert_eq(captured.size(), 1, "a Go button emits jump_requested")


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
