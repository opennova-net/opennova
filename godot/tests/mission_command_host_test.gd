extends GutTest

# MissionCommandHost consumes NovaWorld.mission_commands and applies PLAYPARTANIM to the resolved
# target model(s). Asset-free: a fake registry returning canned nodes + fake model nodes capturing
# play_part_anim calls. Covers target resolution per action_type, the ANIMTIME 16.16->seconds decode,
# the kind/sub-type/action-type filtering, and signal wiring.

const Host := preload("res://engine/world/mission_command_host.gd")

const ACT_KILL_GROUP := 2
const ACT_CHANGE_GROUP_AI := 3
const ACT_AREA_AI_RED := 12
const ACT_AREA_AI_BLUE := 13
const ACT_CHANGE_SINGLE_AI := 21
const AI_SUB_ACCURACY := 8
const AI_SUB_PLAYPARTANIM := 34
const KIND_HOST_ACTION := 0
const KIND_OUTPUT_TEXT := 3


class FakeModel:
	extends Node
	var calls: Array = []
	func play_part_anim(channel: int, play_type: int, time_s: float) -> void:
		calls.append([channel, play_type, time_s])


class FakeRegistry:
	extends RefCounted
	var single: Node = null
	var group: Array = []
	var zone: Array = []
	func resolve_single(_bms_id: int) -> Node:
		return single
	func resolve_group(_group_id: int) -> Array:
		return group
	func resolve_zone(_zone_index: int) -> Array:
		return zone


class FakeWorld:
	extends Node
	signal mission_commands(commands: Array)


func _cmd(action_type: int, action_sub: int, p1: int, p2: int, p3: int, p4: int, kind: int = KIND_HOST_ACTION) -> Dictionary:
	return {
		"kind": kind, "action_type": action_type, "action_sub_type": action_sub,
		"param1": p1, "param2": p2, "param3": p3, "param4": p4,
	}


func _host(reg) -> Object:
	var h := Host.new()
	autofree(h)
	h.setup(reg, null)
	return h


func test_single_ai_playpartanim_routes_to_node() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var reg := FakeRegistry.new()
	reg.single = model
	var host := _host(reg)
	# channel 2, play, time = 2.0s (raw 2 * 65536)
	host._on_mission_commands([_cmd(ACT_CHANGE_SINGLE_AI, AI_SUB_PLAYPARTANIM, 1001, 2, 1, 2 * 65536)])
	assert_eq(model.calls.size(), 1, "one play_part_anim call")
	var call: Array = model.calls[0]
	assert_eq(int(call[0]), 2, "channel = param2")
	assert_eq(int(call[1]), 1, "play_type = param3")
	assert_almost_eq(float(call[2]), 2.0, 0.0001, "time_s = param4 / 65536")
	assert_eq(int(host.get_stats()["applied"]), 1, "counted as applied")


func test_group_ai_applies_to_each_member() -> void:
	var m1 := FakeModel.new(); add_child_autofree(m1)
	var m2 := FakeModel.new(); add_child_autofree(m2)
	var reg := FakeRegistry.new()
	reg.group = [m1, m2]
	var host := _host(reg)
	host._on_mission_commands([_cmd(ACT_CHANGE_GROUP_AI, AI_SUB_PLAYPARTANIM, 5, 1, -1, 65536)])
	assert_eq(m1.calls.size(), 1, "member 1 played")
	assert_eq(m2.calls.size(), 1, "member 2 played")
	assert_eq(int((m1.calls[0] as Array)[1]), -1, "play_type reverse passes through")
	assert_almost_eq(float((m1.calls[0] as Array)[2]), 1.0, 0.0001, "1 second")


func test_area_ai_red_and_blue_apply_to_zone_members() -> void:
	var m1 := FakeModel.new(); add_child_autofree(m1)
	var reg := FakeRegistry.new()
	reg.zone = [m1]
	var host := _host(reg)
	host._on_mission_commands([_cmd(ACT_AREA_AI_RED, AI_SUB_PLAYPARTANIM, 0, 1, 1, 65536)])
	assert_eq(m1.calls.size(), 1, "AreaAiRed applies to zone members")
	host._on_mission_commands([_cmd(ACT_AREA_AI_BLUE, AI_SUB_PLAYPARTANIM, 0, 1, 1, 65536)])
	assert_eq(m1.calls.size(), 2, "AreaAiBlue too")


func test_non_host_action_kind_is_ignored() -> void:
	var model := FakeModel.new(); add_child_autofree(model)
	var reg := FakeRegistry.new(); reg.single = model
	var host := _host(reg)
	host._on_mission_commands([_cmd(ACT_CHANGE_SINGLE_AI, AI_SUB_PLAYPARTANIM, 1, 1, 1, 65536, KIND_OUTPUT_TEXT)])
	assert_eq(model.calls.size(), 0, "a non-HostAction command is ignored")
	assert_eq(int(host.get_stats()["ignored"]), 1)


func test_non_playpartanim_subtype_is_ignored() -> void:
	var model := FakeModel.new(); add_child_autofree(model)
	var reg := FakeRegistry.new(); reg.single = model
	var host := _host(reg)
	# An AI-change action but sub-type Accuracy (8), not PLAYPARTANIM (not yet implemented host-side).
	host._on_mission_commands([_cmd(ACT_CHANGE_SINGLE_AI, AI_SUB_ACCURACY, 1, 90, 0, 0)])
	assert_eq(model.calls.size(), 0, "a non-PLAYPARTANIM AI sub-type is ignored")
	assert_eq(int(host.get_stats()["ignored"]), 1)


func test_non_ai_change_action_is_ignored() -> void:
	var model := FakeModel.new(); add_child_autofree(model)
	var reg := FakeRegistry.new(); reg.single = model
	var host := _host(reg)
	# KillGroup (2) is a HostAction but not part of the AI-change family.
	host._on_mission_commands([_cmd(ACT_KILL_GROUP, AI_SUB_PLAYPARTANIM, 1, 1, 1, 65536)])
	assert_eq(model.calls.size(), 0, "a non AI-change action is ignored")
	assert_eq(int(host.get_stats()["ignored"]), 1)


func test_unresolved_target_counts_unresolved() -> void:
	var reg := FakeRegistry.new()  # single stays null -> nothing resolves
	var host := _host(reg)
	host._on_mission_commands([_cmd(ACT_CHANGE_SINGLE_AI, AI_SUB_PLAYPARTANIM, 1234, 1, 1, 65536)])
	assert_eq(int(host.get_stats()["unresolved"]), 1, "no target -> unresolved, no crash")
	assert_eq(int(host.get_stats()["applied"]), 0)


func test_attach_drives_host_from_world_signal_idempotently() -> void:
	var model := FakeModel.new(); add_child_autofree(model)
	var reg := FakeRegistry.new(); reg.single = model
	var host := _host(reg)
	var world := FakeWorld.new(); add_child_autofree(world)
	host.attach(world)
	host.attach(world)  # second attach must not double-connect
	assert_eq(world.mission_commands.get_connections().size(), 1, "attach is idempotent")
	world.mission_commands.emit([_cmd(ACT_CHANGE_SINGLE_AI, AI_SUB_PLAYPARTANIM, 1, 1, 1, 65536)])
	assert_eq(model.calls.size(), 1, "emitting the world signal drives the host")
