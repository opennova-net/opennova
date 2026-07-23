extends GutTest

# Skeletal-animation runtime (.bad/.adm) tests:
#  - skin plumbing: build_lod_submeshes emits ARRAY_BONES/ARRAY_WEIGHTS for skinned models
#    and nothing extra for static ones (regression-safe).
#  - NovaSkeletalAnim resource basics (graceful failure, empty state).
#  - NovaObjectModel's new skeletal methods parse and no-op safely without a skeletal set.

const CHARMODEL := "res://../fixtures/threedi/3di3/CharModel.3di"
const SHED := "res://../fixtures/threedi/3di3/Shed.3di"
const VIEWMODEL_RIG_TMP := "res://.godot/viewmodel_rig_test"
const MissionObjectPlacerScript = preload("res://engine/mission/mission_object_placer.gd")
const NovaObjectModelScript = preload("res://engine/object/nova_object_model.gd")


func _open(path: String) -> NovaObjectData:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(path)), OK, "fixture should open: %s" % path)
	return data


func _stage_viewmodel_rig_file(name: String, source: String) -> void:
	var out := FileAccess.open(
		ProjectSettings.globalize_path(VIEWMODEL_RIG_TMP.path_join(name)), FileAccess.WRITE)
	assert_not_null(out, "staging %s" % name)
	if out != null:
		out.store_buffer(FileAccess.get_file_as_bytes(source))
		out.close()


func _clear_viewmodel_rig_fixture() -> void:
	var path := ProjectSettings.globalize_path(VIEWMODEL_RIG_TMP)
	if not DirAccess.dir_exists_absolute(path):
		return
	var dir := DirAccess.open(path)
	if dir != null:
		for name in dir.get_files():
			DirAccess.remove_absolute(path.path_join(name))
	DirAccess.remove_absolute(path)


func test_skinned_model_emits_bone_arrays() -> void:
	var data := _open(CHARMODEL)
	if not data.is_skinned(0):
		pass_test("CharModel.3di is not flagged skinned; skin-array assertions skipped.")
		return
	var submeshes: Array = data.build_lod_submeshes(0)
	assert_false(submeshes.is_empty(), "Skinned model should build submeshes.")
	var found_skinned := false
	for entry in submeshes:
		var sm: Dictionary = entry
		if not bool(sm.get("is_skinned", false)):
			continue
		found_skinned = true
		var mesh: ArrayMesh = sm.get("mesh")
		var arrays: Array = mesh.surface_get_arrays(0)
		var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var bones = arrays[Mesh.ARRAY_BONES]
		var weights = arrays[Mesh.ARRAY_WEIGHTS]
		assert_true(bones is PackedInt32Array and not bones.is_empty(), "Skinned mesh has ARRAY_BONES.")
		assert_true(weights is PackedFloat32Array and not weights.is_empty(), "Skinned mesh has ARRAY_WEIGHTS.")
		assert_eq(bones.size(), weights.size(), "bones and weights are parallel.")
		# Godot stores 4 (or 8) bones/weights per vertex.
		var per: int = int(bones.size()) / maxi(verts.size(), 1)
		assert_true(per == 4 or per == 8, "4 or 8 influences per vertex (got %d)." % per)
		# Weights sum to ~1 per vertex.
		for i in range(verts.size()):
			var s := 0.0
			for k in range(per):
				s += float(weights[i * per + k])
			assert_almost_eq(s, 1.0, 0.02, "vertex %d weights sum to 1" % i)
	assert_true(found_skinned, "At least one skinned submesh expected on a skinned model.")


func test_static_model_has_no_bone_arrays() -> void:
	var data := _open(SHED)
	assert_false(data.is_skinned(0), "Shed.3di should be a static model.")
	var submeshes: Array = data.build_lod_submeshes(0)
	assert_false(submeshes.is_empty(), "Static model should still build submeshes.")
	for entry in submeshes:
		var sm: Dictionary = entry
		assert_false(bool(sm.get("is_skinned", false)), "Static submesh must not be skinned.")
		var mesh: ArrayMesh = sm.get("mesh")
		var arrays: Array = mesh.surface_get_arrays(0)
		var bones = arrays[Mesh.ARRAY_BONES]
		assert_true(bones == null or (bones is PackedInt32Array and bones.is_empty()), "No ARRAY_BONES on a static mesh.")


func test_rigid_fake_skin_when_skeletal() -> void:
	# A rigid (non-skinned) model gets "fake skinning" when a skeleton is applied: every vertex
	# fully weighted (1.0) to one bone = its subobject (part) index. This is how a .adm drives a
	# rigid first-person weapon. The default (skeletal=false) path stays bone-free (covered above).
	var data := _open(SHED)
	var submeshes: Array = data.build_lod_submeshes(0, true, 64)
	assert_false(submeshes.is_empty(), "Skeletal build still yields submeshes.")
	for entry in submeshes:
		var sm: Dictionary = entry
		assert_true(bool(sm.get("is_skinned", false)), "Rigid submesh is fake-skinned under skeletal mode.")
		var mesh: ArrayMesh = sm.get("mesh")
		var arrays: Array = mesh.surface_get_arrays(0)
		var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var bones = arrays[Mesh.ARRAY_BONES]
		var weights = arrays[Mesh.ARRAY_WEIGHTS]
		assert_true(bones is PackedInt32Array and bones.size() == verts.size() * 4, "Fake-skin ARRAY_BONES = 4/vert.")
		assert_true(weights is PackedFloat32Array and weights.size() == verts.size() * 4, "Fake-skin ARRAY_WEIGHTS = 4/vert.")
		# First vertex: fully weighted to a single in-range bone.
		assert_almost_eq(float(weights[0]), 1.0, 0.001, "primary weight 1.0")
		assert_almost_eq(float(weights[1]), 0.0, 0.001, "secondary weight 0")
		var b0 := int(bones[0])
		assert_true(b0 >= 0 and b0 < 64, "fake-skin bone index within skeleton range")


func test_viewmodel_graphic_bones_are_covered_when_adm_model_is_shorter() -> void:
	# M14 regression shape: its M21B_1st graphic and reload BAD carry 42 indexed
	# parts/bones, while the M21_1st model sharing the ADM basename has only 40.
	# Reproduce that mismatch with committed fixtures: a 19-part graphic whose skin
	# reaches bone 18 and a 1-part ADM-named decoy model. The visible gun's model
	# table must size the shared gun+arms rig, or late parts clamp to the last bone.
	_clear_viewmodel_rig_fixture()
	var tmp := ProjectSettings.globalize_path(VIEWMODEL_RIG_TMP)
	assert_eq(DirAccess.make_dir_recursive_absolute(tmp), OK)
	_stage_viewmodel_rig_file("CharModel.3di", CHARMODEL)
	_stage_viewmodel_rig_file("soldier.3di", SHED)
	_stage_viewmodel_rig_file("soldier.adm", "res://../fixtures/anim/soldier.adm")
	_stage_viewmodel_rig_file("idle.bad", "res://../fixtures/anim/idle.bad")
	_stage_viewmodel_rig_file("walk.bad", "res://../fixtures/anim/walk.bad")

	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(tmp), OK)
	var placer := MissionObjectPlacerScript.new(root, null)
	var parent := Node3D.new()
	add_child_autofree(parent)
	var model = placer.build_model_from_graphic("CharModel", "soldier", parent, "anim_idle", null)
	assert_not_null(model, "the synthetic viewmodel resolves")
	if model != null:
		var bone_count: int = model.get_skeletal_anim().get_bone_count()
		var max_weighted_bone := -1
		for entry in model.get_object_data().build_lod_submeshes(0, true, bone_count):
			var mesh := (entry as Dictionary).get("mesh") as ArrayMesh
			if mesh == null:
				continue
			var arrays := mesh.surface_get_arrays(0)
			var bones = arrays[Mesh.ARRAY_BONES]
			var weights = arrays[Mesh.ARRAY_WEIGHTS]
			if not (bones is PackedInt32Array and weights is PackedFloat32Array):
				continue
			for i in range(mini(bones.size(), weights.size())):
				if float(weights[i]) > 0.0:
					max_weighted_bone = maxi(max_weighted_bone, int(bones[i]))
		assert_eq(bone_count, 19, "the visible graphic's model table sizes the rig")
		assert_eq(max_weighted_bone, 18, "the fixture exercises its final model bone")
		assert_lt(max_weighted_bone, bone_count,
			"every weighted gun part has a matching animation bone")
	_clear_viewmodel_rig_fixture()


func test_skeletal_anim_resource_basics() -> void:
	var sk := NovaSkeletalAnim.new()
	assert_false(sk.is_loaded(), "Fresh NovaSkeletalAnim is not loaded.")
	assert_eq(sk.get_bone_count(), 0)
	assert_false(sk.load_from_resource_root(null, "missing.adm"), "Null resource root fails gracefully.")
	assert_false(sk.is_loaded())
	assert_false(sk.has_clip("anim_walk"))
	assert_eq(sk.eval_pose("anim_walk", 0.0).size(), 0, "eval_pose on an unloaded set is empty.")
	assert_eq(sk.get_clip_keys().size(), 0)
	# slot_to_key on an unloaded set has nothing to resolve / fall back to -> empty (no crash).
	assert_eq(sk.slot_to_key(2), "", "slot_to_key on an unloaded set is empty.")


func test_load_from_bad_files_binds_raw_bads() -> void:
	# The PLAYER_INFO preview path [orig: PlayerInfo_InitPreviewModel @ 0x5600d0]: build a
	# skeletal set from explicit raw .bad files with no .adm. Uses the committed idle.bad as
	# both the rest/skeleton source and the clip, registered under the canonical idle key.
	var sk := NovaSkeletalAnim.new()
	if not sk.has_method("load_from_bad_files"):
		fail_test("NovaSkeletalAnim exposes load_from_bad_files(root, skeleton_bad, key_to_bad)")
		return
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/anim"))
	assert_true(sk.load_from_bad_files(root, "idle.bad", {"anim_idle": "idle.bad"}),
		"raw .bad bind loads: %s" % sk.get_last_error())
	assert_true(sk.is_loaded(), "loaded after a successful raw-.bad bind")
	assert_true(sk.has_clip("anim_idle"), "the idle clip is registered under anim_idle")
	assert_gt(sk.get_bone_count(), 0, "the skeleton bones came from the rest .bad")
	assert_eq(sk.slot_to_key(1), "anim_idle", "kBodyAnimIdle resolves to the registered idle clip")
	assert_eq(sk.eval_pose("anim_idle", 0.0).size(), sk.get_bone_count(),
		"eval_pose returns one transform per skeleton bone")


func test_load_from_bad_files_fails_gracefully() -> void:
	var sk := NovaSkeletalAnim.new()
	if not sk.has_method("load_from_bad_files"):
		fail_test("NovaSkeletalAnim exposes load_from_bad_files")
		return
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/anim"))
	# Missing skeleton .bad -> false + error, not loaded.
	assert_false(sk.load_from_bad_files(root, "does_not_exist.bad", {"anim_idle": "idle.bad"}),
		"a missing skeleton .bad fails")
	assert_false(sk.is_loaded())
	assert_ne(sk.get_last_error(), "", "an error message is reported")
	# Null resource root also fails without crashing.
	assert_false(sk.load_from_bad_files(null, "idle.bad", {"anim_idle": "idle.bad"}),
		"a null resource root fails")


func test_model_skeletal_methods_no_op_without_set() -> void:
	var model = NovaObjectModelScript.new()
	add_child_autofree(model)
	model.set_skeletal_anim(null)  # exercises the rebuild() skeletal branch with no skeletal
	assert_false(model.has_skeleton(), "No Skeleton3D without a skeletal set.")
	model.play_body_clip("anim_walk")  # no-op
	model.play_body_anim(2)  # no-op without a skeletal set (present-pass entry point)
	model.play_body_anim(-1)  # no-op on "no clip" slot
	model.stop_body_clip()
	assert_eq(model.get_active_body_clip(), "", "No active clip without a skeletal set.")


func _loaded_skeletal() -> NovaSkeletalAnim:
	var sk := NovaSkeletalAnim.new()
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/anim"))
	assert_true(sk.load_from_resource_root(root, "soldier.adm"),
		"soldier.adm fixture loads: %s" % sk.get_last_error())
	return sk


func test_emplaced_pose_collapses_right_hand_bone_and_restores_off_mount() -> void:
	# Retail zeros model bone 16 (BN17 R Hand) while an organic occupies a
	# controller/gunner/driver parent slot. The personal weapon is baked into the
	# character mesh, so this is a skeletal collapse, not a child-node visibility gate.
	# [orig: Entity_BuildBoneTransformMatrices special rows; world-wac-ai-re.md §14.1]
	var data := _open(CHARMODEL)
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/anim"))
	var sk := NovaSkeletalAnim.new()
	assert_true(sk.load_from_resource_root(root, "soldier.adm",
			data.get_bone_origins(), data.get_bone_parents()),
			"the 19-bone character rig loads: %s" % sk.get_last_error())
	assert_gt(sk.get_bone_count(), 16, "the fixture contains retail's BN17 R Hand row")

	var deltas: Array = []
	for _i in range(9):
		deltas.append(Basis.IDENTITY)
	var normal: Array = sk.eval_pose_overlay(
			"anim_idle", 0.0, sk.get_overlay_classes(), deltas, "", 0.0, false)
	var mounted: Array = sk.eval_pose_overlay(
			"anim_idle", 0.0, sk.get_overlay_classes(), deltas, "", 0.0, true)
	var normal_hand: Transform3D = normal[16]
	var mounted_hand: Transform3D = mounted[16]
	assert_true(mounted_hand.basis.x.is_zero_approx()
			and mounted_hand.basis.y.is_zero_approx()
			and mounted_hand.basis.z.is_zero_approx(),
			"mounted BN17 has the zero-scale transform retail uses to clip the baked weapon")
	assert_true(mounted_hand.origin.is_equal_approx(normal_hand.origin),
			"the clip pose preserves BN17's sampled local joint")
	assert_true(normal_hand.is_equal_approx(sk.eval_pose("anim_idle", 0.0)[16]),
			"leaving the emplaced weapon restores BN17's authored pose")


func _char_model_at(data: NovaObjectData, skeletal: NovaSkeletalAnim,
		world_transform: Transform3D, collapse_right_hand: bool):
	var model = NovaObjectModelScript.new()
	add_child_autofree(model)
	model.set_skeletal_anim(skeletal)
	model.set_object_data(data)
	model.global_transform = world_transform
	model.play_body_clip_at("anim_idle", 0)
	if collapse_right_hand:
		model.set_right_hand_collapsed(true)
	return model


# CPU-evaluate the same linear skin matrices Godot submits for this Skeleton3D.
# MeshInstance3D.bake_mesh_from_current_skeleton_pose() requires a renderer-
# registered SkinReference that headless GUT does not create reliably, so this
# keeps the regression deterministic while still measuring deformed triangles,
# rather than inferring fidelity from one bone transform.
func _skinned_geometry_metrics(data: NovaObjectData, skeleton: Skeleton3D) -> Dictionary:
	var matrices: Array[Transform3D] = []
	for bone in range(skeleton.get_bone_count()):
		matrices.append(skeleton.global_transform
				* skeleton.get_bone_global_pose(bone)
				* skeleton.get_bone_global_rest(bone).affine_inverse())

	var bounds := AABB()
	var have_bounds := false
	var max_triangle_edge := 0.0
	var right_hand_vertices := 0
	for entry_var in data.build_lod_submeshes(0, true, skeleton.get_bone_count()):
		var entry: Dictionary = entry_var
		if not bool(entry.get("is_skinned", false)):
			continue
		var mesh := entry.get("mesh") as ArrayMesh
		if mesh == null:
			continue
		var arrays: Array = mesh.surface_get_arrays(0)
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var bones: PackedInt32Array = arrays[Mesh.ARRAY_BONES]
		var weights: PackedFloat32Array = arrays[Mesh.ARRAY_WEIGHTS]
		var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		var influences_per_vertex := int(bones.size()) / vertices.size()
		var skinned := PackedVector3Array()
		skinned.resize(vertices.size())
		for vertex_index in range(vertices.size()):
			var point := Vector3.ZERO
			var touches_right_hand := false
			for influence in range(influences_per_vertex):
				var at := vertex_index * influences_per_vertex + influence
				var weight := float(weights[at])
				if weight <= 0.0:
					continue
				point += (matrices[int(bones[at])] * vertices[vertex_index]) * weight
				touches_right_hand = touches_right_hand or int(bones[at]) == 16
			skinned[vertex_index] = point
			if touches_right_hand:
				right_hand_vertices += 1
			if have_bounds:
				bounds = bounds.expand(point)
			else:
				bounds = AABB(point, Vector3.ZERO)
				have_bounds = true

		var triangle_indices := indices
		if triangle_indices.is_empty():
			triangle_indices.resize(vertices.size())
			for vertex_index in range(vertices.size()):
				triangle_indices[vertex_index] = vertex_index
		for triangle in range(0, triangle_indices.size() - 2, 3):
			var a := skinned[int(triangle_indices[triangle])]
			var b := skinned[int(triangle_indices[triangle + 1])]
			var c := skinned[int(triangle_indices[triangle + 2])]
			max_triangle_edge = maxf(max_triangle_edge,
					maxf(a.distance_to(b), maxf(b.distance_to(c), c.distance_to(a))))

	return {
		"right_hand_vertices": right_hand_vertices,
		"span": bounds.size.length(),
		"max_triangle_edge": max_triangle_edge,
	}


func test_mounted_hand_collapse_does_not_stretch_skinned_triangles() -> void:
	var data := _open(CHARMODEL)
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/anim"))
	var skeletal := NovaSkeletalAnim.new()
	assert_true(skeletal.load_from_resource_root(root, "soldier.adm",
			data.get_bone_origins(), data.get_bone_parents()))
	var placement := Transform3D(
			Basis(Vector3.UP, 0.37), Vector3(7.0, 3.0, -11.0))
	# Keep the models separate: applying a zero-scale pose and restoring it on one
	# Skeleton3D can make the result depend on setter/update history.
	var authored_model = _char_model_at(data, skeletal, placement, false)
	var collapsed_model = _char_model_at(data, skeletal, placement, true)
	await get_tree().process_frame
	await get_tree().process_frame
	authored_model.get_skeleton().force_update_all_bone_transforms()
	collapsed_model.get_skeleton().force_update_all_bone_transforms()
	var authored := _skinned_geometry_metrics(data, authored_model.get_skeleton())
	var collapsed := _skinned_geometry_metrics(data, collapsed_model.get_skeleton())

	assert_gt(int(authored.right_hand_vertices), 0,
			"the fixture has vertices influenced by retail's BN17 R Hand row")
	assert_gt(float(authored.max_triangle_edge), 0.0,
			"the authored fixture has measurable triangle edges")
	assert_lte(float(collapsed.max_triangle_edge),
			float(authored.max_triangle_edge) * 1.1,
			"collapsing BN17 cannot stretch a triangle across the frame")
	assert_lte(float(collapsed.span), float(authored.span) * 1.1,
			"collapsing BN17 cannot balloon the character's skinned bounds")


func test_model_collapses_right_hand_at_joint_without_aim_overlay() -> void:
	# The evaluator's zero-scale BN17 pose is a clip marker, not a world-space
	# destination. Exercise NovaObjectModel's no-overlay fallback at a non-zero
	# placement: the host must collapse the bone at its current joint. Sending
	# partially weighted vertices to world origin stretches triangles across the
	# frame instead of clipping the baked weapon.
	var data := _open(CHARMODEL)
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/anim"))
	var sk := NovaSkeletalAnim.new()
	assert_true(sk.load_from_resource_root(root, "soldier.adm",
			data.get_bone_origins(), data.get_bone_parents()))
	var model = NovaObjectModelScript.new()
	add_child_autofree(model)
	model.set_skeletal_anim(sk)
	model.set_object_data(data)
	model.global_transform = Transform3D(
			Basis(Vector3.UP, 0.37), Vector3(7.0, 3.0, -11.0))
	model.play_body_clip_at("anim_idle", 0)
	await get_tree().process_frame
	await get_tree().process_frame
	var skeleton: Skeleton3D = model.get_skeleton()
	skeleton.force_update_all_bone_transforms()
	var authored_hand := (skeleton.global_transform
			* skeleton.get_bone_global_pose(16)
			* skeleton.get_bone_global_rest(16).affine_inverse())
	var authored_joint := (skeleton.global_transform
			* skeleton.get_bone_global_pose(16).origin)

	model.set_right_hand_collapsed(true)
	await get_tree().process_frame
	await get_tree().process_frame
	skeleton.force_update_all_bone_transforms()
	var collapsed_hand := (skeleton.global_transform
			* skeleton.get_bone_global_pose(16)
			* skeleton.get_bone_global_rest(16).affine_inverse())
	assert_true(collapsed_hand.basis.x.is_zero_approx()
			and collapsed_hand.basis.y.is_zero_approx()
			and collapsed_hand.basis.z.is_zero_approx(),
			"BN17 scale collapses even without overlay data")
	assert_true(collapsed_hand.origin.is_equal_approx(authored_joint),
			"BN17 collapses at the hand joint instead of dragging skin to world origin")

	model.set_right_hand_collapsed(false)
	await get_tree().process_frame
	await get_tree().process_frame
	skeleton.force_update_all_bone_transforms()
	var restored_hand := (skeleton.global_transform
			* skeleton.get_bone_global_pose(16)
			* skeleton.get_bone_global_rest(16).affine_inverse())
	assert_true(restored_hand.is_equal_approx(authored_hand),
			"dismount restores the authored hand transform after a joint-local collapse")


func _bone_poses(skel: Skeleton3D) -> Array:
	var out: Array = []
	for i in range(skel.get_bone_count()):
		out.append(Transform3D(Basis(skel.get_bone_pose_rotation(i)), skel.get_bone_pose_position(i)))
	return out


func test_scrub_while_paused_moves_playhead_and_pose() -> void:
	# The ANIMS workflow's scrub seam: set_animation_time poses the skeleton
	# IMMEDIATELY even while paused. SHED is rigid, so it fake-skins into a real
	# Skeleton3D headless (no render needed for bone poses).
	var model = NovaObjectModelScript.new()
	add_child_autofree(model)
	model.set_skeletal_anim(_loaded_skeletal())
	model.set_object_data(_open(SHED))
	assert_true(model.has_skeleton(), "the rigid model fake-skins into a Skeleton3D")
	model.set_playing(false)  # paused scrubbing is the point
	model.play_body_clip("anim_walk_forward")

	var sk = model.get_skeletal_anim()
	var mid: float = sk.get_clip_length("anim_walk_forward") * 0.5
	# The fixture clips carry root-motion data, not visually-moving bones, so
	# pin the IMMEDIATE re-pose by vandalizing a bone pose first: the scrub must
	# overwrite it with eval_pose's value without waiting for a frame tick.
	var skel: Skeleton3D = model.get_skeleton()
	skel.set_bone_pose_position(0, Vector3(123.0, 456.0, 789.0))
	model.set_animation_time(mid)
	assert_almost_eq(model.get_animation_time(), mid, 0.001, "the playhead followed the scrub")
	var expected: Transform3D = sk.eval_pose("anim_walk_forward", mid)[0]
	assert_ne(skel.get_bone_pose_position(0), Vector3(123.0, 456.0, 789.0),
		"a paused scrub re-posed the skeleton (no frame tick needed)")
	assert_true(skel.get_bone_pose_position(0).is_equal_approx(expected.origin),
		"...with eval_pose's transform at the scrubbed playhead")


func test_set_animation_time_wraps_or_clamps_per_clip() -> void:
	# Mirrors eval_pose's own branch (loop: fmod over the clip; one-shot: clamp)
	# so the stored playhead and the rendered pose can never disagree. Branch on
	# the fixture clip's REAL loop flag rather than assuming it.
	var model = NovaObjectModelScript.new()
	add_child_autofree(model)
	model.set_skeletal_anim(_loaded_skeletal())
	model.set_object_data(_open(SHED))
	model.play_body_clip("anim_walk_forward")
	var sk = model.get_skeletal_anim()
	var length: float = sk.get_clip_length("anim_walk_forward")
	assert_gt(length, 0.0, "the fixture clip has a length")

	model.set_animation_time(length + 0.25)
	if sk.is_clip_looping("anim_walk_forward"):
		assert_almost_eq(model.get_animation_time(), fposmod(length + 0.25, length), 0.001,
			"looping clips wrap past the end")
	else:
		assert_almost_eq(model.get_animation_time(), length, 0.001, "one-shots clamp at the end")

	model.set_animation_time(-0.5)
	if sk.is_clip_looping("anim_walk_forward"):
		assert_almost_eq(model.get_animation_time(), fposmod(-0.5, length), 0.001,
			"negative scrubs wrap from the end")
	else:
		assert_almost_eq(model.get_animation_time(), 0.0, 0.001, "one-shots clamp at zero")


func test_play_body_clip_is_idempotent_for_same_key() -> void:
	var model = NovaObjectModelScript.new()
	add_child_autofree(model)
	model.set_skeletal_anim(_loaded_skeletal())
	model.set_object_data(_open(SHED))
	model.play_body_clip("anim_walk_forward")
	model.set_animation_time(0.2)

	model.play_body_clip("anim_walk_forward")

	assert_almost_eq(model.get_animation_time(), 0.2, 0.001,
		"re-playing the active clip must not restart its playhead")


func test_play_body_clip_at_pins_ida_phase_ticks() -> void:
	var model = NovaObjectModelScript.new()
	add_child_autofree(model)
	model.set_skeletal_anim(_loaded_skeletal())
	model.set_object_data(_open(SHED))
	if not model.has_method("play_body_clip_at"):
		fail_test("NovaObjectModel exposes play_body_clip_at(key, phase_ticks)")
		return
	var sk = model.get_skeletal_anim()
	var fps: float = sk.get_clip_fps("anim_walk_forward")
	assert_gt(fps, 0.0, "fixture clip has a valid fps")
	var phase_ticks := 5
	var expected_time := float(phase_ticks) / (2.0 * fps)

	model.call("play_body_clip_at", "anim_walk_forward", phase_ticks)
	var skeleton: Skeleton3D = model.get_skeleton()
	assert_eq(skeleton.modifier_callback_mode_process,
			Skeleton3D.MODIFIER_CALLBACK_MODE_PROCESS_MANUAL,
			"direct-pose skeletons do not run an empty modifier callback every frame")
	var pinned_pose := skeleton.get_bone_pose_position(0)
	model.advance_body_animation(0.5)

	assert_almost_eq(model.get_animation_time(), expected_time, 0.001,
		"IDA half-frame phase ticks map to skeleton pose seconds")
	assert_true(skeleton.get_bone_pose_position(0).is_equal_approx(pinned_pose),
		"externally phased playback does not free-run between sim snapshots")


func test_play_body_clip_seeded_consumes_ticks_then_free_runs() -> void:
	var model = NovaObjectModelScript.new()
	add_child_autofree(model)
	model.set_skeletal_anim(_loaded_skeletal())
	model.set_object_data(_open(SHED))
	var fps: float = model.get_skeletal_anim().get_clip_fps("anim_walk_forward")
	var expected := 5.0 / (2.0 * fps)

	model.play_body_clip_seeded("anim_walk_forward", 5)
	assert_almost_eq(model.get_animation_time(), expected, 0.001,
		"the accepted player transition consumes its half-frame tick seed")
	model.advance_body_animation(0.05)
	assert_false(is_equal_approx(model.get_animation_time(), expected),
		"a seeded remote clip advances locally on the very next render frame")


func test_remote_body_same_state_does_not_rescrub_player_phase() -> void:
	var model = NovaObjectModelScript.new()
	add_child_autofree(model)
	model.set_skeletal_anim(_loaded_skeletal())
	model.set_object_data(_open(SHED))
	var flags := int(NovaSimulation.infantry_anim_flags(1))

	model.apply_remote_body_state(1, "anim_walk_forward", flags, 10)
	model.advance_body_animation(0.05)
	var locally_advanced := model.get_animation_time()
	model.apply_remote_body_state(1, "anim_walk_forward", flags, 22)
	assert_almost_eq(model.get_animation_time(), locally_advanced, 0.001,
		"steady-state off15 samples do not hard-scrub a free-running channel")


func test_remote_body_locked_state_promotes_pending_at_tick_zero() -> void:
	var model = NovaObjectModelScript.new()
	add_child_autofree(model)
	model.set_skeletal_anim(_loaded_skeletal())
	model.set_object_data(_open(SHED))
	var locked_flags := int(NovaSimulation.infantry_anim_flags(41))
	assert_ne(locked_flags & 0x4, 0, "state 41 is transition-locked in the retail table")

	model.apply_remote_body_state(41, "anim_idle", locked_flags, 0)
	model.apply_remote_body_state(1, "anim_walk_forward",
			int(NovaSimulation.infantry_anim_flags(1)), 200)
	assert_eq(model.get_active_body_clip(), "anim_idle",
		"a locked current clip defers the incoming wire request")
	var current_length: float = model.get_skeletal_anim().get_clip_length("anim_idle")
	assert_gt(current_length, 0.0)
	model.advance_body_animation(current_length + 0.01)
	assert_eq(model.get_active_body_clip(), "anim_walk_forward",
		"the queued request promotes when the current clip completes")
	assert_almost_eq(model.get_animation_time(), 0.0, 0.001,
		"queued off15 belongs to the old channel; promotion starts at tick zero")


func test_remote_body_exit_gate_accepts_only_incoming_flag_one() -> void:
	var model = NovaObjectModelScript.new()
	add_child_autofree(model)
	model.set_skeletal_anim(_loaded_skeletal())
	model.set_object_data(_open(SHED))
	var exit_flags := int(NovaSimulation.infantry_anim_flags(115))
	var blocked_flags := int(NovaSimulation.infantry_anim_flags(43))
	var interrupt_flags := int(NovaSimulation.infantry_anim_flags(1))
	assert_ne(exit_flags & 0x20, 0, "state 115 uses the retail exit gate")
	assert_eq(blocked_flags & 0x1, 0)
	assert_ne(interrupt_flags & 0x1, 0)

	model.apply_remote_body_state(115, "anim_idle", exit_flags, 0)
	model.apply_remote_body_state(43, "anim_walk_forward", blocked_flags, 90)
	assert_eq(model.get_active_body_clip(), "anim_idle",
		"an incoming state without flag 0x1 queues behind an exit-gated clip")
	var fps: float = model.get_skeletal_anim().get_clip_fps("anim_run_forward")
	var expected := 6.0 / (2.0 * fps)
	model.apply_remote_body_state(1, "anim_run_forward", interrupt_flags, 6)
	assert_eq(model.get_active_body_clip(), "anim_run_forward",
		"incoming flag 0x1 interrupts an exit-gated current state")
	assert_almost_eq(model.get_animation_time(), expected, 0.001,
		"an immediately accepted interrupt consumes its own player phase seed")


func test_skinned_model_reports_nonzero_bounds() -> void:
	# Skinned (and rigid-fake-skinned) submeshes hang under the Skeleton3D, not the Robj part
	# nodes. get_model_bounds() must still report real bounds for a fully-skinned model, or any
	# bounds consumer (e.g. the avatar menu-portrait framing) sees an empty AABB and never frames.
	var model = NovaObjectModelScript.new()
	add_child_autofree(model)
	model.set_skeletal_anim(_loaded_skeletal())
	model.set_object_data(_open(SHED))
	assert_true(model.has_skeleton(), "the rigid model fake-skins into a Skeleton3D")
	await get_tree().process_frame
	var bounds: AABB = model.get_model_bounds()
	assert_gt(bounds.size.length(), 0.0, "a fully-skinned model still reports non-zero bounds")


func test_scrub_no_ops_without_skeletal_or_clip() -> void:
	var model = NovaObjectModelScript.new()
	add_child_autofree(model)
	model.set_animation_time(1.0)  # no skeletal set: must not crash
	assert_eq(model.get_animation_time(), 0.0, "no skeletal -> playhead reads 0")
	model.set_skeletal_anim(_loaded_skeletal())
	model.set_object_data(_open(SHED))
	model.set_animation_time(1.0)  # skeletal set, but no ACTIVE clip
	assert_eq(model.get_animation_time(), 0.0, "no active clip -> scrub is a no-op")


func test_object_preview_arms_overlay() -> void:
	# Public-API check for the arms overlay (object_preview.load_arms/clear_arms). No .adm is needed
	# -- with none loaded the arms render static at rest, a valid loaded state. Uses the committed
	# CharModel fixture mounted as a resource root.
	var preview := ObjectPreview.new()
	add_child_autofree(preview)
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/threedi/3di3"))
	# Failure path: no resource root -> false + error, no arms model.
	assert_false(preview.load_arms("CharModel.3di", null), "load_arms with null root fails")
	assert_ne(preview.get_arms_error(), "", "arms error reported on failure")
	assert_false(preview.has_arms(), "no arms model after a failed load")
	# Success path: a second model node is created.
	assert_true(preview.load_arms("CharModel.3di", root), "load_arms: %s" % preview.get_arms_error())
	assert_true(preview.has_arms(), "arms model exists after load")
	# Clear removes it.
	preview.clear_arms()
	assert_false(preview.has_arms(), "arms model removed after clear")


func test_multi_clip_adm_rows_register_variants() -> void:
	# Multi-clip .adm rows: every quoted token registers a VARIANT of the same key in
	# file order — the original's per-slot circular ring; the CURSOR lives with the
	# FSM owner (NovaSimulation), so this surface is peek-only data.
	# [orig: AnimMap_ParseConfigLine @0x40cb60; AnimMap_RegisterBoneNode @0x40c2d0]
	var dir := ProjectSettings.globalize_path("res://.godot/skeletal_variants_test")
	if not DirAccess.dir_exists_absolute(dir):
		assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	for bad in ["idle.bad", "walk.bad"]:
		var out := FileAccess.open(dir.path_join(bad), FileAccess.WRITE)
		assert_not_null(out)
		out.store_buffer(FileAccess.get_file_as_bytes("res://../fixtures/anim/" + bad))
		out.close()
	var adm := FileAccess.open(dir.path_join("variants.adm"), FileAccess.WRITE)
	assert_not_null(adm)
	adm.store_string("
anim_reset				\"idle.bad\"
"
		+ "anim_wpn_idle				\"idle.bad\"
"
		+ "anim_wpn_reload				\"walk.bad\" \"walk.bad\" \"idle.bad\"
")
	adm.close()

	var sk := NovaSkeletalAnim.new()
	var root := NovaResourceRoot.new()
	root.set_root_dir(dir)
	assert_true(sk.load_from_resource_root(root, "variants.adm"),
		"variants.adm loads: %s" % sk.get_last_error())
	assert_eq(sk.get_clip_variant_count("anim_wpn_reload"), 3,
		"the triple row registered three variants (duplication = rotation weighting)")
	assert_eq(sk.get_clip_variant_count("anim_wpn_idle"), 1, "single rows stay single")
	var lengths: PackedFloat32Array = sk.get_clip_variant_lengths("anim_wpn_reload")
	assert_eq(lengths.size(), 3, "one ring length per variant, file order")
	assert_almost_eq(float(lengths[0]), sk.get_clip_length("anim_wpn_reload", 0), 0.0001)
	assert_almost_eq(float(lengths[2]), sk.get_clip_length("anim_wpn_reload", 2), 0.0001)
	# Out-of-range variants wrap modulo the count (per-part load divergence guard).
	assert_almost_eq(sk.get_clip_length("anim_wpn_reload", 5),
		sk.get_clip_length("anim_wpn_reload", 2), 0.0001, "variant 5 wraps to 5 %% 3")
	assert_eq(sk.eval_pose("anim_wpn_reload", 0.0, 2).size(), sk.get_bone_count(),
		"variant-aware eval poses every bone")

	# The model's variant latch: play_body_clip_variant re-poses on a variant change
	# and resumes on the same key+variant (mirrors play_body_clip's same-key resume).
	var model = NovaObjectModelScript.new()
	add_child_autofree(model)
	model.set_skeletal_anim(sk)
	model.play_body_clip_variant("anim_wpn_reload", 2)
	assert_eq(model.get_active_body_clip(), "anim_wpn_reload")
	model.set_animation_time(0.05)
	var t_before: float = model.get_animation_time()
	model.play_body_clip_variant("anim_wpn_reload", 2)  # same variant: resume
	assert_almost_eq(model.get_animation_time(), t_before, 0.0001,
		"a same-key same-variant replay resumes (no playhead reset)")
	model.play_body_clip_variant("anim_wpn_reload", 0)  # variant change: restart
	assert_eq(model.get_animation_time(), 0.0, "a variant change restarts the clip")

	for name in ["idle.bad", "walk.bad", "variants.adm"]:
		DirAccess.remove_absolute(dir.path_join(name))
	DirAccess.remove_absolute(dir)
