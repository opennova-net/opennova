extends GutTest

# A surface whose material runs one of retail's skinned vertex programs is
# posed by the object shaders from its model's bone palette (skin.gdshaderinc),
# not by Godot's skinning: the lit programs light the vertex's first bone
# frame while they blend its position over four (renderer::ObjectSkinNormal).
# The model publishes the palette whenever its Skeleton3D settles a pose and
# gives those surfaces a culling box from the posed per-bone bind boxes.
# [orig: CRenderBatchQueue_FlushBatches @ 0x5DA170 (palette entry k is the
#  strip's bone-table entry k), _BaseInc.fx CalcSkinWorldPosAndNormal]

const PERSON := "res://../fixtures/threedi/synth/person.3di"
const ANIM := "res://../fixtures/anim"


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


# The drawn surface instances hanging under the model's skeleton.
func _skinned_instances(model: ObjectModel) -> Array[MeshInstance3D]:
	var out: Array[MeshInstance3D] = []
	var skeleton: Skeleton3D = model.get_skeleton()
	for child in skeleton.get_children():
		var instance := child as MeshInstance3D
		if instance != null and instance.visible and instance.mesh != null:
			out.append(instance)
	return out


# The palette the object shaders read: bone pose x skin bind (the skin binds
# the rest pose's inverse, Skeleton3D.create_skin_from_rest_transforms).
func _expected_palette(skeleton: Skeleton3D) -> Array[Transform3D]:
	var out: Array[Transform3D] = []
	for bone in skeleton.get_bone_count():
		out.append(skeleton.get_bone_global_pose(bone)
				* skeleton.get_bone_global_rest(bone).affine_inverse())
	return out


func _assert_palette_is_the_pose(model: ObjectModel, message: String) -> void:
	var palette: Array = model.get_skin_palette()
	var expected := _expected_palette(model.get_skeleton())
	assert_eq(palette.size(), expected.size(), message + ": one row per bone")
	for bone in mini(palette.size(), expected.size()):
		assert_true((palette[bone] as Transform3D).is_equal_approx(expected[bone]),
				"%s: bone %d is its pose x bind" % [message, bone])


func test_skinned_effect_surfaces_ride_the_model_palette() -> void:
	var model := _person_model()
	assert_true(model.has_skeleton(), "the .adm builds the rig")
	var instances := _skinned_instances(model)
	assert_gt(instances.size(), 0, "the person draws skinned strips")
	for instance in instances:
		assert_eq(instance.skeleton, NodePath(),
				"%s is not handed to Godot's skinning" % instance.name)
		var material := instance.material_override as ShaderMaterial
		assert_not_null(material, "%s carries its object material" % instance.name)
		if material == null:
			continue
		assert_true(bool(material.get_shader_parameter("u_skin_palette_bound")),
				"%s's material reads the model palette" % instance.name)
		assert_true(material.get_shader_parameter("u_skin_palette") is Texture2D,
				"%s's material binds the palette texture" % instance.name)
		# Each vertex's light fallbacks (the table entries before its first,
		# prepare_model_mesh) ride CUSTOM0 as rig parts, 255 for none.
		assert_ne(instance.mesh.surface_get_format(0) & Mesh.ARRAY_FORMAT_CUSTOM0, 0,
				"%s carries its light fallback channel" % instance.name)
		var fallbacks: PackedByteArray = instance.mesh.surface_get_arrays(0)[Mesh.ARRAY_CUSTOM0]
		var vertices: PackedVector3Array = instance.mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
		assert_eq(fallbacks.size(), vertices.size() * 4,
				"%s: four fallback bytes per vertex" % instance.name)
		for part in fallbacks:
			if part != 255 and part >= model.get_skeleton().get_bone_count():
				fail_test("%s: fallback %d names no rig part" % [instance.name, part])
				break
	_assert_palette_is_the_pose(model, "the built rig")


# Any writer poses the skeleton; the palette settles with the skeleton's own
# deferred update. The spine and an upper arm turn well off the bind pose.
func _bend(model: ObjectModel) -> void:
	var skeleton: Skeleton3D = model.get_skeleton()
	skeleton.set_bone_pose_rotation(2, Quaternion(Vector3.RIGHT, deg_to_rad(40.0)))
	skeleton.set_bone_pose_rotation(9, Quaternion(Vector3.FORWARD, deg_to_rad(90.0)))


func test_the_palette_follows_every_settled_pose() -> void:
	var model := _person_model()
	_bend(model)
	await get_tree().process_frame
	_assert_palette_is_the_pose(model, "a bent pose")
	var palette: Array = model.get_skin_palette()
	assert_false((palette[9] as Transform3D).is_equal_approx(Transform3D.IDENTITY),
			"the bent arm moves its palette row off the bind pose")


# The image the palette texture uploads holds what the object shaders read:
# per bone one row of three RGBAF texels, each a row of the bone's matrix with
# its origin in the fourth channel (skin.gdshaderinc obj_skin_rows). The
# model's own image, since a headless renderer keeps no texture update to
# read back.
func test_the_palette_texture_holds_the_settled_pose() -> void:
	var model := _person_model()
	_bend(model)
	await get_tree().process_frame
	var image: Image = model.get_skin_palette_image()
	assert_not_null(image, "the model publishes its palette image")
	if image == null:
		return
	assert_eq(image.get_format(), Image.FORMAT_RGBAF, "32-bit float texels")
	var expected := _expected_palette(model.get_skeleton())
	assert_eq(image.get_width(), 3, "three texels a bone")
	assert_eq(image.get_height(), expected.size(), "one row per bone")
	for bone in mini(image.get_height(), expected.size()):
		var matrix := expected[bone]
		for row in 3:
			var want := Color(matrix.basis.x[row], matrix.basis.y[row], matrix.basis.z[row],
					matrix.origin[row])
			var got := image.get_pixel(row, bone)
			if not got.is_equal_approx(want):
				fail_test("bone %d row %d holds %s, not %s" % [bone, row, got, want])
				return
	pass_test("every palette row is its bone's settled matrix")


func test_the_culling_box_covers_the_posed_skin() -> void:
	var model := _person_model()
	_bend(model)
	await get_tree().process_frame
	var palette: Array = model.get_skin_palette()
	for instance in _skinned_instances(model):
		var box: AABB = instance.custom_aabb
		assert_true(box.has_volume(), "%s carries a posed culling box" % instance.name)
		var arrays: Array = instance.mesh.surface_get_arrays(0)
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var bones: PackedInt32Array = arrays[Mesh.ARRAY_BONES]
		var weights: PackedFloat32Array = arrays[Mesh.ARRAY_WEIGHTS]
		var grown := box.grow(0.001)
		for i in vertices.size():
			# The vertex program's blend: three stored weights and the rest on
			# index byte 3.
			var w0 := weights[i * 4]
			var w1 := weights[i * 4 + 1]
			var w2 := weights[i * 4 + 2]
			var p := (palette[bones[i * 4]] as Transform3D) * vertices[i] * w0 \
					+ (palette[bones[i * 4 + 1]] as Transform3D) * vertices[i] * w1 \
					+ (palette[bones[i * 4 + 2]] as Transform3D) * vertices[i] * w2 \
					+ (palette[bones[i * 4 + 3]] as Transform3D) * vertices[i] \
							* (1.0 - (w0 + w1 + w2))
			if not grown.has_point(p):
				fail_test("%s: posed vertex %d %s escapes its culling box %s" % [
						instance.name, i, p, box])
				return
	pass_test("every posed vertex lies in its surface's culling box")
