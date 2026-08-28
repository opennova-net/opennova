extends GutTest

# DebugOptions: the declarative option registry. Rows are integrity-checked
# against the REAL target scripts (setter must exist).

const GameWorldScript := preload("res://game/world/game_world.gd")
const LocalPlayerPresenterScript := preload("res://game/world/local_player_presenter.gd")


func _script_method_names(script: Script) -> PackedStringArray:
	var names := PackedStringArray()
	var walker := script
	while walker != null:
		for entry in walker.get_script_method_list():
			names.append(String(entry["name"]))
		walker = walker.get_base_script()
	return names


func test_registry_rows_are_complete_and_unique() -> void:
	var seen: Dictionary = {}
	for option in DebugOptions.OPTIONS:
		var id := StringName(option["id"])
		assert_false(seen.has(id), "option id '%s' is unique" % id)
		seen[id] = true
		assert_false(String(option["label"]).is_empty(), "'%s' carries a label" % id)
		assert_false(String(option["tooltip"]).is_empty(), "'%s' carries a tooltip" % id)
		assert_has([DebugOptions.KIND_CHECK, DebugOptions.KIND_SLIDER,
				DebugOptions.KIND_ENUM], int(option["kind"]),
				"'%s' has a known kind" % id)
		assert_has([DebugOptions.TARGET_WORLD, DebugOptions.TARGET_PLAYER],
				StringName(option["target"]), "'%s' has a known target" % id)
		if int(option["kind"]) == DebugOptions.KIND_CHECK:
			assert_eq(typeof(option["default"]), TYPE_BOOL,
					"checkbox '%s' defaults to a bool" % id)
	assert_true(DebugOptions.find(&"nonexistent_option").is_empty(),
			"unknown ids resolve to an empty row")


func test_every_setter_exists_on_its_target_script() -> void:
	var world_methods := _script_method_names(GameWorldScript)
	var player_methods := _script_method_names(LocalPlayerPresenterScript)
	for option in DebugOptions.OPTIONS:
		var setter := String(option["setter"])
		if option["target"] == DebugOptions.TARGET_WORLD:
			assert_has(world_methods, setter,
					"GameWorld implements '%s' for '%s'" % [setter, option["id"]])
		else:
			assert_has(player_methods, setter,
					"LocalPlayerPresenter implements '%s' for '%s'" % [setter, option["id"]])


