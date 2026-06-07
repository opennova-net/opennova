extends Node

# Consumes the mission runtime's per-tick command stream (NovaWorld.mission_commands) and applies the
# host-side effects the runtime cannot perform itself. Today it implements PLAYPARTANIM: start / stop a
# model part animation on the targeted entity, group, or zone. Dispatch is a table keyed by AI sub-type
# so further host actions slot in later; only PLAYPARTANIM is wired now. Targets are resolved through a
# MissionEntityRegistry.
#
# [orig: Jointops EventAction_Dispatch @0x4542e0 -> Entity_HandleAlertStateEvent @0x43dee0 ->
#  Entity_ApplyCommand @0x43ab60 case 0x22 (PLAYPARTANIM).] See notes/mission/anim-ai-grill-2026-06-07.md.
#
# Referenced via preload() (no class_name), same convention as MissionObjectPlacer / MissionEntityRegistry.

# MissionRuntimeCommandKind::HostAction (== NovaMissionRuntime.COMMAND_HOST_ACTION).
const KIND_HOST_ACTION := 0

# bms::ActionType -- the AI-change family that carries an AI sub-type in action_sub_type.
const ACT_CHANGE_GROUP_AI := 3
const ACT_AREA_AI_RED := 12
const ACT_AREA_AI_BLUE := 13
const ACT_CHANGE_SINGLE_AI := 21

# bms::AIActionSubType.
const AI_SUB_PLAYPARTANIM := 34

# ANIMTIME is stored as 16.16 fixed-point seconds (raw = seconds * 65536); the runtime copies params
# verbatim, so the host converts to seconds exactly once. [orig: Entity_ApplyCommand case 0x22 fild*1/65536]
const ANIMTIME_FIXED_ONE := 65536.0

var _registry
var _mission
var _handlers: Dictionary = {}
var _stats: Dictionary = { "applied": 0, "unresolved": 0, "ignored": 0 }


func setup(registry, mission) -> void:
	_registry = registry
	_mission = mission
	_handlers = { AI_SUB_PLAYPARTANIM: _handle_play_part_anim }


## Connect to a NovaWorld's mission_commands signal (idempotent).
func attach(world) -> void:
	if world != null and world.has_signal("mission_commands") \
			and not world.mission_commands.is_connected(_on_mission_commands):
		world.mission_commands.connect(_on_mission_commands)


func get_stats() -> Dictionary:
	return _stats.duplicate()


func _on_mission_commands(commands: Array) -> void:
	for c in commands:
		_dispatch(c as Dictionary)


func _dispatch(cmd: Dictionary) -> void:
	if int(cmd.get("kind", -1)) != KIND_HOST_ACTION:
		_stats.ignored += 1
		return
	if not _is_ai_change(int(cmd.get("action_type", -1))):
		_stats.ignored += 1
		return
	var handler: Callable = _handlers.get(int(cmd.get("action_sub_type", -1)), Callable())
	if not handler.is_valid():
		_stats.ignored += 1
		return
	handler.call(cmd)


func _is_ai_change(action_type: int) -> bool:
	return action_type == ACT_CHANGE_SINGLE_AI or action_type == ACT_CHANGE_GROUP_AI \
		or action_type == ACT_AREA_AI_RED or action_type == ACT_AREA_AI_BLUE


func _handle_play_part_anim(cmd: Dictionary) -> void:
	var nodes := _resolve_targets(cmd)
	if nodes.is_empty():
		_stats.unresolved += 1
		return
	var channel := int(cmd.get("param2", 0))                       # ANIMNUM (channel 1/2)
	var play_type := int(cmd.get("param3", 0))                     # ANIMPLAYTYPE {1 play, 0 stop, -1 rev}
	var time_s := float(int(cmd.get("param4", 0))) / ANIMTIME_FIXED_ONE  # ANIMTIME 16.16 -> seconds
	var applied := false
	for node in nodes:
		if node != null and is_instance_valid(node) and node.has_method("play_part_anim"):
			node.play_part_anim(channel, play_type, time_s)
			applied = true
	if applied:
		_stats.applied += 1
	else:
		_stats.unresolved += 1


func _resolve_targets(cmd: Dictionary) -> Array:
	if _registry == null:
		return []
	var target := int(cmd.get("param1", 0))
	match int(cmd.get("action_type", -1)):
		ACT_CHANGE_SINGLE_AI:
			var node = _registry.resolve_single(target)
			return [node] if node != null else []
		ACT_CHANGE_GROUP_AI:
			return _registry.resolve_group(target)
		ACT_AREA_AI_RED, ACT_AREA_AI_BLUE:
			return _registry.resolve_zone(target)
	return []
