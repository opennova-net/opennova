extends GutTest

# ObjectModel.set_section_visibility_mask drives per-part (COBJ section)
# visibility on the Robj_<N> render nodes — the draw-side consumer of the
# render-occlusion section masks. Bit N visible = part N draws; -1 restores
# everything; rebuild-created parts honor the applied mask. Pinned on a real
# multi-part fixture (Pmpjk01: five ROBJ parts, shared material indexes).
# [orig: g_HiddenSectionMask consumption in Terrain_RenderSectorModels
#  @ 0x5c5d30; docs/render/render-occlusion-re.md §5]

const PMP_3DI := "res://../fixtures/threedi/objects/Pmpjk01/Pmpjk01.3di"


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


func _part_instances(part: Node3D) -> Array[GeometryInstance3D]:
	var out: Array[GeometryInstance3D] = []
	for child in part.get_children():
		var instance := child as GeometryInstance3D
		if instance != null:
			out.append(instance)
	return out


func _assert_entity_light(instance: GeometryInstance3D, expected: Vector4,
		message: String) -> void:
	var actual: Variant = instance.get_instance_shader_parameter("u_entity_light")
	assert_true(actual is Vector4 and (actual as Vector4).is_equal_approx(expected),
			"%s: %s carries %s, expected %s" % [
					message, instance.name, actual, expected])


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


func test_interior_parts_carry_the_authored_light_transfer_as_instance_state() -> void:
	# Retail's batch-entry bit leaves ROBJ 0 outdoors (effectScale 1, no
	# interior lerp) and applies the ItemDef+0x218 daylight fraction to the
	# non-zero ROBJ parts. Both sides ride the one u_entity_light instance
	# uniform on top of the pass-global lighting block, so a material index
	# shared across that boundary stays one material.
	# [orig: Terrain_RenderSectorModels @ 0x5c5d30 (the model+536 daylight
	#  push per visible building); setup_entity_lighting_and_shader_constants
	#  @ 0x5d98a0]
	var m := ObjectModel.new()
	add_child_autofree(m)
	m.set_process(false)
	m.set_interior_section_light_transfer(0.2)
	m.set_object_data(_object_data())

	var parts: Dictionary = m.get_render_part_nodes()
	var exterior_count := 0
	for instance in _part_instances(parts[0] as Node3D):
		exterior_count += 1
		_assert_entity_light(instance, Vector4(1.0, 0.0, 1.0, 0.0),
				"ROBJ 0 surfaces receive the full outdoor daylight")
	assert_gt(exterior_count, 0, "the exterior part draws at least one surface")

	var interior_count := 0
	for key in parts.keys():
		if int(key) == 0:
			continue
		for instance in _part_instances(parts[key] as Node3D):
			interior_count += 1
			_assert_entity_light(instance, Vector4(1.0, 1.0, 0.2, 0.0),
					"non-zero ROBJ surfaces carry the authored interior daylight fraction")
	assert_gt(interior_count, 0, "the fixture draws interior surfaces")
