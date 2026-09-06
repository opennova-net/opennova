extends GutTest

# ClipProjector: a Godot Animation over a BN## rig becomes a .bad clip the
# runtime's own loader (SkeletalAnim) plays back to the same pose.

const FPS := 30
const HEAD_ROT := PI / 2


func _rig() -> Node3D:
	var root := Node3D.new()
	root.name = "person"
	var lod := Node3D.new()
	lod.name = "LOD0"
	root.add_child(lod)
	var skeleton := Skeleton3D.new()
	skeleton.name = "Skeleton3D"
	lod.add_child(skeleton)
	# Godot's importer orders bones as the DCC listed them; the rows are the BN numbers.
	skeleton.add_bone("BN03 Proxy")
	skeleton.add_bone("BN01 Hips")
	skeleton.add_bone("BN02 Head")
	skeleton.set_bone_parent(0, 1)
	skeleton.set_bone_parent(2, 1)
	skeleton.set_bone_rest(1, Transform3D(Basis(), Vector3(0.0, 1.0, 0.0)))
	skeleton.set_bone_rest(2, Transform3D(Basis(), Vector3(0.0, 0.7, 0.0)))
	skeleton.set_bone_rest(0, Transform3D(Basis(), Vector3(0.0, -1.0, 0.0)))
	return root


func _swing(axis: Vector3) -> Animation:
	var animation := Animation.new()
	animation.length = 1.0
	var track := animation.add_track(Animation.TYPE_ROTATION_3D)
	animation.track_set_path(track, NodePath("LOD0/Skeleton3D:BN02 Head"))
	animation.rotation_track_insert_key(track, 0.0, Quaternion.IDENTITY)
	animation.rotation_track_insert_key(track, 1.0, Quaternion(axis, HEAD_ROT))
	return animation


func _spec(name: String, animation: String) -> ClipSpec:
	var spec := ClipSpec.new()
	spec.clip_name = name
	spec.animation = animation
	spec.loop = true
	spec.forward_speed = 3.0
	spec.triggers = PackedInt32Array([1])
	return spec


func test_the_rest_hold_is_the_reset_clip_shape() -> void:
	var root := _rig()
	add_child_autofree(root)
	var spec := _spec("rst", "")
	spec.hold_frames = 2
	spec.loop = true
	var projector := ClipProjector.new()
	var clip := projector.project(root, null, spec, FPS, -1)
	assert_not_null(clip, projector.get_last_error())
	if clip == null:
		return
	assert_eq(clip.get_fps(), FPS)
	assert_eq(clip.get_frame_count(), 2, "hold_frames intervals")
	assert_eq(clip.get_bone_count(), 3)
	assert_eq(clip.get_bone_name(0), "BN01 Hips")
	assert_eq(clip.get_bone_name(1), "BN02 Head")
	assert_eq(clip.get_bone_name(2), "BN03 Proxy")
	assert_eq(clip.get_bone_parent(0), -1)
	assert_eq(clip.get_bone_parent(1), 0)
	assert_eq(clip.get_bone_parent(2), 0)
	assert_eq(clip.get_bone_position(0), Vector3(0, 1, 0), "the root's pivot")
	assert_eq(clip.get_bone_position(1), Vector3(0, 0.7, 0), "parent-relative pivots")
	assert_eq(clip.get_bone_position(2), Vector3(0, -1, 0))
	for bone in 3:
		assert_eq(clip.get_channel_key_count(bone), 3, "frame_count + 1 keys")
		for key in 3:
			assert_true(clip.get_channel_rotation(bone, key).is_equal_approx(Quaternion.IDENTITY),
					"the rest hold is the identity channel")
	assert_eq(clip.get_event_count(), 3)
	assert_almost_eq(clip.get_event_bottom(0), 1.0, 0.0001, "the root's height above the ground proxy")
	assert_almost_eq(clip.get_event_top(0), 1.7, 0.0001, "the highest bone above the ground proxy")
	assert_eq(clip.get_event_velocity(1), Vector3(0.0, 0.0, 3.0 / FPS), "root motion per frame, forward in z")
	assert_eq(clip.get_event_trigger(0), 1)
	assert_eq(clip.get_event_trigger(1), 0)
	assert_false(clip.has_translations())
	assert_true(clip.is_loop())


func test_a_swing_about_x_keys_the_model_space_rotation() -> void:
	var root := _rig()
	add_child_autofree(root)
	var projector := ClipProjector.new()
	var clip := projector.project(root, _swing(Vector3.RIGHT), _spec("swing", "swing"), FPS, -1)
	assert_not_null(clip, projector.get_last_error())
	if clip == null:
		return
	assert_eq(clip.get_frame_count(), 30, "one second at 30 fps")
	assert_eq(clip.get_channel_key_count(1), 31)
	assert_true(clip.get_channel_rotation(1, 0).is_equal_approx(Quaternion.IDENTITY))
	assert_true(clip.get_channel_rotation(1, 30).is_equal_approx(Quaternion(Vector3.RIGHT, HEAD_ROT)),
			"the rotation is keyed as authored")
	assert_true(clip.get_channel_rotation(0, 30).is_equal_approx(Quaternion.IDENTITY), "the hips stay")
	assert_false(clip.has_translations(), "a bone turning on its own pivot follows the forward kinematics")


func test_a_swing_about_y_crosses_unchanged_too() -> void:
	var root := _rig()
	add_child_autofree(root)
	var projector := ClipProjector.new()
	var clip := projector.project(root, _swing(Vector3.UP), _spec("swingy", "swingy"), FPS, -1)
	assert_not_null(clip, projector.get_last_error())
	if clip == null:
		return
	var q := clip.get_channel_rotation(1, 30)
	assert_true(q.is_equal_approx(Quaternion(Vector3.UP, HEAD_ROT)),
			"the clip's frame is the presentation frame: a turn about y keeps its sense")


func test_the_runtime_loader_plays_the_projected_clip_back() -> void:
	var root := _rig()
	add_child_autofree(root)
	var projector := ClipProjector.new()
	var reset := projector.project(root, null, _spec("rst", ""), FPS, -1)
	# A one-shot: a loop's playhead at its full length wraps to the seam key.
	var swing_spec := _spec("swing", "swing")
	swing_spec.loop = false
	var swing := projector.project(root, _swing(Vector3.RIGHT), swing_spec, FPS, -1)
	assert_not_null(reset)
	assert_not_null(swing)
	if reset == null or swing == null:
		return
	# A loose root outside user:// (ResourceRoot refuses roots under the user data dir).
	var dir := OS.get_cache_dir().path_join("clip_projector_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	assert_eq(reset.save_to_path(dir.path_join("rst.bad")), OK, reset.get_last_error())
	assert_eq(swing.save_to_path(dir.path_join("swing.bad")), OK, swing.get_last_error())
	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.set_root_dir(dir), OK, resource_root.get_last_error())
	var anim := SkeletalAnim.new()
	var origins := PackedVector3Array([Vector3(0, 1, 0), Vector3(0, 0.7, 0), Vector3(0, -1, 0)])
	var parents := PackedInt32Array([-1, 0, 0])
	assert_true(anim.load_from_bad_files(resource_root, "rst.bad", {"anim_idle": "swing.bad"}, origins, parents),
			anim.get_last_error())
	var pose := anim.eval_pose("anim_idle", 1.0)
	assert_eq(pose.size(), 3)
	if pose.size() != 3:
		return
	var head: Transform3D = pose[1]
	assert_true(head.basis.get_rotation_quaternion().is_equal_approx(Quaternion(Vector3.RIGHT, HEAD_ROT)),
			"the runtime plays the head's swing back: %s" % head.basis.get_rotation_quaternion())
	assert_true(head.origin.is_equal_approx(Vector3(0, 0.7, 0)), "on its pivot")
	var hips: Transform3D = pose[0]
	assert_true(hips.basis.get_rotation_quaternion().is_equal_approx(Quaternion.IDENTITY))
	var start := anim.eval_pose("anim_idle", 0.0)
	assert_true((start[1] as Transform3D).basis.get_rotation_quaternion().is_equal_approx(Quaternion.IDENTITY))


func test_the_runtime_loader_keeps_a_turn_about_y() -> void:
	var root := _rig()
	add_child_autofree(root)
	var projector := ClipProjector.new()
	var reset := projector.project(root, null, _spec("rst", ""), FPS, -1)
	var swing_spec := _spec("swingy", "swingy")
	swing_spec.loop = false
	var swing := projector.project(root, _swing(Vector3.UP), swing_spec, FPS, -1)
	assert_not_null(reset)
	assert_not_null(swing)
	if reset == null or swing == null:
		return
	var dir := OS.get_cache_dir().path_join("clip_projector_y_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	assert_eq(reset.save_to_path(dir.path_join("rst.bad")), OK)
	assert_eq(swing.save_to_path(dir.path_join("swingy.bad")), OK)
	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.set_root_dir(dir), OK)
	var anim := SkeletalAnim.new()
	var origins := PackedVector3Array([Vector3(0, 1, 0), Vector3(0, 0.7, 0), Vector3(0, -1, 0)])
	var parents := PackedInt32Array([-1, 0, 0])
	assert_true(anim.load_from_bad_files(resource_root, "rst.bad", {"anim_idle": "swingy.bad"}, origins, parents),
			anim.get_last_error())
	var pose := anim.eval_pose("anim_idle", 1.0)
	assert_eq(pose.size(), 3)
	if pose.size() != 3:
		return
	var head: Transform3D = pose[1]
	assert_true(head.basis.get_rotation_quaternion().is_equal_approx(Quaternion(Vector3.UP, HEAD_ROT)),
			"the runtime plays a turn about y back with the authored sense: %s" % head.basis.get_rotation_quaternion())
