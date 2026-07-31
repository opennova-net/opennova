extends GutTest

## Capability-hook coverage for the Environment workspace adapter: identity,
## open filters, the save-needs-a-path gate, and undo/redo delegation. Mirrors
## sound_workspace_test / credits_workspace_test.

const EnvironmentWorkspaceAdapter = preload("res://modtools/environment/environment_workspace.gd")
const EnvironmentEditorScript = preload("res://modtools/environment/environment_editor.gd")


func _make_workspace() -> Array:
	var editor = add_child_autofree(EnvironmentEditorScript.new())
	editor.create_default_environment(false)
	var ws = EnvironmentWorkspaceAdapter.new(editor)
	return [ws, editor]


func test_identity_and_open_hooks() -> void:
	var pair := _make_workspace()
	var ws = pair[0]
	assert_eq(ws.get_workspace_id(), "environment")
	assert_eq(ws.get_open_resource_kind(), "environment")
	assert_eq(ws.get_new_action_label(), "New Environment")
	var joined := ",".join(ws.get_open_dialog_filters())
	assert_true(joined.to_lower().contains("env"), "open filters include .env")


func test_save_requires_a_path() -> void:
	var pair := _make_workspace()
	var ws = pair[0]
	var editor = pair[1]

	assert_true(ws.can_save_as(), "Save As is available once a document exists.")
	assert_false(ws.can_save(), "Save is unavailable with no path, even when dirty.")

	editor.env_file.set_env_name("Dirty")
	assert_true(editor.is_dirty, "Editing should dirty the document.")
	assert_false(ws.can_save(), "Save stays unavailable while there is still no path.")

	editor.set_current_path("user://env_ws_test.env")
	assert_true(ws.can_save(), "Save becomes available once a path is set and dirty.")


func test_undo_redo_delegates_to_editor() -> void:
	var pair := _make_workspace()
	var ws = pair[0]
	var editor = pair[1]

	assert_false(ws.can_undo(), "No history initially.")
	editor.begin_edit()
	editor.env_file.set_fog_level(222.0)
	editor.commit_edit()
	assert_true(ws.can_undo(), "Adapter should report undo available after an edit.")

	ws.undo()
	assert_almost_eq(editor.env_file.get_fog_level(), 1000.0, 0.5, "Adapter undo should revert via the editor.")
	assert_true(ws.can_redo(), "Adapter should report redo available after undo.")
	ws.redo()
	assert_almost_eq(editor.env_file.get_fog_level(), 222.0, 0.5, "Adapter redo should reapply via the editor.")


func test_inspector_builds_and_shows_model_fields() -> void:
	# Exercises build_inspector + _build_ui end-to-end (the new Sky Models row and
	# commit-trigger wiring) and confirms the inspector reflects the document.
	var pair := _make_workspace()
	var ws = pair[0]
	var editor = pair[1]
	editor.env_file.set_sun_3di("msun.3di")

	var mount = add_child_autofree(Control.new())
	ws.build_inspector(mount)
	# Inspector is a child of mount; sync runs on _ready.
	assert_eq(mount.get_child_count(), 1, "build_inspector should mount one inspector.")
	var inspector = mount.get_child(0)
	assert_true(inspector.has_method("sync_from_editor"), "Mounted node should be the EnvironmentInspector.")
	inspector.sync_from_editor()
	assert_eq(inspector._sun_model.get_value(), "msun.3di", "Inspector should show the env's sun model name.")
