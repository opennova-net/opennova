#include <runtime/terrain/foliage_detail_collector.h>

#include <cmath>
#include <cstddef>

namespace opennova {
namespace {

// [orig: Terrain_CollectNearFoliagePatches @ 0x603e60] — the cell size,
// distance limit, and patch capacity live in the public header (shared with
// the preview dispatcher).
constexpr int kSectorSize = 512;
constexpr int kDetailCellSize = kFoliageDetailCellSize;
constexpr int kFirstMipLevel = 1;
constexpr int kLastMipLevel = 6;
constexpr float kMaximumDistance = kFoliageDetailDistanceLimit;
constexpr size_t kPatchCapacity = kFoliageDetailPatchCapacity;

bool sector_atlas_origin(int sector_id, int &atlas_x, int &atlas_z) {
	switch (sector_id) {
		case 0: // Flat sectors retain quadrant 1's raw node heights.
		case 1:
			atlas_x = 0;
			atlas_z = 0;
			return true;
		case 3:
			atlas_x = kSectorSize;
			atlas_z = 0;
			return true;
		case 2:
			atlas_x = 0;
			atlas_z = kSectorSize;
			return true;
		case 4:
			atlas_x = kSectorSize;
			atlas_z = kSectorSize;
			return true;
		default:
			return false;
	}
}

float clamped_axis_distance(float point, float minimum, float maximum) {
	if (point < minimum) {
		return minimum - point;
	}
	if (point > maximum) {
		return point - maximum;
	}
	return 0.0f;
}

class DetailPatchCollector {
public:
	DetailPatchCollector(const Mipchain &p_mipchain,
			float p_camera_x,
			float p_camera_y,
			float p_camera_z,
			uint32_t p_key_flags,
			std::vector<FoliageDetailPatch> &p_patches)
		: mipchain(p_mipchain),
		  camera_x(p_camera_x),
		  camera_y(p_camera_y),
		  camera_z(p_camera_z),
		  key_flags(p_key_flags),
		  patches(p_patches) {}

	void collect(int atlas_x, int atlas_z, int world_x, int world_z,
			int size, int mip_level) {
		collect_node(atlas_x, atlas_z, world_x, world_z, size, mip_level);
	}

private:
	const Mipchain &mipchain;
	float camera_x;
	float camera_y;
	float camera_z;
	uint32_t key_flags;
	std::vector<FoliageDetailPatch> &patches;

	void collect_node(int atlas_x, int atlas_z,
			int world_x, int world_z,
			int size, int mip_level) {
		if (patches.size() >= kPatchCapacity) {
			return;
		}

		// Retail descends every nonleaf before testing distance. An ancestor's
		// mixed-height center can be far from an in-range leaf's own center.
		// [orig: Terrain_CollectNearFoliagePatches @ 0x603E60,
		// nonleaf walk @ 0x603E67..0x603E8F; leaf gate @ 0x603F63]
		if (size != kDetailCellSize) {
			const int child_size = size / 2;
			const int child_level = mip_level + 1;
			// Recovered child order: northwest, northeast, southwest, southeast.
			collect_node(atlas_x, atlas_z,
					world_x, world_z, child_size, child_level);
			collect_node(atlas_x + child_size, atlas_z,
					world_x + child_size, world_z, child_size, child_level);
			collect_node(atlas_x, atlas_z + child_size,
					world_x, world_z + child_size, child_size, child_level);
			collect_node(atlas_x + child_size, atlas_z + child_size,
					world_x + child_size, world_z + child_size, child_size, child_level);
			return;
		}

		const int level_width = 1 << mip_level;
		const int mip_x = atlas_x / size;
		const int mip_z = atlas_z / size;
		const uint8_t *entry =
				mipchain.levels[mip_level] + (mip_z * level_width + mip_x) * 2;

		const float minimum_y = static_cast<float>(entry[0]) * 0.5f;
		const float maximum_y = static_cast<float>(entry[1]) * 0.5f;
		const float center_y = (minimum_y + maximum_y) * 0.5f;
		const float maximum_x = static_cast<float>(world_x + size);
		const float maximum_z = static_cast<float>(world_z + size);

		const float dx = clamped_axis_distance(
				camera_x, static_cast<float>(world_x), maximum_x);
		const float dy = std::fabs(center_y - camera_y);
		const float dz = clamped_axis_distance(
				camera_z, static_cast<float>(world_z), maximum_z);
		const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (distance > kMaximumDistance) {
			return;
		}

		// HIGH15 = the cell's X-min, LOW15 = its Z-min: retail packs the
		// PolyTrn sector of -camera_y (FB20, the Godot-Z axis) plus the
		// node's minimum into the low half, and the generator adds local B
		// to it. [orig: Terrain_CollectNearFoliagePatches @ 0x603f69..0x603f8a;
		// Foliage_GenerateInstances_0 @ 0x5fff84..0x5fffa2]
		const uint32_t x_min = static_cast<uint32_t>(world_x) & 0x7fffu;
		const uint32_t z_min = static_cast<uint32_t>(world_z) & 0x7fffu;
		patches.push_back({key_flags | (x_min << 16) | z_min, distance, maximum_y,
				atlas_x, atlas_z});
	}
};

} // namespace

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
		std::vector<FoliageDetailPatch> &patches) {
	if (patches.size() >= kPatchCapacity ||
			mipchain.level_count <= kLastMipLevel) {
		return;
	}
	for (int level = kFirstMipLevel; level <= kLastMipLevel; ++level) {
		if (mipchain.levels[level] == nullptr) {
			return;
		}
	}

	// The handoff node's sector-local rect selects the collector's start
	// depth: 512 = the whole-sector root, 64 = one traversal leaf's subtree.
	if (node_size < kDetailCellSize || node_size > kSectorSize ||
			(node_size & (node_size - 1)) != 0) {
		return;
	}
	int mip_level = kFirstMipLevel;
	for (int size = kSectorSize; size > node_size; size /= 2) {
		++mip_level;
	}
	if (node_local_x < 0 || node_local_z < 0 ||
			node_local_x + node_size > kSectorSize ||
			node_local_z + node_size > kSectorSize ||
			(node_local_x % node_size) != 0 || (node_local_z % node_size) != 0) {
		return;
	}

	int atlas_x = 0;
	int atlas_z = 0;
	if (!sector_atlas_origin(sector_id, atlas_x, atlas_z)) {
		return;
	}

	// The flat flag survives collection and consumes the same 128-entry
	// budget. Distance still uses raw node +52, before the generator turns
	// flagged keys into empty cache entries. [orig: Terrain_RenderVisibleSectors
	// @ 0x6090C0, flag @ 0x60924A; Terrain_CollectNearFoliagePatches
	// @ 0x603E60, raw center @ 0x603F46, packed flag @ 0x603F7E..0x603F8A]
	const uint32_t key_flags = sector_id == 0 ? 0x80000000u : 0u;
	DetailPatchCollector collector(
			mipchain, camera_x, camera_y, camera_z, key_flags, patches);
	collector.collect(atlas_x + node_local_x, atlas_z + node_local_z,
			world_origin_x + node_local_x, world_origin_z + node_local_z,
			node_size, mip_level);
}

} // namespace opennova
