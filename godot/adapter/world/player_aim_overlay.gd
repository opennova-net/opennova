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
## The THIRD-PERSON held weapon's own attach basis (BMS euler degrees) and retail's
## draw verdict for it. The basis is deliberately none of `segment_angles`: the head
## class carries full-aim pitch and the arm class the blended yaw, so either one aims
## the gun visibly off-axis. [orig: the attach build @0x4b1bdc..0x4b1bf8; the gate
## Entity_CanFireWeapon @0x4dcb10]
var weapon_attach_angles := Vector3.ZERO
var weapon_visible := false
## True when retail would pose this body's weapon at the HAND instead of at
## `weapon_attach_angles` — the weapon channel's hold state carrying flag 0x80 (knife,
## grenade and designator holds, both melee attacks, binoculars, both reloads).
## [orig: gate @0x4b21b6 / branch @0x4b220f]
var weapon_hand_frame := false


## Decode the simulation's transport dict; null when the overlay is absent/invalid
## (no local player, or the sim predates the overlay).
static func from_sim_dict(d: Dictionary) -> PlayerAimOverlay:
	if not bool(d.get("valid", false)):
		return null
	var out := PlayerAimOverlay.new()
	out.body_angles = d.get("body", Vector3.ZERO)
	out.segment_angles = d.get("angles", PackedVector3Array())
	out.weapon_attach_angles = d.get("weapon_attach", Vector3.ZERO)
	out.weapon_visible = bool(d.get("weapon_visible", false))
	out.weapon_hand_frame = bool(d.get("weapon_hand_frame", false))
	return out
