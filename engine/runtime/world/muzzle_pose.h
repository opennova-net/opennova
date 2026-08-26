// Optional model-aware muzzle-pose seam. The portable world owns AI/fire
// policy; an asset-aware host can resolve the authored skeletal userpoint at
// the exact logic-tick call site. A missing/declined provider leaves the
// existing fresh-stamp/chest fallback authoritative.
#ifndef OPENNOVA_WORLD_MUZZLE_POSE_H
#define OPENNOVA_WORLD_MUZZLE_POSE_H

#include "world/entity.h"

#include <cstdint>

namespace opennova::world {

class World;

struct IMuzzlePoseProvider {
	virtual ~IMuzzlePoseProvider() = default;
	virtual bool resolve_muzzle_pose(
			World &world, EntityHandle entity, int32_t out[3]) = 0;
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_MUZZLE_POSE_H
