// The one model-aware pose seam between the portable world and the
// asset-aware embedder: a rider's live seat-bone pose, the muzzle/userpoint
// origins fire resolves against, and the per-section matrices every
// collision walk consumes. The world owns policy and asks at the exact
// logic-tick call site -- retail computes each of these synchronously at
// every consumer. A missing or declining provider leaves the entity's raw
// origin/euler authoritative (retail's own fallback
// [orig: Entity_GetAttachmentWorldPosition @0x4b2767; the raw copy
//  @0x545e1f]), the static seat geometry for riders, and the shared
// entity matrix for collision sections.
#pragma once

#include <runtime/world/entity.h>

#include <cstdint>
#include <vector>

namespace opennova::threedi {
struct Threedi3di3;
}

namespace opennova::world {

class World;
struct Seat;
struct MountedPose;
struct CollisionMatrix;
struct CollisionModel;

// Optional matrices returned by Entity_BuildBoneTransformMatrices @0x4B1290.
// The weapon anchor includes its hand-pivot nudge; Head includes the face offset.
enum class SkeletalAnchor : uint8_t { HeldWeapon, Head };

class IPoseProvider {
public:
	virtual ~IPoseProvider() = default;

	// --- seats ---------------------------------------------------------------
	// A rider's pose on the carrier's live seat bone (vehicle_mount.h names
	// the two retail attach paths). False keeps the static seat geometry.
	virtual bool resolve_mounted_pose(World &, const Entity & /*carrier*/,
			const Seat &, MountedPose &) {
		return false;
	}
	// The ordinary-seat lookup on the carrier's model by 1-based bone
	// (userpoint) index. False exactly where retail's lookup fails: no model
	// (the husk's while the husk bit is set and one is attached, else the
	// intact model), bone 0 or past the model's bone count, or no collision
	// block with bounding volumes behind the model. A provider without the
	// carrier's model data answers true: the static seat geometry stands.
	// [orig: Entity_GetBoneTransformAndOrientation @0x4B0C50]
	virtual bool resolve_seat_bone(World &, const Entity & /*carrier*/, int /*bone_index*/) {
		return true;
	}

	// --- muzzles / userpoints ------------------------------------------------
	// A person's launch userpoint on its posed skeleton (16.16 world).
	// [orig: Entity_GetAttachmentWorldPosition @0x4b2670]
	virtual bool resolve_muzzle_pose(World &, EntityHandle, int32_t /*out*/[3]) {
		return false;
	}
	// Organic userpoint through the skeletal pose. Its orientation remains
	// the entity's live yaw/pitch/roll. Zero is the caller's raw-pose fallback.
	// [orig: Entity_GetAttachmentWorldPosition @0x4B2670]
	virtual bool resolve_organic_attachment(World &, EntityHandle, uint8_t, int32_t[3]) {
		return false;
	}
	// The posed hand/head attachment origin used by organic escort and dragging.
	// False retains the raw entity origin, as the no-model branch does.
	// [orig: Entity_BuildBoneTransformMatrices @0x4B1290]
	virtual bool resolve_skeletal_anchor(World &, EntityHandle, SkeletalAnchor, int32_t[3]) {
		return false;
	}
	// A vehicle/eweap userpoint (1-based table index) through the model's
	// current PANM part pose: out = {x, y, z (16.16), yaw, pitch, roll (BAM32,
	// the posed bone's euler)}. [orig: Entity_ComputeUserpointWorldTransform
	//  @0x545c60 -> Userpoint_ComputeWorldTransform @0x56c420]
	virtual bool resolve_userpoint_transform(
			World &, EntityHandle, int /*userpoint_index*/, int32_t /*out*/[6]) {
		return false;
	}
	// The same transform over any model posed through the entity: `model`
	// null is the entity's own userpoint model, else that model (a fired
	// weapon's third-person gfx3) through the entity's placement and CTRL
	// publication. A non-null `out_direction` also receives the record's
	// authored direction through the posed bone (16.16), and the reported
	// euler turns by that direction's local yaw/pitch; without it the euler
	// is the posed bone's own. The default serves the plain query only.
	// [orig: Userpoint_ComputeWorldTransform @0x56c420 (modelData argument;
	//  the direction leg @0x56C524..0x56C5F4)]
	virtual bool resolve_userpoint_frame(World &world, EntityHandle entity,
			const threedi::Threedi3di3 *model, int userpoint_index, int32_t out[6],
			int32_t out_direction[3]) {
		return model == nullptr && out_direction == nullptr &&
				resolve_userpoint_transform(world, entity, userpoint_index, out);
	}
	// Named model point through the live part pose, used by AI entry walks.
	// [orig: Entity_GetBoneTransformAndOrientation @0x4B0C50]
	virtual bool resolve_named_transform(World &, EntityHandle, const char *, int32_t[6]) {
		return false;
	}
	// Last matching model userpoint, independent of whether a posed matrix
	// is available. [orig: Entity_FindAttachBone @0x4B9580]
	virtual int last_named_userpoint(World &, EntityHandle, const char *) { return 0; }
	// A userpoint (1-based table index) through the entity placement matrix
	// ALONE, no part pose: the aim/LOS origin's TARGET point (16.16 world).
	// [orig: Entity_ComputeWeaponFireOrigin @0x43b5f6 Math_FixedPointTransformPoint22
	//  (entity+0xB4, userpoint record)]
	virtual bool resolve_userpoint_rigid(
			World &, EntityHandle, int /*userpoint_index*/, int32_t /*out*/[3]) {
		return false;
	}

	// The unposed COBJ pivot of a userpoint's part, through placement only.
	// The turret solve aims from this pivot before resolving its live muzzle.
	// [orig: Entity_ComputeWeaponFireTransform_0 @0x456980]
	virtual bool resolve_userpoint_pivot(World &, EntityHandle, int, int32_t[3]) { return false; }

	// Unposed COBJ pivot through the entity placement, for door transition sound.
	// [orig: Entity_ProcessSectionDamageTransition @0x43F496..0x43F501]
	virtual bool resolve_section_pivot(World &, EntityHandle, int, int32_t[3]) { return false; }

	// --- collision sections --------------------------------------------------
	// An embedder may learn about dynamic entities after its mission-start
	// model sweep (notably the local player deploy). Query callers get one
	// shared, idempotent way to attach that entity before choosing an
	// unresolved fallback. True means the provider attached a usable instance.
	virtual bool ensure_collision_instance(World &, EntityHandle) { return false; }
	// The final world-space matrix array every collision walk consumes. Matrix
	// slot i corresponds to COBJ/collision section i by ordinal;
	// COBJ::parent_subobject_index is hierarchy metadata, not a selector.
	// View construction may cache the result: implementations treat the
	// queried CollisionWorld's model/instance/pose state as read-only here;
	// late attachment belongs in ensure_collision_instance().
	// [orig: model+168 callback -> one 16-dword matrix per COBJ, consumed in
	//  lockstep by Physics_RaycastAgainstBoneCollision @ 0x4e4cb0.]
	virtual bool build_section_matrices(World &, EntityHandle, int32_t /*model_id*/,
			const CollisionMatrix & /*entity_world*/, const CollisionModel &,
			std::vector<CollisionMatrix> & /*out*/) {
		return false;
	}
};

} // namespace opennova::world
