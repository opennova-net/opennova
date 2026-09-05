extends GutTest

# End-to-end checks for the 3DI loader boundary: PANM/LGHT bytes name a
# model-local CTRL record, while runtime evaluation consumes the shared retail
# 96-register bus.

const MOUNT := "res://../fixtures/threedi/synth/mount.3di"
# Authored variants of the fixture above, minted once from the retired edit
# surface (fixtures/README.md); each test reads back the
# authored content it depends on before probing the runtime.
const SYN_CTRL1_HEAT_GLOW := "res://../fixtures/threedi/synth/mount_ctrl1_heat_glow.3di"
const SYN_CTRL1_NOT_RETAIL := "res://../fixtures/threedi/synth/mount_ctrl1_not_retail.3di"
const SYN_YAW_STYLE114 := "res://../fixtures/threedi/synth/mount_yaw_style114.3di"
const SYN_CTRL1_LOD_FRAC_YAW_STYLE114 := "res://../fixtures/threedi/synth/mount_ctrl1_lod_frac_yaw_style114.3di"
const SYN_MTRL0_RGBGEN113_REG1 := "res://../fixtures/threedi/synth/mount_mtrl0_rgbgen113_reg1.3di"
const SYN_ARMRY_LGHT0_COLORGEN113 := "res://../fixtures/threedi/synth/armory_lght0_colorgen113_flicker.3di"
# The mount's controlled yaw track: the first LOD0 track on register 1 is
# row 1's rotation x, driving part 1 (the cradle). The authored rows are the
# minimal_3di_gen ctest's ("cradle yaw row"), which byte-compares every
# variant below; the register renames (mount_ctrl1_*) leave the row alone.
const YAW_PART := 1


func _open(path: String) -> ObjectData:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(path)), OK)
	return data


func _pose(data: ObjectData, part_index: int,
		controls: Dictionary) -> Transform3D:
	var poses: Dictionary = data.evaluate_panm(0, 0, controls)
	assert_true(poses.has(part_index))
	return poses.get(part_index, Transform3D())


func _transform_delta(a: Transform3D, b: Transform3D) -> float:
	var delta := a.origin.distance_to(b.origin)
	for axis in range(3):
		delta += (a.basis[axis] - b.basis[axis]).length()
	return delta


func _assert_cached_apply_matches(data: ObjectData,
		controls: Dictionary) -> void:
	var reference: Dictionary = data.evaluate_panm(0, 0, controls)
	var nodes: Array = []
	for _part in range(reference.size()):
		var node := Node3D.new()
		add_child_autofree(node)
		nodes.append(node)
	assert_gt(int(data.apply_panm_to_nodes(0, 0, controls, nodes, 0)), 0)
	for key in reference:
		var part := int(key)
		assert_true((nodes[part] as Node3D).transform.is_equal_approx(
				reference[key] as Transform3D),
				"cached part %d should use the same global CTRL bus" % part)


func test_panm_uses_case_insensitive_signed_global_dwords() -> void:
	var data := _open(MOUNT)
	var part := YAW_PART # EWEAP_GUNYAW drives the cradle
	var zero := _pose(data, part, {"EWEAP_GUNYAW": 0})
	var half := _pose(data, part, {"EWEAP_GUNYAW": 0x8000})
	var full_upper := _pose(data, part, {"EWEAP_GUNYAW": 0x10000})
	var full_lower := _pose(data, part, {"eweap_gunyaw": 0x10000})
	var almost_full := _pose(data, part, {"EWEAP_GUNYAW": 0xffff})
	var negative := _pose(data, part, {"EWEAP_GUNYAW": -0x8000})
	var wrapped_full := _pose(data, part, {
		"EWEAP_GUNYAW": 0x100010000,
	})
	var wrapped_negative := _pose(data, part, {
		"EWEAP_GUNYAW": -0x100008000,
	})

	assert_true(full_lower.is_equal_approx(full_upper),
			"global register lookup should be ASCII case-insensitive")
	assert_true(wrapped_full.is_equal_approx(full_upper),
			"Godot integers should wrap through the low retail dword")
	assert_true(wrapped_negative.is_equal_approx(negative),
			"wrapped dword bits should retain their signed int32 meaning")
	assert_gt(_transform_delta(half, zero), 0.001,
			"the controlled authored yaw track should consume the global slot")
	assert_gt(_transform_delta(negative, zero), 0.001,
			"negative signed dwords must not clamp to zero")
	assert_gt(_transform_delta(full_upper, almost_full), 0.000001,
			"the exact 0x10000 endpoint must not truncate to uint16")
	_assert_cached_apply_matches(data, {"eWeAp_GuNyAw": -0x8000})


func _register_name(data: ObjectData, index: int) -> String:
	var registers: Array = data.get_control_registers()
	if index >= registers.size():
		return ""
	return String((registers[index] as Dictionary).get("name", ""))


func test_duplicate_and_unknown_authored_names_follow_retail_loader_aliases() -> void:
	var duplicate := _open(SYN_CTRL1_HEAT_GLOW)
	assert_eq(_register_name(duplicate, 0), "HEAT_GLOW")
	assert_eq(_register_name(duplicate, 1), "HEAT_GLOW",
			"the fixture authors CTRL 1 as a duplicate HEAT_GLOW")
	assert_gt(_transform_delta(
			_pose(duplicate, YAW_PART, {"HEAT_GLOW": 0x8000}),
			_pose(duplicate, YAW_PART, {"HEAT_GLOW": 0})), 0.001,
			"duplicate local CTRL records should alias one global slot")

	var unknown := _open(SYN_CTRL1_NOT_RETAIL)
	assert_eq(_register_name(unknown, 1), "NOT_RETAIL",
			"the fixture authors CTRL 1 as a name retail never registers")
	assert_gt(_transform_delta(
			_pose(unknown, YAW_PART, {"lod_frac": 0x8000}),
			_pose(unknown, YAW_PART, {"LOD_FRAC": 0})), 0.001,
			"an unknown authored CTRL name should alias retail ordinal zero")


func test_wave_styles_receive_the_loader_resolved_phase_ordinal() -> void:
	# Both fixtures carry the pristine yaw track (control 113, local ordinal 1,
	# the cradle's row) re-styled to 114 — the minimal_3di_gen ctest pins the
	# authored style on both ("yaw style 114").
	var normal := _open(SYN_YAW_STYLE114)
	var patched := _open(SYN_CTRL1_LOD_FRAC_YAW_STYLE114)
	assert_eq(_register_name(patched, 1), "LOD_FRAC",
			"the patched fixture authors CTRL 1 as LOD_FRAC")

	assert_gt(_transform_delta(
			_pose(normal, YAW_PART, {}),
			_pose(patched, YAW_PART, {})), 0.001,
			"style 114 should use global ordinal 55 vs LOD_FRAC ordinal zero as phase")


func test_light_controls_share_the_case_insensitive_global_bus() -> void:
	var data := _open(SYN_ARMRY_LGHT0_COLORGEN113)
	var light := data.get_light_info(0)
	assert_eq(light.colorgen_style, 113,
			"the fixture authors LGHT 0 as a register-driven color generator")
	assert_false(light.disable_lightobjects)
	var upper: Color = (data.evaluate_lights(
			0, {"FLICKER": 0x8000})[0] as Dictionary).get("color")
	var mixed: Color = (data.evaluate_lights(
			0, {"fLiCkEr": 0x8000})[0] as Dictionary).get("color")
	assert_true(mixed.is_equal_approx(upper),
			"light CTRL lookup should use the same case-insensitive global bus")
	assert_almost_eq(mixed.r, 127.0 / 255.0, 0.00001)


func test_material_case_aliases_collapse_in_dictionary_order() -> void:
	# MTRL 0 authors a register-driven RGB generator on register 1 (the
	# minimal_3di_gen ctest pins the row: "material alias").
	var data := _open(SYN_MTRL0_RGBGEN113_REG1)

	var high: Vector3 = data.eval_material_runtime(0, 0, {
		"EWEAP_GUNYAW": 0,
		"eweap_gunyaw": 65536,
	}).get("rgb_mod")
	var low: Vector3 = data.eval_material_runtime(0, 0, {
		"eweap_gunyaw": 65536,
		"EWEAP_GUNYAW": 0,
	}).get("rgb_mod")
	assert_true(high.is_equal_approx(Vector3.ONE),
			"the later case alias should own the single global slot")
	assert_true(low.is_equal_approx(Vector3.ZERO),
			"reversing insertion order should reverse the same-slot winner")
