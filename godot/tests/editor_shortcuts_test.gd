extends GutTest

# B6: the shell's global document shortcuts (_shortcut_input) and the ONE
# focus guard — undo/redo only; Ctrl+S is deliberately unguarded. Behavior
# delta pinned: every workspace gets keyboard undo through this one path
# (the per-workspace viewport handlers are gone).

const EditorWorkstationScene = preload("res://modtools/editor/editor_workstation.tscn")
const EditorWorkstationScript = preload("res://modtools/editor/editor_workstation.gd")


func _shell() -> EditorWorkstation:
	return add_child_autofree(EditorWorkstationScene.instantiate())


func _ctrl(keycode: int, shift := false) -> InputEventKey:
	var key := InputEventKey.new()
	key.keycode = keycode
	key.ctrl_pressed = true
	key.shift_pressed = shift
	key.pressed = true
	return key


func _credits_doc_with_one_step(shell: EditorWorkstation) -> Object:
	shell.set_active_workspace(EditorWorkstationScript.Workspace.CREDITS)
	var ws := shell.get_workspace_adapter(EditorWorkstationScript.Workspace.CREDITS)
	var doc: Object = ws.get_editor_document()
	doc.push_undo_step(func() -> void:
		var entry := CbinTextEntry.new()
		entry.set_text("step")
		doc.resource.add_entry(entry))
	assert_eq(doc.resource.get_entry_count(), 1)
	return doc


func test_ctrl_z_y_route_to_the_active_workspace() -> void:
	var shell := _shell()
	var doc := _credits_doc_with_one_step(shell)

	shell._shortcut_input(_ctrl(KEY_Z))
	assert_eq(doc.resource.get_entry_count(), 0, "Ctrl+Z undoes via the workspace hook.")
	shell._shortcut_input(_ctrl(KEY_Y))
	assert_eq(doc.resource.get_entry_count(), 1, "Ctrl+Y redoes.")
	shell._shortcut_input(_ctrl(KEY_Z))
	assert_eq(doc.resource.get_entry_count(), 0, "Ctrl+Z again.")
	shell._shortcut_input(_ctrl(KEY_Z, true))
	assert_eq(doc.resource.get_entry_count(), 1, "Ctrl+Shift+Z is the other redo binding.")


func test_focus_guard_blocks_undo_redo_only() -> void:
	var shell := _shell()
	var doc := _credits_doc_with_one_step(shell)
	var field: LineEdit = add_child_autofree(LineEdit.new())
	field.grab_focus()

	shell._shortcut_input(_ctrl(KEY_Z))
	assert_eq(doc.resource.get_entry_count(), 1,
		"A focused text field keeps its native Ctrl+Z (guard blocks the global one).")

	field.release_focus()
	shell._shortcut_input(_ctrl(KEY_Z))
	assert_eq(doc.resource.get_entry_count(), 0,
		"With no text focus the global undo runs.")


func test_ctrl_s_is_unguarded_and_consumed() -> void:
	var shell := _shell()
	shell.set_active_workspace(EditorWorkstationScript.Workspace.CREDITS)
	var field: LineEdit = add_child_autofree(LineEdit.new())
	field.grab_focus()

	# Credits with no path: save_current -> ERR_UNAVAILABLE -> a status toast,
	# never a native dialog, so this is headless-safe. The point pinned here:
	# the focus guard does NOT apply to Ctrl+S (the save flow flushes buffers).
	shell._shortcut_input(_ctrl(KEY_S))
	assert_true(get_viewport().is_input_handled(),
		"Ctrl+S is claimed by the shell even while a text field has focus.")
