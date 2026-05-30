extends GutTest


func test_runtime_and_modtools_do_not_ship_resource_roots() -> void:
	var forbidden_dirs := [
		"res://" + "assets",
		"res://modtools/" + "assets",
	]
	for dir_path in forbidden_dirs:
		assert_false(
			DirAccess.dir_exists_absolute(ProjectSettings.globalize_path(dir_path)),
			"%s must not exist; game data belongs in the configured resource root." % dir_path
		)
