extends GutTest

# NovaObjectModel.set_section_visibility_mask drives per-part (COBJ section)
# visibility on the Robj_<N> render nodes — the draw-side consumer of the
# render-occlusion section masks. Bit N visible = part N draws; -1 restores
# everything; a rebuild-created part honors the applied mask. Asset-free: a
# typed harness fabricates Robj nodes through the same builder path the mesh
# build uses. [orig: g_HiddenSectionMask consumption in
# Terrain_RenderSectorModels @ 0x5c5d30; docs/render/render-occlusion-re.md §5]


class ModelHarness:
	extends NovaObjectModel

	func ensure_render_part_node(robj_index: int) -> Node3D:
		return self._get_or_create_robj_node(robj_index)


func _model_with_parts(count: int) -> ModelHarness:
	var m := ModelHarness.new()
	autofree(m)
	for i in range(count):
		m.ensure_render_part_node(i)
	return m


func test_mask_bits_toggle_part_nodes() -> void:
	var m := _model_with_parts(3)
	var parts: Dictionary = m.get_render_part_nodes()
	m.set_section_visibility_mask(0b101)
	assert_true((parts[0] as Node3D).visible, "bit 0 (exterior) stays visible")
	assert_false((parts[1] as Node3D).visible, "a cleared section bit hides its part")
	assert_true((parts[2] as Node3D).visible)

	m.set_section_visibility_mask(-1)
	assert_true((parts[1] as Node3D).visible, "-1 restores everything")


func test_rebuilt_parts_honor_the_applied_mask() -> void:
	var m := _model_with_parts(1)
	m.set_section_visibility_mask(0b1)  # only part 0 visible
	var late: Node3D = m.ensure_render_part_node(1)
	assert_false(late.visible, "a part created after the mask applies it")
	var exterior: Node3D = m.ensure_render_part_node(0)
	assert_true(exterior.visible, "an existing visible part is returned unchanged")


func test_same_mask_reapply_is_a_no_op() -> void:
	var m := _model_with_parts(2)
	m.set_section_visibility_mask(0b10)
	var part: Node3D = m.get_render_part_nodes()[0]
	part.visible = true  # host override; an identical mask must not stomp it
	m.set_section_visibility_mask(0b10)
	assert_true(part.visible, "re-applying the same mask changes nothing")
