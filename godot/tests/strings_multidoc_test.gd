extends GutTest

## The Strings workspace as the document-tabs pilot: N open tables, dirty
## isolation, tab-aware reopen/cross-jump, and multi-tab session restore
## (including the pre-tabs last_path fallback).

const EditorWorkstationScene = preload("res://modtools/editor/editor_workstation.tscn")
const EditorWorkstationScript = preload("res://modtools/editor/editor_workstation.gd")

const STATE_PATH := "user://strings_editor_state.cfg"
const FIXTURE := "res://fixtures/strings/menu.bin"
const SECOND_TABLE := "user://strings_multidoc_second.bin"


func before_each() -> void:
	if FileAccess.file_exists(STATE_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_PATH))
	var bytes := FileAccess.get_file_as_bytes(FIXTURE)
	var out := FileAccess.open(SECOND_TABLE, FileAccess.WRITE)
	out.store_buffer(bytes)
	out.close()


func after_all() -> void:
	for path in [STATE_PATH, SECOND_TABLE]:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))


func _strings_ws(workstation: Node):
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	return workstation._get_workspace(EditorWorkstationScript.Workspace.STRINGS)


func test_dirty_badge_isolates_to_the_edited_tab() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var ws = _strings_ws(workstation)
	await get_tree().process_frame
	assert_eq(ws.open_file(FIXTURE), OK)
	assert_eq(ws.open_file(SECOND_TABLE), OK)

	ws.add_entry_default()  # edits the ACTIVE table (the second)

	var tabs: Array[DocumentTabRow] = ws.get_document_tabs()
	assert_false(tabs[0].dirty, "the untouched tab stays clean")
	assert_true(tabs[1].dirty, "only the edited tab is dirty")
	assert_true(ws.has_unsaved_changes(), "the workspace reports any dirty tab")

	assert_eq(ws.activate_document(0), OK)
	assert_false(ws.strings_editor.is_dirty, "switching tabs lands on the clean document")
	assert_true(ws.has_unsaved_changes(), "the background dirty tab still counts")


func test_reopening_an_open_table_activates_its_tab_with_edits_intact() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var ws = _strings_ws(workstation)
	await get_tree().process_frame
	assert_eq(ws.open_file(FIXTURE), OK)
	ws.add_entry_default()
	var entries_after_edit: int = ws.strings_editor.string_table.get_entry_count()
	assert_eq(ws.open_file(SECOND_TABLE), OK)

	assert_eq(ws.open_file(FIXTURE), OK, "reopening an open table succeeds")
	assert_eq(ws.get_active_document_index(), 0, "…by activating its existing tab")
	assert_eq(ws.get_document_tabs().size(), 2, "…not by adding a duplicate")
	assert_eq(ws.strings_editor.string_table.get_entry_count(), entries_after_edit,
		"the unsaved edit survived (no silent reload)")


func test_cross_jump_lands_in_the_existing_tab() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var ws = _strings_ws(workstation)
	await get_tree().process_frame
	assert_eq(ws.open_file(FIXTURE), OK)
	assert_eq(ws.open_file(SECOND_TABLE), OK)

	assert_eq(ws.open_strings_table(FIXTURE, "BTN_NEW_GAME"), OK)
	assert_eq(ws.get_active_document_index(), 0, "the cross-jump activates the open tab")
	assert_true(ws.strings_editor.selected_index >= 0, "…and focuses the key")


func test_session_restore_reopens_all_tabs_and_active_index() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var ws = _strings_ws(workstation)
	await get_tree().process_frame
	assert_eq(ws.open_file(FIXTURE), OK)
	assert_eq(ws.open_file(SECOND_TABLE), OK)
	assert_eq(ws.activate_document(0), OK)  # saves active_index = 0
	workstation.queue_free()
	await get_tree().process_frame

	var workstation2 = add_child_autofree(EditorWorkstationScene.instantiate())
	var ws2 = _strings_ws(workstation2)
	await get_tree().process_frame
	var tabs: Array[DocumentTabRow] = ws2.get_document_tabs()
	assert_eq(tabs.size(), 2, "both tables reopen")
	assert_eq(ws2.get_active_document_index(), 0, "the active tab is restored")
	assert_eq(ws2.strings_editor.current_path, FIXTURE, "the alias points at the restored active tab")


func test_legacy_last_path_session_still_restores_one_tab() -> void:
	var cfg := ConfigFile.new()
	cfg.set_value("session", "last_path", FIXTURE)
	cfg.save(STATE_PATH)

	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var ws = _strings_ws(workstation)
	await get_tree().process_frame
	assert_eq(ws.get_document_tabs().size(), 1, "a pre-tabs session restores a single tab")
	assert_eq(ws.strings_editor.current_path, FIXTURE, "…holding the last-opened table")
	assert_false(ws.strings_editor.is_dirty, "a restored table starts clean")


func test_new_strings_opens_its_own_tab_beside_unsaved_work() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var ws = _strings_ws(workstation)
	await get_tree().process_frame
	assert_eq(ws.open_file(FIXTURE), OK)
	ws.add_entry_default()

	assert_eq(ws.new_current(), OK)
	assert_eq(ws.get_document_tabs().size(), 2, "New leaves the dirty table open in its own tab")
	assert_eq(ws.strings_editor.current_path, "", "the new active document is untitled")
	var tabs: Array[DocumentTabRow] = ws.get_document_tabs()
	assert_true(tabs[0].dirty, "the unsaved work is intact")
