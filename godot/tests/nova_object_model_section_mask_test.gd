extends GutTest

# ObjectModel.set_section_visibility_mask drives per-part (COBJ section)
# visibility on the Robj_<N> render nodes — the draw-side consumer of the
# render-occlusion section masks. Bit N visible = part N draws; -1 restores
# everything; rebuild-created parts honor the applied mask. Pinned on a real
# multi-part fixture (Pmpjk01: five ROBJ parts, shared material indexes).
# [orig: g_HiddenSectionMask consumption in Terrain_RenderSectorModels
#  @ 0x5c5d30; docs/render/render-occlusion-re.md §5]

const PMP_3DI := "res://../fixtures/3dp/Pmpjk01/Pmpjk01.3di"


func _object_data() -> ObjectData:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(PMP_3DI)), OK,
			"multi-part fixture loads")
	return data


func _model() -> ObjectModel:
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_process(false)
	model.set_object_data(_object_data())
	return model


func _part_materials(part: Node3D) -> Array:
	var out: Array = []
	for i in range(part.get_child_count()):
		var instance := part.get_child(i) as MeshInstance3D
		if instance != null and instance.material_override != null:
			out.append(instance.material_override)
	return out


func test_mask_bits_toggle_part_nodes() -> void:
	var m := _model()
	var parts: Dictionary = m.get_render_part_nodes()
	assert_eq(parts.size(), 5, "the fixture carries five ROBJ parts")
	m.set_section_visibility_mask(0b10101)
	assert_true((parts[0] as Node3D).visible, "bit 0 (exterior) stays visible")
	assert_false((parts[1] as Node3D).visible, "a cleared section bit hides its part")
	assert_true((parts[2] as Node3D).visible)
	assert_false((parts[3] as Node3D).visible)
	assert_true((parts[4] as Node3D).visible)

	m.set_section_visibility_mask(-1)
	assert_true((parts[1] as Node3D).visible, "-1 restores everything")
	assert_true((parts[3] as Node3D).visible)


func test_rebuilt_parts_honor_the_applied_mask() -> void:
	var m := _model()
	m.set_section_visibility_mask(0b00001)  # only part 0 visible
	# A rebuild recreates every Robj node from the document; the fresh nodes
	# must come up under the mask that was applied before they existed.
	m.set_object_data(_object_data())
	var parts: Dictionary = m.get_render_part_nodes()
	assert_true((parts[0] as Node3D).visible, "the exterior part is returned visible")
	for key in parts.keys():
		if int(key) == 0:
			continue
		assert_false((parts[key] as Node3D).visible,
				"a part created after the mask applies it")


func test_same_mask_reapply_is_a_no_op() -> void:
	var m := _model()
	m.set_section_visibility_mask(0b00010)
	var part: Node3D = m.get_render_part_nodes()[0]
	part.visible = true  # owner override; an identical mask must not stomp it
	m.set_section_visibility_mask(0b00010)
	assert_true(part.visible, "re-applying the same mask changes nothing")


func test_interior_parts_duplicate_shared_material_and_apply_light_transfer() -> void:
	# Retail's batch-entry bit leaves ROBJ 0 outdoors and applies the
	# ItemDef+0x218 daylight fraction to non-zero ROBJ parts. A material index
	# shared across that boundary must split into per-context instances —
	# otherwise one stamp would overwrite the other — while parts on the same
	# side keep sharing.
	var m := ObjectModel.new()
	add_child_autofree(m)
	m.set_process(false)
	m.set_interior_section_light_transfer(0.2)
	m.set_object_data(_object_data())

	var state := EnvLightState.new()
	var values := EnvLightValues.retail_noon_defaults()
	values.dir_color = Vector3(1.0, 0.5, 0.25)
	state.publish(values)
	m.set_environment_state(state)
	m.advance_runtime_frame(0.016)

	var parts: Dictionary = m.get_render_part_nodes()
	var exterior_ids := {}
	for material in _part_materials(parts[0] as Node3D):
		exterior_ids[(material as ShaderMaterial).get_instance_id()] = true
		assert_eq((material as ShaderMaterial).get_shader_parameter("u_dir_light_color"),
				values.dir_color, "ROBJ 0 materials receive the full outdoor daylight")
	assert_gt(exterior_ids.size(), 0, "the exterior part draws at least one surface")

	var interior_count := 0
	for key in parts.keys():
		if int(key) == 0:
			continue
		for material in _part_materials(parts[key] as Node3D):
			interior_count += 1
			assert_false(exterior_ids.has((material as ShaderMaterial).get_instance_id()),
					"a shared material index splits at the lighting-context boundary")
			var indoor: Vector3 = (material as ShaderMaterial).get_shader_parameter(
					"u_dir_light_color")
			assert_true(indoor.is_equal_approx(values.dir_color * 0.2),
					"non-zero ROBJ parts receive the authored interior daylight fraction")
	assert_gt(interior_count, 0, "the fixture draws interior surfaces")

	var distinct := {}
	for material in m.get_surface_materials():
		if material != null:
			distinct[(material as ShaderMaterial).get_instance_id()] = true
	assert_lt(distinct.size(), m.get_surface_materials().size(),
			"parts with the same context still share one material")
