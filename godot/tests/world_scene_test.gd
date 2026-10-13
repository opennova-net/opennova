extends GutTest

# The world's scene stack is ONE table (godot/src/world/world_scene.h): the
# packaged game_world.tscn and the rows a device builds from the table must be
# the same nodes, named, classed, pathed and layered alike, in the same order.

const GAME_WORLD_SCENE := "res://game/world/game_world.tscn"


func _assert_rows(parent: Node, label: String) -> void:
	var rows: Array = GameWorld.get_scene_rows()
	assert_eq(parent.get_child_count(), rows.size(), "%s: one child per row" % label)
	for i in range(mini(parent.get_child_count(), rows.size())):
		var row: Dictionary = rows[i]
		var node := parent.get_child(i)
		assert_eq(String(node.name), String(row["name"]), "%s: row %d's name" % [label, i])
		assert_eq(node.get_class(), String(row["class"]), "%s: %s's class" % [label, row["name"]])
		var paths: Dictionary = row["paths"]
		for property in paths:
			assert_eq(
				node.get(property), paths[property], "%s: %s.%s" % [label, row["name"], property]
			)
		var layer := int(row["capture_layer_mask"])
		if layer != 0:
			assert_eq(
				int(node.get("environment_capture_layer_mask")),
				layer,
				"%s: %s's capture layer" % [label, row["name"]]
			)


func test_the_packaged_scene_is_the_table() -> void:
	var world := (load(GAME_WORLD_SCENE) as PackedScene).instantiate()
	autofree(world)
	_assert_rows(world, "game_world.tscn")


func test_rows_built_from_the_table_match_the_packaged_scene() -> void:
	var parent := Node3D.new()
	autofree(parent)
	GameWorld.build_scene_rows(parent, false)
	_assert_rows(parent, "built")
	var clear := parent.get_node("ClearColor") as WorldEnvironment
	assert_not_null(clear.environment, "the clear colour's environment")
	assert_eq(
		clear.environment.background_mode,
		Environment.BG_COLOR,
		"a colour clear: a sky background with no sky renders black"
	)
	assert_eq(
		clear.environment.ambient_light_source,
		Environment.AMBIENT_SOURCE_DISABLED,
		"no ambient: the fixed-function shaders own every colour"
	)
	for name in ["MissionEnvironment", "Water", "Weather", "EnvironmentCubeCapture"]:
		assert_false(parent.get_node(name).is_globals_held(), "the game never holds: %s" % name)


func test_held_rows_hold_their_process_wide_globals() -> void:
	var parent := Node3D.new()
	autofree(parent)
	GameWorld.build_scene_rows(parent, true)
	for name in ["MissionEnvironment", "Water", "Weather", "EnvironmentCubeCapture"]:
		assert_true(parent.get_node(name).is_globals_held(), "held from the start: %s" % name)


func test_built_rows_resolve_their_siblings_in_the_tree() -> void:
	var parent := Node3D.new()
	add_child_autofree(parent)
	GameWorld.build_scene_rows(parent, true)
	await get_tree().process_frame
	var terrain := parent.get_node("Terrain") as Terrain
	assert_eq(
		terrain.get_node(terrain.weather_path),
		parent.get_node("Weather"),
		"the terrain's light rows read the weather's flicker ring"
	)
	var sky := parent.get_node("SkyDome") as SkyDome
	assert_eq(
		sky.get_node(sky.weather_path),
		parent.get_node("Weather"),
		"the sky's cloud scroll is the weather's"
	)


func test_a_held_weather_writes_no_process_wide_state_until_it_republishes() -> void:
	var parent := Node3D.new()
	add_child_autofree(parent)
	GameWorld.build_scene_rows(parent, true)
	var environment := parent.get_node("MissionEnvironment") as MissionEnvironment
	var data := EnvFile.new()
	data.reset_to_default()
	environment.environment_data = data
	var weather := parent.get_node("Weather") as Weather
	await get_tree().process_frame
	var writes := Weather.get_global_writes()
	weather.advance_frame(1.0 / 62.5)
	assert_eq(Weather.get_global_writes(), writes, "a held weather writes none of its globals")
	weather.republish_globals()
	assert_eq(Weather.get_global_writes(), writes + 1, "republishing writes them whatever the hold")
