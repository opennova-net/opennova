extends GutTest


func test_runtime_ticks_in_memory_mission_and_emits_commands() -> void:
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_event(0, 0, 0).is_empty())
	var chain := mission.add_event_action(0, {
		"action_type": 6,
		"param1": 77,
	})
	assert_false(chain.is_empty())

	var runtime := NovaMissionRuntime.new()
	assert_true(runtime.load_from_mission(mission))

	var commands := runtime.tick()
	assert_eq(commands.size(), 1)
	assert_eq(int((commands[0] as Dictionary)["kind"]), NovaMissionRuntime.COMMAND_OUTPUT_TEXT)
	assert_eq(int((commands[0] as Dictionary)["param1"]), 77)
	assert_true(runtime.has_event_fired(0))
	assert_true(runtime.tick().is_empty(), "fire-once events do not repeat without ResetEvent.")


func test_mission_data_exposes_core_param_schema() -> void:
	var mission := NovaMissionData.new()
	var reset := mission.get_action_param_schema(34)
	assert_eq(int((reset["params"][0] as Dictionary)["kind"]), MissionParamSchema.Kind.EVENT)
	var event_trigger := mission.get_trigger_param_schema(3, 5)
	assert_eq(int((event_trigger["params"][0] as Dictionary)["kind"]), MissionParamSchema.Kind.EVENT)
