extends GutTest

# Proves the physics-pick MECHANISM end-to-end, isolated from the editor: a StaticBody3D
# with a CollisionShape3D, in an own-world SubViewport, is hit by an intersect_ray down
# its location and reports its entity_ref meta. If this passes, any in-editor pick failure
# is viewport wiring, not the body/query path. The control test confirms the main-tree
# world also works, to localise the difference.

func _make_body(ref: Dictionary, pos: Vector3) -> StaticBody3D:
	var body := StaticBody3D.new()
	body.set_meta("entity_ref", ref)
	var cs := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = Vector3(2, 2, 2)
	cs.shape = box
	body.add_child(cs)
	body.position = pos
	return body


func test_intersect_ray_hits_body_in_own_world_subviewport() -> void:
	var sub := SubViewport.new()
	sub.own_world_3d = true
	sub.world_3d = World3D.new()
	add_child_autofree(sub)

	var body := _make_body({"kind": 1, "index": 7}, Vector3(0, 0, -10))
	sub.add_child(body)

	await get_tree().physics_frame
	await get_tree().physics_frame

	var ss := body.get_world_3d().direct_space_state
	assert_not_null(ss, "own-world SubViewport exposes a direct space state")
	var q := PhysicsRayQueryParameters3D.create(Vector3(0, 0, 0), Vector3(0, 0, -20))
	var hit := ss.intersect_ray(q)
	assert_false(hit.is_empty(), "ray hits the static body in the own-world SubViewport")
	if not hit.is_empty():
		assert_eq(hit["collider"], body, "the hit collider is our body")
		var got: Dictionary = (hit["collider"] as Object).get_meta("entity_ref", {})
		assert_eq(int(got.get("kind", -1)), 1)
		assert_eq(int(got.get("index", -1)), 7)


func test_intersect_ray_hits_body_in_main_tree_world() -> void:
	# Control: a body directly under the GUT scene tree (main window world) is hittable.
	var body := _make_body({"kind": 2, "index": 3}, Vector3(5, 0, -10))
	add_child_autofree(body)

	await get_tree().physics_frame
	await get_tree().physics_frame

	var ss := body.get_world_3d().direct_space_state
	assert_not_null(ss, "main tree world exposes a direct space state")
	var q := PhysicsRayQueryParameters3D.create(Vector3(5, 0, 0), Vector3(5, 0, -20))
	var hit := ss.intersect_ray(q)
	assert_false(hit.is_empty(), "ray hits the static body in the main tree world")
