extends GutTest

# CollisionDebugView: the F3 "Show collision" 3D overlay. Drives it with a
# duck-typed world/sim pair feeding a crafted get_collision_debug() dictionary
# (the same shape NovaSimulation emits) through the public refresh seam to pin:
# hull boxes + the player capsule draw, the ground-gap label reports, rotation
# refreshes cached hulls, and a vanished sim clears everything instead of erroring.

const ViewScript := preload("res://engine/debug/collision_debug_view.gd")


# Node-based doubles: the view's setup takes the owner's world NODE (a GameWorld
# in production) and duck-types get_sim()/get_collision_debug() off it.
class FakeSim:
	extends Node
	var debug: Dictionary = {}
	func get_collision_debug() -> Dictionary:
		return debug


class FakeWorld:
	extends Node
	var sim: Node = null
	func get_sim():
		return sim


static func _unit_box_corners() -> PackedVector3Array:
	# The corner order the sim emits: index bit0 = max x, bit1 = max y, bit2 = max z.
	var corners := PackedVector3Array()
	corners.resize(8)
	for c in range(8):
		corners[c] = Vector3(
			1.0 if (c & 1) != 0 else 0.0,
			1.0 if (c & 2) != 0 else 0.0,
			1.0 if (c & 4) != 0 else 0.0)
	return corners


static func _quarter_turn_box_corners() -> PackedVector3Array:
	# The same local unit box after a +90-degree world-space yaw about its origin.
	var corners := _unit_box_corners()
	for c in range(corners.size()):
		var corner := corners[c]
		corners[c] = Vector3(corner.z, corner.y, -corner.x)
	return corners


static func _shifted_box_corners() -> PackedVector3Array:
	var corners := _unit_box_corners()
	for c in range(corners.size()):
		corners[c] += Vector3(2.0, 0.0, 0.0)
	return corners


func _debug_payload() -> Dictionary:
	return {
		"instances": [{
			"entity_handle": 7,
			"pos": Vector3.ZERO,
			"heading": 0.0,
			"volumes": [{ "type": 1, "corners": _unit_box_corners() }],
		}],
		"player": {
			"valid": true,
			"position": Vector3(0, 1.5, 0),
			"capsule_bottom": 1.4,
			"capsule_top": 1.8,
			"foot_clearance": 0.05,
			"points": PackedVector3Array([Vector3(0, 2, 0), Vector3(0, 2, 0), Vector3(0, 1.5, 0)]),
			"radii": PackedFloat32Array([0.3, 0.3125, 0.7]),
		},
	}


func _make_world(payload: Dictionary) -> Node:
	var world: FakeWorld = autofree(FakeWorld.new())
	var sim: FakeSim = autofree(FakeSim.new())
	sim.debug = payload
	world.sim = sim
	return world


func _make_view(world: Node) -> Node3D:
	var view: Node3D = ViewScript.new()
	add_child_autofree(view)
	view.setup(world)
	return view


func test_draws_hulls_capsule_and_gap_label() -> void:
	var world := _make_world(_debug_payload())
	var view := _make_view(world)
	view.refresh_now()

	var hull := view.get_node("CollisionHullLines") as MeshInstance3D
	assert_gt((hull.mesh as ImmediateMesh).get_surface_count(), 0, "hull wireframe drawn")
	var player := view.get_node("CollisionPlayerLines") as MeshInstance3D
	assert_gt((player.mesh as ImmediateMesh).get_surface_count(), 0, "player capsule drawn")
	var label := view.get_node("CollisionGapLabel") as Label3D
	assert_true(label.visible, "the ground-gap label shows for a valid player capture")
	assert_string_contains(label.text, "ground gap 0.05")


func test_missing_sim_clears_instead_of_erroring() -> void:
	var world := _make_world(_debug_payload())
	var view := _make_view(world)
	view.refresh_now()

	# The sim goes away (mission unloaded): the next frame clears every surface.
	world.sim = null
	view.refresh_now()
	var hull := view.get_node("CollisionHullLines") as MeshInstance3D
	assert_eq((hull.mesh as ImmediateMesh).get_surface_count(), 0, "hulls cleared")
	var player := view.get_node("CollisionPlayerLines") as MeshInstance3D
	assert_eq((player.mesh as ImmediateMesh).get_surface_count(), 0, "capsule cleared")
	assert_false((view.get_node("CollisionGapLabel") as Label3D).visible, "label hidden")


func test_empty_world_draws_nothing() -> void:
	var world := _make_world({ "instances": [], "player": { "valid": false } })
	var view := _make_view(world)
	view.refresh_now()
	var hull := view.get_node("CollisionHullLines") as MeshInstance3D
	assert_eq((hull.mesh as ImmediateMesh).get_surface_count(), 0)
	assert_false((view.get_node("CollisionGapLabel") as Label3D).visible)


func test_rotation_only_refreshes_hull_geometry() -> void:
	var payload := _debug_payload()
	var world := _make_world(payload)
	var view := _make_view(world)
	view.refresh_now()
	var hull := view.get_node("CollisionHullLines") as MeshInstance3D
	var initial_bounds := (hull.mesh as ImmediateMesh).get_aabb()

	# The entity did not translate and kept the same collision model. Its new
	# heading changes only the transformed world-space corners in the sim payload.
	var instance: Dictionary = payload["instances"][0]
	instance["heading"] = 90.0
	(instance["volumes"][0] as Dictionary)["corners"] = _quarter_turn_box_corners()
	view.refresh_now()

	var rotated_bounds := (hull.mesh as ImmediateMesh).get_aabb()
	assert_ne(rotated_bounds, initial_bounds,
		"a vehicle rotating in place redraws its collision hull")
	assert_eq(rotated_bounds.position, Vector3(0.0, 0.0, -1.0),
		"the redrawn hull uses the rotated corners from the sim")


func test_same_pose_replacement_refreshes_hull_geometry() -> void:
	var payload := _debug_payload()
	var world := _make_world(payload)
	var view := _make_view(world)
	view.refresh_now()
	var hull := view.get_node("CollisionHullLines") as MeshInstance3D
	var initial_bounds := (hull.mesh as ImmediateMesh).get_aabb()

	# Mission reloads can reuse a handle at the same position and heading while
	# replacing its collision model. Geometry itself must participate in the key.
	var instance: Dictionary = payload["instances"][0]
	(instance["volumes"][0] as Dictionary)["corners"] = _shifted_box_corners()
	view.refresh_now()

	var replacement_bounds := (hull.mesh as ImmediateMesh).get_aabb()
	assert_ne(replacement_bounds, initial_bounds,
		"same-pose replacement geometry invalidates the hull cache")
	assert_eq(replacement_bounds.position, Vector3(2.0, 0.0, 0.0),
		"the redraw uses the replacement model's corners")
