extends GutTest

# The per-model _process residual fix: clock-DERIVED render work (material
# generators, PANM transforms, lights, the env push) runs only while the model
# can render, while time-ACCUMULATING state (commanded part anims, the skeletal
# clip clock) advances regardless — a door commanded open while culled is open
# when next seen, and every skipped value re-derives from the absolute clock on
# the next visible frame. Retail computes these constants per SUBMITTED model
# only [orig: Terrain_RenderSectorModels @ 0x5c5d30].

const HOUSE_3DI := "res://../fixtures/threedi/3di3/House.3di"


class GateSpyModel:
	extends NovaObjectModel
	var regs: Array = ["reg0"]
	var env_applies := 0
	var light_applies := 0
	var robj_applies := 0
	func _resolve_anim_channel_register(slot: int) -> String:
		return String(regs[slot]) if slot >= 0 and slot < regs.size() else ""
	func _apply_environment_to_materials() -> void:
		env_applies += 1
	func _apply_lights() -> void:
		light_applies += 1
	func _apply_robj_transforms() -> bool:
		robj_applies += 1
		return false


func _house_data() -> NovaObjectData:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(HOUSE_3DI)), OK,
			"the committed House fixture opens")
	return data


# Spy model with a real document but NO rebuild (object_data assigned
# directly), so the routing runs without meshes/materials in the way.
func _spy_model() -> GateSpyModel:
	var model := GateSpyModel.new()
	add_child_autofree(model)
	model.object_data = _house_data()
	return model


func test_hidden_model_skips_render_work_but_advances_part_anims() -> void:
	var model := _spy_model()
	model._has_lights = true  # forces the runtime path
	model.play_part_anim(1, 1, 1.0)
	model.visible = false

	model._process(0.5)
	assert_eq(model.light_applies, 0, "a hidden model pushes no light state")
	assert_eq(model.env_applies, 0, "a hidden model pushes no environment state")
	assert_almost_eq(int(model.get_ctrl_values().get("reg0", -1)), 32768, 2,
			"the commanded part anim still advanced while hidden")

	model.visible = true
	model._process(0.5)
	assert_eq(model.light_applies, 1, "render work resumes on the visible frame")
	assert_eq(model.env_applies, 1)
	assert_eq(int(model.get_ctrl_values().get("reg0", -1)), 65535,
			"the sweep completed across the hidden/visible boundary")


func test_hidden_fast_path_skips_the_env_push() -> void:
	var model := _spy_model()  # no lights/panm/materials -> the fast path
	model.visible = false
	model._process(0.016)
	assert_eq(model.env_applies, 0, "hidden fast-path frames do nothing")
	model.visible = true
	model._process(0.016)
	assert_eq(model.env_applies, 1, "a visible fast-path frame keeps the env gate")


func test_live_panm_transforms_rederive_on_the_visible_frame() -> void:
	var model := _spy_model()
	model._has_live_panm = true
	model.visible = false
	model._process(0.016)
	model._process(0.016)
	assert_eq(model.robj_applies, 0, "no PANM evaluation while hidden")
	model.visible = true
	model._process(0.016)
	assert_eq(model.robj_applies, 1,
			"the visible frame re-derives transforms from the absolute clock")


func test_light_push_skips_identical_values() -> void:
	# Real rebuild -> real ShaderMaterials. House has no .3di lights, so the
	# evaluated state is the stable count-0 push: the first apply writes it,
	# the second must not touch the materials again (poison survives).
	var model := NovaObjectModel.new()
	add_child_autofree(model)
	model.set_object_data(_house_data())
	var materials: Array = model.get_surface_materials()
	if materials.is_empty():
		pass_test("fixture built no surface materials under this renderer")
		return
	model._has_lights = true
	model._apply_lights()
	var material := materials[0] as ShaderMaterial
	assert_eq(int(material.get_shader_parameter("u_local_light_count")), 0,
			"the first apply pushes the evaluated count-0 state")
	material.set_shader_parameter("u_local_light_count", 99)
	model._apply_lights()
	assert_eq(int(material.get_shader_parameter("u_local_light_count")), 99,
			"an identical evaluation pushes nothing (the poison survives)")


func test_pose_param_latches_dirty_without_writing_bones() -> void:
	# advance_body_animation(delta, pose=false) advances the clip clock and
	# leaves _body_pose_dirty latched for the next visible frame.
	var model := _spy_model()
	model._skeleton = Skeleton3D.new()
	model.add_child(model._skeleton)
	model._skeletal = RefCounted.new()  # duck double: no pose methods needed pre-pose
	model._anim_key = "anim_idle"
	model._anim_playing = true
	model.set_playing(true)
	model._body_pose_dirty = false

	model.advance_body_animation(0.25, false)
	assert_true(bool(model._body_pose_dirty),
			"the clock advanced and the pose stayed pending")
	assert_almost_eq(float(model._anim_time), 0.25, 0.0001,
			"the clip clock accumulated while unposed")
