extends RefCounted

# Shared adapter from NovaSimulation's packed, authoritative aim-overlay result to
# NovaObjectModel's node-frame skeletal deltas. Selection and mounted config formulas
# stay in libs/anim; presentation only converts the final body + nine segment matrices
# into Godot space. MissionPresentPass and WirePresentPass must both go through here.

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const OVERLAY_CLASS_COUNT := 9


static func apply(node, snap: PackedFloat32Array, base: int) -> void:
	if node == null:
		return
	# The mounted-seat selector is resolved beside the overlay in native code for
	# both authoritative and decoded-wire entities. Apply its final skeletal
	# verdict here so placed and wire models cannot diverge. Player/Passenger rows
	# keep the bit clear; the producer owns the exact Flags/seat predicate.
	# [orig: BN17 special row @0x4b1290]
	if node.has_method("set_right_hand_collapsed"):
		node.set_right_hand_collapsed(
				int(snap[base + NovaSimulation.PF_RIGHT_HAND_COLLAPSED]) != 0)
	if not node.has_method("set_aim_overlay"):
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
