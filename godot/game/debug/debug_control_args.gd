class_name DebugControlArgs
extends RefCounted
## The typed argument checks of the DebugControls action rows (ADR 0042 d5):
## every action validates its Array argument here before any engine call, over
## the engine domains DebugControls re-exports (mission coordinates, entity
## health, mission variables, audio bus range). Split out of DebugControls
## 2026-08-30 to keep that table under the shipping-script size ratchet; the
## checks are pure statics with no owner, so the split changes no behavior.

static func transport(args: Array) -> bool:
	return args.size() == 1 \
			and typeof(args[0]) == TYPE_STRING \
			and String(args[0]) in ["resume", "pause", "step"]


static func mission_variable(args: Array) -> bool:
	if args.size() != 2 or not is_integer_number(args[0]) \
			or not is_integer_number(args[1]):
		return false
	var index := int(args[0])
	var value := int(args[1])
	return index >= 0 and index < DebugControls.MISSION_VAR_COUNT \
			and value >= -2147483648 and value <= 2147483647


static func health(args: Array) -> bool:
	if args.size() != 2 or not is_integer_number(args[0]) \
			or not is_integer_number(args[1]):
		return false
	var index := int(args[0])
	var health := int(args[1])
	return index >= 0 and health >= DebugControls.ENTITY_HEALTH_MIN \
			and health <= DebugControls.ENTITY_HEALTH_MAX


static func entity_position(args: Array) -> bool:
	return args.size() == 2 and is_integer_number(args[0]) \
			and int(args[0]) >= 0 \
			and args[1] is Vector3 and mission_position(args[1])


## [wire_handle, attrib, attrib2]: a packed engine handle below the invalid
## sentinel and two unsigned 32-bit words.
static func item_attrib(args: Array) -> bool:
	if args.size() != 3 or not is_integer_number(args[0]) \
			or not is_integer_number(args[1]) or not is_integer_number(args[2]):
		return false
	return int(args[0]) >= 0 and int(args[0]) < 0xFFFF \
			and int(args[1]) >= 0 and int(args[1]) <= 0xFFFFFFFF \
			and int(args[2]) >= 0 and int(args[2]) <= 0xFFFFFFFF


static func teleport(args: Array) -> bool:
	if args.size() != 3 or not (args[0] is Vector3) \
			or not mission_position(args[0]) \
			or not is_finite_number(args[1]) \
			or not is_finite_number(args[2]):
		return false
	var yaw := float(args[1])
	var pitch := float(args[2])
	return is_finite(yaw) and yaw >= -360.0 and yaw <= 360.0 \
			and is_finite(pitch) and pitch >= -90.0 and pitch <= 90.0


static func deploy_pick(args: Array) -> bool:
	return args.size() == 1 and is_integer_number(args[0]) and int(args[0]) >= 0


static func weapon_name(args: Array) -> bool:
	return args.size() == 1 and typeof(args[0]) == TYPE_STRING \
			and not String(args[0]).strip_edges().is_empty()


static func look(args: Array) -> bool:
	return args.size() == 2 and is_finite_number(args[0]) and is_finite_number(args[1])


static func kill_group(args: Array) -> bool:
	return args.size() == 1 and is_integer_number(args[0]) and int(args[0]) > 0


static func crew_vehicle(args: Array) -> bool:
	return args.size() == 2 and is_integer_number(args[0]) \
			and is_integer_number(args[1]) \
			and int(args[0]) > 0 and int(args[1]) > 0


static func crew_local_player(args: Array) -> bool:
	return args.size() == 1 and is_integer_number(args[0]) and int(args[0]) > 0


static func audio_bus_volume(args: Array) -> bool:
	return args.size() == 2 \
			and typeof(args[0]) == TYPE_STRING \
			and not String(args[0]).is_empty() \
			and is_finite_number(args[1]) \
			and float(args[1]) >= DebugControls.AUDIO_BUS_VOLUME_MIN_DB \
			and float(args[1]) <= DebugControls.AUDIO_BUS_VOLUME_MAX_DB


static func audio_bus_switch(args: Array) -> bool:
	return args.size() == 2 \
			and typeof(args[0]) == TYPE_STRING \
			and not String(args[0]).is_empty() \
			and typeof(args[1]) == TYPE_BOOL


static func mission_position(position: Vector3) -> bool:
	return position.is_finite() \
			and position.x >= DebugControls.MISSION_COORD_MIN and position.x <= DebugControls.MISSION_COORD_MAX \
			and position.y >= DebugControls.MISSION_COORD_MIN and position.y <= DebugControls.MISSION_COORD_MAX \
			and position.z >= DebugControls.MISSION_COORD_MIN and position.z <= DebugControls.MISSION_COORD_MAX


static func is_finite_number(value: Variant) -> bool:
	if typeof(value) != TYPE_INT and typeof(value) != TYPE_FLOAT:
		return false
	return is_finite(float(value))


static func is_integer_number(value: Variant) -> bool:
	return is_finite_number(value) and float(value) == floorf(float(value))
