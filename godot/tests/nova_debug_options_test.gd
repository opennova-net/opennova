extends GutTest

# NovaDebugOptions + NovaDebugOptionState: the declarative option registry
# that keeps the game shell and the editor wired identically. Rows are
# integrity-checked against the REAL target scripts (setter must exist), and
# the state object's single-write-path contract is pinned.

const GameWorldScript := preload("res://engine/world/game_world.gd")
const LocalPlayerPresenterScript := preload("res://engine/world/local_player_presenter.gd")


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
	for option in NovaDebugOptions.OPTIONS:
		var id := StringName(option["id"])
		assert_false(seen.has(id), "option id '%s' is unique" % id)
		seen[id] = true
		assert_false(String(option["label"]).is_empty(), "'%s' carries a label" % id)
		assert_false(String(option["tooltip"]).is_empty(), "'%s' carries a tooltip" % id)
		assert_has([NovaDebugOptions.KIND_CHECK, NovaDebugOptions.KIND_SLIDER,
				NovaDebugOptions.KIND_ENUM], int(option["kind"]),
				"'%s' has a known kind" % id)
		assert_has([NovaDebugOptions.TARGET_WORLD, NovaDebugOptions.TARGET_PLAYER],
				StringName(option["target"]), "'%s' has a known target" % id)
		if int(option["kind"]) == NovaDebugOptions.KIND_CHECK:
			assert_eq(typeof(option["default"]), TYPE_BOOL,
					"checkbox '%s' defaults to a bool" % id)
	assert_true(NovaDebugOptions.find(&"nonexistent_option").is_empty(),
			"unknown ids resolve to an empty row")


func test_every_setter_exists_on_its_target_script() -> void:
	var world_methods := _script_method_names(GameWorldScript)
	var player_methods := _script_method_names(LocalPlayerPresenterScript)
	for option in NovaDebugOptions.OPTIONS:
		var setter := String(option["setter"])
		if option["target"] == NovaDebugOptions.TARGET_WORLD:
			assert_has(world_methods, setter,
					"GameWorld implements '%s' for '%s'" % [setter, option["id"]])
		else:
			assert_has(player_methods, setter,
					"LocalPlayerPresenter implements '%s' for '%s'" % [setter, option["id"]])


func test_state_defaults_and_single_write_path() -> void:
	var state := NovaDebugOptionState.new()
	assert_eq(state.value(&"show_skeletons"), false, "unset reads the registry default")
	assert_null(state.value(&"nonexistent_option"), "unknown ids read null")

	var emissions: Array = []
	state.changed.connect(func(id: StringName, v: Variant): emissions.append([id, v]))
	state.set_value(&"show_skeletons", true)
	state.set_value(&"show_skeletons", true)
	assert_eq(emissions.size(), 1, "a repeated value never re-fires")
	assert_eq(emissions[0], [&"show_skeletons", true])
	assert_eq(state.value(&"show_skeletons"), true)

	state.set_value(&"nonexistent_option", true)
	assert_eq(emissions.size(), 1, "unknown ids are ignored")


func test_registered_control_resyncs_without_refiring() -> void:
	var state := NovaDebugOptionState.new()
	var check := CheckBox.new()
	autofree(check)
	var toggles := [0]
	check.toggled.connect(func(_v: bool): toggles[0] += 1)
	state.register_control(&"hide_foliage", check)
	state.set_value(&"hide_foliage", true)
	assert_true(check.button_pressed, "a programmatic write lands on the control")
	assert_eq(toggles[0], 0, "...without re-firing its toggled signal")
