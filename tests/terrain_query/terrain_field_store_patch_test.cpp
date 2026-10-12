// TerrainFieldStore::patch_heights: a rect of the heights the store holds
// written in place (an editor's sculpt stroke), so every copy of the height
// view reads the new ground with no rebuild.
//
// Covers, over a small synthetic atlas:
//   1. The rect's texels take the source's, `stride` samples a row; every other
//      texel keeps its height.
//   2. The view keeps pointing at the same buffer: a copy taken before the
//      patch (the world's, the kernel's) reads the new heights.
//   3. A rect not inside the heights, a short stride, no source and an empty
//      store are refused with nothing written.

#include <formats/cpt/cpt.h>
#include <formats/trn/trn.h>
#include <runtime/terrain_query/coords.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/terrain_query/terrain_field_build.h>
#include <runtime/terrain_query/terrain_field_store.h>

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace opennova;

static int g_fail = 0;

static void check(bool cond, const char *msg) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		g_fail = 1;
	}
}

namespace {

constexpr int kDim = 8;

uint16_t initial(int x, int z) { return static_cast<uint16_t>((z * kDim + x) * 256); }

CptFile make_cpt() {
	CptFile cpt;
	cpt.depth_buffer.resize(static_cast<size_t>(kDim) * kDim);
	for (int z = 0; z < kDim; ++z)
		for (int x = 0; x < kDim; ++x) cpt.depth_buffer[static_cast<size_t>(z) * kDim + x] = initial(x, z);
	return cpt;
}

TrnConfig make_trn() {
	TrnConfig trn;
	trn.sector_count = 1;
	trn.sector_rows = 1;
	for (int r = 0; r < 16; ++r)
		for (int c = 0; c < 16; ++c) trn.sector_grid[r][c] = 1;
	return trn;
}

uint16_t at(const terrain::TerrainHeightField &field, int x, int z) {
	return field.heightmap[static_cast<size_t>(z) * static_cast<size_t>(field.dim) + static_cast<size_t>(x)];
}

bool untouched(const terrain::TerrainHeightField &field) {
	for (int z = 0; z < kDim; ++z)
		for (int x = 0; x < kDim; ++x)
			if (at(field, x, z) != initial(x, z)) return false;
	return true;
}

void test_patch_in_place() {
	terrain::TerrainFieldStore store;
	terrain::terrain_field_store_build(store, make_cpt(), make_trn());
	check(store.valid(), "the store builds over the synthetic atlas");
	const terrain::TerrainHeightField before = store.height_field();
	const uint16_t *buffer = before.heightmap;

	// A 3 x 2 rect at (2, 4) from a source 5 samples a row.
	const int stride = 5;
	std::vector<uint16_t> src(static_cast<size_t>(stride) * 2);
	for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint16_t>(60000 + i);
	check(store.patch_heights(terrain::CoordsRect{2, 4, 3, 2}, src.data(), stride), "an inside rect patches");

	const terrain::TerrainHeightField &after = store.height_field();
	check(after.heightmap == buffer, "the view keeps its buffer");
	for (int z = 0; z < kDim; ++z) {
		for (int x = 0; x < kDim; ++x) {
			const bool inside = x >= 2 && x < 5 && z >= 4 && z < 6;
			const uint16_t want = inside ? src[static_cast<size_t>(z - 4) * stride + (x - 2)] : initial(x, z);
			if (at(after, x, z) != want) {
				std::fprintf(stderr, "FAIL: texel (%d, %d) = %u, want %u\n", x, z, at(after, x, z), want);
				g_fail = 1;
			}
			// The copy taken before the patch reads the same buffer.
			check(at(before, x, z) == at(after, x, z), "a copy of the view reads the patched heights");
		}
	}
}

void test_refusals() {
	terrain::TerrainFieldStore store;
	const std::vector<uint16_t> src(kDim * kDim, 1);
	check(!store.patch_heights(terrain::CoordsRect{0, 0, 1, 1}, src.data(), kDim), "an empty store refuses");

	terrain::terrain_field_store_build(store, make_cpt(), make_trn());
	const terrain::CoordsRect refused[] = {
			{-1, 0, 2, 2}, {0, -1, 2, 2}, {7, 0, 2, 1}, {0, 7, 1, 2}, {0, 0, 0, 1}, {0, 0, 1, 0}, {0, 0, 9, 1},
	};
	for (const terrain::CoordsRect &rect : refused)
		check(!store.patch_heights(rect, src.data(), kDim), "a rect not inside the heights is refused");
	check(!store.patch_heights(terrain::CoordsRect{0, 0, 4, 1}, src.data(), 3), "a stride short of the rect is refused");
	check(!store.patch_heights(terrain::CoordsRect{0, 0, 1, 1}, nullptr, 1), "no source is refused");
	check(untouched(store.height_field()), "a refused patch writes nothing");

	// The whole atlas is a rect inside it.
	check(store.patch_heights(terrain::CoordsRect{0, 0, kDim, kDim}, src.data(), kDim), "the whole atlas patches");
	check(at(store.height_field(), kDim - 1, kDim - 1) == 1, "the last texel written");
}

} // namespace

int main() {
	test_patch_in_place();
	test_refusals();
	if (g_fail == 0) std::printf("terrain_field_store_patch: all tests passed\n");
	return g_fail;
}
