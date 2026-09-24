#pragma once

// Dedicated 16u terrain-cell collection for foliage detail rendering.
// [orig: Terrain_CollectNearFoliagePatches @ 0x603e60]
//
// Retail never walks this collection radially: the frustum-culled quadtree
// traversal hands a SUBTREE to the collector at each frustum-surviving
// emitted node of LOD level >= 3 whose raw traversal distance less 16.0 is
// within the 42-unit foliage limit [orig: Terrain_TraverseQuadtreeNode
// handoff @ 0x60905c..0x60907c]. Cells behind the camera therefore never enter the
// visible-key list, which keeps the far-slot pool's working set below its
// 16-bit-index capacity (see foliage-re.md D-FOLIAGE-13).

#include <runtime/terrain/quadtree.h>

#include <cstdint>
#include <vector>

namespace opennova {

// The witnessed detail-collection scheme [orig:
// Terrain_CollectNearFoliagePatches @ 0x603e60]: detail foliage lives in 16u
// terrain cells, collected only within the 42-unit foliage limit, into a
// visible-key list capped at 128 patches. One home for the collector AND the
// shell-side preview dispatcher.
constexpr int kFoliageDetailCellSize = 16;
constexpr float kFoliageDetailDistanceLimit = 42.0f;
constexpr int kFoliageDetailPatchCapacity = 128;

struct FoliageDetailPatch {
	uint32_t key = 0;
	float distance = 0.0f;
	// The collected leaf's maximum height (the node's AABB max y at
	// node+0x28): the detail passes split patches by it against the water
	// [orig: Foliage_RenderFarPatches @ 0x60a1a0..0x60a1a2].
	float max_height = 0.0f;
};

// Appends the near 16u cells of one quadtree subtree of a resolved 512u
// world sector. (node_local_x, node_local_z, node_size) is the handoff
// node's sector-local rect: (0, 0, 512) collects the whole sector, a 64u
// leaf collects its own cells. node_size must be a power of two in
// [16, 512]. Collection order and the 128-entry capacity apply to the whole
// in/out vector. Sector 0 uses quadrant 1's raw mip heights and sets key
// bit 31: flat cells occupy list/cache slots but generate no foliage geometry.
void collect_foliage_detail_patches(
		const Mipchain &mipchain,
		int sector_id,
		int world_origin_x,
		int world_origin_z,
		int node_local_x,
		int node_local_z,
		int node_size,
		float camera_x,
		float camera_y,
		float camera_z,
		std::vector<FoliageDetailPatch> &patches);

} // namespace opennova
