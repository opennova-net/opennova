extends RefCounted

# Shared adapter from NovaSimulation's packed, authoritative aim-overlay result to
# NovaObjectModel's node-frame skeletal deltas. Selection and mounted config formulas
# stay in libs/anim; presentation only converts the final body + nine segment matrices
# into Godot space. MissionPresentPass and WirePresentPass must both go through here.

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const OVERLAY_CLASS_COUNT := 9


static func apply(node, snap: PackedFloat32Array, base: int) -> void:
	if node == null or not node.has_method("set_aim_overlay"):
		return
	if int(snap[base + NovaSimulation.PF_AIM_OVERLAY_VALID]) == 0:
		node.set_aim_overlay([])
		return

	var body_angles := Vector3(
			snap[base + NovaSimulation.PF_AIM_BODY_PITCH_DEG],
			snap[base + NovaSimulation.PF_AIM_BODY_YAW_DEG],
			snap[base + NovaSimulation.PF_AIM_BODY_ROLL_DEG])
	var body_basis := MissionObjectPlacer.bms_to_godot_basis(body_angles)
	node.basis = body_basis

	var inverse_body := body_basis.inverse()
	var deltas: Array = []
	deltas.resize(OVERLAY_CLASS_COUNT)
	for overlay_class in range(OVERLAY_CLASS_COUNT):
		var offset: int = (base + NovaSimulation.PF_AIM_ANGLES
				+ overlay_class * NovaSimulation.PF_AIM_CLASS_STRIDE)
		var segment_angles := Vector3(
				snap[offset], snap[offset + 1], snap[offset + 2])
		deltas[overlay_class] = (inverse_body
				* MissionObjectPlacer.bms_to_godot_basis(segment_angles))
	node.set_aim_overlay(deltas)
