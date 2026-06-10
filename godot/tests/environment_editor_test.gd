extends GutTest

const EnvironmentEditorScript = preload("res://modtools/environment/environment_editor.gd")


func test_environment_editor_tracks_dirty_state_from_env_resource() -> void:
	var editor = add_child_autofree(EnvironmentEditorScript.new())
	editor.create_default_environment(false)
	assert_false(editor.is_dirty, "Default startup environment should be clean.")
	assert_eq(editor.get_project_title(), "untitled", "Clean default environment should use the untitled title.")

	editor.env_file.set_env_name("Storm Test")

	assert_true(editor.is_dirty, "EnvFile edits should dirty the environment document.")
	assert_eq(editor.get_project_title(), "Storm Test*", "Dirty environment title should include the dirty marker.")


func test_set_time_of_day_emits_changes_once_per_step() -> void:
	# Each TOD step must fan out exactly one environment_changed / state_changed.
	# The inner EnvFile.set_curtime echo previously doubled the fan-out, which is
	# what made dragging time-of-day rebuild the shell twice per pixel.
	var editor = add_child_autofree(EnvironmentEditorScript.new())
	editor.create_default_environment(false)
	watch_signals(editor)

	editor.set_time_of_day(1300.0)

	assert_signal_emit_count(editor, "environment_changed", 1, "Time-of-day changes should fan out a single environment_changed per step.")
	assert_signal_emit_count(editor, "state_changed", 1, "Time-of-day changes should fan out a single state_changed per step.")
	assert_true(editor.is_dirty, "A time-of-day edit should still mark the environment dirty.")


func test_environment_editor_exports_current_env_file() -> void:
	var editor = add_child_autofree(EnvironmentEditorScript.new())
	editor.create_default_environment(false)
	editor.env_file.set_env_name("Storm Test")
	var dir_path := "user://environment_editor_test"
	var user_dir := DirAccess.open("user://")
	assert_not_null(user_dir, "Godot user directory should be available for export tests.")
	if user_dir == null:
		return
	if not user_dir.dir_exists("environment_editor_test"):
		assert_eq(user_dir.make_dir("environment_editor_test"), OK, "Test export directory should be creatable.")

	var err: Error = editor.export_to_dir(dir_path)

	assert_eq(err, OK, "Environment editor should export the active EnvFile.")
	assert_true(FileAccess.file_exists(dir_path.path_join("storm_test.env")), "Export should use a sanitized .env filename.")


func test_undo_redo_reverts_scalar_edit() -> void:
	var editor = add_child_autofree(EnvironmentEditorScript.new())
	editor.create_default_environment(false)
	assert_false(editor.can_undo(), "A fresh document should have no undo history.")

	editor.begin_edit()
	editor.env_file.set_fog_level(321.0)
	editor.commit_edit()

	assert_true(editor.can_undo(), "Committing an edit should push an undo step.")
	assert_almost_eq(editor.env_file.get_fog_level(), 321.0, 0.5, "Edit should apply before undo.")

	editor.undo()
	assert_almost_eq(editor.env_file.get_fog_level(), 1000.0, 0.5, "Undo should restore the prior fog level.")
	assert_true(editor.can_redo(), "Undo should make a redo step available.")

	editor.redo()
	assert_almost_eq(editor.env_file.get_fog_level(), 321.0, 0.5, "Redo should reapply the edit.")


func test_unchanged_edit_burst_pushes_no_step() -> void:
	var editor = add_child_autofree(EnvironmentEditorScript.new())
	editor.create_default_environment(false)
	editor.begin_edit()
	# No mutation between begin and commit.
	editor.commit_edit()
	assert_false(editor.can_undo(), "A no-op editing burst should not push an undo step.")


func test_structural_keyframe_change_is_one_undo_step() -> void:
	var editor = add_child_autofree(EnvironmentEditorScript.new())
	editor.create_default_environment(false)
	var before: int = editor.env_file.get_tod_keyframes().size()

	editor.push_undo_step(func():
		var frames: Array = editor.env_file.get_tod_keyframes()
		var kf := NovaEnvKeyframe.new()
		kf.set_time(900)
		frames.append(kf)
		editor.env_file.set_tod_keyframes(frames))

	assert_eq(editor.env_file.get_tod_keyframes().size(), before + 1, "Structural mutation should apply.")
	editor.undo()
	assert_eq(editor.env_file.get_tod_keyframes().size(), before, "Undo should remove the added keyframe.")


func test_history_clears_on_open_and_new() -> void:
	var editor = add_child_autofree(EnvironmentEditorScript.new())
	editor.create_default_environment(false)
	editor.begin_edit()
	editor.env_file.set_fog_level(123.0)
	editor.commit_edit()
	assert_true(editor.can_undo(), "Edit should be undoable before reset.")

	editor.create_default_environment(false)
	assert_false(editor.can_undo(), "Creating a new document should clear undo history.")
