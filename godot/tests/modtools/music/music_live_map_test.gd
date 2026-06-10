extends GutTest

# The GraphEdit section map built by live_mode._refresh_map from the read-only
# structural model. Topology assertions mirror the C++ mus_model_test /
# mus_section_model_test fixtures for jo_gamemus.

const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const MusicSectionGraphClass = preload("res://modtools/music/music_section_graph.gd")
const LiveModeScene = preload("res://modtools/music/ui/live_mode.tscn")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const PAIR_BANK := "user://music_map_pair.sbf"
const PAIR_SCRIPT := "user://music_map_pair.bin"


class ReadOnlyMusicScript:
	extends RefCounted

	func get_default_script_name() -> StringName:
		return &"readonly"

	func get_program_ast(_script_name: StringName) -> Array:
		return [{
			"name": "Begin",
			"index": 0,
			"statements": [{"kind": "done", "text": "done"}],
		}]

	func get_section_names(_script_name: StringName) -> PackedStringArray:
		return PackedStringArray(["Begin"])


class ReadOnlyDocument:
	extends RefCounted
	signal changed
	var mus_script := ReadOnlyMusicScript.new()

	func script_loaded() -> bool:
		return true

	func bank_loaded() -> bool:
		return false

	func can_author() -> bool:
		return false

	func authoring_blocked_reason() -> String:
		return "Fix script errors first"


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
	assert_eq(lm.get_node("%StateLabel").text, "Start playback before jumping to a state",
		"stopped node click explains why it did not jump")
	# Running: a click jumps the VM to that node's section.
	lm.get_node("%StartButton").pressed.emit()
	await get_tree().create_timer(0.1).timeout
	assert_eq(lm._director.vm_state(), 1, "RUNNING after Start")
	assert_eq(lm.get_node("%StateLabel").text, "RUNNING",
		"successful Start clears the stopped-click guidance")
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


func test_map_rebuild_does_not_leave_orphan_nodes():
	var lm := _make_live()
	await get_tree().process_frame
	var before := int(Performance.get_monitor(Performance.OBJECT_ORPHAN_NODE_COUNT))
	lm._on_section(&"Missionnull")
	var after := int(Performance.get_monitor(Performance.OBJECT_ORPHAN_NODE_COUNT))
	assert_eq(after, before, "rebuilding the map frees old GraphNodes without orphaning them")


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
	# A state that runs if/switch/var/call logic surfaces a WORD badge on its map
	# node ("2 if · 1 choose"), so the logic that isn't a track chip is readable
	# without opening it -- and without memorizing a glyph legend.
	var lm := _make_live()
	await get_tree().process_frame
	var found := false
	for n in _nodes(lm.get_node("%SectionMap")):
		for t in _node_label_texts(n):
			if t.contains(" if") or t.contains(" choose") or t.contains(" set") or t.contains(" call"):
				found = true
	assert_true(found, "states that run logic surface a word badge on the map node")


func test_edge_colors_distinct_by_kind():
	assert_ne(MusicSectionGraphClass.edge_color(MusicSectionGraphClass.KIND_SWITCH),
		MusicSectionGraphClass.edge_color(MusicSectionGraphClass.KIND_TRANSITION),
		"switch wire colour differs from a plain transition")
	assert_ne(MusicSectionGraphClass.edge_color(MusicSectionGraphClass.KIND_BRANCH),
		MusicSectionGraphClass.edge_color(MusicSectionGraphClass.KIND_TRANSITION),
		"branch wire colour differs from a plain transition")


func test_drill_into_swaps_map_for_program_view():
	# Double-clicking a state (or calling _drill_into) replaces the center map with
	# that section's logic-graph blueprint; Back restores the map.
	var lm := _make_live()
	await get_tree().process_frame
	var begin = _node_titled(_nodes(lm.get_node("%SectionMap")), "Begin")
	assert_not_null(begin, "Begin node present")
	var sec := String(begin.get_meta("section", ""))
	lm._drill_into(sec)
	await get_tree().process_frame
	assert_true(lm._program_view.visible, "program view shown after drill-in")
	assert_false(lm.get_node("%SectionMap").visible, "map hidden while drilled in")
	assert_gt((lm._program_view.top_level_rows() as Array).size(), 0,
		"the program view built rows for the section")
	lm._back_to_map()
	assert_false(lm._program_view.visible, "Back hides the program view")
	assert_true(lm.get_node("%SectionMap").visible, "Back restores the map")


func test_new_project_opens_start_blueprint():
	var doc = MusicEditorDocument.new()
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	lm._on_new_project_pressed()
	await get_tree().process_frame
	assert_true(lm._program_view.visible, "New music program opens the start state's blueprint")
	assert_false(lm.get_node("%SectionMap").visible, "map hides while the new start blueprint is open")
	assert_eq(lm._logic_section_name, "Begin", "new projects drill into the generated Begin state")
	assert_not_null(lm._program_view.empty_hint(),
		"the new start program shows the first-step empty-state hint")


func test_added_state_is_badged_as_unlinked_until_connected():
	var doc = MusicEditorDocument.new()
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	lm._on_new_project_pressed()
	await get_tree().process_frame
	lm._back_to_map()
	lm._on_add_state()
	await get_tree().process_frame
	assert_true(lm._program_view.visible, "Add State opens the new state's blueprint")
	assert_string_contains(lm._breadcrumb_label.text, "unlinked",
		"new unreachable states are called out in the breadcrumb")
	lm._back_to_map()
	var node := _node_titled(_nodes(lm.get_node("%SectionMap")), "State_1")
	assert_not_null(node, "new state appears on the map")
	if node == null:
		return
	assert_string_contains(node.title, "unlinked", "unreachable state is badged on the map")
	assert_string_contains(node.tooltip_text, "Add a transition",
		"unreachable state tooltip tells the author how to connect it")


func test_map_node_has_direct_blueprint_button():
	# The primary edit path should be discoverable without knowing the double-click
	# or right-click gestures: each state node carries an explicit blueprint button.
	var lm := _make_live()
	await get_tree().process_frame
	var begin = _node_titled(_nodes(lm.get_node("%SectionMap")), "Begin")
	assert_not_null(begin, "Begin node present")
	var open_btn: Button = begin.find_child("OpenBlueprintButton", true, false)
	assert_not_null(open_btn, "state node exposes a direct blueprint button")
	if open_btn == null:
		return
	open_btn.pressed.emit()
	await get_tree().process_frame
	assert_true(lm._program_view.visible, "pressing the node button opens the blueprint")
	assert_eq(lm._logic_section_name, String(begin.get_meta("section", "")),
		"button opens that state, not a hard-coded section")


func test_rename_dialog_validates_before_confirm():
	var lm := _make_live()
	await get_tree().process_frame
	lm._drill_into("Begin")
	await get_tree().process_frame
	lm._open_rename_dialog("Begin")
	await get_tree().process_frame
	var dlg := _first_confirmation_dialog(lm)
	assert_not_null(dlg, "rename dialog opened")
	if dlg == null:
		return
	var edit: LineEdit = dlg.find_child("StateNameEdit", true, false)
	var hint: Label = dlg.find_child("StateNameValidationHint", true, false)
	assert_not_null(edit, "rename dialog exposes the name input")
	assert_not_null(hint, "rename dialog exposes inline validation")
	if edit == null or hint == null:
		dlg.queue_free()
		return
	var ok := dlg.get_ok_button()
	assert_true(ok.disabled, "unchanged name disables Confirm")
	assert_string_contains(hint.text, "different")
	edit.text = "play"
	edit.text_changed.emit(edit.text)
	assert_true(ok.disabled, "reserved word disables Confirm")
	assert_string_contains(hint.text, "reserved")
	edit.text = "Fresh_State"
	edit.text_changed.emit(edit.text)
	assert_false(ok.disabled, "valid new name enables Confirm")
	assert_eq(hint.text, "")
	dlg.queue_free()


func test_read_only_blueprint_disables_state_actions_with_reason():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	lm._document = ReadOnlyDocument.new()
	lm._drill_into("Begin")
	await get_tree().process_frame
	var rename := _button_with_text(lm._breadcrumb, "✎ Rename")
	var delete := _button_with_text(lm._breadcrumb, "✕ Delete")
	assert_not_null(rename, "breadcrumb rename button present")
	assert_not_null(delete, "breadcrumb delete button present")
	if rename == null or delete == null:
		return
	assert_true(rename.disabled, "read-only script disables breadcrumb rename")
	assert_true(delete.disabled, "read-only script disables breadcrumb delete")
	assert_eq(rename.tooltip_text, "Fix script errors first")
	assert_eq(delete.tooltip_text, "Fix script errors first")


func test_read_only_blueprint_keeps_add_palette_with_reason():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	lm._document = ReadOnlyDocument.new()
	lm._drill_into("Begin")
	await get_tree().process_frame
	var add_menu: MenuButton = lm._program_view.add_menu_button()
	assert_not_null(add_menu, "read-only program view still shows the Add palette")
	if add_menu == null:
		return
	assert_true(add_menu.disabled, "read-only program view disables Add instead of hiding it")
	assert_eq(add_menu.tooltip_text, "Fix script errors first")


func test_read_only_map_disables_add_state_with_reason():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	lm._document = ReadOnlyDocument.new()
	lm._refresh_button_state()
	assert_true(lm._add_state_btn.disabled, "read-only script disables map Add State")
	assert_eq(lm._add_state_btn.tooltip_text, "Fix script errors first")


func _crumb_texts(lm: Control) -> Array:
	var out := []
	for c in lm._crumb_segments.get_children():
		if c is Button:
			out.append(String((c as Button).text))
	return out


func test_drill_hops_record_a_breadcrumb_trail():
	# Open Begin, then hop to Testmission (the "open ▸" path is the same
	# _drill_into): the trail shows the whole journey and back walks it.
	var lm := _make_live()
	await get_tree().process_frame
	lm._drill_into("Begin")
	lm._drill_into("Testmission")
	await get_tree().process_frame
	assert_eq(_crumb_texts(lm), ["Map", "Begin", "Testmission"], "trail records every hop")
	assert_false(lm._nav_back_btn.disabled, "back is available mid-trail")
	lm._nav.go_back()
	assert_eq(lm._logic_section_name, "Begin", "back returns to the previous state")
	assert_false(lm._nav_fwd_btn.disabled, "forward becomes available after back")
	lm._nav.go_forward()
	assert_eq(lm._logic_section_name, "Testmission", "forward retraces the hop")


func test_breadcrumb_segment_click_jumps_back():
	var lm := _make_live()
	await get_tree().process_frame
	lm._drill_into("Begin")
	lm._drill_into("Testmission")
	await get_tree().process_frame
	# Segment 0 is "Map": clicking it returns to the map but keeps forward history.
	var segs: Array = []
	for c in lm._crumb_segments.get_children():
		if c is Button:
			segs.append(c)
	(segs[0] as Button).pressed.emit()
	assert_true(lm.get_node("%SectionMap").visible, "Map segment click restores the map")
	assert_eq(lm._logic_section_name, "", "no state open on the map")
	assert_false(lm._nav_fwd_btn.disabled, "the drilled states stay reachable via forward")


func test_follow_live_drills_replace_instead_of_flooding_the_trail():
	# A transitioning VM (pin=false drills) must not grow the history with every
	# state it enters -- the trail stays one hop deep and back lands on the map.
	var lm := _make_live()
	await get_tree().process_frame
	lm._drill_into("Begin", false)
	lm._drill_into("Testmission", false)
	lm._drill_into("Missionnull", false)
	await get_tree().process_frame
	assert_eq(_crumb_texts(lm), ["Map", "Missionnull"], "follow-live replaces the current hop")
	lm._nav.go_back()
	assert_true(lm.get_node("%SectionMap").visible, "back from a followed state lands on the map")


func test_states_sidebar_lists_map_and_every_state():
	var lm := _make_live()
	await get_tree().process_frame
	var list: ItemList = lm.get_node("%StatesList")
	assert_eq(list.item_count, 9, "map row + 8 jo_gamemus states")
	assert_eq(String(list.get_item_metadata(0)), "", "row 0 is the map")
	var star_rows := 0
	for i in range(list.item_count):
		if String(list.get_item_text(i)).contains("★"):
			star_rows += 1
	assert_eq(star_rows, 1, "exactly one state carries the start badge in the sidebar")


func test_states_sidebar_click_navigates_and_mirrors_location():
	var lm := _make_live()
	await get_tree().process_frame
	var list: ItemList = lm.get_node("%StatesList")
	# Find Begin's row and select it the way a user would.
	var begin_row := -1
	for i in range(list.item_count):
		if String(list.get_item_metadata(i)) == "Begin":
			begin_row = i
			break
	assert_gt(begin_row, 0, "Begin present in the sidebar")
	list.item_selected.emit(begin_row)
	await get_tree().process_frame
	assert_true(lm._program_view.visible, "sidebar click opens the state's blueprint")
	assert_eq(lm._logic_section_name, "Begin")
	assert_true(list.is_selected(begin_row), "sidebar selection mirrors the open state")
	list.item_selected.emit(0)
	await get_tree().process_frame
	assert_true(lm.get_node("%SectionMap").visible, "the map row returns to the map")
	assert_true(list.is_selected(0), "sidebar selection follows back to the map row")


func test_from_scratch_add_state_reachable_while_drilled():
	# A new project drops the user straight into Begin's blueprint, where the map
	# toolbar (and its Add State button) is hidden. The sidebar's pinned Add state
	# must carry the flow: press it, get State_1, land in its blueprint with the
	# trail recording the hop -- no "go back to the map first" knowledge required.
	var doc = MusicEditorDocument.new()
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	lm._on_new_project_pressed()
	await get_tree().process_frame
	assert_true(lm._program_view.visible, "from-scratch lands in Begin's blueprint")
	assert_false(lm._map_toolbar.visible, "the map toolbar (old Add State home) is hidden here")
	var side_add: Button = lm._sidebar_add_state_btn
	assert_not_null(side_add, "the sidebar carries a pinned Add state button")
	if side_add == null:
		return
	assert_true(side_add.is_visible_in_tree(), "sidebar Add state stays visible while drilled in")
	assert_false(side_add.disabled, "and is enabled on a fresh editable project")
	side_add.pressed.emit()
	await get_tree().process_frame
	assert_eq(lm._logic_section_name, "State_1", "pressing it creates and opens the new state")
	assert_eq(_crumb_texts(lm), ["Map", "Begin", "State_1"], "the hop lands on the trail")


func _button_with_text(root: Node, text: String) -> Button:
	if root == null:
		return null
	for c in root.get_children():
		if c is Button and String((c as Button).text) == text:
			return c
	return null


func _first_confirmation_dialog(root: Node) -> ConfirmationDialog:
	for c in root.get_children():
		if c is ConfirmationDialog:
			return c
	return null


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
