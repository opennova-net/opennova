#include <terrain/foliage_detail_collector.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool expect_patch(const opennova::FoliageDetailPatch &patch,
		uint32_t key, float distance, const char *message) {
	if (patch.key == key && std::fabs(patch.distance - distance) <= 0.0001f) {
		return true;
	}
	std::fprintf(stderr,
			"FAIL: %s (got key 0x%08x, distance %.6f; expected 0x%08x, %.6f)\n",
			message, patch.key, patch.distance, key, distance);
	return false;
}

void make_quadrant_mipchain(opennova::Mipchain &mipchain,
		const uint8_t min_height[4], const uint8_t max_height[4]) {
	constexpr int kLevelCount = 7;
	size_t byte_count = 0;
	for (int level = 0; level < kLevelCount; ++level) {
		const size_t width = size_t{1} << level;
		byte_count += width * width * 2;
	}

	mipchain = {};
	mipchain.data.resize(byte_count);
	mipchain.level_count = kLevelCount;

	size_t offset = 0;
	for (int level = 0; level < kLevelCount; ++level) {
		const int width = 1 << level;
		const int half = width > 1 ? width / 2 : 1;
		mipchain.levels[level] = mipchain.data.data() + offset;
		for (int z = 0; z < width; ++z) {
			for (int x = 0; x < width; ++x) {
				const int quadrant = (x >= half ? 1 : 0) + (z >= half ? 2 : 0);
				uint8_t *entry = mipchain.levels[level] + (z * width + x) * 2;
				entry[0] = min_height[quadrant];
				entry[1] = max_height[quadrant];
			}
		}
		offset += static_cast<size_t>(width) * width * 2;
	}
}

} // namespace

int main() {
	bool ok = true;

	// The recovered distance clamps X/Z to the node AABB but measures Y from
	// the node center. These bounds are y=[0,20], so the center is y=10.
	const uint8_t ranged_min[4] = {0, 0, 0, 0};
	const uint8_t ranged_max[4] = {40, 40, 40, 40};
	opennova::Mipchain ranged;
	make_quadrant_mipchain(ranged, ranged_min, ranged_max);

	std::vector<opennova::FoliageDetailPatch> patches;
	opennova::collect_foliage_detail_patches(
			ranged, 1, 0, 0, 0, 0, 512,  24.0f, 52.0f, 40.0f, patches);
	ok &= expect(patches.size() == 1,
			"Y-center distance 42 must select exactly the containing 16u cell");
	if (patches.size() == 1) {
		ok &= expect_patch(patches[0], 0x00100030u, 42.0f,
				"detail key must pack high15=X-left and low15=Z-top");
	}

	patches.clear();
	opennova::collect_foliage_detail_patches(
			ranged, 1, 0, 0, 0, 0, 512,  24.0f, 52.125f, 40.0f, patches);
	ok &= expect(patches.empty(), "distance above 42 must be rejected");

	patches.clear();
	opennova::collect_foliage_detail_patches(
			ranged, 1, 0, 0, 0, 0, 512,  -42.0f, 10.0f, 8.0f, patches);
	ok &= expect(patches.size() == 1,
			"X-to-AABB distance 42 must be included");
	if (patches.size() == 1) {
		ok &= expect_patch(patches[0], 0x00000010u, 42.0f,
				"horizontal clamp must retain the first 16u cell at the threshold");
	}

	// Each sector ID selects one 512u atlas quadrant. The camera is exactly
	// 42 units above that quadrant's node center, leaving one accepted cell.
	const uint8_t quadrant_height[4] = {20, 40, 60, 80};
	opennova::Mipchain quadrants;
	make_quadrant_mipchain(quadrants, quadrant_height, quadrant_height);
	const struct {
		int sector_id;
		float camera_y;
	} quadrant_vectors[] = {
		{1, 52.0f},
		{3, 62.0f},
		{2, 72.0f},
		{4, 82.0f},
	};
	for (const auto &vector : quadrant_vectors) {
		patches.clear();
		opennova::collect_foliage_detail_patches(
				quadrants, vector.sector_id, 0, 0, 0, 0, 512,
				24.0f, vector.camera_y, 40.0f, patches);
		ok &= expect(patches.size() == 1,
				"sector ID must select its recovered atlas quadrant");
		if (patches.size() == 1) {
			ok &= expect_patch(patches[0], 0x00100030u, 42.0f,
					"quadrant lookup must not change the world-space detail key");
		}
	}

	patches.clear();
	opennova::collect_foliage_detail_patches(
			ranged, 1, 512, -512, 0, 0, 512,  536.0f, 52.0f, -472.0f, patches);
	ok &= expect(patches.size() == 1,
			"translated sector must retain the same local 16u selection");
	if (patches.size() == 1) {
		ok &= expect_patch(patches[0], 0x02107e30u, 42.0f,
				"signed world coordinates must wrap into the two 15-bit key fields");
	}

	// Starting one slot below the recovered global capacity makes the first
	// NW leaf observable and proves collection stops exactly at 128.
	patches.assign(127, opennova::FoliageDetailPatch{0x12345678u, -1.0f});
	opennova::collect_foliage_detail_patches(
			ranged, 1, 0, 0, 0, 0, 512,  16.0f, 10.0f, 16.0f, patches);
	ok &= expect(patches.size() == 128,
			"detail collection must stop at the global 128-patch capacity");
	if (patches.size() == 128) {
		ok &= expect_patch(patches.back(), 0x00000010u, 0.0f,
				"NW/NE/SW/SE recursion must visit the northwest leaf first");
	}

	// Subtree handoff [orig: Terrain_TraverseQuadtreeNode @ 0x60905c..0x60907c]:
	// a frustum-surviving emitted node hands only ITS rect to the collector.
	// The 16u cell containing the camera collects alone with distance 0.
	patches.clear();
	opennova::collect_foliage_detail_patches(
			ranged, 1, 0, 0, 16, 32, 16, 24.0f, 10.0f, 40.0f, patches);
	ok &= expect(patches.size() == 1,
			"a 16u subtree handoff must collect exactly its own cell");
	if (patches.size() == 1) {
		ok &= expect_patch(patches[0], 0x00100030u, 0.0f,
				"the subtree cell key must match the whole-sector walk's key");
	}

	patches.clear();
	opennova::collect_foliage_detail_patches(
			ranged, 1, 0, 0, 448, 448, 64, 24.0f, 10.0f, 40.0f, patches);
	ok &= expect(patches.empty(),
			"a far subtree must prune on its own clamped-AABB distance");

	patches.clear();
	opennova::collect_foliage_detail_patches(
			ranged, 1, 0, 0, 8, 0, 64, 24.0f, 10.0f, 40.0f, patches);
	ok &= expect(patches.empty(),
			"a misaligned subtree rect must be rejected outright");

	if (!ok) {
		return 1;
	}
	std::puts("OK: terrain foliage detail collection matches recovered 16u vectors");
	return 0;
}
