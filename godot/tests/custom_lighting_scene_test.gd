extends GutTest


func _source(path: String) -> String:
	var file := FileAccess.open(path, FileAccess.READ)
	assert_not_null(file, "%s should be readable." % path)
	return file.get_as_text() if file != null else ""


func test_runtime_scene_owns_the_native_world_lighting_stack() -> void:
	var scene := _source("res://game/world/game_world.tscn")
	var world := _source("res://game/world/game_world.gd")
	var sun_header := _source("res://src/env/nova_sun_shadow.h")

	assert_true(scene.contains('type="WorldEnvironment"'),
			"The hard-cut renderer must carry one native WorldEnvironment.")
	assert_true(scene.contains('type="ProceduralSkyMaterial"'),
			"The native environment must provide procedural radiance.")
	assert_true(world.contains("_sun_shadow = SunShadow.new()"),
			"GameWorld must construct the ENV/TOD-driven sun device.")
	assert_true(sun_header.contains("class SunShadow : public DirectionalLight3D"),
			"The ENV/TOD sun must be a native DirectionalLight3D.")


func test_world_surfaces_use_native_lighting_without_a_runtime_profile_switch() -> void:
	var terrain := _source("res://shaders/terrain.gdshader")
	var foliage_high := _source("res://shaders/foliage_detail_high.gdshader")
	var foliage_low := _source("res://shaders/foliage_detail_low.gdshader")
	var project := _source("res://project.godot")

	assert_false(terrain.contains("unshaded"))
	assert_false(foliage_high.contains("unshaded"))
	assert_false(foliage_low.contains("unshaded"))
	assert_true(bool(ProjectSettings.get_setting(
			"rendering/anti_aliasing/quality/use_taa", false)))
	assert_true(bool(ProjectSettings.get_setting(
			"rendering/anti_aliasing/quality/use_debanding", false)))
	assert_false(project.contains("facelift_profile"),
			"The renderer transition is a hard cut, not a runtime profile matrix.")
