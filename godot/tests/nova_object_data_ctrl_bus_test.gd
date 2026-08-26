extends GutTest

# End-to-end checks for the 3DI loader boundary: PANM/LGHT bytes name a
# model-local CTRL record, while runtime evaluation consumes the shared retail
# 96-register bus.

const B50CAL := "res://../fixtures/threedi/objects/B50Cal/B50Cal.3di"
const TRACK_NAMES := [
	"rotation_x", "rotation_y", "rotation_z",
	"scale_x", "scale_y", "scale_z", "translation",
]


func _open(path: String) -> ObjectData:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(path)), OK)
	return data


func _controlled_track(data: ObjectData, local_ordinal: int) -> Dictionary:
	for anim_index in range(data.get_part_anim_count(0)):
		var anim: Dictionary = data.get_part_anim_info(0, anim_index)
		for track_name in TRACK_NAMES:
			var track: Dictionary = anim.get(track_name, {})
			if int(track.get("control", 0)) == 113 and \
					int(track.get("control_param", -1)) == local_ordinal:
				return {
					"anim_index": anim_index,
					"track_name": track_name,
					"part_index": int(anim.get("transform_as", -1)),
				}
	return {}


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
	var data := _open(B50CAL)
	var track := _controlled_track(data, 1) # EWEAP_GUNYAW
	assert_false(track.is_empty(), "B50Cal should carry its authored yaw track")
	if track.is_empty():
		return
	var part := int(track.get("part_index", -1))
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
