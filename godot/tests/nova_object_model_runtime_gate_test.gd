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
			"two half-second host frames preserve retail's fixed 16 ms tick count")
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


func test_light_push_skips_identical_values() -> void:
	# Real rebuild -> real ShaderMaterials and the fixture's real light block.
	# Re-evaluating at the same clock must not touch the materials again.
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
	assert_eq(int(material.get_shader_parameter("u_local_light_count")), 1,
			"rebuild pushes the fixture's dominant light")
	material.set_shader_parameter("u_local_light_count", 99)
	model.advance_runtime_frame(0.0)
	assert_eq(int(material.get_shader_parameter("u_local_light_count")), 99,
			"an identical evaluation pushes nothing (the poison survives)")


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
