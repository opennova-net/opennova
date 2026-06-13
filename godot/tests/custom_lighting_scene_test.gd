extends GutTest

const SCENE_PATHS := [
	"res://game/main_game.tscn",
	"res://modtools/editor/editor_main.tscn",
]


func test_runtime_and_editor_scenes_use_custom_lighting_only() -> void:
	for path in SCENE_PATHS:
		var file := FileAccess.open(path, FileAccess.READ)
		assert_not_null(file, "%s should be readable." % path)
		if file == null:
			continue
		var scene_text := file.get_as_text()
		assert_false(scene_text.contains("type=\"WorldEnvironment\""), "%s should not use Godot WorldEnvironment." % path)
		assert_false(scene_text.contains("type=\"DirectionalLight3D\""), "%s should not use Godot DirectionalLight3D." % path)
