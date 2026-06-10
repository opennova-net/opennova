extends GutTest

## Shell-integration tests: the Strings workspace appears in the rail, mounts its
## table in the viewport host, and exposes its document actions.

const EditorWorkstationScene = preload("res://modtools/editor/editor_workstation.tscn")
const EditorWorkstationScript = preload("res://modtools/editor/editor_workstation.gd")

const STATE_PATH := "user://strings_editor_state.cfg"


func before_each() -> void:
	# Session restore reads this on activate; stale state from another test (or a
	# real editor run sharing user://) would change what opens.
	if FileAccess.file_exists(STATE_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_PATH))


func after_all() -> void:
	if FileAccess.file_exists(STATE_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_PATH))


func test_strings_workspace_in_rail() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var rail: BoxContainer = workstation.get_node("%WorkspaceRail")
	var row_texts := []
	for child in rail.get_children():
		if child is Button:
			row_texts.append((child as Button).text)
	assert_true(row_texts.has("Strings"), "Strings should join the workspace nav.")
	assert_eq(row_texts[5], "Strings", "Strings should be the last switchable workspace row.")


func test_strings_workspace_mounts_self_contained_view() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame

	var host: Control = workstation.get_node("%ViewportHost")
	assert_eq(host.get_child_count(), 1, "Strings should own the viewport host while active.")
	var view: Control = host.get_child(0)
	assert_eq(view.name, "StringsEditorView", "Strings should mount its self-contained editor view.")
	# The view bundles the table and the detail editor (no separate right dock).
	assert_not_null(view.find_child("StringsTableView", true, false), "The view should contain the entry table.")
	assert_not_null(view.find_child("StringsDetailPanel", true, false), "The view should contain the detail editor.")

	# The shared right asset dock stays hidden for Strings.
	var asset_dock: Control = workstation.get_node("%AssetDock")
	assert_false(asset_dock.visible, "Strings should not use the right asset dock.")


func test_strings_workspace_exposes_document_actions() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame

	var actions: BoxContainer = workstation.get_node("%WorkspaceActionsHost")
	assert_not_null(_find_button_by_text(actions, "Open Strings..."), "Strings should expose an Open action.")
	assert_not_null(_find_button_by_text(actions, "New Strings"), "Strings should expose a New action.")


func test_open_populates_table() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame

	var ws = workstation._get_workspace(EditorWorkstationScript.Workspace.STRINGS)
	assert_not_null(ws, "the strings workspace adapter should exist")
	var err: int = ws.open_file("res://fixtures/strings/menu.bin")
	assert_eq(err, OK, "opening the fixture should succeed")
	await get_tree().process_frame

	var host: Control = workstation.get_node("%ViewportHost")
	var tree: Tree = host.get_child(0).find_child("StringsTree", true, false)
	assert_not_null(tree, "the table Tree should exist")
	var root := tree.get_root()
	assert_not_null(root, "the table should have a root")
	assert_eq(root.get_child_count(), 6, "all six fixture entries should be listed after open")


func test_section_filter_scopes_rows() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame
	var ws = workstation._get_workspace(EditorWorkstationScript.Workspace.STRINGS)
	assert_eq(ws.open_file("res://fixtures/strings/menu.bin"), OK)
	await get_tree().process_frame

	var host: Control = workstation.get_node("%ViewportHost")
	var tree: Tree = host.get_child(0).find_child("StringsTree", true, false)

	assert_eq(tree.get_root().get_child_count(), 6, "the All filter should show every entry")

	ws.set_section_filter(0)  # menu_main has 3 entries
	await get_tree().process_frame
	assert_eq(tree.get_root().get_child_count(), 3, "filtering to section 0 should scope to its entries")

	ws.set_section_filter(-1)  # back to All
	await get_tree().process_frame
	assert_eq(tree.get_root().get_child_count(), 6, "clearing the filter should restore every entry")


func test_detail_panel_content_fills_panel() -> void:
	# Regression: the detail/inspector roots are bare Controls; their inner
	# make_inspector_box wrapper must be stretched to fill, not collapse to a
	# min-size box in the top-left corner.
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	for _i in 3:
		await get_tree().process_frame

	var host: Control = workstation.get_node("%ViewportHost")
	var detail: Control = host.get_child(0).find_child("StringsDetailPanel", true, false)
	assert_not_null(detail, "detail panel should be mounted")
	assert_gt(detail.size.x, 50.0, "detail panel should have a real width from the split")
	var wrapper: Control = detail.get_child(0)
	assert_almost_eq(wrapper.size.x, detail.size.x, 2.0, "content wrapper should fill the panel width")
	assert_gt(wrapper.size.y, 80.0, "content wrapper should fill the panel height, not collapse")


func test_session_restore_reopens_last_file() -> void:
	# First session: open the fixture (which persists the session state).
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame
	var ws = workstation._get_workspace(EditorWorkstationScript.Workspace.STRINGS)
	assert_eq(ws.open_file("res://fixtures/strings/menu.bin"), OK)
	workstation.queue_free()
	await get_tree().process_frame

	# Second session: activating the workspace should reopen the last file.
	var workstation2 = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation2.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame
	var ws2 = workstation2._get_workspace(EditorWorkstationScript.Workspace.STRINGS)
	assert_eq(ws2.strings_editor.current_path, "res://fixtures/strings/menu.bin",
		"the last opened table should be restored on activate")
	assert_eq(ws2.strings_editor.string_table.get_entry_count(), 6)
	assert_false(ws2.strings_editor.is_dirty, "a restored table starts clean")


func test_inspector_lookup_tester_resolves_like_the_game() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame
	var ws = workstation._get_workspace(EditorWorkstationScript.Workspace.STRINGS)
	assert_eq(ws.open_file("res://fixtures/strings/menu.bin"), OK)
	await get_tree().process_frame

	var lookup: LineEdit = workstation.find_child("StringsLookupEdit", true, false)
	var result: RichTextLabel = workstation.find_child("StringsLookupResult", true, false)
	assert_not_null(lookup, "the inspector should host the lookup tester")
	assert_not_null(result, "the lookup tester should have a result readout")

	lookup.text = "menu_main:BTN_NEW_GAME"
	lookup.text_changed.emit(lookup.text)
	assert_string_contains(result.text, "New Game", "a scoped hit should preview the in-game text ({hot} stripped)")

	lookup.text = "hud:BTN_NEW_GAME"
	lookup.text_changed.emit(lookup.text)
	assert_string_contains(result.text, "??hud:BTN_NEW_GAME??",
		"a scoped miss should show the engine's ?? marker")

	var normalize: Button = workstation.find_child("StringsNormalizeButton", true, false)
	assert_not_null(normalize, "the normalize affordance should exist")
	assert_false(normalize.visible, "normalize stays hidden while the table is grouped")


func _find_button_by_text(root: Node, text: String) -> Button:
	if root is Button and (root as Button).text == text:
		return root
	for child in root.get_children():
		var found := _find_button_by_text(child, text)
		if found != null:
			return found
	return null
