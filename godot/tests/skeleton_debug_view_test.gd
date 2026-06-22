extends GutTest

# SkeletonDebugView: the F3 overlay's "Show skeletons" 3D bone overlay. Pins that it walks
# a world subtree for Skeleton3D nodes and emits line geometry for the bones it finds (and
# emits NOTHING — no invalid empty surface — when there are none).

const SkeletonDebugView := preload("res://engine/debug/skeleton_debug_view.gd")


func _make_skeleton() -> Skeleton3D:
	var skel := Skeleton3D.new()
	skel.add_bone("root")
	skel.add_bone("child")
	skel.set_bone_parent(1, 0)
	skel.set_bone_rest(0, Transform3D.IDENTITY)
	skel.set_bone_rest(1, Transform3D(Basis.IDENTITY, Vector3(0.0, 1.0, 0.0)))
	skel.reset_bone_poses()
	return skel


func test_draws_bones_for_skeletons_in_the_subtree() -> void:
	var root := Node3D.new()
	add_child_autofree(root)
	root.add_child(_make_skeleton())
	var view := SkeletonDebugView.new()
	add_child_autofree(view)
	view.setup(root)
	await get_tree().process_frame  # let the skeleton initialise its poses

	view._process(0.0)
	assert_gt(view._mesh.get_surface_count(), 0,
		"a skeleton in the subtree produces bone-line geometry")


func test_no_skeletons_emits_no_surface() -> void:
	# Walking a subtree with no Skeleton3D must leave the mesh empty — emitting an empty
	# PRIMITIVE_LINES surface is invalid in Godot.
	var root := Node3D.new()
	add_child_autofree(root)
	root.add_child(Node3D.new())  # a plain node, no skeleton
	var view := SkeletonDebugView.new()
	add_child_autofree(view)
	view.setup(root)

	view._process(0.0)
	assert_eq(view._mesh.get_surface_count(), 0, "nothing to draw -> no surface, no error")


func test_rebuilds_each_frame_when_a_skeleton_disappears() -> void:
	# The per-frame walk keeps the view correct as models spawn / despawn: once the only
	# skeleton is gone, the next _process clears back to an empty mesh.
	var root := Node3D.new()
	add_child_autofree(root)
	var skel := _make_skeleton()
	root.add_child(skel)
	var view := SkeletonDebugView.new()
	add_child_autofree(view)
	view.setup(root)
	await get_tree().process_frame
	view._process(0.0)
	assert_gt(view._mesh.get_surface_count(), 0)

	root.remove_child(skel)
	skel.free()
	view._process(0.0)
	assert_eq(view._mesh.get_surface_count(), 0, "the freed skeleton drops out next frame")
