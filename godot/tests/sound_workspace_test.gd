extends GutTest

## Capability-hook coverage for the Sound workspace adapter: it advertises the
## "sound" resource kind + *.lwf filters, exposes save/save-as, and mounts an
## inspector. Mirrors strings_workspace_test / mission_workspace_test.

const SoundWorkspaceAdapter = preload("res://modtools/sound/sound_workspace.gd")


func test_identity_and_open_hooks() -> void:
	var ws = SoundWorkspaceAdapter.new()
	assert_eq(ws.get_workspace_id(), "sound")
	assert_eq(ws.get_open_resource_kind(), "sound")
	assert_eq(ws.get_new_action_label(), "New Sound Profile")
	var joined := ",".join(ws.get_open_dialog_filters())
	assert_true(joined.to_lower().contains("lwf"), "open filters include .lwf")


func test_document_save_and_inspector() -> void:
	var shell := Node.new()
	add_child_autofree(shell)
	var ws = SoundWorkspaceAdapter.new()
	ws.set_editor_shell(shell)

	var doc = ws.get_document()
	assert_not_null(doc, "workspace creates a SoundController")
	assert_true(ws.can_save_as(), "save-as available once a document exists")
	assert_false(ws.can_save(), "save unavailable with no path")
	assert_eq(ws.get_project_title(), "untitled.lwf", "plain title before any load")

	var mount := Control.new()
	add_child_autofree(mount)
	ws.build_inspector(mount)
	assert_gt(mount.get_child_count(), 0, "build_inspector mounts a child")
