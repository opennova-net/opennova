extends GutTest

## The document-tab tier: the shell's tab strip (signal-driven, hidden for
## single-document workspaces), the dirty-close prompt routing, and the
## DocumentTabSet model.

const EditorWorkstationScene = preload("res://modtools/editor/editor_workstation.tscn")
const EditorWorkstationScript = preload("res://modtools/editor/editor_workstation.gd")
const DocumentTabSetScript = preload("res://modtools/framework/document_tab_set.gd")

const STATE_PATH := "user://strings_editor_state.cfg"
const FIXTURE := "res://fixtures/strings/menu.bin"
const SECOND_TABLE := "user://document_tabs_second.bin"


func before_each() -> void:
	if FileAccess.file_exists(STATE_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_PATH))
	# A second distinct table path: same bytes, different file.
	var bytes := FileAccess.get_file_as_bytes(FIXTURE)
	var out := FileAccess.open(SECOND_TABLE, FileAccess.WRITE)
	out.store_buffer(bytes)
	out.close()


func after_all() -> void:
	for path in [STATE_PATH, SECOND_TABLE]:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))


func _strings_shell() -> Dictionary:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	return {"shell": workstation, "ws": workstation._get_workspace(EditorWorkstationScript.Workspace.STRINGS)}


func _tab_row(shell: Node) -> HBoxContainer:
	return shell.get_node("%DocumentTabRow")


func _tab_buttons(shell: Node) -> Array:
	var row := _tab_row(shell)
	var buttons := []
	for child in row.get_children():
		if child is Button and not String(child.name).begins_with("DocumentTabClose"):
			buttons.append(child)
	return buttons


func test_strip_hidden_for_single_document_workspace() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	# Terrain (the default workspace) does not opt into document tabs.
	assert_false((workstation.get_node("%DocumentTabStrip") as Control).visible,
		"the strip stays hidden for single-document workspaces")


func test_strings_shows_one_untitled_tab() -> void:
	var ctx := _strings_shell()
	await get_tree().process_frame
	assert_true((ctx.shell.get_node("%DocumentTabStrip") as Control).visible,
		"the strip shows for the tabbed Strings workspace")
	var buttons := _tab_buttons(ctx.shell)
	assert_eq(buttons.size(), 1, "a fresh Strings workspace holds one tab")
	assert_eq((buttons[0] as Button).text, "Untitled", "the pristine tab is Untitled")


func test_opening_tables_adds_tabs_and_click_activates() -> void:
	var ctx := _strings_shell()
	await get_tree().process_frame
	assert_eq(ctx.ws.open_file(FIXTURE), OK)
	assert_eq(ctx.ws.open_file(SECOND_TABLE), OK)
	await get_tree().process_frame

	var buttons := _tab_buttons(ctx.shell)
	assert_eq(buttons.size(), 2, "two opened tables, two tabs (the pristine tab was reused)")
	assert_eq(ctx.ws.get_active_document_index(), 1, "the last opened table is active")

	(buttons[0] as Button).pressed.emit()
	assert_eq(ctx.ws.get_active_document_index(), 0, "clicking a tab activates its document")
	assert_eq(ctx.ws.strings_editor.current_path, FIXTURE, "the alias follows the active tab")


func test_clean_close_removes_tab() -> void:
	var ctx := _strings_shell()
	await get_tree().process_frame
	assert_eq(ctx.ws.open_file(FIXTURE), OK)
	assert_eq(ctx.ws.open_file(SECOND_TABLE), OK)
	await get_tree().process_frame

	var close0 := _tab_row(ctx.shell).find_child("DocumentTabClose0", false, false) as Button
	close0.pressed.emit()
	await get_tree().process_frame

	assert_eq(_tab_buttons(ctx.shell).size(), 1, "a clean close removes the tab")
	assert_eq(ctx.ws.strings_editor.current_path, SECOND_TABLE, "the surviving tab is bound")
	assert_null(ctx.shell.find_child("UnsavedChangesDialog", true, false),
		"no prompt for a clean close")


func test_dirty_close_prompts_cancel_keeps_discard_closes() -> void:
	var ctx := _strings_shell()
	await get_tree().process_frame
	assert_eq(ctx.ws.open_file(FIXTURE), OK)
	ctx.ws.add_entry_default()
	assert_true(ctx.ws.strings_editor.is_dirty, "the edit dirtied the table")
	await get_tree().process_frame

	var close0 := _tab_row(ctx.shell).find_child("DocumentTabClose0", false, false) as Button
	close0.pressed.emit()
	var dialog := ctx.shell.find_child("UnsavedChangesDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "a dirty close pops the shared unsaved-changes dialog")
	assert_true(dialog.visible, "the dialog is shown")

	dialog.canceled.emit()
	assert_eq(_tab_buttons(ctx.shell).size(), 1, "keep editing leaves the tab open")
	assert_true(ctx.ws.strings_editor.is_dirty, "the edits survive")

	close0 = _tab_row(ctx.shell).find_child("DocumentTabClose0", false, false) as Button
	close0.pressed.emit()
	dialog.custom_action.emit(&"discard")
	await get_tree().process_frame
	assert_eq(ctx.ws.strings_editor.current_path, "", "discard closed the table; a fresh Untitled was seeded")
	assert_false(ctx.ws.strings_editor.is_dirty, "the seeded replacement is pristine")


func test_strip_is_not_rebuilt_by_the_per_frame_poll() -> void:
	var ctx := _strings_shell()
	await get_tree().process_frame
	assert_eq(ctx.ws.open_file(FIXTURE), OK)
	await get_tree().process_frame

	var ids_before := []
	for child in _tab_row(ctx.shell).get_children():
		ids_before.append(child.get_instance_id())
	await get_tree().process_frame
	await get_tree().process_frame
	var ids_after := []
	for child in _tab_row(ctx.shell).get_children():
		ids_after.append(child.get_instance_id())
	assert_eq(ids_after, ids_before,
		"with no document events, the strip's children survive frames untouched (signal-driven, not polled)")


func test_prompt_callables_are_consumed_on_dispatch() -> void:
	# The dialog outcomes are one-shot: after Save dispatches, a stray second
	# dispatch (double signal, stale dialog) must not re-run the callable.
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var outcome := {"saved": 0}
	workstation.prompt_unsaved_for(func() -> void: outcome.saved += 1, func() -> void: pass)
	workstation._save_export._on_prompt_save_changes()
	workstation._save_export._on_prompt_save_changes()
	assert_eq(outcome.saved, 1, "the save outcome runs exactly once")


# --- DocumentTabSet (the model) ---

class FakeDoc:
	extends RefCounted
	var current_path := ""
	var is_dirty := false


func test_tab_set_add_activate_and_rows() -> void:
	var tabs: DocumentTabSet = DocumentTabSetScript.new()
	var changes := [0]
	tabs.changed.connect(func() -> void: changes[0] += 1)
	var a := FakeDoc.new()
	a.current_path = "user://a.bin"
	var b := FakeDoc.new()
	b.is_dirty = true

	tabs.add(a)
	tabs.add(b)
	assert_eq(tabs.count(), 2)
	assert_eq(tabs.get_active_index(), 1, "add activates by default")
	assert_eq(changes[0], 2, "each add emits once")

	var rows := tabs.tabs()
	assert_eq(rows[0].label, "a.bin")
	assert_eq(rows[1].label, "Untitled")
	assert_true(rows[1].dirty)
	assert_eq(tabs.index_of_path("USER://A.BIN"), 0, "path match is case-insensitive")
	assert_eq(tabs.index_of_path(""), -1, "empty path never matches")


func test_tab_set_remove_clamps_active_to_neighbor() -> void:
	var tabs: DocumentTabSet = DocumentTabSetScript.new()
	var a := FakeDoc.new()
	var b := FakeDoc.new()
	var c := FakeDoc.new()
	tabs.add(a)
	tabs.add(b)
	tabs.add(c)
	assert_eq(tabs.set_active(2), OK)

	assert_eq(tabs.remove_at(2), c, "remove returns the document")
	assert_eq(tabs.get_active_index(), 1, "active clamps to the nearest survivor")

	assert_eq(tabs.set_active(0), OK)
	tabs.remove_at(1)
	assert_eq(tabs.get_active_index(), 0, "removing behind the active keeps it")

	tabs.remove_at(0)
	assert_eq(tabs.get_active_index(), -1, "an empty set has no active document")
	assert_null(tabs.get_active())


func test_tab_set_set_active_noop_does_not_emit() -> void:
	var tabs: DocumentTabSet = DocumentTabSetScript.new()
	tabs.add(FakeDoc.new())
	var changes := [0]
	tabs.changed.connect(func() -> void: changes[0] += 1)
	assert_eq(tabs.set_active(0), OK, "re-activating the active index is OK")
	assert_eq(changes[0], 0, "but emits nothing")
	assert_eq(tabs.set_active(5), ERR_INVALID_PARAMETER, "out of range is rejected")
