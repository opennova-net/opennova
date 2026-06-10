extends GutTest

# Skeletal-animation runtime (.bad/.adm) tests:
#  - skin plumbing: build_lod_submeshes emits ARRAY_BONES/ARRAY_WEIGHTS for skinned models
#    and nothing extra for static ones (regression-safe).
#  - NovaSkeletalAnim resource basics (graceful failure, empty state).
#  - NovaObjectModel's new skeletal methods parse and no-op safely without a skeletal set.

const CHARMODEL := "res://../fixtures/threedi/3di3/CharModel.3di"
const SHED := "res://../fixtures/threedi/3di3/Shed.3di"
const NovaObjectModelScript = preload("res://engine/object/nova_object_model.gd")


func _open(path: String) -> NovaObjectData:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(path)), OK, "fixture should open: %s" % path)
	return data


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
