extends GutTest

# MissionParamSchema: the human-authored (RE-derived) map of trigger/action param meanings. These tests
# pin the contract the inspector relies on: always 4 fully-defaulted slots, graceful degradation to RAW for
# unmapped types, and the specific picker kinds the editor renders dropdowns for.

const Schema := preload("res://modtools/mission/mission_param_schema.gd")


func test_trigger_slots_always_returns_four() -> void:
	var s := Schema.trigger_slots(1, 1)
	assert_eq(s.params.size(), 4, "always four param slots")
	for slot in s.params:
		assert_true(slot.kind >= 0, "each slot has a kind")
		assert_true(slot is MissionParamSlotSpec, "each slot is a typed spec")


func test_unmapped_trigger_is_all_raw() -> void:
	var s := Schema.trigger_slots(1, 999)  # Group main type, no such sub-type
	for slot in s.params:
		assert_eq(slot.kind, Schema.Kind.RAW, "unmapped sub-type degrades to raw")
	assert_eq(s.desc, "", "unmapped sub-type has no description")


func test_unknown_main_type_is_all_raw() -> void:
	var s := Schema.trigger_slots(42, 0)
	for slot in s.params:
		assert_eq(slot.kind, Schema.Kind.RAW, "unknown main type degrades to raw")


func test_group_within_area_param2_is_zone() -> void:
	var s := Schema.trigger_slots(1, 10)  # GroupIsWithinArea
	assert_eq(s.params[0].kind, Schema.Kind.GROUP, "param1 is a group ref")
	assert_eq(s.params[1].kind, Schema.Kind.ZONE, "param2 is a zone ref")


func test_single_distance_param3_is_raw_with_label() -> void:
	var s := Schema.trigger_slots(2, 43)  # SingleFartherThan
	assert_eq(s.params[0].kind, Schema.Kind.ENTITY, "param1 is an entity ref")
	assert_eq(s.params[2].kind, Schema.Kind.RAW, "the distance is a raw value")
	assert_true(s.params[2].label.to_lower().find("m") != -1, "the distance label names metres")


func test_event_trigger_uses_default_fallback() -> void:
	# Main type 3 (Event) has no per-sub rows, only a _default; any sub-type should resolve param1 = event.
	var s := Schema.trigger_slots(3, 0)
	assert_eq(s.params[0].kind, Schema.Kind.EVENT, "event-trigger param1 is an event ref")
	var s2 := Schema.trigger_slots(3, 5)
	assert_eq(s2.params[0].kind, Schema.Kind.EVENT, "the _default applies to any sub-type")


func test_mission_variable_uses_default_fallback() -> void:
	for sub in [1, 2, 3, 4, 5]:
		var s := Schema.trigger_slots(4, sub)
		assert_eq(s.params[1].kind, Schema.Kind.RAW, "compare value is raw")
		assert_true(s.desc.find("{p1}") != -1 or s.desc.find("variable") != -1, "has a description for sub %d" % sub)


func test_reset_event_action_param1_is_event() -> void:
	var s := Schema.action_slots(34)  # ResetEvent
	assert_eq(s.params[0].kind, Schema.Kind.EVENT, "ResetEvent param1 is an event ref")


func test_redirect_group_param3_is_waypoint() -> void:
	var s := Schema.action_slots(1)  # RedirectGroupTo
	assert_eq(s.params[0].kind, Schema.Kind.GROUP, "param1 is a group ref")
	assert_eq(s.params[2].kind, Schema.Kind.WAYPOINT, "param3 is a waypoint ref")


func test_unmapped_action_is_all_raw() -> void:
	var s := Schema.action_slots(999)
	assert_eq(s.params.size(), 4, "always four slots")
	for slot in s.params:
		assert_eq(slot.kind, Schema.Kind.RAW, "unmapped action degrades to raw")


func test_change_team_action_param2_is_team_enum() -> void:
	# dfx2med Med_ParamTeam @0x449A90: a fixed 3-team table {0 Neutral, 1 Good, 2 Evil}. Actions 16/24 param2.
	for action in [16, 24]:
		var s := Schema.action_slots(action)
		var team := s.params[1]
		assert_eq(team.kind, Schema.Kind.ENUM, "team is an enum picker (action %d)" % action)
		var values: Array = []
		for it in team.enum_items:
			values.append(int((it as Dictionary)["value"]))
		assert_eq(values, [0, 1, 2], "team enum is exactly {0,1,2} (action %d)" % action)


func test_subgoal_actions_param1_is_bounded_1_to_8() -> void:
	# dfx2med Med_ParamSubGoalWon/Lost @0x449710/@0x449830: 8 fixed slots, i=1..8. Actions 14/15/35/36 param1.
	for action in [14, 15, 35, 36]:
		var s := Schema.action_slots(action)
		var sub := s.params[0]
		assert_eq(sub.kind, Schema.Kind.ENUM, "subgoal is an enum picker (action %d)" % action)
		var values: Array = []
		for it in sub.enum_items:
			values.append(int((it as Dictionary)["value"]))
		assert_eq(values, [1, 2, 3, 4, 5, 6, 7, 8], "subgoal enum is 1..8 (action %d)" % action)


func test_is_picker_classifies_kinds() -> void:
	assert_true(Schema.is_picker(Schema.Kind.ENUM), "enum is a picker")
	assert_true(Schema.is_picker(Schema.Kind.GROUP), "group is a picker")
	assert_true(Schema.is_picker(Schema.Kind.ZONE), "zone is a picker")
	assert_false(Schema.is_picker(Schema.Kind.RAW), "raw is not a picker")
