extends GutTest

# 3di collision-volume exposure + convex-hull building for exact mission picking.
# This locks the two hard-won facts: the collision coordinate frame
# ((x,y,z) -> (y,z,x), validated against the visual mesh) and the plane convention
# (Godot Plane distance is the negated stored offset -- a wrong sign collapses every
# hull to the AABB fallback). Uses the real 3di fixtures; the def fixtures ship no
# .3di, so the placer's asset-free tests live elsewhere.

const CollisionHull := preload("res://game/object/collision_hull.gd")

const FIXTURE_DIR := "res://../fixtures/threedi/synth"


func _model(name: String) -> ObjectData:
	var data := ObjectData.new()
	var path := ProjectSettings.globalize_path("%s/%s.3di" % [FIXTURE_DIR, name])
	assert_eq(data.open_file(path), OK, "%s.3di loads" % name)
	return data


func _hull_union_aabb(volumes: Array) -> AABB:
	var aabb := AABB()
	var first := true
	for v in volumes:
		for p in CollisionHull.hull_points(v):
			if first:
				aabb = AABB(p, Vector3.ZERO)
				first = false
			else:
				aabb = aabb.expand(p)
	return aabb


func _visual_aabb(data: ObjectData) -> AABB:
	var aabb := AABB()
	var first := true
	for s in data.build_lod_submeshes(0):
		var entry: Dictionary = s
		var mesh: ArrayMesh = entry.get("mesh")
		if mesh == null:
			continue
		# Render vertices are absolute model coordinates (the PANM node matrices
		# pivot about ROBJ abs themselves), so the mesh AABB is already the
		# part's model-space extent.
		var m: AABB = mesh.get_aabb()
		if first:
			aabb = m
			first = false
		else:
			aabb = aabb.merge(m)
	return aabb


func test_models_expose_collision_volumes() -> void:
	for name in ["house", "shed", "carrier"]:
		var data := _model(name)
		assert_true(data.has_collision(), "%s reports collision" % name)
		var vols: Array = data.get_collision_volumes()
		assert_gt(vols.size(), 0, "%s exposes volumes" % name)
		var v: Dictionary = vols[0]
		for key in ["type", "min", "max", "planes"]:
			assert_true(v.has(key), "%s volume dict has '%s'" % [name, key])
		assert_true(v["planes"] is Array, "planes is an array of Plane")


func test_hulls_are_real_convex_not_box_fallback() -> void:
	# A correct plane sign yields plane-derived hulls; a wrong one collapses every
	# volume to the 8-corner AABB fallback. house carries multi-plane volumes, so at
	# least one hull must be carved (> 8 points) -- the canary for the sign fix.
	var data := _model("house")
	var carved := 0
	for v in data.get_collision_volumes():
		var pts := CollisionHull.hull_points(v)
		assert_true(pts.size() >= 4, "hull is non-degenerate")
		if pts.size() > 8:
			carved += 1
	assert_gt(carved, 0, "house has carved (multi-plane) hulls, not just boxes")


func test_hull_frame_overlaps_visual_mesh() -> void:
	# The hull union sits on the drawn model: X/Z extents match the visual within
	# tolerance (proves the (y,z,x) frame). Y may legitimately differ (collision
	# extent vs visual silhouette), so it is not asserted tightly.
	for name in ["shed", "carrier"]:
		var data := _model(name)
		var hull := _hull_union_aabb(data.get_collision_volumes())
		var vis := _visual_aabb(data)
		assert_true(hull.intersects(vis), "%s hull intersects the visual AABB" % name)
		assert_almost_eq(hull.size.x, vis.size.x, maxf(0.2, vis.size.x * 0.25), "%s X extent matches visual" % name)
		assert_almost_eq(hull.size.z, vis.size.z, maxf(0.2, vis.size.z * 0.25), "%s Z extent matches visual" % name)


func test_shapes_for_returns_convex_shapes() -> void:
	var data := _model("shed")
	var vols: Array = data.get_collision_volumes()
	var shapes := CollisionHull.shapes_for(vols)
	assert_eq(shapes.size(), vols.size(), "one shape per volume")
	for s in shapes:
		assert_true(s is ConvexPolygonShape3D, "shape is a convex hull")
		assert_true((s as ConvexPolygonShape3D).points.size() >= 4, "shape carries hull points")


func test_color_is_stable_and_distinct_per_type() -> void:
	assert_eq(CollisionHull.color_for_type(1), CollisionHull.color_for_type(1), "color is stable for a type")
	assert_ne(CollisionHull.color_for_type(1), CollisionHull.color_for_type(7), "distinct types get distinct colors")
	assert_eq(CollisionHull.name_for_type(1), "CB", "known type maps to its abbreviation")
	assert_eq(CollisionHull.name_for_type(999), "", "unknown type has no name")


func test_model_without_collision_has_no_volumes() -> void:
	var data := _model("gun")
	assert_false(data.has_collision(), "gun carries no collision volumes")
	assert_eq(data.get_collision_volumes().size(), 0, "and none are exposed")


func test_skinned_person_reports_face_and_sphere_collision_without_volumes() -> void:
	var data := _model("person")
	assert_true(data.is_skinned(0), "person is the skeletal collision fixture")
	assert_eq(data.get_collision_volumes().size(), 0, "person has no BVOL collision")
	assert_true(data.has_collision(), "person still reports its CFAC/COBJ collision")
