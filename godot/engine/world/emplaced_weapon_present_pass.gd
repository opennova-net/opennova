extends RefCounted

## Apply retail's semantic emplaced-weapon CTRL registers. These are independent
## of PLAYPARTANIM's model-order channels: B50Cal's CTRL[0] is HEAT_GLOW, while
## its turret and barrel bind the exact names below.

const GUN_YAW := "EWEAP_GUNYAW"
const GUN_PITCH := "EWEAP_GUNPITCH"


static func clear(node) -> void:
	if not node.has_method("clear_ctrl_value"):
		return
	node.clear_ctrl_value(GUN_YAW)
	node.clear_ctrl_value(GUN_PITCH)


static func apply(
		node, snap: PackedFloat32Array, base: int, clear_when_invalid: bool = true) -> int:
	if not node.has_method("set_ctrl_value"):
		return 0
	if int(snap[base + NovaSimulation.PF_EMPLACED_CONTROLS_VALID]) == 1:
		node.set_ctrl_value(
				GUN_YAW, int(snap[base + NovaSimulation.PF_EWEAP_GUNYAW]))
		node.set_ctrl_value(
				GUN_PITCH, int(snap[base + NovaSimulation.PF_EWEAP_GUNPITCH]))
		return 2
	# Nodes persist across dismount/death, so remove only the two controls this
	# presenter owns. clear_ctrl_values() would also erase live WAC channels.
	if clear_when_invalid:
		clear(node)
	return 0
