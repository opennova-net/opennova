extends RefCounted

# Shared adapter from NovaSimulation's packed, authoritative aim-overlay result to
# NovaObjectModel's node-frame skeletal deltas. Selection and mounted config formulas
# stay in libs/anim; presentation only converts the final body + nine segment matrices
# into Godot space. MissionPresentPass and WirePresentPass must both go through here.

## Final root basis for a presented row. Aim-valid rows own the body rotation;
## all others retain the ordinary entity rotation supplied by the presenter.
# The conversion and dispatch bodies live in the native walk
# (NovaPresentApplier, godot/engine/simulation/nova_present_applier.cpp) so the
# mission pass's per-row hot path and this shared adapter cannot diverge; these
# statics are the stable GDScript seams the wire pass and tests keep calling.

static func root_basis(
		snap: PackedFloat32Array, base: int, fallback: Basis) -> Basis:
	return NovaPresentApplier.aim_root_basis(snap, base, fallback)


static func apply(
		node, snap: PackedFloat32Array, base: int,
		drive_root_basis: bool = true) -> void:
	# The mounted-seat selector's final skeletal verdict applies to placed and
	# wire models alike [orig: BN17 special row @0x4b1290].
	NovaPresentApplier.aim_apply(node, snap, base, drive_root_basis)


## The valid-overlay leg alone: capability and validity already checked by the
## caller (the native mission walk gates both per row from its rebuilt plan).
static func apply_valid(
		node, snap: PackedFloat32Array, base: int,
		drive_root_basis: bool = true) -> void:
	NovaPresentApplier.aim_apply_valid(node, snap, base, drive_root_basis)
