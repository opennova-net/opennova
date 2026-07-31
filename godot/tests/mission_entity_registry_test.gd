extends GutTest

# MissionEntityRegistry resolves an event action's target (SSN / group / zone) back to the live animatable
# model nodes the placer tagged with "entity_ref" meta. Asset-free: fake animatable nodes (exposing
# play_part_anim, the marker the registry filters on) + a fake mission supplying area-trigger rects.

const Registry := preload("res://engine/world/mission_entity_registry.gd")


class FakeAnimNode:
	extends Node
	func play_part_anim(_channel: int, _play_type: int, _time_s: float) -> void:
		pass


class FakeMission:
	extends RefCounted
	var triggers: Array = []
	func get_area_triggers() -> Array:
		return triggers


func _node(parent: Node, bms_id: int, group: int, team: int, pos: Vector3) -> FakeAnimNode:
	var n := FakeAnimNode.new()
	n.set_meta("entity_ref", {
		"kind": 1, "index": 0, "bms_id": bms_id, "group": group, "team": team, "position": pos,
	})
	parent.add_child(n)
	return n


func test_resolve_single_by_bms_id() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var a := _node(container, 1001, 0, 0, Vector3.ZERO)
	var b := _node(container, 1002, 0, 0, Vector3.ZERO)
	var reg := Registry.new()
	reg.build(container, null)
	assert_eq(reg.resolve_single(1001), a, "bms_id 1001 -> node a")
	assert_eq(reg.resolve_single(1002), b, "bms_id 1002 -> node b")
	assert_null(reg.resolve_single(9999), "unknown bms_id -> null")
	assert_null(reg.resolve_single(0), "bms_id 0 -> null (no entity has SSN 0)")


func test_get_animatable_nodes_returns_the_indexed_node_column() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var a := _node(container, 1001, 0, 0, Vector3.ZERO)
	var b := _node(container, 1002, 0, 0, Vector3.ZERO)
	var plain := Node.new()  # no play_part_anim/entity_ref: never indexed
	container.add_child(plain)
	var reg := Registry.new()
	reg.build(container, null)
	var nodes := reg.get_animatable_nodes()
	assert_eq(nodes.size(), 2, "only the animatable set is listed")
	assert_has(nodes, a)
	assert_has(nodes, b)
	assert_does_not_have(nodes, plain)


func test_kind_index_fallback_uses_distinct_packed_integer_keys() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var a := _node(container, 0, 0, 0, Vector3.ZERO)
	var b := _node(container, 0, 0, 0, Vector3.ZERO)
	a.set_meta("entity_ref", {
		"kind": 1, "index": 0x1000000, "bms_id": 0,
		"group": -1, "team": 0, "position": Vector3.ZERO,
	})
	b.set_meta("entity_ref", {
		"kind": 2, "index": 0, "bms_id": 0,
		"group": -1, "team": 0, "position": Vector3.ZERO,
	})
	var reg := Registry.new()
	reg.build(container, null)
	assert_eq(reg.resolve(0, 1, 0x1000000), a)
	assert_eq(reg.resolve(0, 2, 0), b)
	assert_eq(reg.resolve(9999, 2, 0), b,
			"an absent primary SSN still falls back to the packed origin")
	var generation := reg.get_generation()
	reg.clear()
	assert_gt(reg.get_generation(), generation,
			"clear invalidates any presentation row plan bound to this registry")


func test_resolve_group_members() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var a := _node(container, 1, 5, 0, Vector3.ZERO)
	var b := _node(container, 2, 5, 0, Vector3.ZERO)
	var c := _node(container, 3, 7, 0, Vector3.ZERO)
	var reg := Registry.new()
	reg.build(container, null)
	var g5 := reg.resolve_group(5)
	assert_eq(g5.size(), 2, "group 5 has two members")
	assert_true(g5.has(a) and g5.has(b), "group 5 = {a, b}")
	var g7 := reg.resolve_group(7)
	assert_eq(g7.size(), 1, "group 7 has one member")
	assert_true(g7.has(c), "group 7 = {c}")
	assert_eq(reg.resolve_group(99), [], "unknown group -> empty")
	assert_eq(reg.resolve_group(-1), [], "negative group -> empty")


func test_resolve_zone_by_position() -> void:
	var container := Node.new()
	add_child_autofree(container)
	# Mission-space positions: BMS x/y are the horizontal plane, z is vertical.
	var inside := _node(container, 1, 0, 0, Vector3(10, 10, 5))
	var _outside := _node(container, 2, 0, 0, Vector3(100, 100, 5))
	var mission := FakeMission.new()
	mission.triggers = [{ "min": Vector3(0, 0, 0), "max": Vector3(50, 50, 50), "constrain_z": false }]
	var reg := Registry.new()
	reg.build(container, mission)
	var z0 := reg.resolve_zone(0)
	assert_eq(z0.size(), 1, "one node inside the rect")
	assert_true(z0.has(inside), "only the in-rect node is in zone 0")
	assert_eq(reg.resolve_zone(1), [], "out-of-range zone index -> empty")
	assert_eq(reg.resolve_zone(-1), [], "negative zone index -> empty")


func test_resolve_zone_constrain_z() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var low := _node(container, 1, 0, 0, Vector3(10, 10, 5))
	var _high := _node(container, 2, 0, 0, Vector3(10, 10, 500))  # outside the vertical band
	var mission := FakeMission.new()
	mission.triggers = [{ "min": Vector3(0, 0, 0), "max": Vector3(50, 50, 50), "constrain_z": true }]
	var reg := Registry.new()
	reg.build(container, mission)
	var z0 := reg.resolve_zone(0)
	assert_eq(z0.size(), 1, "constrain_z keeps only the node within the vertical band")
	assert_true(z0.has(low), "the low node is in; the high node is excluded")


func test_freed_member_is_filtered() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var a := _node(container, 1, 5, 0, Vector3.ZERO)
	var b := _node(container, 2, 5, 0, Vector3.ZERO)
	var reg := Registry.new()
	reg.build(container, null)
	container.remove_child(b)
	b.free()
	assert_null(reg.resolve_single(2), "a freed node resolves to null")
	var g5 := reg.resolve_group(5)
	assert_eq(g5.size(), 1, "the freed member is filtered from its group")
	assert_true(g5.has(a))


func test_non_animatable_children_are_ignored() -> void:
	var container := Node.new()
	add_child_autofree(container)
	# A plain node with entity_ref but no play_part_anim (e.g. an editor pick collider or static batch)
	# is not a valid PLAYPARTANIM target and must not be indexed.
	var collider := Node.new()
	collider.set_meta("entity_ref", { "bms_id": 42, "group": 3, "position": Vector3.ZERO })
	container.add_child(collider)
	var reg := Registry.new()
	reg.build(container, null)
	assert_null(reg.resolve_single(42), "a non-animatable node is not indexed by SSN")
	assert_eq(reg.resolve_group(3), [], "nor by group")
