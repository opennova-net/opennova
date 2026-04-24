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
