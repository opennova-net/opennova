extends GutTest

# The world's scene stack is ONE table (godot/src/world/world_scene.h): the
# packaged game_world.tscn and the rows a device builds from the table must be
# the same nodes, named, classed, pathed and layered alike, in the same order,
# and every property the packaged scene sets on a row the built row holds too.

const GAME_WORLD_SCENE := "res://game/world/game_world.tscn"
const HELD_ROWS := [
	"MissionEnvironment",
	"Water",
	"Weather",
	"EnvironmentCubeCapture",
	"Celestial",
	"FoliageDispatcher",
]


func _assert_rows(parent: Node, label: String) -> void:
	var rows: Array = WorldSceneTable.get_rows()
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


# Two values alike: a resource by every property it stores, anything else by value.
func _assert_same_value(packaged: Variant, built: Variant, what: String) -> void:
	if packaged is Resource:
		assert_true(built is Resource, "%s: a resource on both sides" % what)
		if not built is Resource:
			return
		for property in (packaged as Resource).get_property_list():
			if (int(property["usage"]) & PROPERTY_USAGE_STORAGE) == 0:
				continue
			var name := String(property["name"])
			if name in ["resource_path", "resource_name", "resource_local_to_scene", "script"]:
				continue
			assert_eq(built.get(name), packaged.get(name), "%s.%s" % [what, name])
		return
	assert_eq(built, packaged, what)


func test_the_packaged_scene_is_the_table() -> void:
	var world := (load(GAME_WORLD_SCENE) as PackedScene).instantiate()
	autofree(world)
	_assert_rows(world, "game_world.tscn")


func test_rows_built_from_the_table_match_the_packaged_scene() -> void:
	var parent := Node3D.new()
	autofree(parent)
	WorldSceneTable.build(parent, false)
	_assert_rows(parent, "built")
	for name in HELD_ROWS:
		assert_false(parent.get_node(name).is_globals_held(), "the game never holds: %s" % name)


func test_every_property_the_packaged_scene_sets_the_built_rows_hold() -> void:
	var state := (load(GAME_WORLD_SCENE) as PackedScene).get_state()
	var parent := Node3D.new()
	autofree(parent)
	WorldSceneTable.build(parent, false)
	# Node 0 is the GameWorld itself; its children follow in order.
	assert_eq(
		state.get_node_count() - 1, parent.get_child_count(), "one built row per packaged child"
	)
	for i in range(1, state.get_node_count()):
		var name := String(state.get_node_name(i))
		var built := parent.get_node_or_null(NodePath(name))
		assert_not_null(built, "the packaged %s is built" % name)
		if built == null:
			continue
		assert_eq(built.get_index(), i - 1, "%s keeps its place" % name)
		assert_eq(built.get_class(), String(state.get_node_type(i)), "%s's class" % name)
		for p in range(state.get_node_property_count(i)):
			var property := String(state.get_node_property_name(i, p))
			_assert_same_value(
				state.get_node_property_value(i, p), built.get(property), "%s.%s" % [name, property]
			)


func test_held_rows_hold_their_process_wide_globals() -> void:
	var parent := Node3D.new()
	autofree(parent)
	WorldSceneTable.build(parent, true)
	for name in HELD_ROWS:
		assert_true(parent.get_node(name).is_globals_held(), "held from the start: %s" % name)


func test_built_rows_resolve_their_siblings_in_the_tree() -> void:
	var parent := Node3D.new()
	add_child_autofree(parent)
	WorldSceneTable.build(parent, true)
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
	WorldSceneTable.build(parent, true)
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


func test_clearing_the_terrain_lets_every_row_go_of_it() -> void:
	var parent := Node3D.new()
	add_child_autofree(parent)
	WorldSceneTable.build(parent, true)
	var data := TerrainData.new()
	for name in ["Terrain", "Water", "EnvironmentCubeCapture", "Celestial", "FoliageDispatcher"]:
		parent.get_node(name).set_terrain_data(data)
	WorldSceneTable.clear_terrain(parent)
	for name in ["Terrain", "Water", "EnvironmentCubeCapture", "Celestial", "FoliageDispatcher"]:
		assert_null(parent.get_node(name).get_terrain_data(), "%s lets the terrain go" % name)
