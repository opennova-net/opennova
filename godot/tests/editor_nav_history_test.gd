extends GutTest

## Global navigation history: the EditorNavHistory stacks themselves, and the
## shell's Back/Forward over lazy departure snapshots — recorded only at the
## user navigation entry points (_on_workspace_pressed, open_in_workspace),
## restored through _navigate_to, driven by the top-bar buttons, Alt+arrows,
## and the mouse back/forward thumb buttons.

const EditorWorkstationScene = preload("res://modtools/editor/editor_workstation.tscn")
const EditorWorkstationScript = preload("res://modtools/editor/editor_workstation.gd")

const STRINGS_STATE_PATH := "user://strings_editor_state.cfg"
const STRINGS_FIXTURE := "res://fixtures/strings/menu.bin"
const SCRATCH_TABLE := "user://nav_history_scratch.bin"


func before_each() -> void:
	if FileAccess.file_exists(STRINGS_STATE_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STRINGS_STATE_PATH))
	# A second distinct table path: same bytes, different file.
	var bytes := FileAccess.get_file_as_bytes(STRINGS_FIXTURE)
	var out := FileAccess.open(SCRATCH_TABLE, FileAccess.WRITE)
	out.store_buffer(bytes)
	out.close()


func after_all() -> void:
	for path in [STRINGS_STATE_PATH, SCRATCH_TABLE]:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))


func _entry(id: int, path: String) -> EditorNavLocation:
	return EditorNavLocation.make(id, path)


# --- The pure history stacks -------------------------------------------------


func test_record_arms_back_and_navigation_invalidates_forward() -> void:
	var nav := EditorNavHistory.new()
	assert_false(nav.can_go_back(), "no history before any navigation")
	assert_false(nav.can_go_forward())
	nav.record(_entry(1, "a.bin"))
	assert_true(nav.can_go_back(), "a departure should arm Back")
	nav.commit_back(_entry(2, "b.bin"))
	assert_true(nav.can_go_forward(), "going back should arm Forward")
	nav.record(_entry(3, "c.bin"))
	assert_false(nav.can_go_forward(), "a new navigation should clear the forward stack")


func test_commit_back_returns_entry_and_forward_round_trips() -> void:
	var nav := EditorNavHistory.new()
	nav.record(_entry(1, "a.bin"))
	var entry := nav.commit_back(_entry(2, "b.bin"))
	assert_eq(entry.workspace_id, 1, "Back should yield the recorded departure")
	assert_eq(entry.path, "a.bin")
	assert_false(nav.can_go_back())
	var fwd := nav.commit_forward(_entry(1, "a.bin"))
	assert_eq(fwd.path, "b.bin", "Forward should yield the location Back left")
	assert_eq(nav.back_count(), 1, "going forward should re-feed the back stack")


func test_consecutive_duplicate_records_collapse() -> void:
	var nav := EditorNavHistory.new()
	nav.record(_entry(1, "a.bin"))
	nav.record(_entry(1, "a.bin"))
	assert_eq(nav.back_count(), 1, "the same departure twice in a row is one entry")


func test_cap_drops_the_oldest_entry() -> void:
	var nav := EditorNavHistory.new()
	for i in range(EditorNavHistory.MAX_ENTRIES + 1):
		nav.record(_entry(i, "f%d.bin" % i))
	assert_eq(nav.back_count(), EditorNavHistory.MAX_ENTRIES)
	var entry: EditorNavLocation = null
	while nav.can_go_back():
		entry = nav.commit_back(_entry(99, ""))
	assert_eq(entry.workspace_id, 1, "entry 0 should have been dropped, not 1")


func test_drop_back_discards_without_touching_forward() -> void:
	var nav := EditorNavHistory.new()
	nav.record(_entry(1, "a.bin"))
	nav.record(_entry(2, "b.bin"))
	nav.commit_back(_entry(3, "c.bin"))
	nav.drop_back()
	assert_false(nav.can_go_back(), "the dead entry should be gone")
	assert_eq(nav.forward_count(), 1, "dropping a back entry should leave forward alone")


func test_peeks_return_null_when_empty() -> void:
	var nav := EditorNavHistory.new()
	assert_null(nav.peek_back())
	assert_null(nav.peek_forward())


# --- The shell: recording, restoring, buttons, shortcuts ----------------------


func _make_shell() -> Node:
	var shell: Node = EditorWorkstationScene.instantiate()
	add_child_autofree(shell)
	return shell


func test_rail_press_records_and_back_forward_round_trip() -> void:
	var shell := _make_shell()
	var back_btn: Button = shell.get_node("%NavBackButton")
	var fwd_btn: Button = shell.get_node("%NavForwardButton")
	assert_true(back_btn.disabled, "no history at startup")
	assert_true(fwd_btn.disabled)
	shell._on_workspace_pressed(EditorWorkstationScript.Workspace.STRINGS)
	assert_eq(shell.get_active_workspace_id(), EditorWorkstationScript.Workspace.STRINGS)
	assert_false(back_btn.disabled, "leaving Mission should arm Back")
	shell.go_back()
	assert_eq(shell.get_active_workspace_id(), EditorWorkstationScript.Workspace.MISSION,
		"Back should return to the departed workspace")
	assert_false(fwd_btn.disabled, "Back should arm Forward")
	shell.go_forward()
	assert_eq(shell.get_active_workspace_id(), EditorWorkstationScript.Workspace.STRINGS,
		"Forward should redo the switch")
	assert_true(fwd_btn.disabled, "Forward should be spent after redoing")


func test_link_jump_records_and_back_restores_the_document() -> void:
	var shell := _make_shell()
	assert_eq(shell.open_in_workspace("strings", STRINGS_FIXTURE), OK)
	assert_eq(shell.get_active_workspace_id(), EditorWorkstationScript.Workspace.STRINGS)
	shell.go_back()
	assert_eq(shell.get_active_workspace_id(), EditorWorkstationScript.Workspace.MISSION,
		"Back should leave the jump target")
	shell.go_forward()
	assert_eq(shell.get_active_workspace_id(), EditorWorkstationScript.Workspace.STRINGS)
	var ws: EditorWorkspace = shell._get_workspace(EditorWorkstationScript.Workspace.STRINGS)
	assert_eq(String(ws.get_current_resource_path()), STRINGS_FIXTURE,
		"Forward should land on the jumped-to document")


func test_same_location_focus_jump_records_nothing() -> void:
	var shell := _make_shell()
	assert_eq(shell.open_in_workspace("strings", STRINGS_FIXTURE), OK)
	var count: int = shell._nav_history.back_count()
	assert_eq(shell.open_in_workspace("strings", STRINGS_FIXTURE, FocusPayload.for_key("BTN_NEW_GAME")), OK)
	assert_eq(shell._nav_history.back_count(), count,
		"a focus-only jump does not move the user, so it must not grow history")


func test_failed_jump_records_nothing() -> void:
	var shell := _make_shell()
	assert_ne(shell.open_in_workspace("strings", "user://nav_history_missing.bin"), OK,
		"the missing table should fail to open")
	assert_false(shell._nav_history.can_go_back(), "a failed open is not a navigation")


func test_environment_press_records_nothing() -> void:
	var shell := _make_shell()
	shell._on_workspace_pressed(EditorWorkstationScript.Workspace.ENVIRONMENT)
	assert_eq(shell.get_active_workspace_id(), EditorWorkstationScript.Workspace.MISSION,
		"the Environment popup must not change the active workspace")
	assert_false(shell._nav_history.can_go_back(), "the popup is not a location")


func test_back_reuses_a_still_open_document_without_a_disk_read() -> void:
	var shell := _make_shell()
	assert_eq(shell.open_in_workspace("strings", SCRATCH_TABLE), OK)
	shell._on_workspace_pressed(EditorWorkstationScript.Workspace.MISSION)
	# The table stays open in the (persistent) Strings workspace, so Back must
	# succeed by switching alone — deleting the file proves no reopen happens.
	DirAccess.remove_absolute(ProjectSettings.globalize_path(SCRATCH_TABLE))
	shell.go_back()
	assert_eq(shell.get_active_workspace_id(), EditorWorkstationScript.Workspace.STRINGS)
	var ws: EditorWorkspace = shell._get_workspace(EditorWorkstationScript.Workspace.STRINGS)
	assert_eq(String(ws.get_current_resource_path()), SCRATCH_TABLE,
		"the still-open tab satisfies Back without touching disk")


func test_back_to_a_dead_entry_drops_it_and_stays_put() -> void:
	var shell := _make_shell()
	shell._nav_history.record(_entry(
		EditorWorkstationScript.Workspace.STRINGS, "user://nav_history_gone.bin"))
	shell.go_back()
	assert_eq(shell.get_active_workspace_id(), EditorWorkstationScript.Workspace.MISSION,
		"a dead destination must not move the user")
	assert_false(shell._nav_history.can_go_back(), "the dead entry should be dropped")
	assert_string_contains(shell._status._message_text, "Could not open")


func test_alt_arrows_and_mouse_thumb_buttons_navigate() -> void:
	var shell := _make_shell()
	shell._on_workspace_pressed(EditorWorkstationScript.Workspace.STRINGS)
	var key := InputEventKey.new()
	key.pressed = true
	key.alt_pressed = true
	key.keycode = KEY_LEFT
	shell._unhandled_input(key)
	assert_eq(shell.get_active_workspace_id(), EditorWorkstationScript.Workspace.MISSION,
		"Alt+Left should go back")
	var mouse := InputEventMouseButton.new()
	mouse.pressed = true
	mouse.button_index = MOUSE_BUTTON_XBUTTON2
	shell._unhandled_input(mouse)
	assert_eq(shell.get_active_workspace_id(), EditorWorkstationScript.Workspace.STRINGS,
		"the mouse forward thumb button should go forward")
	var mouse_back := InputEventMouseButton.new()
	mouse_back.pressed = true
	mouse_back.button_index = MOUSE_BUTTON_XBUTTON1
	shell._unhandled_input(mouse_back)
	assert_eq(shell.get_active_workspace_id(), EditorWorkstationScript.Workspace.MISSION,
		"the mouse back thumb button should go back")


class BusyWorkspaceStub:
	extends EditorWorkspace

	func is_busy() -> bool:
		return true


func test_busy_shell_blocks_back() -> void:
	var shell := _make_shell()
	shell._on_workspace_pressed(EditorWorkstationScript.Workspace.STRINGS)
	shell._workspaces[999] = BusyWorkspaceStub.new()
	shell.go_back()
	assert_eq(shell.get_active_workspace_id(), EditorWorkstationScript.Workspace.STRINGS,
		"a busy shell should ignore Back")
	assert_true(shell._nav_history.can_go_back(), "the entry should survive for later")
	shell._workspaces.erase(999)
