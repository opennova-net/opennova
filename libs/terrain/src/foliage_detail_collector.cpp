#include <terrain/foliage_detail_collector.h>

#include <cmath>
#include <cstddef>

namespace opennova {
namespace {

// [orig: Terrain_CollectNearFoliagePatches @ 0x603e60]
constexpr int kSectorSize = 512;
constexpr int kDetailCellSize = 16;
constexpr int kFirstMipLevel = 1;
constexpr int kLastMipLevel = 6;
constexpr float kMaximumDistance = 42.0f;
constexpr size_t kPatchCapacity = 128;

bool sector_atlas_origin(int sector_id, int &atlas_x, int &atlas_z) {
	switch (sector_id) {
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
			std::vector<FoliageDetailPatch> &p_patches)
		: mipchain(p_mipchain),
		  camera_x(p_camera_x),
		  camera_y(p_camera_y),
		  camera_z(p_camera_z),
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
	std::vector<FoliageDetailPatch> &patches;

	void collect_node(int atlas_x, int atlas_z,
			int world_x, int world_z,
			int size, int mip_level) {
		if (patches.size() >= kPatchCapacity) {
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

		if (size == kDetailCellSize) {
			const uint32_t x_left = static_cast<uint32_t>(world_x) & 0x7fffu;
			const uint32_t z_top =
					static_cast<uint32_t>(world_z + kDetailCellSize) & 0x7fffu;
			patches.push_back({(x_left << 16) | z_top, distance});
			return;
		}

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

	DetailPatchCollector collector(
			mipchain, camera_x, camera_y, camera_z, patches);
	collector.collect(atlas_x + node_local_x, atlas_z + node_local_z,
			world_origin_x + node_local_x, world_origin_z + node_local_z,
			node_size, mip_level);
}

} // namespace opennova
