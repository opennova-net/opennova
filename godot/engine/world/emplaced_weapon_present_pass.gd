extends RefCounted

## Apply retail's semantic emplaced-weapon CTRL registers. These are independent
## of PLAYPARTANIM, which publishes only VEHICLE_SPECIAL1/2. B50Cal's turret
## and barrel bind the exact names below.

const GUN_YAW := "EWEAP_GUNYAW"
const GUN_PITCH := "EWEAP_GUNPITCH"


# Bodies live in the native walk (NovaPresentApplier) so the mission pass's
# per-row hot path and this shared adapter cannot diverge; these statics are the
# stable GDScript seams other passes and tests keep calling.

static func clear(node) -> void:
	NovaPresentApplier.emplaced_clear(node)


static func apply(
		node, snap: PackedFloat32Array, base: int, clear_when_invalid: bool = true) -> int:
	# Nodes persist across dismount/death, so the invalid leg removes only the
	# two controls this presenter owns — clear_ctrl_values() would also erase
	# live WAC channels.
	return NovaPresentApplier.emplaced_apply(node, snap, base, clear_when_invalid)
