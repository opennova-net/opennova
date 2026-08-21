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


func test_shared_material_indices_reuse_one_native_lit_material() -> void:
	var m := _model()
	var indices: PackedInt32Array = m.get_surface_material_indices()
	var materials: Array = m.get_surface_materials()
	assert_eq(materials.size(), indices.size())
	var material_ids := {}
	for i in range(indices.size()):
		var material := materials[i] as ShaderMaterial
		assert_not_null(material)
		if material_ids.has(indices[i]):
			assert_eq(material.get_instance_id(), material_ids[indices[i]],
					"one decoded material index reuses one native surface material")
		else:
			material_ids[indices[i]] = material.get_instance_id()
		assert_null(material.get_shader_parameter("u_dir_light_color"),
				"object materials no longer carry copied environment lighting")
