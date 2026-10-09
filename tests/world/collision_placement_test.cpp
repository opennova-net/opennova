// The collision placement and probe helpers (runtime/world/collision.h): the uniform item scale on
// a placement's rotation (collision_matrix_scale_rotation [orig: @0x614210]), the placement from
// mission eulers (the heading table path for pure yaw, the Euler builder with pitch or roll [orig:
// @ 0x613f40]), a local box placed (collision_matrix_box_bounds), the ground-settle tail's probe
// origin ((z + 6143) & ~0x17FF [orig: Entity_MovementCollisionResolver @0x4B3D6E..0x4B3DA9]) and
// the terrain height under a column (terrain_column_height).
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "common/test_expect.h"

using namespace opennova::world;

namespace {

constexpr int32_t fx(double units) { return static_cast<int32_t>(units * 65536.0); }

bool same_matrix(const CollisionMatrix &a, const CollisionMatrix &b) {
	for (int i = 0; i < 16; ++i)
		if (a.m[i] != b.m[i]) return false;
	return true;
}

bool near_fixed(int32_t a, int32_t b, int32_t slack = 64) { return std::abs(a - b) <= slack; }

int test_scale() {
	const int32_t pos[3] = {fx(10), fx(20), fx(30)};
	const CollisionMatrix base = collision_matrix_from_euler(0x10000000, 0x01000000, 0x02000000, pos);
	CollisionMatrix scaled = base;
	collision_matrix_scale_rotation(scaled, 0);
	TEST_EXPECT(same_matrix(scaled, base));
	collision_matrix_scale_rotation(scaled, 0x20000); // x2
	for (const int i : {0, 1, 2, 4, 5, 6, 8, 9, 10})
		TEST_EXPECT(scaled.m[i] == static_cast<int32_t>((static_cast<int64_t>(base.m[i]) * 0x20000) >> 16));
	for (const int i : {3, 7, 11, 12, 13, 14, 15}) TEST_EXPECT(scaled.m[i] == base.m[i]);
	std::printf("scale: the nine rotation entries scaled, the translation kept, 0 unscaled\n");
	return 0;
}

int test_placement() {
	const int32_t pos[3] = {fx(1), fx(2), fx(3)};
	// Pure yaw: the heading table path, scaled.
	CollisionMatrix yaw = collision_matrix_from_heading(bam_heading_from_mission_yaw_deg(30.0), pos);
	collision_matrix_scale_rotation(yaw, 0x18000);
	TEST_EXPECT(same_matrix(collision_matrix_from_placement(30.0, 0.0, 0.0, pos, 0x18000), yaw));
	// Pitch or roll authored: the Euler builder.
	const CollisionMatrix tilted = collision_matrix_from_euler(bam_heading_from_mission_yaw_deg(30.0),
	                                                           bam_from_degrees_wrapped(5.0), bam_from_degrees_wrapped(0.0), pos);
	TEST_EXPECT(same_matrix(collision_matrix_from_placement(30.0, 5.0, 0.0, pos, 0), tilted));
	const CollisionMatrix rolled = collision_matrix_from_euler(bam_heading_from_mission_yaw_deg(0.0),
	                                                           bam_from_degrees_wrapped(0.0), bam_from_degrees_wrapped(-7.0), pos);
	TEST_EXPECT(same_matrix(collision_matrix_from_placement(0.0, 0.0, -7.0, pos, 0), rolled));
	std::printf("placement: the heading path for pure yaw, the Euler builder with pitch or roll\n");
	return 0;
}

int test_box_bounds() {
	const int32_t pos[3] = {fx(10), fx(20), fx(30)};
	const int32_t box[6] = {fx(-1), fx(-2), fx(-3), fx(1), fx(2), fx(3)};
	int32_t lo[3], hi[3];
	// Mission yaw 90 is heading 0: the box only moved.
	collision_matrix_box_bounds(collision_matrix_from_placement(90.0, 0.0, 0.0, pos, 0), box, lo, hi);
	TEST_EXPECT(near_fixed(lo[0], fx(9)) && near_fixed(lo[1], fx(18)) && near_fixed(lo[2], fx(27)));
	TEST_EXPECT(near_fixed(hi[0], fx(11)) && near_fixed(hi[1], fx(22)) && near_fixed(hi[2], fx(33)));
	// A quarter turn swaps the horizontal spans.
	collision_matrix_box_bounds(collision_matrix_from_placement(0.0, 0.0, 0.0, pos, 0), box, lo, hi);
	TEST_EXPECT(near_fixed(hi[0] - lo[0], fx(4)) && near_fixed(hi[1] - lo[1], fx(2)) && near_fixed(hi[2] - lo[2], fx(6)));
	// Each bound is a corner's: every corner lies within them.
	const CollisionMatrix leaning = collision_matrix_from_placement(17.0, 11.0, 3.0, pos, 0x14000);
	collision_matrix_box_bounds(leaning, box, lo, hi);
	for (int corner = 0; corner < 8; ++corner) {
		const int32_t point[3] = {box[(corner & 1) ? 3 : 0], box[(corner & 2) ? 4 : 1], box[(corner & 4) ? 5 : 2]};
		int32_t out[3];
		leaning.transform_point(point, out);
		for (int axis = 0; axis < 3; ++axis) TEST_EXPECT(out[axis] >= lo[axis] && out[axis] <= hi[axis]);
	}
	std::printf("box: a moved box, a turned one, a leaning scaled one inside its bounds\n");
	return 0;
}

int test_probe_origin() {
	// The resolver's expression, every port's.
	for (const int32_t z : {0, 1, 6143, 6144, 0x10000, fx(3.25), fx(-2.5), -1, -6144, fx(1000.5)})
		TEST_EXPECT(ground_probe_origin_z(z) == ((z + 6143) & ~0x17FF));
	// Wraps where the add would overflow.
	TEST_EXPECT(ground_probe_origin_z(INT32_MAX) ==
	            static_cast<int32_t>((static_cast<uint32_t>(INT32_MAX) + 0x17FFu) & ~0x17FFu));
	std::printf("probe origin: (z + 6143) & ~0x17FF, wrapping\n");
	return 0;
}

int test_terrain_column() {
	// A 512 x 512 field, flat at 2 units (heights in 1/256 units).
	const int dim = 512;
	std::vector<uint16_t> heights(static_cast<size_t>(dim) * dim, static_cast<uint16_t>(2.0 * 256.0));
	std::vector<int> sectors(256, 1);
	opennova::terrain::TerrainHeightField field;
	field.heightmap = heights.data();
	field.dim = dim;
	field.layout.sector_grid = sectors.data();
	int32_t height = 0;
	TEST_EXPECT(terrain_column_height(&field, fx(40), fx(40), height));
	TEST_EXPECT(near_fixed(height, fx(2.0)));
	// The settle tail's 2.0 u column from a body half a unit over it reads the same ground.
	const int32_t over_ground[3] = {fx(40), fx(40), fx(2.5)};
	TEST_EXPECT(terrain_settle_clearance(&field, over_ground, 0, false) == fx(2.5) - height);
	TEST_EXPECT(!terrain_column_height(nullptr, fx(40), fx(40), height));
	std::printf("terrain column: the flat field's height under a column, none without a field\n");
	return 0;
}

} // namespace

int main() {
	if (test_scale() != 0) return 1;
	if (test_placement() != 0) return 1;
	if (test_box_bounds() != 0) return 1;
	if (test_probe_origin() != 0) return 1;
	if (test_terrain_column() != 0) return 1;
	return 0;
}
