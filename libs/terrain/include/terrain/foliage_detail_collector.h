#pragma once

// Dedicated 16u terrain-cell collection for foliage detail rendering.
// [orig: Terrain_CollectNearFoliagePatches @ 0x603e60]

#include <terrain/quadtree.h>

#include <cstdint>
#include <vector>

namespace opennova {

struct FoliageDetailPatch {
	uint32_t key = 0;
	float distance = 0.0f;
};

// Appends the near 16u cells for one resolved 512u world sector. Collection
// order and the 128-entry capacity apply to the whole in/out vector.
void collect_foliage_detail_patches(
		const Mipchain &mipchain,
		int sector_id,
		int world_origin_x,
		int world_origin_z,
		float camera_x,
		float camera_y,
		float camera_z,
		std::vector<FoliageDetailPatch> &patches);

} // namespace opennova
