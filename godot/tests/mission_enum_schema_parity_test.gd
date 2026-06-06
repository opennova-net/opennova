extends GutTest

# Drift guard between the engine's trigger/action enum reflectors (NovaMissionData.get_trigger_*_types
# / get_action_types, whose names come from libs/mission) and the human-authored param schema
# (MissionParamSchema.TRIGGERS / ACTIONS). A new named enum value added in the C++ format must either
# gain a schema row OR be listed on an explicit raw-only allowlist below -- otherwise it silently
# degrades to a raw spinbox in the inspector with no param labels. Adding it here forces the choice to
# be deliberate (and reviewed) instead of accidental.
#
# NOTE: ACTIONS is keyed by action TYPE, so it is checked against get_action_types() -- NOT
# get_action_sub_types (an orthogonal axis the inspector intentionally renders as a raw "Value" slot).

const Schema := preload("res://modtools/mission/mission_param_schema.gd")

# Trigger (main_type -> [sub_type, ...]) deliberately left raw (no schema row). Every entry is a
# named engine enum the inspector currently renders as a plain labelled int; a NEW enum value lands
# outside this list and fails the test until it gets a schema row or is added here on purpose.
const TRIGGER_RAW_ALLOWLIST := {
	1: [0],              # Group: Null (no condition).
	2: [0],              # Single: Null (no condition).
	5: [0],              # SecondTimeThrough: single "Null" sub-type, no params.
	6: [1, 2, 3],        # Teammate: enabled / medic-assisting / evacuating -- no param slots (dfx2med).
	7: [18, 19, 20, 21], # Player: berserk / first / third / cockpit view -- no param slots (dfx2med).
}
# Action types deliberately left raw (no ACTIONS row), confirmed against dfx2med:
#   0  Null (no-op)
#   12/13 AreaAiRed/Blue -- AI-bit driven (slot1+ depend on the AI sub-type; rendered as raw Value)
#   39 Teammates -- param shape depends on the teammate sub-type (medic/evac targets)
#   41 ExecuteWac -- editor exposes no param slot
const ACTION_RAW_ALLOWLIST := [
	0, 12, 13, 39, 41,
]


func _mission() -> NovaMissionData:
	# The enum reflectors are pure name tables (no loaded document needed).
	return NovaMissionData.new()


func test_every_named_trigger_sub_type_has_schema_or_allowlist() -> void:
	var m := _mission()
	var missing: Array = []
	for main in m.get_trigger_main_types():
		var main_type := int((main as Dictionary)["value"])
		var by_main: Dictionary = Schema.TRIGGERS.get(main_type, {})
		var has_default: bool = by_main.has("_default")
		var allow: Array = TRIGGER_RAW_ALLOWLIST.get(main_type, [])
		for sub in m.get_trigger_sub_types(main_type):
			var sub_dict := sub as Dictionary
			var sub_type := int(sub_dict["value"])
			if has_default or by_main.has(sub_type) or allow.has(sub_type):
				continue
			missing.append("trigger main=%d sub=%d (%s)" % [main_type, sub_type, String(sub_dict["name"])])
	assert_eq(missing, [], "named trigger sub-types missing a schema row or allowlist entry")


func test_every_named_action_type_has_schema_or_allowlist() -> void:
	var m := _mission()
	var missing: Array = []
	for a in m.get_action_types():
		var a_dict := a as Dictionary
		var action_type := int(a_dict["value"])
		if Schema.ACTIONS.has(action_type) or ACTION_RAW_ALLOWLIST.has(action_type):
			continue
		missing.append("action=%d (%s)" % [action_type, String(a_dict["name"])])
	assert_eq(missing, [], "named action types missing a schema row or allowlist entry")


func test_no_schema_row_references_an_unnamed_enum() -> void:
	# Reverse guard: a schema row pointing at an enum value the engine no longer names (a rename /
	# removal on the C++ side) is dead and should be cleaned up. Skips "_default" keys.
	var m := _mission()
	var stale: Array = []
	for main_type in Schema.TRIGGERS:
		var named := {}
		for sub in m.get_trigger_sub_types(int(main_type)):
			named[int((sub as Dictionary)["value"])] = true
		for sub_type in Schema.TRIGGERS[main_type]:
			if sub_type is String:
				continue  # "_default"
			if not named.has(int(sub_type)):
				stale.append("TRIGGERS[%d][%d]" % [int(main_type), int(sub_type)])
	var named_actions := {}
	for a in m.get_action_types():
		named_actions[int((a as Dictionary)["value"])] = true
	for action_type in Schema.ACTIONS:
		if not named_actions.has(int(action_type)):
			stale.append("ACTIONS[%d]" % int(action_type))
	assert_eq(stale, [], "schema rows referencing unnamed / removed enum values")
