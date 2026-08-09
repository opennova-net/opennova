extends GutTest

# NovaEntityIndex resolves an event action's target (SSN / group / zone) back
# to the live animatable models the placer REGISTERED at construction time
# ({model, ref} records — nothing scans scene children). Asset-free: real
# native NovaObjectModel instances with no object data; the index holds ids,
# not behavior.


func _entry(parent: Node, bms_id: int, group: int, team: int,
		pos: Vector3) -> Dictionary:
	var model := NovaObjectModel.new()
	parent.add_child(model)
	return { "model": model, "ref": {
		"kind": 1, "index": 0, "bms_id": bms_id, "group": group,
		"team": team, "position": pos,
	} }


func test_resolve_single_by_bms_id() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var a := _entry(container, 1001, 0, 0, Vector3.ZERO)
	var b := _entry(container, 1002, 0, 0, Vector3.ZERO)
	var index := NovaEntityIndex.new()
	index.build([a, b], [])
	assert_eq(index.resolve_single(1001), a["model"], "bms_id 1001 -> model a")
	assert_eq(index.resolve_single(1002), b["model"], "bms_id 1002 -> model b")
	assert_null(index.resolve_single(9999), "unknown bms_id -> null")
	assert_null(index.resolve_single(0), "bms_id 0 -> null (no entity has SSN 0)")


func test_get_animatable_nodes_returns_the_registered_column() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var a := _entry(container, 1001, 0, 0, Vector3.ZERO)
	var b := _entry(container, 1002, 0, 0, Vector3.ZERO)
	var index := NovaEntityIndex.new()
	index.build([a, b], [])
	var nodes := index.get_animatable_nodes()
	assert_eq(nodes.size(), 2, "exactly the registered set is listed")
	assert_has(nodes, a["model"])
	assert_has(nodes, b["model"])


func test_kind_index_fallback_uses_distinct_packed_integer_keys() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var a := _entry(container, 0, -1, 0, Vector3.ZERO)
	var b := _entry(container, 0, -1, 0, Vector3.ZERO)
	(a["ref"] as Dictionary)["kind"] = 1
	(a["ref"] as Dictionary)["index"] = 0x1000000
	(b["ref"] as Dictionary)["kind"] = 2
	(b["ref"] as Dictionary)["index"] = 0
	var index := NovaEntityIndex.new()
	index.build([a, b], [])
	assert_eq(index.resolve(0, 1, 0x1000000), a["model"])
	assert_eq(index.resolve(0, 2, 0), b["model"])
	assert_eq(index.resolve(9999, 2, 0), b["model"],
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
	var index := NovaEntityIndex.new()
	index.build([a, b, c], [])
	var g5 := index.resolve_group(5)
	assert_eq(g5.size(), 2, "group 5 has two members")
	assert_true(g5.has(a["model"]) and g5.has(b["model"]), "group 5 = {a, b}")
	var g7 := index.resolve_group(7)
	assert_eq(g7.size(), 1, "group 7 has one member")
	assert_true(g7.has(c["model"]), "group 7 = {c}")
	assert_eq(index.resolve_group(99), [], "unknown group -> empty")
	assert_eq(index.resolve_group(-1), [], "negative group -> empty")


func test_resolve_zone_by_position() -> void:
	var container := Node.new()
	add_child_autofree(container)
	# Mission-space positions: BMS x/y are the horizontal plane, z is vertical.
	var inside := _entry(container, 1, 0, 0, Vector3(10, 10, 5))
	var outside := _entry(container, 2, 0, 0, Vector3(100, 100, 5))
	var triggers := [{
		"min": Vector3(0, 0, 0), "max": Vector3(50, 50, 50),
		"constrain_z": false,
	}]
	var index := NovaEntityIndex.new()
	index.build([inside, outside], triggers)
	var z0 := index.resolve_zone(0)
	assert_eq(z0.size(), 1, "one model inside the rect")
	assert_true(z0.has(inside["model"]), "only the in-rect model is in zone 0")
	assert_eq(index.resolve_zone(1), [], "out-of-range zone index -> empty")
	assert_eq(index.resolve_zone(-1), [], "negative zone index -> empty")


func test_resolve_zone_constrain_z() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var low := _entry(container, 1, 0, 0, Vector3(10, 10, 5))
	var high := _entry(container, 2, 0, 0, Vector3(10, 10, 500))
	var triggers := [{
		"min": Vector3(0, 0, 0), "max": Vector3(50, 50, 50),
		"constrain_z": true,
	}]
	var index := NovaEntityIndex.new()
	index.build([low, high], triggers)
	var z0 := index.resolve_zone(0)
	assert_eq(z0.size(), 1, "constrain_z keeps only the model within the vertical band")
	assert_true(z0.has(low["model"]), "the low model is in; the high model is excluded")


func test_freed_member_is_filtered() -> void:
	var container := Node.new()
	add_child_autofree(container)
	var a := _entry(container, 1, 5, 0, Vector3.ZERO)
	var b := _entry(container, 2, 5, 0, Vector3.ZERO)
	var index := NovaEntityIndex.new()
	index.build([a, b], [])
	var freed: Node = b["model"]
	container.remove_child(freed)
	freed.free()
	assert_null(index.resolve_single(2), "a freed model resolves to null")
	var g5 := index.resolve_group(5)
	assert_eq(g5.size(), 1, "the freed member is filtered from its group")
	assert_true(g5.has(a["model"]))


func test_non_model_entries_are_skipped() -> void:
	var container := Node.new()
	add_child_autofree(container)
	# A record whose model slot is not a NovaObjectModel (a collider, a stale
	# handle) is skipped at the typed boundary — registration is the only way
	# in, and only models register.
	var collider := Node.new()
	container.add_child(collider)
	var index := NovaEntityIndex.new()
	index.build([{ "model": collider,
			"ref": { "bms_id": 42, "group": 3, "position": Vector3.ZERO } }], [])
	assert_null(index.resolve_single(42), "a non-model entry is not indexed by SSN")
	assert_eq(index.resolve_group(3), [], "nor by group")
