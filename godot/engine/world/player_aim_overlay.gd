class_name PlayerAimOverlay
extends RefCounted

## The local player's per-segment aim/body overlay for one frame — the typed record
## behind `NovaSimulation.get_local_player_aim_overlay()`'s transport Dictionary
## (ADR 0017: the record is the contract, the dict is its C++-binding encoding).
## `body_angles` is the lagged BODY heading (BMS yaw/pitch/roll degrees — the frame the
## hips render in); `segment_angles` is one BMS euler per anim overlay class, the
## witnessed per-segment aim/body blend the avatar's skeleton applies as the torso
## twist. [orig: Entity_BuildBoneTransformMatrices @0x4b1290;
## docs/world/world-wac-ai-re.md §14 (D-INF-11)]

var body_angles := Vector3.ZERO
var segment_angles := PackedVector3Array()


## Decode the simulation's transport dict; null when the overlay is absent/invalid
## (no local player, or the sim predates the overlay).
static func from_sim_dict(d: Dictionary) -> PlayerAimOverlay:
	if not bool(d.get("valid", false)):
		return null
	var out := PlayerAimOverlay.new()
	out.body_angles = d.get("body", Vector3.ZERO)
	out.segment_angles = d.get("angles", PackedVector3Array())
	return out
