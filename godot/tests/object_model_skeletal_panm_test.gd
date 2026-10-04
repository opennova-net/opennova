extends GutTest

# A model posed by its clip takes its part tracks over the clip's bone
# matrices, as every retail submit composes PANM over the matrices it draws
# with (the first-person gun with its clip's, its arms with the gun's): the
# composed part matrix lands as the bone's skin bind, so the part follows the
# clip and then its own track, while the bones keep the clip's pose.
# [orig: Render_CollectRenderObjectsForBatch @ 0x5D8F3B /
#  Render_CollectRenderBatchesForEntity @ 0x5D94F5 -> Model_TransformBoneMatrices
#  @ 0x58E390; docs/threedi/3di-gp-format-re.md D-3DI-6]

const PERSON := "res://../fixtures/threedi/synth/person_part9_trigger_scale.3di"
const ANIM := "res://../fixtures/anim"
const ARM := 9 # the left upper arm, scaled 1 -> 0 from WPN_TRIGGER


func _person_model() -> ObjectModel:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(PERSON)), OK,
			"the synthetic person opens")
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(ANIM)), OK)
	var skeletal := SkeletalAnim.new()
	assert_true(skeletal.load_from_resource_root(root, "soldier.adm",
			data.get_bone_origins(), data.get_bone_parents()),
			"the 19-bone character rig loads: %s" % skeletal.get_last_error())
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_process(false)
	model.set_skeletal_anim(skeletal)
	model.set_object_data(data)
	return model


# A part's model-space pivot (its ROBJ abs, the parent chain's summed rel
# offsets) in the mesh's frame, which carries the (-x, y, z) flip.
func _model_pivot(part: int) -> Vector3:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(PERSON)), OK)
	var rel: PackedVector3Array = data.get_bone_origins()
	var parents: PackedInt32Array = data.get_bone_parents()
	var at := Vector3.ZERO
	var i := part
	while true:
		at += rel[i]
		if parents[i] == i or parents[i] < 0:
			break
		i = parents[i]
	return Vector3(-at.x, at.y, at.z)


# The clip's own palette: pose x rest bind per bone.
func _clip_palette(skeleton: Skeleton3D) -> Array[Transform3D]:
	var out: Array[Transform3D] = []
	for bone in skeleton.get_bone_count():
		out.append(skeleton.get_bone_global_pose(bone)
				* skeleton.get_bone_global_rest(bone).affine_inverse())
	return out


# A pose off the bind: the spine and the scaled arm turn.
func _bend(model: ObjectModel) -> void:
	var skeleton: Skeleton3D = model.get_skeleton()
	skeleton.set_bone_pose_rotation(2, Quaternion(Vector3.RIGHT, deg_to_rad(40.0)))
	skeleton.set_bone_pose_rotation(ARM, Quaternion(Vector3.FORWARD, deg_to_rad(90.0)))


func test_a_clip_posed_part_takes_its_register_track() -> void:
	var model := _person_model()
	assert_true(model.has_skeleton(), "the .adm builds the rig")
	_bend(model)
	await get_tree().process_frame
	var skeleton: Skeleton3D = model.get_skeleton()
	# The register at zero: the track holds its start (scale 1), and every
	# part draws where the clip put it.
	model.set_ctrl_value("WPN_TRIGGER", 0)
	var clip := _clip_palette(skeleton)
	var palette: Array = model.get_skin_palette()
	assert_eq(palette.size(), clip.size(), "one row per bone")
	for bone in mini(palette.size(), clip.size()):
		assert_true((palette[bone] as Transform3D).is_equal_approx(clip[bone]),
				"bone %d draws its clip pose at the track's start" % bone)

	# The register full: the arm collapses onto its pivot, carried by the
	# clip (the pivot lands where the clip puts it); every other part stays.
	model.set_ctrl_value("WPN_TRIGGER", 0x10000)
	palette = model.get_skin_palette()
	var arm: Transform3D = palette[ARM]
	assert_almost_eq(arm.basis.determinant(), 0.0, 0.0001, "the arm is scaled to nothing")
	var pivot := _model_pivot(ARM)
	var landed: Vector3 = clip[ARM] * pivot
	for offset in [Vector3.ZERO, Vector3(0.1, 0.2, -0.3), Vector3(-0.25, 0.0, 0.5)]:
		assert_true((arm * (pivot + offset)).is_equal_approx(landed),
				"a point of the arm %s collapses onto the clip-carried pivot" % offset)
	for bone in [0, 2, 13, 14]:
		assert_true((palette[bone] as Transform3D).is_equal_approx(clip[bone]),
				"bone %d keeps its clip pose" % bone)
	# The bones themselves keep the clip's pose for every other reader.
	assert_true(skeleton.get_bone_global_pose(ARM).basis.determinant() > 0.5,
			"the arm's bone is not scaled")

	# Back to zero: the arm draws its clip pose again.
	model.set_ctrl_value("WPN_TRIGGER", 0)
	palette = model.get_skin_palette()
	assert_true((palette[ARM] as Transform3D).is_equal_approx(clip[ARM]),
			"the arm returns to its clip pose")
