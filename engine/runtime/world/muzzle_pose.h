// The model-aware muzzle/userpoint seam. The portable world owns AI/fire
// policy; the asset-aware embedder resolves the authored userpoints against
// the sim's own pose at the exact logic-tick call site — retail computes the
// fire origin synchronously at every consumer. A missing/declined provider
// leaves the entity's raw origin/euler authoritative (retail's own fallback
// [orig: Entity_GetAttachmentWorldPosition @0x4b2767; the raw copy
//  @0x545e1f]).
#ifndef OPENNOVA_WORLD_MUZZLE_POSE_H
#define OPENNOVA_WORLD_MUZZLE_POSE_H

#include <runtime/world/entity.h>

#include <cstdint>

namespace opennova::world {

class World;

struct IMuzzlePoseProvider {
	virtual ~IMuzzlePoseProvider() = default;
	// A person's launch userpoint on its posed skeleton (16.16 world).
	// [orig: Entity_GetAttachmentWorldPosition @0x4b2670]
	virtual bool resolve_muzzle_pose(
			World &world, EntityHandle entity, int32_t out[3]) = 0;
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
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_MUZZLE_POSE_H
