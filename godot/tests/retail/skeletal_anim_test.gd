extends GutTest

# Skeletal-animation runtime (.bad/.adm) over the retail install: the native
# pose_skeleton and the scripted eval_pose bone loop agree on a shipped rig.
# The synthetic skin-plumbing and resource cases live in the core half
# (tests/skeletal_anim_test.gd).


func test_pose_skeleton_matches_script_bone_loop() -> void:
	var install_dir := RetailData.install()
	if install_dir.is_empty():
		pending("OPENNOVA_JO_DIR / retail JO PFFs are required for the pose equivalence witness")
		return
	var root := ResourceRoot.new()
	assert_eq(root.mount_runtime(install_dir, "", false, "jo"), OK)
	var sk := SkeletalAnim.new()
	assert_true(sk.load_from_resource_root(root, "M82_1st.adm"),
			"M82_1st.adm loads from the runtime mount")
	assert_true(sk.has_clip("anim_reset"), "M82_1st carries anim_reset")

	var native_skel := Skeleton3D.new()
	var script_skel := Skeleton3D.new()
	add_child_autofree(native_skel)
	add_child_autofree(script_skel)
	var bones: Array = sk.get_skeleton_bones()
	assert_gt(bones.size(), 0, "rig has bones")
	for skel in [native_skel, script_skel]:
		# Pose equivalence is indexed. Retail labels may contain ':' or '/',
		# which Godot rejects; ObjectModel sanitizes them for the live rig.
		for i in range(bones.size()):
			(skel as Skeleton3D).add_bone("bone_%d" % i)
		for i in range(bones.size()):
			var b: Dictionary = bones[i]
			var parent := int(b.get("parent_index", -1))
			if parent >= 0:
				(skel as Skeleton3D).set_bone_parent(i, parent)
			(skel as Skeleton3D).set_bone_rest(i, b.get("rest", Transform3D()))

	var playhead := 0.37
	sk.pose_skeleton(native_skel, "anim_reset", playhead, 0,
			PackedInt32Array(), [], "", 0.0, false)
	var pose: Array = sk.eval_pose("anim_reset", playhead, 0)
	var count: int = mini(pose.size(), script_skel.get_bone_count())
	assert_gt(count, 0, "pose covers bones")
	for i in range(count):
		var t: Transform3D = pose[i]
		script_skel.set_bone_pose_position(i, t.origin)
		script_skel.set_bone_pose_rotation(i, t.basis.get_rotation_quaternion())
		script_skel.set_bone_pose_scale(i, t.basis.get_scale())
	for i in range(count):
		var dp: float = native_skel.get_bone_pose_position(i).distance_to(
				script_skel.get_bone_pose_position(i))
		assert_lt(dp, 0.00001, "bone %d position matches" % i)
		var qn: Quaternion = native_skel.get_bone_pose_rotation(i)
		var qs: Quaternion = script_skel.get_bone_pose_rotation(i)
		# godot-cpp's Basis->Quaternion conversion differs from core Godot's in
		# branch selection at component boundaries (the same family as the
		# Basis(axis, angle) divergence in godot/src/CLAUDE.md): the script
		# loop ran core's math, pose_skeleton runs godot-cpp's. Observed max
		# ~0.0007 rad on boundary bones — render-invisible; exactness across the
		# boundary is unattainable by construction.
		assert_lt(absf(qn.angle_to(qs)), 0.002, "bone %d rotation matches" % i)
		var ds: float = native_skel.get_bone_pose_scale(i).distance_to(
				script_skel.get_bone_pose_scale(i))
		assert_lt(ds, 0.00001, "bone %d scale matches" % i)

	# The BN17 collapse branch: zero scale, identity rotation, sampled origin.
	if count > 16:
		sk.pose_skeleton(native_skel, "anim_reset", playhead, 0,
				PackedInt32Array(), [], "", 0.0, true)
		assert_eq(native_skel.get_bone_pose_scale(16), Vector3.ZERO,
				"collapse_right_hand zero-scales bone 16")
		assert_eq(native_skel.get_bone_pose_rotation(16), Quaternion.IDENTITY,
				"collapse_right_hand clears bone 16 rotation")
