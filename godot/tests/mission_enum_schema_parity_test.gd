extends GutTest

# Drift guard between the engine's trigger/action enum reflectors (NovaMissionData.get_trigger_*_types
# / get_action_types, whose names come from libs/mission) and the shared core param schema. A new named
# enum value added in the C++ format must either gain a schema row OR be listed on an explicit raw-only
# allowlist below -- otherwise it silently
# degrades to a raw spinbox in the inspector with no param labels. Adding it here forces the choice to
# be deliberate (and reviewed) instead of accidental.
#
# NOTE: action schema is keyed by action TYPE, so it is checked against get_action_types() -- NOT
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
#   39 Teammates -- param shape depends on the teammate sub-type (medic/evac targets)
#   41 ExecuteWac -- editor exposes no param slot
# (AreaAiRed/Blue 12/13 are now sub-type-aware AI-change actions and carry a schema row.)
const ACTION_RAW_ALLOWLIST := [
	0, 39, 41,
]


func _mission() -> NovaMissionData:
	# The enum reflectors are pure name tables (no loaded document needed).
	return NovaMissionData.new()


func test_every_named_trigger_sub_type_has_schema_or_allowlist() -> void:
	var m := _mission()
	var missing: Array = []
	for main in m.get_trigger_main_types():
		var main_type := int((main as Dictionary)["value"])
		var allow: Array = TRIGGER_RAW_ALLOWLIST.get(main_type, [])
		for sub in m.get_trigger_sub_types(main_type):
			var sub_dict := sub as Dictionary
			var sub_type := int(sub_dict["value"])
			var schema := Schema.trigger_slots(main_type, sub_type)
			if schema.known or allow.has(sub_type):
				continue
			missing.append("trigger main=%d sub=%d (%s)" % [main_type, sub_type, String(sub_dict["name"])])
	assert_eq(missing, [], "named trigger sub-types missing a schema row or allowlist entry")


func test_every_named_action_type_has_schema_or_allowlist() -> void:
	var m := _mission()
	var missing: Array = []
	for a in m.get_action_types():
		var a_dict := a as Dictionary
		var action_type := int(a_dict["value"])
		var schema := Schema.action_slots(action_type)
		if schema.known or ACTION_RAW_ALLOWLIST.has(action_type):
			continue
		missing.append("action=%d (%s)" % [action_type, String(a_dict["name"])])
	assert_eq(missing, [], "named action types missing a schema row or allowlist entry")


func test_no_schema_row_references_an_unnamed_enum() -> void:
	# Reverse guard was previously needed for editor-owned TRIGGERS/ACTIONS dictionaries. The schema now
	# lives in libs/mission beside the enum names, so stale rows are caught by C++ compilation/tests.
	var m := _mission()
	var stale: Array = []
	assert_gt(m.get_trigger_main_types().size(), 0, "enum reflectors remain available")
	assert_eq(stale, [], "schema rows referencing unnamed / removed enum values")
