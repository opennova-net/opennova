class_name PresentHeldWeapon
extends RefCounted

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

## Placement for the THIRD-PERSON held weapon — the gun in a soldier's hands. Shared by
## every present path that draws one (the local player's own avatar, and wire-decoded
## remote players) so the two cannot drift apart.
##
## The original draws this model RIGID: one matrix is `qmemcpy`'d into EVERY bone slot of
## the weapon model, so it carries no skeleton, no clip and no pose of its own — the whole
## appearance is the transform built here.
## [orig: BoneCallback_org0_World draw 5 @0x4e3c87..0x4e3d99, the all-bones fill @0x4e3d71]


## The weapon rides bone INDEX 16 (".bad row BN17 R Hand"). Our rig is index-driven and
## the model bone order IS the BN order, which is the same index space the original reads
## through its bone-matrix array. [world-wac-ai-re §14.2/§14.4]
const BONE_INDEX := 16

## The fixed nudge applied to that bone's own pivot, in raw def units.
## [orig: flt_7C68E8 = 0.05 applied +X and -Y, flt_7C9BA8 = 0.051 applied +Z, read off the
##  model bone-def table row 16 at modelDef+0x424 @0x4b2186]
##
## The X term is NEGATED rather than copied: the original authors these in its x-negated
## render frame, the same reason MissionObjectPlacer.positions_from_model negates authored
## X. Copying the sign literally puts the gun on the wrong side of the hand by a small,
## easy-to-miss margin.
const ATTACH_NUDGE := Vector3(-0.05, -0.05, 0.051)


static func find_skeleton(root: Node) -> Skeleton3D:
	if root == null:
		return null
	if root is Skeleton3D:
		return root
	for child in root.get_children():
		var found := find_skeleton(child)
		if found != null:
			return found
	return null


## The weapon's world transform, or null when this body cannot place one.
##
## Position is bone 16's own pivot nudged and carried through that bone's posed matrix —
## the original's `M16 · (pivot16 + nudge)`. Since `M16 · pivot16` IS the joint world
## position, that reduces to joint + M16_rotation · nudge, so no pivot table lookup is
## needed here.
##
## Orientation is one of TWO frames, and the original chooses between them on a single bit.
##
##  * The ENTITY frame (default): the weapon's own attach triple, passed in. It is neither
##    bone 16's rotation nor any aim-overlay class — the head class carries full-aim pitch
##    and the arm class the blended yaw, so either one aims the gun visibly off-axis. The
##    builder is the very function the original uses to place an ordinary world object, so
##    a gfx3 is posed exactly like a placed .3di; weapon models are authored muzzle +Z /
##    up +Y (userpoint-measured), which is what that basis expects.
##  * The HAND frame: bone 16's own matrix with a fixed calibration, `Ry_e · Rz_e · M16`.
##    Selected when the WEAPON-channel hold state carries `g_animStateFlagsTable` bit 0x80
##    — the knife, grenade and designator holds, both melee attacks, binoculars, BOTH
##    reload states, and the death family. That is ordinary play, not an edge case: it is
##    why a knife sits in the fist instead of standing on end (`M9K_3rd` is authored +Y-long,
##    so the entity frame would draw it near-vertical), and why a rifle follows the hands
##    through a reload.
##
## [orig: matrix build @0x4b2180..0x4b22f8; translation-only overwrite @0x4b22cf..0x4b22f8;
##  entity attach basis @0x4b1bdc..0x4b1bf8; hand-frame gate @0x4b21b6, branch @0x4b220f,
##  Rz @0x4b2215..0x4b2251, Ry @0x4b2256..0x4b22c2; the state's writer
##  Entity_UpdateInfantryPlayerBody @0x4b5dad..0x4b5ea9; the shared placement builder
##  Math_BuildFixedPointToFloatMatrix4x4 @0x612200, also the generic object placement
##  @0x4e2912. world-wac-ai-re §14.4/§14.4a/§14.4b]
static func attach_transform(
		body: Node3D, attach_angles_bms: Vector3, hand_frame: bool = false) -> Variant:
	var skel := find_skeleton(body)
	if skel == null or skel.get_bone_count() <= BONE_INDEX:
		return null
	# `M16 · pivot16` IS the joint world position, so the pivot term needs no table lookup.
	# The NUDGE does: the original adds it to the pivot in MODEL space and carries the sum
	# through the same matrix, so it must ride bone 16's MODEL->WORLD rotation. In Godot
	# that is the bone's pose taken relative to its REST, not the posed basis itself —
	# bone 16's rest basis is a large rotation (measured: it is nowhere near identity), so
	# multiplying by the posed basis both rotates the offset by that rest and leaves it in
	# skeleton space while the position it is added to is in world space. The hand frame
	# needs exactly the same term, so it is computed once for both.
	var joint_world := skel.global_transform * skel.get_bone_global_pose(BONE_INDEX)
	var model_to_world := joint_world * skel.get_bone_global_rest(BONE_INDEX).affine_inverse()
	return Transform3D(
			hand_frame_basis(model_to_world.basis) if hand_frame
					else MissionObjectPlacer.bms_to_godot_basis(attach_angles_bms),
			joint_world.origin + model_to_world.basis * ATTACH_NUDGE)


## The HAND frame's 3x3, given bone 16's MODEL->WORLD rotation.
##
## The original composes `var_13C0 = Ry_e · Rz_e · boneMatrix[16]` row-major, so in column
## form the two calibration rotations apply to the model BEFORE the bone matrix — i.e. they
## sit on the right here. Both angles come straight off the binary, and both survive the
## conversion to Godot unchanged in sign: the rotation builder stores `-sin θ`, which makes
## each block a rotation by `-θ`, and conjugating through the loader's X-negation flips the
## sign back. Two inversions, so the authored constants are used as-is — a coincidence worth
## stating, because it looks like a missing negation and is not one.
## [orig: Rz `dbl_7C9BA0` @0x7C9BA0 with axisIndex 0 @0x4b2240; Ry `dbl_7C9B98` @0x7C9B98
##  with axisIndex 2 @0x4b2294; sin negated via `dbl_7C57B0` @0x4b221f/@0x4b2273 inside
##  Math_BuildRotationMatrix4x4_ByAxis @0x611db0]
const HAND_FRAME_Z_RAD := 0.5759761961496483    # +33.001 deg about the model's +Z
const HAND_FRAME_Y_RAD := -1.3613982818082597   # -78.002 deg about the model's +Y

static func hand_frame_basis(bone_model_to_world: Basis) -> Basis:
	return bone_model_to_world \
			* Basis(Vector3.BACK, HAND_FRAME_Z_RAD) \
			* Basis(Vector3.UP, HAND_FRAME_Y_RAD)
