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
	var pinned_pose := model.get_skeleton().get_bone_pose_position(0)
	model._process(0.5)

	assert_almost_eq(model.get_animation_time(), expected_time, 0.001,
		"IDA half-frame phase ticks map to skeleton pose seconds")
	assert_true(model.get_skeleton().get_bone_pose_position(0).is_equal_approx(pinned_pose),
		"externally phased playback does not free-run between sim snapshots")


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
