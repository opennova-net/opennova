extends GutTest

# The GraphEdit section map built by live_mode._refresh_map from the read-only
# structural model. Topology assertions mirror the C++ mus_model_test /
# mus_section_model_test fixtures for jo_gamemus.

const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const LiveModeScene = preload("res://modtools/music/ui/live_mode.tscn")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const PAIR_BANK := "user://music_map_pair.sbf"
const PAIR_SCRIPT := "user://music_map_pair.bin"


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


func _node_titled(nodes: Array, needle: String) -> Node:
	for n in nodes:
		if String(n.title).contains(needle):
			return n
	return null


func _conn_from(conn: Dictionary) -> String:
	return String(conn.get("from_node", conn.get("from", "")))


func _conn_to(conn: Dictionary) -> String:
	return String(conn.get("to_node", conn.get("to", "")))


func test_map_builds_one_node_per_section():
	var lm := _make_live()
	await get_tree().process_frame
	var map = lm.get_node("%SectionMap")
	assert_true(map is GraphEdit, "section map is a GraphEdit")
	assert_eq(_nodes(map).size(), 8, "8 sections -> 8 GraphNodes for jo_gamemus")


func test_exactly_one_entry_badge_and_idle_badge():
	var lm := _make_live()
	await get_tree().process_frame
	var nodes := _nodes(lm.get_node("%SectionMap"))
	var entry_count := 0
	for n in nodes:
		if String(n.title).contains("★ start"):
			entry_count += 1
	assert_eq(entry_count, 1, "exactly one node carries the start badge")
	var idle = _node_titled(nodes, "Missionnull")
	assert_not_null(idle, "Missionnull node present")
	assert_string_contains(idle.title, "↻ idle", "the self-looping idle section is badged")


func test_begin_fans_out_to_several_sections():
	var lm := _make_live()
	await get_tree().process_frame
	var map = lm.get_node("%SectionMap")
	var begin = _node_titled(_nodes(map), "Begin")
	assert_not_null(begin, "Begin node present")
	var begin_name := String(begin.name)
	var out_count := 0
	for conn in map.get_connection_list():
		if _conn_from(conn) == begin_name:
			out_count += 1
	assert_gt(out_count, 2, "Begin's switch fans out to several sections (Missionnull/win/lose + Testmission)")


func test_no_self_connections_drawn():
	var lm := _make_live()
	await get_tree().process_frame
	var map = lm.get_node("%SectionMap")
	for conn in map.get_connection_list():
		assert_ne(_conn_from(conn), _conn_to(conn),
			"idle self-loops are drawn as a badge, never as a self-connection")


func test_node_click_jumps_only_while_running():
	var lm := _make_live()
	await get_tree().process_frame
	var map = lm.get_node("%SectionMap")
	var nodes := _nodes(map)
	# Stopped: a click does not move the VM.
	var before := String(lm._current_section)
	lm._on_map_node_selected(nodes[0])
	assert_eq(String(lm._current_section), before, "node click is a no-op jump while stopped")
	# Running: a click jumps the VM to that node's section.
	lm.get_node("%StartButton").pressed.emit()
	await get_tree().create_timer(0.1).timeout
	assert_eq(lm._director.vm_state(), 1, "RUNNING after Start")
	# Start rebuilt the map (entry section_entered), so the pre-Start `nodes`
	# references are now freed; re-capture the live ones.
	nodes = _nodes(map)
	var target: Node = null
	var target_sec := ""
	for n in nodes:
		var sec := String(n.get_meta("section", ""))
		if sec != String(lm._current_section):
			target = n
			target_sec = sec
			break
	assert_not_null(target, "found a different section to jump to")
	lm._on_map_node_selected(target)
	assert_eq(String(lm._current_section), target_sec, "node click jumps the VM while running")


func test_idle_ticks_do_not_rebuild_map():
	# The headline Direction-B invariant: a self-looping idle section re-fires
	# section_entered ~60/sec, and that must NOT rebuild the GraphEdit (the bug
	# the design fixed). A real transition to a different section MUST rebuild.
	var lm := _make_live()
	await get_tree().process_frame
	var map = lm.get_node("%SectionMap")
	lm._last_state = lm.VM_RUNNING
	lm._on_section(&"Missionnull")  # real transition: one rebuild
	var ids := []
	for n in _nodes(map):
		ids.append(n.get_instance_id())
	assert_gt(ids.size(), 0, "map populated")
	for i in range(60):
		lm._on_section(&"Missionnull")  # idle self-loop flood
	assert_eq(lm._idle_ticks, 60, "idle ticks counted")
	var ids2 := []
	for n in _nodes(map):
		ids2.append(n.get_instance_id())
	assert_eq(ids2, ids, "idle ticks must NOT recreate GraphNodes (no 60/sec thrash)")
	lm._on_section(&"Begin")  # a different real transition MUST rebuild
	var ids3 := []
	for n in _nodes(map):
		ids3.append(n.get_instance_id())
	assert_ne(ids3, ids2, "a real transition rebuilds the map (guard isn't a no-op)")


func _node_label_texts(node: Node) -> Array:
	var out := []
	for c in node.get_children():
		if c is Label:
			out.append(String(c.text))
	return out


func test_map_shows_blueprint_grid_and_minimap():
	var lm := _make_live()
	await get_tree().process_frame
	var map = lm.get_node("%SectionMap")
	assert_true(map.minimap_enabled, "minimap on for navigating bigger graphs")
	assert_true(map.show_grid, "blueprint grid on")


func test_nodes_carry_a_logic_badge():
	# A state that runs if/switch/var/call logic surfaces a glyph badge on its map
	# node, so the logic that isn't a track chip is visible without opening it.
	var lm := _make_live()
	await get_tree().process_frame
	var found := false
	for n in _nodes(lm.get_node("%SectionMap")):
		for t in _node_label_texts(n):
			if t.contains("◇") or t.contains("⋔") or t.contains("✎") or t.contains("ƒ"):
				found = true
	assert_true(found, "states that run logic surface a ◇/⋔/✎/ƒ badge on the map node")


func test_edge_colors_distinct_by_kind():
	assert_ne(MusicSectionGraph.edge_color(MusicSectionGraph.KIND_SWITCH),
		MusicSectionGraph.edge_color(MusicSectionGraph.KIND_TRANSITION),
		"switch wire colour differs from a plain transition")
	assert_ne(MusicSectionGraph.edge_color(MusicSectionGraph.KIND_BRANCH),
		MusicSectionGraph.edge_color(MusicSectionGraph.KIND_TRANSITION),
		"branch wire colour differs from a plain transition")


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
