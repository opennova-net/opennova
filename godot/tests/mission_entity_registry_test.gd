extends GutTest

# EntityIndex resolves an event action's target (SSN / group / zone) back
# to the live animatable models the placer REGISTERED at construction time
# (each model carrying its EntityRef — nothing scans scene children).
# Asset-free: real native ObjectModel instances with no object data; the
# index holds ids, not behavior.


func _entry(parent: Node, bms_id: int, group: int, team: int,
		pos: Vector3) -> ObjectModel:
	var model := ObjectModel.new()
	parent.add_child(model)
	var ref := EntityRef.make(1, 0, bms_id)
	ref.group = group
	ref.team = team
	ref.position = pos
	model.entity_ref = ref
	return model


func test_resolve_single_by_bms_id() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var a := _entry(container, 1001, 0, 0, Vector3.ZERO)
	var b := _entry(container, 1002, 0, 0, Vector3.ZERO)
	var index := EntityIndex.new()
	index.build([a, b], null)
	assert_eq(index.resolve_single(1001), a, "bms_id 1001 -> model a")
	assert_eq(index.resolve_single(1002), b, "bms_id 1002 -> model b")
	assert_null(index.resolve_single(9999), "unknown bms_id -> null")
	assert_null(index.resolve_single(0), "bms_id 0 -> null (no entity has SSN 0)")


func test_get_animatable_nodes_returns_the_registered_column() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var a := _entry(container, 1001, 0, 0, Vector3.ZERO)
	var b := _entry(container, 1002, 0, 0, Vector3.ZERO)
	var index := EntityIndex.new()
	index.build([a, b], null)
	var nodes := index.get_animatable_nodes()
	assert_eq(nodes.size(), 2, "exactly the registered set is listed")
	assert_has(nodes, a)
	assert_has(nodes, b)


func test_kind_index_fallback_uses_distinct_packed_integer_keys() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var a := _entry(container, 0, -1, 0, Vector3.ZERO)
	var b := _entry(container, 0, -1, 0, Vector3.ZERO)
	a.entity_ref.kind = 1
	a.entity_ref.index = 0x1000000
	b.entity_ref.kind = 2
	b.entity_ref.index = 0
	var index := EntityIndex.new()
	index.build([a, b], null)
	assert_eq(index.resolve(0, 1, 0x1000000), a)
	assert_eq(index.resolve(0, 2, 0), b)
	assert_eq(index.resolve(9999, 2, 0), b,
			"an absent primary SSN still falls back to the packed origin")
	var generation := index.get_generation()
	index.clear()
	assert_gt(index.get_generation(), generation,
			"clear invalidates any presentation row plan bound to this index")


func test_resolve_group_members() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var a := _entry(container, 1, 5, 0, Vector3.ZERO)
	var b := _entry(container, 2, 5, 0, Vector3.ZERO)
	var c := _entry(container, 3, 7, 0, Vector3.ZERO)
	var index := EntityIndex.new()
	index.build([a, b, c], null)
	var g5 := index.resolve_group(5)
	assert_eq(g5.size(), 2, "group 5 has two members")
	assert_true(g5.has(a) and g5.has(b), "group 5 = {a, b}")
	var g7 := index.resolve_group(7)
	assert_eq(g7.size(), 1, "group 7 has one member")
	assert_true(g7.has(c), "group 7 = {c}")
	assert_eq(index.resolve_group(99), [], "unknown group -> empty")
	assert_eq(index.resolve_group(-1), [], "negative group -> empty")


func test_resolve_zone_by_position() -> void:
	var container := Node.new()
	add_child_autofree(container)
	# Mission-space positions: BMS x/y are the horizontal plane, z is vertical.
	var inside := _entry(container, 1, 0, 0, Vector3(10, 10, 5))
	var outside := _entry(container, 2, 0, 0, Vector3(100, 100, 5))
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_eq(mission.add_area_trigger(Vector3.ZERO, Vector3(50, 50, 50), true, false, 0), 0)
	var index := EntityIndex.new()
	index.build([inside, outside], mission)
	var z0 := index.resolve_zone(0)
	assert_eq(z0.size(), 1, "one model inside the rect")
	assert_true(z0.has(inside), "only the in-rect model is in zone 0")
	assert_eq(index.resolve_zone(1), [], "out-of-range zone index -> empty")
	assert_eq(index.resolve_zone(-1), [], "negative zone index -> empty")


func test_resolve_zone_constrain_z() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var low := _entry(container, 1, 0, 0, Vector3(10, 10, 5))
	var high := _entry(container, 2, 0, 0, Vector3(10, 10, 500))
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_eq(mission.add_area_trigger(Vector3.ZERO, Vector3(50, 50, 50), true, true, 0), 0)
	var index := EntityIndex.new()
	index.build([low, high], mission)
	var z0 := index.resolve_zone(0)
	assert_eq(z0.size(), 1, "constrain_z keeps only the model within the vertical band")
	assert_true(z0.has(low), "the low model is in; the high model is excluded")


func test_freed_member_is_filtered() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var a := _entry(container, 1, 5, 0, Vector3.ZERO)
	var b := _entry(container, 2, 5, 0, Vector3.ZERO)
	b.entity_ref.index = 1
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_eq(mission.add_area_trigger(Vector3(-1, -1, -1), Vector3.ONE, true, false, 0), 0)
	var index := EntityIndex.new()
	index.build([a, b], mission)
	assert_eq(index.resolve(2, 1, 0), b, "a live primary wins over a different origin")
	var freed: Node = b
	container.remove_child(freed)
	freed.free()
	assert_null(index.resolve_single(2), "a freed model resolves to null")
	var g5 := index.resolve_group(5)
	assert_eq(g5.size(), 1, "the freed member is filtered from its group")
	assert_true(g5.has(a))

	assert_eq(index.resolve(2, 1, 0), a, "a freed primary falls back to the live origin")
	assert_eq(index.resolve_zone(0), [a], "zone results filter the freed model")
	assert_eq(index.get_animatable_nodes(), [a], "the diagnostic set filters the freed model")


func test_models_without_an_entity_ref_are_skipped() -> void:
	var container := Node.new()
	add_child_autofree(container)
	# A registered model carrying no EntityRef (a helper, a viewmodel) is
	# skipped at the typed boundary — only entity models index.
	var helper := ObjectModel.new()
	container.add_child(helper)
	var index := EntityIndex.new()
	index.build([helper], null)
	assert_eq(index.get_animatable_nodes().size(), 0, "a ref-less model is not indexed")
	assert_null(index.resolve_single(42), "nor resolvable by SSN")
