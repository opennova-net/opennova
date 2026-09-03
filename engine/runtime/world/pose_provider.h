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

namespace opennova::world {

class World;
struct Seat;
struct MountedPose;
struct CollisionMatrix;
struct CollisionModel;

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

	// --- muzzles / userpoints ------------------------------------------------
	// A person's launch userpoint on its posed skeleton (16.16 world).
	// [orig: Entity_GetAttachmentWorldPosition @0x4b2670]
	virtual bool resolve_muzzle_pose(World &, EntityHandle, int32_t /*out*/[3]) {
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
	// A userpoint (1-based table index) through the entity placement matrix
	// ALONE, no part pose: the aim/LOS origin's TARGET point (16.16 world).
	// [orig: Entity_ComputeWeaponFireOrigin @0x43b5f6 Math_FixedPointTransformPoint22
	//  (entity+0xB4, userpoint record)]
	virtual bool resolve_userpoint_rigid(
			World &, EntityHandle, int /*userpoint_index*/, int32_t /*out*/[3]) {
		return false;
	}

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
