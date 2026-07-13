class_name MissionParamSchema
extends RefCounted

## Designer-facing meaning of trigger/action param1..4. The semantic table lives
## in libs/mission so runtime and editor consume the same reverse-engineered core
## data; this wrapper preserves the old editor-facing GDScript API.

enum Kind { RAW, GROUP, ENTITY, ZONE, EVENT, WAYPOINT, BOOL, ENUM, FIXED_SECONDS }

static var _mission_schema_source: NovaMissionData


static func _source() -> NovaMissionData:
	if _mission_schema_source == null:
		_mission_schema_source = NovaMissionData.new()
	return _mission_schema_source


static func trigger_slots(main_type: int, sub_type: int) -> MissionParamSpec:
	return MissionParamSpec.from_dict(_source().get_trigger_param_schema(main_type, sub_type))


static func action_slots(action_type: int, sub_type: int = 0) -> MissionParamSpec:
	return MissionParamSpec.from_dict(_source().get_action_param_schema(action_type, sub_type))


static func is_picker(kind: int) -> bool:
	return kind == Kind.GROUP or kind == Kind.ENTITY or kind == Kind.ZONE \
		or kind == Kind.EVENT or kind == Kind.WAYPOINT or kind == Kind.BOOL or kind == Kind.ENUM
