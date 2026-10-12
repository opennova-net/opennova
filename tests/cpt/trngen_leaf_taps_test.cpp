// The region bake's reach (engine/formats/cpt/trngen/terrain_region_bake.h): which leaves read an edited
// depth texel (a leaf's 65 x 65 vertices read its corner plus 0 to 64 on each axis, wrapping at 1024, a
// locked axis of its quadrant wrapping within its 512, as TrnGen's vertex expansion reads them), each
// leaf's footprint (what its draw-back can write: its square, cut at the last row and column, no wrap),
// and the .cpt tile index of a node (the levels' offsets 0, 1, 5, 21, 85, rows outer).
#include <formats/cpt/trngen/terrain_region_bake.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

using namespace opennova::trngen;

namespace {

int failures = 0;

void expect(bool ok, const std::string &what) {
	if (!ok) {
		++failures;
		std::fprintf(stderr, "FAIL: %s\n", what.c_str());
	}
}

int leaf(int x, int y) { return cpt_tile_index(4, x, y); }

std::string list(const std::vector<int> &tiles) {
	std::string out;
	for (int t : tiles) {
		const DepthRect r = leaf_footprint(t);
		out += "(" + std::to_string(r.x) + "," + std::to_string(r.y) + ")";
	}
	return out;
}

void expect_leaves(const std::vector<DepthRect> &rects, const std::array<CornerLockFlags, 4> &locks,
                   std::vector<int> want, const std::string &what) {
	std::sort(want.begin(), want.end());
	const std::vector<int> got = leaves_tapping(rects, locks);
	expect(got == want, what + ": got " + list(got) + ", want " + list(want));
}

void test_tile_index() {
	expect(cpt_tile_index(0, 0, 0) == 0, "the root is tile 0");
	expect(cpt_tile_index(1, 0, 0) == 1 && cpt_tile_index(1, 512, 0) == 2 && cpt_tile_index(1, 0, 512) == 3 &&
	           cpt_tile_index(1, 512, 512) == 4,
	       "the quadrants are tiles 1 to 4, rows outer");
	expect(cpt_tile_index(2, 256, 0) == 6 && cpt_tile_index(3, 0, 128) == 21 + 8, "depths 2 and 3 start at 5 and 21");
	expect(cpt_tile_index(4, 0, 0) == 85 && cpt_tile_index(4, 960, 960) == 340, "the leaves are tiles 85 to 340");
	const DepthRect first = leaf_footprint(85), last = leaf_footprint(340), mid = leaf_footprint(leaf(320, 128));
	expect(first.x == 0 && first.y == 0 && first.w == 65 && first.h == 65, "leaf (0, 0)'s footprint is its 65 x 65");
	expect(last.x == 960 && last.y == 960 && last.w == 64 && last.h == 64,
	       "the last leaf's footprint stops at 1023 (the draw-back does not wrap)");
	expect(mid.x == 320 && mid.y == 128 && mid.w == 65, "a leaf's footprint is at its corner");
}

void test_taps() {
	const std::array<CornerLockFlags, 4> none{};
	expect_leaves({{300, 140, 5, 5}}, none, {leaf(256, 128)}, "a texel inside a leaf is that leaf's alone");
	expect_leaves({{320, 140, 1, 1}}, none, {leaf(256, 128), leaf(320, 128)},
	              "a texel on a 64 column is read by the leaves on both sides");
	expect_leaves({{320, 128, 1, 1}}, none, {leaf(256, 64), leaf(320, 64), leaf(256, 128), leaf(320, 128)},
	              "a texel on a 64 corner is read by four leaves");
	expect_leaves({{0, 300, 1, 1}}, none, {leaf(0, 256), leaf(960, 256)},
	              "column 0 is read by leaf 0 and, across the wrap, by the last column's leaf");
	expect_leaves({{1023, 300, 1, 1}}, none, {leaf(960, 256)}, "column 1023 is the last leaf's alone");
	expect_leaves({{0, 0, 1, 1}}, none, {leaf(0, 0), leaf(960, 0), leaf(0, 960), leaf(960, 960)},
	              "texel (0, 0) is read by the four corner leaves across both wraps");
	expect_leaves({{100, 100, 0, 5}, {600, 600, 5, 0}}, none, {}, "an empty rectangle reaches nothing");
	expect_leaves({{300, 140, 5, 5}, {302, 142, 2, 2}, {700, 700, 1, 1}}, none, {leaf(256, 128), leaf(640, 640)},
	              "several rectangles, overlapping, each leaf once");

	// Locks: the top-left quadrant locks x, the top-right x and y.
	std::array<CornerLockFlags, 4> locks{};
	locks[0] = {1, 0};
	locks[1] = {1, 1};
	expect_leaves({{0, 100, 1, 1}}, locks, {leaf(0, 64), leaf(448, 64)},
	              "a locked x reads the quadrant's column 0 from its last leaf, not across the map");
	expect_leaves({{512, 100, 1, 1}}, locks, {leaf(512, 64), leaf(960, 64)},
	              "the top-right quadrant's column 512 is read by its own first and last leaves");
	expect_leaves({{512, 0, 1, 1}}, locks,
	              {leaf(512, 0), leaf(960, 0), leaf(512, 448), leaf(960, 448), leaf(448, 960), leaf(512, 960)},
	              "a locked y reads the quadrant's row 0 from its own last row of leaves; the unlocked bottom "
	              "quadrants read it across the map's wrap");
	expect_leaves({{448, 100, 1, 1}}, locks, {leaf(384, 64), leaf(448, 64)},
	              "inside a locked quadrant a 64 column is read by both sides as usual");
}

} // namespace

int main() {
	test_tile_index();
	test_taps();
	if (failures) return 1;
	std::printf("OK: the region bake's leaf taps, footprints and tile indices\n");
	return 0;
}
