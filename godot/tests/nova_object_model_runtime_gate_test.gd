extends GutTest

# The per-model _process residual fix: clock-DERIVED render work (material
# generators, PANM transforms, lights, the env push) runs only while the model
# can render, while time-ACCUMULATING state (commanded part anims, the skeletal
# clip clock) advances regardless — a door commanded open while culled is open
# when next seen, and every skipped value re-derives from the absolute clock on
# the next visible frame. Retail computes these constants per SUBMITTED model
# only [orig: Terrain_RenderSectorModels @ 0x5c5d30].

const HOUSE_3DI := "res://../fixtures/threedi/3di3/House.3di"
const PMP_3DI := "res://../fixtures/3dp/Pmpjk01/Pmpjk01.3di"
const ARMRY_3DI := "res://../fixtures/3dp/armry01/Armry01.3di"
const SHED_3DI := "res://../fixtures/threedi/3di3/Shed.3di"
const ANIM_FIXTURES := "res://../fixtures/anim"


class GateSpyModel:
	extends NovaObjectModel
	var regs: Array = ["VEHICLE_SPECIAL1"]
	var env_applies := 0
	var light_applies := 0
	var robj_applies := 0

	func reset_observations() -> void:
		env_applies = 0
		light_applies = 0
		robj_applies = 0

	func _resolve_anim_channel_register(slot: int) -> String:
		return String(regs[slot]) if slot >= 0 and slot < regs.size() else ""

	func _apply_environment_to_materials() -> void:
		env_applies += 1

	func _apply_lights() -> void:
		light_applies += 1

	func _apply_robj_transforms() -> bool:
		robj_applies += 1
		return false


func _object_data(path: String) -> NovaObjectData:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(path)), OK,
			"the committed object fixture opens: %s" % path)
	return data


# Spy model with a real document installed through the production mutator.
# Rebuild itself exercises the observed methods, so reset those setup calls
# before measuring an explicit runtime frame.
func _spy_model(path := HOUSE_3DI) -> GateSpyModel:
	var model := GateSpyModel.new()
	add_child_autofree(model)
	model.set_process(false)
	model.set_object_data(_object_data(path))
	model.reset_observations()
	return model


func _loaded_skeletal() -> NovaSkeletalAnim:
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(ANIM_FIXTURES)), OK,
			"the committed animation fixture root mounts")
	var skeletal := NovaSkeletalAnim.new()
	assert_true(skeletal.load_from_resource_root(root, "soldier.adm"),
			"soldier.adm loads: %s" % skeletal.get_last_error())
	return skeletal


func test_hidden_model_skips_render_work_but_advances_part_anims() -> void:
	var model := _spy_model()
	model.play_part_anim(1, 1, 1.0)
	model.visible = false

	model.advance_runtime_frame(0.5)
	assert_eq(model.light_applies, 0, "a hidden model pushes no light state")
	assert_eq(model.env_applies, 0, "a hidden model pushes no environment state")
	assert_eq(int(model.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 31 * 1048,
			"the commanded part anim still advanced while hidden")

	model.visible = true
	model.advance_runtime_frame(0.5)
	assert_eq(model.light_applies, 1, "render work resumes on the visible frame")
	assert_eq(model.env_applies, 1)
	assert_eq(int(model.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 62 * 1048,
			"two half-second render frames preserve retail's fixed 16 ms tick count")
	model.advance_runtime_frame(0.008)
	assert_eq(int(model.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 65536,
			"the 63rd retail tick strictly overshoots and clamps the sweep endpoint")


func test_hidden_fast_path_skips_the_env_push() -> void:
	var model := _spy_model()  # no lights/panm/materials -> the fast path
	model.visible = false
	model.advance_runtime_frame(0.016)
	assert_eq(model.env_applies, 0, "hidden fast-path frames do nothing")
	model.visible = true
	model.advance_runtime_frame(0.016)
	assert_eq(model.env_applies, 1, "a visible fast-path frame keeps the env gate")


func test_live_panm_transforms_rederive_on_the_visible_frame() -> void:
	var model := _spy_model(PMP_3DI)
	model.visible = false
	model.advance_runtime_frame(0.016)
	model.advance_runtime_frame(0.016)
	assert_eq(model.robj_applies, 0, "no PANM evaluation while hidden")
	model.visible = true
	model.advance_runtime_frame(0.016)
	assert_eq(model.robj_applies, 1,
			"the visible frame re-derives transforms from the absolute clock")


func test_retail_runtime_does_not_submit_model_authored_lght() -> void:
	# Jointops loads LGHT into the model resource but has no gameplay read of
	# that field after load. Runtime models therefore keep the shader's
	# count-zero defaults even when the source file carries authored lights.
	var model := NovaObjectModel.new()
	add_child_autofree(model)
	model.set_process(false)
	var data := _object_data(ARMRY_3DI)
	assert_gt(data.get_light_count(), 0, "the fixture carries object lights")
	model.set_object_data(data)
	var materials: Array = model.get_surface_materials()
	if materials.is_empty():
		pass_test("fixture built no surface materials under this renderer")
		return
	var material := materials[0] as ShaderMaterial
	assert_eq(int(material.get_shader_parameter("u_local_light_count")), 0,
			"gameplay parity leaves parsed LGHT disabled")
	model.advance_runtime_frame(0.0)
	assert_eq(int(material.get_shader_parameter("u_local_light_count")), 0,
			"runtime frames do not inject parsed LGHT")


func test_explicit_editor_preview_can_show_model_authored_lght() -> void:
	var model := NovaObjectModel.new()
	add_child_autofree(model)
	model.set_process(false)
	model.position = Vector3(11.0, 13.0, 17.0)
	model.set_model_light_preview_enabled(true)
	var data := _object_data(ARMRY_3DI)
	for light_index in range(data.get_light_count()):
		assert_true(data.set_light_field(light_index, "subobject", -1))
		assert_true(data.set_light_field(light_index, "position", Vector3(4.0, 5.0, 6.0)))
	model.set_object_data(data)
	var materials: Array = model.get_surface_materials()
	if materials.is_empty():
		pass_test("fixture built no surface materials under this renderer")
		return
	var material := materials[0] as ShaderMaterial
	assert_eq(int(material.get_shader_parameter("u_local_light_count")), 1,
			"the opt-in object-editor preview can inspect authored LGHT")
	var lights: Array = data.evaluate_lights(0, {})
	var dominant: Dictionary = {}
	var best_intensity := -1.0
	for raw_light in lights:
		var light: Dictionary = raw_light
		var intensity := float(light.get("intensity", 1.0))
		if intensity > best_intensity:
			best_intensity = intensity
			dominant = light
	var model_position: Vector3 = dominant.get("position", Vector3.ZERO)
	var subobject := int(dominant.get("subobject", -1))
	var expected_world := model.global_transform * model_position
	var part_nodes: Dictionary = model.get_render_part_nodes()
	if subobject >= 0 and part_nodes.has(subobject):
		var rest := Transform3D.IDENTITY
		for raw_submesh in data.build_lod_submeshes(model.get_active_lod()):
			var submesh: Dictionary = raw_submesh
			if int(submesh.get("robj_index", -1)) == subobject:
				rest.origin = submesh.get("abs", Vector3.ZERO)
				break
		var part := part_nodes[subobject] as Node3D
		expected_world = part.global_transform * (rest.affine_inverse() * model_position)
	assert_eq(material.get_shader_parameter("u_local_light_position"), expected_world,
			"preview maps model-space LGHT through its attached part's live transform once")
	material.set_shader_parameter("u_local_light_count", 99)
	model.advance_runtime_frame(0.0)
	assert_eq(int(material.get_shader_parameter("u_local_light_count")), 99,
			"an identical preview evaluation pushes nothing")


func _mesh_instances_below(root: Node) -> Array[MeshInstance3D]:
	var out: Array[MeshInstance3D] = []
	for child in root.get_children():
		if child is MeshInstance3D:
			out.append(child as MeshInstance3D)
		out.append_array(_mesh_instances_below(child))
	return out


func test_world_model_shadow_casting_is_explicit_and_receiving_stays_enabled() -> void:
	# Retail's offscreen silhouette pass admits people and DynamicShadow items,
	# never every loaded model. A NovaObjectModel therefore starts receiver-only;
	# the item-aware placer explicitly opts eligible entities into casting.
	# [orig: Entity_InitFromModel @0x40E1BC..0x40E1F7]
	var model := NovaObjectModel.new()
	add_child_autofree(model)
	model.set_process(false)
	model.set_object_data(_object_data(HOUSE_3DI))
	var meshes := _mesh_instances_below(model)
	assert_gt(meshes.size(), 0, "the fixture builds renderable mesh instances")
	for mesh in meshes:
		assert_eq(mesh.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF,
				"ordinary/static models cannot enter the retail dynamic-shadow pass")

	var materials: Array = model.get_surface_materials()
	assert_gt(materials.size(), 0, "the fixture builds object materials")
	for material in materials:
		var receiver := (material as ShaderMaterial).next_pass as ShaderMaterial
		assert_not_null(receiver,
				"world models receive eligible entity silhouettes in a separate pass")
		if receiver != null:
			assert_true(receiver.shader.code.contains("1.0 - ATTENUATION"),
					"the next pass consumes shadow attenuation only")

	model.set_shadow_caster_enabled(true)
	for mesh in meshes:
		assert_eq(mesh.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_ON,
				"an eligible entity explicitly enters the silhouette pass")
		assert_ne(mesh.layers & NovaWater.VISUAL_LAYER_DYNAMIC_SHADOW_CASTER, 0,
				"the dynamic light can select the caster independently of receivers")
		assert_eq(mesh.layers & NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER, 0)
	model.set_static_shadow_caster_enabled(true)
	for mesh in meshes:
		assert_ne(mesh.layers & NovaWater.VISUAL_LAYER_DYNAMIC_SHADOW_CASTER, 0,
				"an item can participate in both witnessed projection systems")
		assert_ne(mesh.layers & NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER, 0,
				"static terrain projection uses its own caster layer")
	model.set_static_shadow_caster_enabled(false)
	model.set_shadow_caster_enabled(false)
	for mesh in meshes:
		assert_eq(mesh.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF,
				"the policy remains live across an item/presentation change")
		assert_eq(mesh.layers & NovaWater.VISUAL_LAYER_SHADOW_CASTER_MASK, 0)


func test_projected_shadow_receiver_rejects_incomplete_material_coverage() -> void:
	assert_true(NovaObjectModel.material_supports_projected_shadow_receiver(
			NovaObjectShaderCache.BLEND_OPAQUE, 0),
			"a one-sided opaque surface can use the simple attenuation catcher")
	assert_false(NovaObjectModel.material_supports_projected_shadow_receiver(
			NovaObjectShaderCache.BLEND_ALPHA, 0),
			"an alpha-blind next pass must not darken a transparent polygon")
	assert_false(NovaObjectModel.material_supports_projected_shadow_receiver(
			NovaObjectShaderCache.BLEND_OPAQUE,
			NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_TEST),
			"alpha-tested holes must not become a solid shadow card")
	assert_false(NovaObjectModel.material_supports_projected_shadow_receiver(
			NovaObjectShaderCache.BLEND_OPAQUE,
			NovaObjectShaderCache.MATERIAL_FLAG_TWO_SIDED),
			"the one-sided catcher cannot safely cover a two-sided base surface")


func test_hidden_skeletal_clock_advances_without_writing_bones() -> void:
	var model := NovaObjectModel.new()
	add_child_autofree(model)
	model.set_process(false)
	model.set_skeletal_anim(_loaded_skeletal())
	model.set_object_data(_object_data(SHED_3DI))
	assert_true(model.has_skeleton(), "the rigid fixture fake-skins into a skeleton")
	model.play_body_clip("anim_walk_forward")
	model.set_playing(true)
	model.advance_runtime_frame(0.0)

	var skeleton := model.get_skeleton()
	var poison := Vector3(123.0, 456.0, 789.0)
	skeleton.set_bone_pose_position(0, poison)
	model.visible = false
	model.advance_runtime_frame(0.1)

	assert_true(skeleton.get_bone_pose_position(0).is_equal_approx(poison),
			"a hidden frame leaves the submitted pose untouched")
	assert_almost_eq(model.get_animation_time(), 0.1, 0.0001,
			"the clip clock accumulated while unposed")

	model.visible = true
	model.advance_runtime_frame(0.0)
	assert_false(skeleton.get_bone_pose_position(0).is_equal_approx(poison),
			"the next visible frame applies the pending pose")
