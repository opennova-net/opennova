// A collision volume's convex solid (formats/threedi/threedi_volume_solid.h), on synthetic planes: a
// box's six planes make the box (six facets, its eight corners), five planes bound no solid, and planes
// reaching past the volume's stored box are clipped to it.
#include <formats/threedi/threedi_volume_solid.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "common/test_expect.h"

using namespace opennova::threedi;

namespace {

// How far a volume's solid reaches past its stored box (0 inside).
double past_box(const std::vector<std::vector<ThreediBuildVec3>> &polygons, const ThreediBoundingVolume &v) {
	const double lo[3] = {v.min_x_fp16 / 65536.0, v.min_y_fp16 / 65536.0, v.min_z_fp16 / 65536.0};
	const double hi[3] = {v.max_x_fp16 / 65536.0, v.max_y_fp16 / 65536.0, v.max_z_fp16 / 65536.0};
	double past = 0.0;
	for (const auto &polygon : polygons)
		for (const ThreediBuildVec3 &p : polygon) {
			const double at[3] = {p.x, p.y, p.z};
			for (int k = 0; k < 3; ++k) past = std::max(past, std::max(lo[k] - at[k], at[k] - hi[k]));
		}
	return past;
}

// A volume's solid lies within its stored box (2 mm: retail's planes meet a few tenths of a millimetre
// off, the box is 16.16).
bool inside_box(const std::vector<std::vector<ThreediBuildVec3>> &polygons, const ThreediBoundingVolume &v) {
	return past_box(polygons, v) <= 0.002;
}

int box_planes_make_the_box() {
	// The six planes of the box -1..2 x 0..1 x 0..3 (n . p + d <= 0 inside).
	ThreediBoundingPlane planes[6] = {};
	const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	const float d[6] = {-2, -1, -1, 0, -3, 0};
	for (int i = 0; i < 6; ++i) {
		std::copy(n[i], n[i] + 3, planes[i].normal);
		planes[i].radius = d[i];
	}
	const auto polygons = threedi_volume_polygons(planes, 6, false);
	TEST_EXPECT(polygons.size() == 6);
	std::set<std::string> corners;
	for (const auto &polygon : polygons) {
		TEST_EXPECT(polygon.size() == 4);
		for (const ThreediBuildVec3 &p : polygon) {
			char key[64];
			// Rounded to the millimetre, + 0.0: a -0 prints as "-0.000".
			const auto mm = [](double v) { return std::round(v * 1000.0) / 1000.0 + 0.0; };
			std::snprintf(key, sizeof(key), "%.3f %.3f %.3f", mm(p.x), mm(p.y), mm(p.z));
			corners.insert(key);
		}
	}
	TEST_EXPECT(corners.size() == 8 && corners.count("-1.000 0.000 0.000") && corners.count("2.000 1.000 3.000"));
	// Five planes bound no solid; a ladder of them keeps its facing (plane 0) when it has a polygon.
	TEST_EXPECT(threedi_volume_polygons(planes, 5, false).empty());
	// Planes reaching past the volume's box: its solid is the part within the box (the quick test).
	ThreediBoundingVolume within{};
	within.collidable_type = 1;
	within.plane_count = 6;
	within.max_x_fp16 = within.max_y_fp16 = within.max_z_fp16 = 0x10000;
	const auto clipped = threedi_volume_solid(within, planes);
	TEST_EXPECT(!clipped.empty() && inside_box(clipped, within));
	TEST_EXPECT(threedi_volume_solid(within, planes).size() >= 6);
	std::printf("volumes: a box's planes make the box (6 facets, 8 corners)\n");
	return 0;
}

// Two forms the shipped volumes take (the retired editor_model_collision_retail sweep reached
// them; 2026-10-19): a corner chamfered by under a millimetre, whose two corners on the top facet
// straddle the facet's winding seam (they sort first and last and collapse to one), and a ladder
// of no thickness, whose facing (plane 0) is all it keeps.
int chamfer_and_flat_ladder() {
	// The box -1..1 x -1..1 x 0..1 with its (-1, -1) edge cut by a plane 0.5 mm in, square to
	// the diagonal.
	ThreediBoundingPlane planes[7] = {};
	const float n[7][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
		{-0.70710677f, -0.70710677f, 0}};
	const float d[7] = {-1, -1, -1, -1, -1, 0, -0.70710677f * (2.0f - 0.0005f)};
	for (int i = 0; i < 7; ++i) {
		std::copy(n[i], n[i] + 3, planes[i].normal);
		planes[i].radius = d[i];
	}
	const auto polygons = threedi_volume_polygons(planes, 7, false);
	// The top facet keeps the cut corner once: the 0.5 mm pair is one cluster across the seam.
	size_t top_corners = 0;
	for (const auto &polygon : polygons) {
		bool top = true;
		for (const ThreediBuildVec3 &p : polygon) top = top && std::fabs(p.z - 1.0) < 1e-6;
		if (top) top_corners = polygon.size();
	}
	TEST_EXPECT(top_corners == 4);
	// A ladder of no thickness: facing +x and -x at x = 0, bounded in y and z. Its side planes
	// hold no area, so it bounds no solid and keeps its facing alone.
	ThreediBoundingPlane ladder[6] = {};
	const float ln[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {-1, 0, 0}};
	const float ld[6] = {0, -0.5f, -0.5f, -3, 0, 0};
	for (int i = 0; i < 6; ++i) {
		std::copy(ln[i], ln[i] + 3, ladder[i].normal);
		ladder[i].radius = ld[i];
	}
	const auto facing = threedi_volume_polygons(ladder, 6, true);
	TEST_EXPECT(facing.size() == 1 && facing[0].size() == 4);
	for (const ThreediBuildVec3 &p : facing.empty() ? std::vector<ThreediBuildVec3>{} : facing[0])
		TEST_EXPECT(std::fabs(p.x) < 1e-6);
	TEST_EXPECT(threedi_volume_polygons(ladder, 6, false).empty());
	std::printf("volumes: a sub-millimetre chamfer collapses across the seam; a flat ladder keeps its facing\n");
	return 0;
}

} // namespace

int main() {
	if (box_planes_make_the_box() != 0) return 1;
	return chamfer_and_flat_ladder();
}
