extends GutTest

# MissionCommandHost poses each entity's PLAYPARTANIM part channels onto its model every tick,
# reading the engine-computed phase from the sim (the action is applied in-engine; the host only
# renders from the AI brain). Asset-free: a fake sim exposing the entity/part-anim query surface,
# a fake registry resolving SSN -> node, and fake models capturing set_part_phase calls.

const Host := preload("res://engine/world/mission_command_host.gd")


class FakeModel:
	extends Node
	var phases: Array = []  # [channel, phase]
	func set_part_phase(channel: int, phase: int) -> void:
		phases.append([channel, phase])


class FakeRegistry:
	extends RefCounted
	var by_ssn: Dictionary = {}  # ssn -> Node
	func resolve_single(ssn: int) -> Node:
		var n = by_ssn.get(ssn, null)
		return n if (n != null and is_instance_valid(n)) else null


# Minimal stand-in for NovaSimulation's entity/part-anim query surface.
class FakeSim:
	extends RefCounted
	var entities: Array = []  # [{ bms_id, ch: { 1: {active, phase}, 2: {...} } }]
	func get_entity_count() -> int:
		return entities.size()
	func get_entity_bms_id(i: int) -> int:
		return int((entities[i] as Dictionary).get("bms_id", 0))
	func get_entity_part_anim_active(i: int, channel: int) -> bool:
		var ch: Dictionary = (entities[i] as Dictionary).get("ch", {})
		return bool((ch.get(channel, {}) as Dictionary).get("active", false))
	func get_entity_part_anim_phase(i: int, channel: int) -> int:
		var ch: Dictionary = (entities[i] as Dictionary).get("ch", {})
		return int((ch.get(channel, {}) as Dictionary).get("phase", 0))


func _host(reg, sim) -> Object:
	var h := Host.new()
	autofree(h)
	h.setup(reg, sim)
	return h


func test_active_channel_poses_to_phase() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var reg := FakeRegistry.new()
	reg.by_ssn = { 1001: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 1001, "ch": { 1: { "active": true, "phase": 32768 } } }]
	var host := _host(reg, sim)
	host.render()
	assert_eq(model.phases.size(), 1, "one channel posed")
	assert_eq(int((model.phases[0] as Array)[0]), 1, "channel 1")
	assert_eq(int((model.phases[0] as Array)[1]), 32768, "engine-computed phase passes through")
	assert_eq(int(host.get_stats()["posed"]), 1)


func test_inactive_channel_not_posed() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var reg := FakeRegistry.new()
	reg.by_ssn = { 1: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 1, "ch": { 1: { "active": false, "phase": 5 } } }]
	var host := _host(reg, sim)
	host.render()
	assert_eq(model.phases.size(), 0, "an untouched channel is left at its default pose")


func test_both_channels_posed() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var reg := FakeRegistry.new()
	reg.by_ssn = { 7: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 7, "ch": { 1: { "active": true, "phase": 100 }, 2: { "active": true, "phase": 200 } } }]
	var host := _host(reg, sim)
	host.render()
	assert_eq(model.phases.size(), 2, "both active channels posed")


func test_zero_bms_id_skipped() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var reg := FakeRegistry.new()
	reg.by_ssn = { 0: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 0, "ch": { 1: { "active": true, "phase": 9 } } }]
	var host := _host(reg, sim)
	host.render()
	assert_eq(model.phases.size(), 0, "an entity with no file id is skipped")


func test_resolves_by_bms_id_not_ssn() -> void:
	# The registry is keyed by the file entity id (bms_id), which is a DIFFERENT id space than the
	# runtime SSN. The host must resolve by bms_id; using the SSN would silently fail to find the node.
	var model := FakeModel.new()
	add_child_autofree(model)
	var reg := FakeRegistry.new()
	reg.by_ssn = { 4242: model }  # keyed by the file id, not any SSN
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 4242, "ch": { 1: { "active": true, "phase": 77 } } }]
	var host := _host(reg, sim)
	host.render()
	assert_eq(model.phases.size(), 1, "resolved by the file entity id")
	assert_eq(int((model.phases[0] as Array)[1]), 77)


func test_unresolved_target_does_not_crash() -> void:
	var reg := FakeRegistry.new()  # empty -> resolves nothing
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 1234, "ch": { 1: { "active": true, "phase": 1 } } }]
	var host := _host(reg, sim)
	host.render()  # must not crash
	assert_eq(int(host.get_stats()["posed"]), 0, "nothing posed when the target is unresolved")
