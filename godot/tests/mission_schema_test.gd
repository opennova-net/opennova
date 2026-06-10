extends GutTest

# NovaMissionData exposes the trigger/action parameter schema (mission_schema.h) to the ONED
# inspector via get_*_param_schema -> MissionParamSchema. Pins that binding path (moved here when
# the duplicate NovaMissionRuntime evaluator was retired).

func test_mission_data_exposes_core_param_schema() -> void:
	var mission := NovaMissionData.new()
	var reset := mission.get_action_param_schema(34)  # ResetEvent -> Event picker
	assert_eq(int((reset["params"][0] as Dictionary)["kind"]), MissionParamSchema.Kind.EVENT)
	var event_trigger := mission.get_trigger_param_schema(3, 5)
	assert_eq(int((event_trigger["params"][0] as Dictionary)["kind"]), MissionParamSchema.Kind.EVENT)
