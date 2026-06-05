extends GutTest


func test_nova_fnt_fixtures_are_not_project_imports() -> void:
	assert_false(
		DirAccess.dir_exists_absolute(ProjectSettings.globalize_path("res://assets/fonts")),
		"Nova .fnt data should live in the external resource root/fixtures, not in godot/assets."
	)
	for filename in DirAccess.get_files_at(ProjectSettings.globalize_path("res://../fixtures/fnt")):
		assert_ne(filename.get_extension().to_lower(), "import",
			"Top-level FNT fixtures should not be Godot import sidecars.")
