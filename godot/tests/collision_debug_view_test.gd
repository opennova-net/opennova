extends GutTest

# CollisionDebugView: the F3 "Show collision" 3D overlay. Drives it with a
# crafted get_collision_debug() CollisionDebugReport (the record Simulation emits)
# through the public render seam to pin: hull boxes + the player capsule draw,
# the ground-gap label reports, rotation refreshes cached hulls, and a
# vanished sim clears everything instead of erroring.

const ViewScript := preload("res://game/debug/collision_debug_view.gd")


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


func _volume(type: int, corners: PackedVector3Array) -> CollisionDebugVolume:
	var volume := CollisionDebugVolume.new()
	volume.type = type
	volume.corners = corners
	return volume


func _probe_box(entity_handle: int, kind: String, corners: PackedVector3Array) -> CollisionProbeBox:
	var box := CollisionProbeBox.new()
	box.entity_handle = entity_handle
	box.kind = kind
	box.corners = corners
	return box


func _debug_payload() -> CollisionDebugReport:
	var report := CollisionDebugReport.new()
	var instance := CollisionDebugInstance.new()
	instance.entity_handle = 7
	instance.add_volume(_volume(1, _unit_box_corners()))
	report.add_instance(instance)
	report.player.valid = true
	report.player.position = Vector3(0, 1.5, 0)
	report.player.capsule_bottom = 1.4
	report.player.capsule_top = 1.8
	report.player.foot_clearance = 0.05
	report.player.points = PackedVector3Array([Vector3(0, 2, 0), Vector3(0, 2, 0), Vector3(0, 1.5, 0)])
	report.player.radii = PackedFloat32Array([0.3, 0.3125, 0.7])
	return report


func _make_view() -> Node3D:
	var view: Node3D = ViewScript.new()
	add_child_autofree(view)
	view.setup(null)
	return view


func test_draws_hulls_capsule_and_gap_label() -> void:
	var view := _make_view()
	view.render_report(_debug_payload())

	var hull := view.get_node("CollisionHullLines") as MeshInstance3D
	assert_gt((hull.mesh as ImmediateMesh).get_surface_count(), 0, "hull wireframe drawn")
	var player := view.get_node("CollisionPlayerLines") as MeshInstance3D
	assert_gt((player.mesh as ImmediateMesh).get_surface_count(), 0, "player capsule drawn")
	var label := view.get_node("CollisionGapLabel") as Label3D
	assert_true(label.visible, "the ground-gap label shows for a valid player capture")
	assert_string_contains(label.text, "ground gap 0.05")


func test_probe_boxes_draw_without_bvol_instances() -> void:
	# The vehicle platform probe boxes ride their own list — a mission whose
	# nearby set has no BVOL volumes must still draw them, and they count as
	# drawables.
	var view := _make_view()
	var report := CollisionDebugReport.new()
	report.add_probe_box(_probe_box(9, "probe", _unit_box_corners()))
	report.add_probe_box(_probe_box(9, "footprint", _shifted_box_corners()))
	view.render_report(report)
	var hull := view.get_node("CollisionHullLines") as MeshInstance3D
	assert_gt((hull.mesh as ImmediateMesh).get_surface_count(), 0,
			"probe wireframes drawn with no BVOL volumes present")
	assert_eq(view.get_debug_drawable_count(), 2,
			"both the probe box and its footprint count as drawables")


func test_missing_sim_clears_instead_of_erroring() -> void:
	var view := _make_view()
	view.render_report(_debug_payload())

	# The sim goes away (mission unloaded): the next frame's real refresh
	# resolves no world/sim and clears every surface.
	view.refresh_now()
	var hull := view.get_node("CollisionHullLines") as MeshInstance3D
	assert_eq((hull.mesh as ImmediateMesh).get_surface_count(), 0, "hulls cleared")
	var player := view.get_node("CollisionPlayerLines") as MeshInstance3D
	assert_eq((player.mesh as ImmediateMesh).get_surface_count(), 0, "capsule cleared")
	assert_false((view.get_node("CollisionGapLabel") as Label3D).visible, "label hidden")


func test_empty_world_draws_nothing() -> void:
	var view := _make_view()
	view.render_report(CollisionDebugReport.new())
	var hull := view.get_node("CollisionHullLines") as MeshInstance3D
	assert_eq((hull.mesh as ImmediateMesh).get_surface_count(), 0)
	assert_false((view.get_node("CollisionGapLabel") as Label3D).visible)


func test_rotation_only_refreshes_hull_geometry() -> void:
	var payload := _debug_payload()
	var view := _make_view()
	view.render_report(payload)
	var hull := view.get_node("CollisionHullLines") as MeshInstance3D
	var initial_bounds := (hull.mesh as ImmediateMesh).get_aabb()

	# The entity did not translate and kept the same collision model. Its new
	# heading changes only the transformed world-space corners in the sim payload.
	var instance: CollisionDebugInstance = payload.instances[0]
	instance.heading = 90.0
	(instance.volumes[0] as CollisionDebugVolume).corners = _quarter_turn_box_corners()
	view.render_report(payload)

	var rotated_bounds := (hull.mesh as ImmediateMesh).get_aabb()
	assert_ne(rotated_bounds, initial_bounds,
		"a vehicle rotating in place redraws its collision hull")
	assert_eq(rotated_bounds.position, Vector3(0.0, 0.0, -1.0),
		"the redrawn hull uses the rotated corners from the sim")


func test_same_pose_replacement_refreshes_hull_geometry() -> void:
	var payload := _debug_payload()
	var view := _make_view()
	view.render_report(payload)
	var hull := view.get_node("CollisionHullLines") as MeshInstance3D
	var initial_bounds := (hull.mesh as ImmediateMesh).get_aabb()

	# Mission reloads can reuse a handle at the same position and heading while
	# replacing its collision model. Geometry itself must participate in the key.
	var instance: CollisionDebugInstance = payload.instances[0]
	(instance.volumes[0] as CollisionDebugVolume).corners = _shifted_box_corners()
	view.render_report(payload)

	var replacement_bounds := (hull.mesh as ImmediateMesh).get_aabb()
	assert_ne(replacement_bounds, initial_bounds,
		"same-pose replacement geometry invalidates the hull cache")
	assert_eq(replacement_bounds.position, Vector3(2.0, 0.0, 0.0),
		"the redraw uses the replacement model's corners")


static func _hits(target: int, kind: int, at: Vector3, age := 0.0) -> PackedFloat32Array:
	# The contact-debug hits channel's stride-6 shape:
	# [target_handle, age_ticks, kind, x, y, z] per event.
	return PackedFloat32Array([float(target), age, float(kind), at.x, at.y, at.z])


func test_hits_flash_the_target_box_on_their_own_mesh() -> void:
	var view := _make_view()
	var payload := _debug_payload()
	payload.hits = _hits(7, 0, Vector3(0.5, 0.5, 0.5))
	payload.hit_ttl = 62
	view.render_report(payload)

	var hits := view.get_node("CollisionHitLines") as MeshInstance3D
	assert_gt((hits.mesh as ImmediateMesh).get_surface_count(), 0,
			"a hit on a drawn box overdraws it on the hit mesh")
	assert_eq(view.get_debug_drawable_count(), 2,
			"hits are decoration: the drawable count still counts shapes only")


func test_hit_with_unknown_target_still_marks_its_point() -> void:
	var view := _make_view()
	var payload := _debug_payload()
	payload.hits = _hits(-1, 4, Vector3(3.0, 0.0, 3.0))
	view.render_report(payload)

	var hits := view.get_node("CollisionHitLines") as MeshInstance3D
	assert_gt((hits.mesh as ImmediateMesh).get_surface_count(), 0,
			"a terrain/no-box hit still draws its cross marker")


func test_hits_clear_with_the_view() -> void:
	var view := _make_view()
	var payload := _debug_payload()
	payload.hits = _hits(7, 2, Vector3(0.5, 0.5, 0.5))
	view.render_report(payload)

	# The sim goes away: the real refresh clears the hit mesh with the rest.
	view.refresh_now()
	var hits := view.get_node("CollisionHitLines") as MeshInstance3D
	assert_eq((hits.mesh as ImmediateMesh).get_surface_count(), 0, "hit flashes cleared")
