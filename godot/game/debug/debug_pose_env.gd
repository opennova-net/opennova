class_name DebugPoseEnv
extends RefCounted

## Dev/headless convenience beside NW_SP_MISSION: NW_SP_DEBUG_POSE =
## "x,y,z[,yaw_deg]" (mission coordinates) retries a one-shot teleport until
## the local player exists, so retail-comparison captures can pin both
## engines to one pose. The retail side replays the identical pose through
## onhook's opennova.debug_snapshot.v1 loader.


## Attempt the pending teleport; returns the env string still pending (empty
## once applied or malformed, so the caller stores the result back).
static func apply(pending: String, sim: Simulation) -> String:
	if pending.is_empty() or sim == null or not bool(sim.has_local_player()):
		return pending
	var parts := pending.split(",")
	if parts.size() < 3:
		push_warning("NW_SP_DEBUG_POSE needs x,y,z[,yaw_deg]; ignoring")
		return ""
	var pos := Vector3(parts[0].to_float(), parts[1].to_float(),
			parts[2].to_float())
	var yaw := parts[3].to_float() if parts.size() > 3 else 0.0
	if sim.debug_teleport_local_player(pos, yaw, 0.0) == OK:
		return ""
	return pending
