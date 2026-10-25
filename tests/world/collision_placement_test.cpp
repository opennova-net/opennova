// The collision placement and probe helpers (runtime/world/collision.h): the scaled Euler builder
// (collision_matrix_from_euler_scaled [orig: @0x614210]), the placement from
// mission eulers (the Euler builder for every placement, pure yaw included [orig:
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
	// A 0 scale is the unscaled builder.
	const CollisionMatrix base = collision_matrix_from_euler(0x10000000, 0x01000000, 0x02000000, pos);
	TEST_EXPECT(same_matrix(
			collision_matrix_from_euler_scaled(0x10000000, 0x01000000, 0x02000000, pos, 0), base));
	// x1.5 with yaw, pitch and roll: the scale << 6 is the diagonal every stage multiplies
	// through, so it nests inside each stage's truncations [orig:
	// Math_BuildFixedPointRotationMatrixFromEulerAnglesAndScale @0x614210 - `shl eax, 6`
	// @0x614273, the roll stage @0x61427A..0x6142C0, the pitch stage @0x614346 / @0x6143FB, the yaw
	// stage @0x6144CD..0x614612]. Scaling the unscaled build afterwards gave m[1] -2411807 and m[2]
	// -24336. Every sine / cosine product sits 0.018 or more from an integer edge (Q3-11).
	const CollisionMatrix scaled =
			collision_matrix_from_euler_scaled(0x10000000, 0x01000000, 0x02000000, pos, 0x18000);
	const int idx[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
	const int32_t want[9] = {5810766, -2411806, -24335, 2406979, 5802615, -344235, 154404, 308622,
		6281982};
	for (int k = 0; k < 9; ++k) TEST_EXPECT(scaled.m[idx[k]] == want[k]);
	TEST_EXPECT(scaled.m[3] == pos[0] && scaled.m[7] == pos[1] && scaled.m[11] == pos[2]);
	std::printf("scale: the scaled builder's own truncations, the translation kept, 0 unscaled\n");
	return 0;
}

int test_placement() {
	const int32_t pos[3] = {fx(1), fx(2), fx(3)};
	// Pure yaw: the Euler builder too, scaled (D-NET-NEW-L6-10) [orig: BoneCallback_Simple
	// @0x4E2600 -> Math_BuildFixedPointMatrixFromEulerAngles @0x613F40].
	const CollisionMatrix yaw =
			collision_matrix_from_euler_scaled(placed_heading_bam(30), 0, 0, pos, 0x18000);
	TEST_EXPECT(same_matrix(collision_matrix_from_placement(placed_heading_bam(30), 0, 0, pos, 0x18000), yaw));
	// Mission yaw 30 places heading 0x2AAA0000 (spawn_angle_bam(60)). The yaw stage's words are
	// trunc(sin / cos(h * dbl_7C3608) * 2^22) [orig: @0x6140B2..0x6140CF]: sin 3632306.79 and
	// cos 2097268.09 truncate to 3632306 / 2097268 (each clear of an integer edge, Q3-11). The
	// direction table's entry (h + 0x200000) >> 22 = 171 sits 0.12 degrees off that heading,
	// so the old pure-yaw table path placed the shell off the visual.
	const CollisionMatrix placed = collision_matrix_from_placement(placed_heading_bam(30), 0, 0, pos, 0);
	TEST_EXPECT(placed_heading_bam(30) == 0x2AAA0000);
	TEST_EXPECT(placed.m[0] == 2097268 && placed.m[1] == -3632306 && placed.m[2] == 0);
	TEST_EXPECT(placed.m[4] == 3632306 && placed.m[5] == 2097268 && placed.m[6] == 0);
	TEST_EXPECT(placed.m[8] == 0 && placed.m[9] == 0 && placed.m[10] == 0x400000);
	TEST_EXPECT(placed.m[3] == pos[0] && placed.m[7] == pos[1] && placed.m[11] == pos[2]);
	TEST_EXPECT(!same_matrix(placed, collision_matrix_from_heading(placed_heading_bam(30), pos)));
	// Pitch or roll authored: the Euler builder over the Pitch / Roll words (D-NET-NEW-L6-3).
	const CollisionMatrix tilted = collision_matrix_from_euler(placed_heading_bam(30),
	                                                           spawn_angle_bam(5), 0, pos);
	TEST_EXPECT(same_matrix(collision_matrix_from_placement(placed_heading_bam(30), spawn_angle_bam(5), 0, pos, 0), tilted));
	const CollisionMatrix rolled = collision_matrix_from_euler(placed_heading_bam(0),
	                                                           0, spawn_angle_bam(-7), pos);
	TEST_EXPECT(same_matrix(collision_matrix_from_placement(placed_heading_bam(0), 0, spawn_angle_bam(-7), pos, 0), rolled));
	std::printf("placement: the Euler builder for pure yaw and with pitch or roll\n");
	return 0;
}

int test_box_bounds() {
	const int32_t pos[3] = {fx(10), fx(20), fx(30)};
	const int32_t box[6] = {fx(-1), fx(-2), fx(-3), fx(1), fx(2), fx(3)};
	int32_t lo[3], hi[3];
	// Mission yaw 90 is heading 0: the box only moved.
	collision_matrix_box_bounds(collision_matrix_from_placement(placed_heading_bam(90), 0, 0, pos, 0), box, lo, hi);
	TEST_EXPECT(near_fixed(lo[0], fx(9)) && near_fixed(lo[1], fx(18)) && near_fixed(lo[2], fx(27)));
	TEST_EXPECT(near_fixed(hi[0], fx(11)) && near_fixed(hi[1], fx(22)) && near_fixed(hi[2], fx(33)));
	// A quarter turn swaps the horizontal spans.
	collision_matrix_box_bounds(collision_matrix_from_placement(placed_heading_bam(0), 0, 0, pos, 0), box, lo, hi);
	TEST_EXPECT(near_fixed(hi[0] - lo[0], fx(4)) && near_fixed(hi[1] - lo[1], fx(2)) && near_fixed(hi[2] - lo[2], fx(6)));
	// Each bound is a corner's: every corner lies within them.
	const CollisionMatrix leaning = collision_matrix_from_placement(placed_heading_bam(17), spawn_angle_bam(11), spawn_angle_bam(3), pos, 0x14000);
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
