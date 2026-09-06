extends GutTest

# The .3di -> scene projection over the minted synthetic set
# (fixtures/threedi/synth, fixtures/README.md): the tree carries the scene
# naming contract's names, the manifest carries what nodes cannot spell, and
# a construct outside the scene form is refused by name, never dropped.

const SYNTH := "res://../fixtures/threedi/synth"

var _roots: Array[Node3D] = []


func after_each() -> void:
	for root in _roots:
		root.free()
	_roots.clear()


func _load(name: String) -> ModelDocument:
	var document := ModelDocument.new()
	assert_eq(document.load_from_path(SYNTH.path_join(name)), OK, document.get_last_error())
	return document


func _project(name: String, manifest: ModelAuthoringManifest) -> Node3D:
	var projector := ModelSceneProjector.new()
	var root := projector.project(_load(name), manifest, ProjectSettings.globalize_path(SYNTH))
	assert_not_null(root, "%s: %s" % [name, projector.get_last_error()])
	if root != null:
		_roots.append(root)
	return root


func test_house_projects_parts_meshes_collision_and_manifest() -> void:
	var manifest := ModelAuthoringManifest.new()
	var root := _project("house.3di", manifest)
	if root == null:
		return
	assert_eq(manifest.model_name, "house")
	assert_false(manifest.skinned)
	assert_eq(manifest.lods.size(), 1)
	assert_eq(manifest.materials.size(), 3, "three FF_ST_OP materials")
	for material in manifest.materials:
		assert_eq((material as ModelMaterialSpec).shader_tag, "FF_ST_OP")
		assert_false((material as ModelMaterialSpec).alpha_strips)
	assert_eq(manifest.expected_bone_rows, 0)
	var lod := root.get_node("LOD0") as Node3D
	assert_not_null(lod)
	var part := lod.get_node("PN01") as Node3D
	assert_not_null(part, "one inert part")
	var mesh := part.get_node("01 Mesh0") as MeshInstance3D
	assert_not_null(mesh)
	assert_eq(mesh.mesh.get_surface_count(), 3, "one surface per strip")
	assert_eq(mesh.mesh.surface_get_name(0), (manifest.materials[0] as ModelMaterialSpec).material_name)
	var collision := root.get_node("Collision") as Node3D
	assert_not_null(collision)
	var section := collision.get_node("CO01") as ModelCollisionSection3D
	assert_not_null(section)
	assert_false(section.sphere)
	assert_not_null(section.get_node("CO01 faces"), "the face box")
	var volumes := 0
	var prism_planes := 0
	for child in section.get_children():
		if child is ModelBoundingVolume3D:
			volumes += 1
			assert_true(String(child.name).ends_with("-colonly"))
			if (child as ModelBoundingVolume3D).planes.size() == 10:
				prism_planes += 1
	assert_eq(volumes, 4, "three CB boxes and the octagonal prism")
	assert_eq(prism_planes, 1, "the prism carries its ten planes")


func test_person_projects_a_skeleton_in_the_retail_rig_order() -> void:
	var manifest := ModelAuthoringManifest.new()
	var root := _project("person.3di", manifest)
	if root == null:
		return
	assert_true(manifest.skinned)
	assert_eq(manifest.expected_bone_rows, 19)
	var skeleton := root.get_node("LOD0/Skeleton3D") as Skeleton3D
	assert_not_null(skeleton)
	assert_eq(skeleton.get_bone_count(), 19)
	assert_eq(skeleton.get_bone_name(0), "BN01")
	assert_eq(skeleton.get_bone_name(14), "BN15", "the head row")
	assert_eq(skeleton.get_bone_parent(0), -1, "the root bone")
	assert_eq(skeleton.get_bone_parent(14), 13, "the head hangs off the neck")
	var skinned_meshes := 0
	for child in skeleton.get_children():
		if child is MeshInstance3D:
			skinned_meshes += 1
			assert_not_null((child as MeshInstance3D).skin)
	assert_true(skinned_meshes > 0, "the skinned strips ride the skeleton")
	var head := root.get_node("Collision/CO15") as ModelCollisionSection3D
	assert_not_null(head)
	assert_true(head.sphere, "the head is a bone sphere")
	assert_almost_eq(head.sphere_radius, 5.0 / 32.0, 0.0001)
	var points := root.get_node("UserPoints") as Node3D
	assert_not_null(points)
	var labels: Array[String] = []
	for child in points.get_children():
		labels.append((child as ModelUserPoint3D).label)
	assert_true("LOOK" in labels)
	assert_true("MFlash01" in labels)


func test_shed_projects_its_light_alpha_strip_and_register() -> void:
	var manifest := ModelAuthoringManifest.new()
	var root := _project("shed.3di", manifest)
	if root == null:
		return
	assert_eq(Array(manifest.control_registers), ["FLICKER"])
	var alpha_materials := 0
	for material in manifest.materials:
		if (material as ModelMaterialSpec).alpha_strips:
			alpha_materials += 1
	assert_eq(alpha_materials, 1, "the bulb strip's material sorts into the alpha bucket")
	var light := root.get_node("Lights/LP01") as ModelLight3D
	assert_not_null(light)
	assert_eq(light.style, 24)
	assert_almost_eq(light.atten_end, 3.0, 0.0001)


func test_pump_projects_two_lods_with_their_panm_rows() -> void:
	var manifest := ModelAuthoringManifest.new()
	var root := _project("pump.3di", manifest)
	if root == null:
		return
	assert_eq(manifest.lods.size(), 2)
	assert_not_null(root.get_node("LOD0"))
	assert_not_null(root.get_node("LOD1"))
	var lod0 := manifest.lods[0] as ModelLodSpec
	assert_eq(lod0.part_animations.size(), 5, "one PANM row per part")
	var beam := lod0.part_animations[1] as ModelPartAnimationRow
	assert_eq(beam.rotation_z[0], 50, "the beam's free-running sine on rotation z")


func test_occlusion_is_refused_by_name() -> void:
	var manifest := ModelAuthoringManifest.new()
	var projector := ModelSceneProjector.new()
	var root := projector.project(_load("armory.3di"), manifest, "")
	assert_null(root)
	var refusals := projector.get_refusals()
	assert_true(refusals.size() >= 1)
	var named := false
	for refusal in refusals:
		if refusal.begins_with("occlusion records"):
			named = true
	assert_true(named, "the refusal names the OCCL records: %s" % [refusals])
	if root != null:
		root.free()
