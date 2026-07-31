extends GutTest

## The Menus workspace on document tabs (B5, mirroring the Strings pilot):
## dirty isolation, tab-aware reopen + cross-jump, New-beside-dirty, per-tab
## undo stash across the shared editor, session restore, and the
## pathless-save shell contract.

const MnuWorkspaceScript = preload("res://modtools/mnu/mnu_workspace.gd")
const EditorWorkstationScene = preload("res://modtools/editor/editor_workstation.tscn")
const EditorWorkstationScript = preload("res://modtools/editor/editor_workstation.gd")

const FIXTURE := "res://../fixtures/mnu/widgets.mnu"
const SECOND := "user://mnu_multidoc_second.mnu"
const STATE_PATH := "user://mnu_editor_state.cfg"


func before_each() -> void:
	if FileAccess.file_exists(STATE_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_PATH))
	var bytes := FileAccess.get_file_as_bytes(FIXTURE)
	var out := FileAccess.open(SECOND, FileAccess.WRITE)
	out.store_buffer(bytes)
	out.close()


func after_all() -> void:
	for path in [STATE_PATH, SECOND]:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))


func _first_root_child(doc: NovaMnuDocument, index: int) -> int:
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	return doc.get_child_ids(root)[index]


func _dirty_active(ws) -> void:
	ws._document.resource.set_widget_name(_first_root_child(ws._document.resource, 1), "Edited")


func test_dirty_badge_isolates_to_the_edited_tab() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(FIXTURE), OK)
	assert_eq(ws.open_file(SECOND), OK)

	_dirty_active(ws)  # edits the ACTIVE menu (the second)

	var tabs: Array[DocumentTabRow] = ws.get_document_tabs()
	assert_eq(tabs.size(), 2, "each menu gets its own tab")
	assert_false(tabs[0].dirty, "the untouched tab stays clean")
	assert_true(tabs[1].dirty, "only the edited tab is dirty")
	assert_true(ws.has_unsaved_changes(), "the workspace reports any dirty tab")

	assert_eq(ws.activate_document(0), OK)
	assert_false(ws._document.is_dirty, "switching tabs lands on the clean document")
	assert_true(ws.has_unsaved_changes(), "the background dirty tab still counts")


func test_reopening_an_open_menu_activates_its_tab_with_edits_intact() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(FIXTURE), OK)
	_dirty_active(ws)
	var edited_id := _first_root_child(ws._document.resource, 1)
	assert_eq(ws.open_file(SECOND), OK)

	assert_eq(ws.open_file(FIXTURE), OK, "reopening an open menu succeeds")
	assert_eq(ws.get_active_document_index(), 0, "…by activating its existing tab")
	assert_eq(ws.get_document_tabs().size(), 2, "…not by adding a duplicate")
	assert_eq(ws._document.resource.get_widget_name(edited_id), "Edited",
		"the unsaved edit survived (no silent reload)")


func test_new_menu_opens_its_own_tab_beside_unsaved_work() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(FIXTURE), OK)
	_dirty_active(ws)

	assert_eq(ws.new_current(), OK)
	assert_eq(ws.get_document_tabs().size(), 2, "New leaves the dirty menu open in its own tab")
	assert_eq(ws._document.current_path, "", "the new active document is untitled")
	var tabs: Array[DocumentTabRow] = ws.get_document_tabs()
	assert_true(tabs[0].dirty, "the unsaved work is intact")


func test_pathless_save_returns_the_shell_save_as_code() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	# The shell's dirty-close Save flow opens Save As only on ERR_INVALID_PARAMETER
	# (editor_workstation save-then-close routing); the document base keeps
	# returning ERR_UNAVAILABLE underneath.
	assert_eq(ws.save_current(), ERR_INVALID_PARAMETER,
		"a pathless workspace save routes the shell to Save As")
	assert_eq(ws._document.save_current(), ERR_UNAVAILABLE,
		"the document-level contract is unchanged")


func test_undo_history_survives_tab_switches() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	var mount := Control.new()
	mount.size = Vector2(800, 480)
	add_child_autofree(mount)
	assert_eq(ws.open_file(FIXTURE), OK)
	ws.mount_viewport(mount)
	await get_tree().process_frame

	# An inspector-style edit through the editor builds real undo history.
	var id := _first_root_child(ws._document.resource, 1)
	ws._editor.apply_edit({"target": "widget", "id": id, "prop": "name", "value": "Edited"})
	assert_true(ws._editor.can_undo(), "the edit is undoable")

	assert_eq(ws.open_file(SECOND), OK)
	assert_false(ws._editor.can_undo(), "the fresh tab starts with no history")

	assert_eq(ws.activate_document(0), OK)
	assert_true(ws._editor.can_undo(), "switching back restores the tab's history")
	ws._editor.undo()
	assert_ne(ws._document.resource.get_widget_name(id), "Edited",
		"the restored history undoes the right document's edit")


func test_closing_a_tab_clamps_active_and_never_leaves_zero_tabs() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(FIXTURE), OK)
	assert_eq(ws.open_file(SECOND), OK)

	assert_eq(ws.close_document(1), OK)
	assert_eq(ws.get_document_tabs().size(), 1, "closing removes the tab")
	assert_eq(ws._document.current_path, FIXTURE, "the neighbor becomes active")

	assert_eq(ws.close_document(0), OK)
	assert_eq(ws.get_document_tabs().size(), 1, "the workspace never holds zero documents")
	assert_eq(ws._document.current_path, "", "…the reseeded document is pristine")


func test_session_restore_reopens_all_tabs_and_active_index() -> void:
	var second_abs := ProjectSettings.globalize_path(SECOND)
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(FIXTURE), OK)
	assert_eq(ws.open_file(second_abs), OK)
	assert_eq(ws.activate_document(0), OK)  # saves active_index = 0
	ws.deactivate()

	var ws2 = autofree(MnuWorkspaceScript.new())
	ws2.activate()
	var tabs: Array[DocumentTabRow] = ws2.get_document_tabs()
	assert_eq(tabs.size(), 2, "both menus reopen")
	assert_eq(ws2.get_active_document_index(), 0, "the active tab is restored")
	assert_eq(ws2._document.current_path, FIXTURE, "the alias points at the restored active tab")


func test_session_active_index_skips_untitled_tabs() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	ws._document.mark_dirty()  # the seeded Untitled tab holds work (unpersisted)
	assert_eq(ws.open_file(FIXTURE), OK)  # full-list index 1
	assert_eq(ws.open_file(SECOND), OK)  # full-list index 2
	assert_eq(ws.activate_document(1), OK)  # FIXTURE active
	ws.deactivate()

	var ws2 = autofree(MnuWorkspaceScript.new())
	ws2.activate()
	assert_eq(ws2.get_document_tabs().size(), 2, "only pathful tabs persist")
	assert_eq(ws2._document.current_path, FIXTURE,
		"active_index is stored in open_paths space - Untitled tabs cannot drift it")


func test_cross_jump_lands_in_the_existing_tab() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var root := ProjectSettings.globalize_path("res://../fixtures/mnu")
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK)

	assert_eq(workstation.open_menu_workspace("sp.mnu", "SINGLE_PLAYER"), OK)
	await get_tree().process_frame
	var ws = workstation._workspaces.get(EditorWorkstationScript.Workspace.MNU)
	assert_eq(ws.get_document_tabs().size(), 1, "the jump landed in one tab")
	ws._document.mark_dirty()

	# Jumping to the same menu again focuses the open tab, edits intact.
	assert_eq(workstation.open_menu_workspace("sp.mnu", "SINGLE_PLAYER"), OK)
	await get_tree().process_frame
	assert_eq(ws.get_document_tabs().size(), 1, "no duplicate tab for an open menu")
	assert_true(ws._document.is_dirty, "the unsaved edit survived the jump")
