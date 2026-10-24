// The game's fixed-point bilinear terrain height (D-NET-431's last LSB): four
// weights from the position's low words (the atlas Y negated), each rounded
// ((a * b + 0x8000) >> 16), each tap's raw16 << 8 times its weight rounded the
// same way, the four summed; the +1 taps wrap inside a locked quadrant. Every
// terrain raycast samples the surface through it (ported with D-NET-507), so
// the warm-up's ground settle lands on it: on TDH_I3A the joiner's join z is
// 0x10C887 on a retail host, where c1's truncating float sample left ours one
// unit low (0x10C886).
// [orig: Terrain_SampleHeightBilinear @0x6067B0 (weights @0x606806..0x60689D,
//  seam masks @0x6068D0..0x606930, taps and sum @0x60694E..); its raycast
//  callers Terrain_RaycastHeightmapLoRes @0x60CC02 / @0x60CDDE,
//  Terrain_RaycastHeightmapHiRes_0 @0x60E784..0x60E803]
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/collision.h> // terrain_clip_segment

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace opennova;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

namespace {

constexpr int32_t q16(int n) { return n * 65536; }

struct Field {
    std::vector<uint16_t> heights = std::vector<uint16_t>(1024 * 1024, 4000);
    std::array<int, 256> sectors{};
    terrain::TerrainHeightField field;

    Field() {
        sectors.fill(1);
        field.heightmap = heights.data();
        field.dim = 1024;
        field.layout.sector_grid = sectors.data();
    }
    uint16_t &at(int x, int z) { return heights[static_cast<size_t>(z) * 1024 + x]; }
};

// The per-term rounding: the exact bilinear value here is 1016104.36 (what a
// truncating float sample returns, floored); the game's sum is 1016105.
void test_the_terms_round() {
    Field f;
    f.at(10, 20) = 3968;
    f.at(11, 20) = 4191;
    f.at(10, 21) = 3932;
    f.at(11, 21) = 4030;
    const int32_t x = q16(10) + 0x1E30;
    const int32_t y = -(q16(20) + 0x7ED5); // atlas row 20, fraction 0x7ED5
    CHECK(terrain::height_field_height_bilinear_fixed(f.field, x, y) == 1016105);
    // A vertical probe through it settles on the same height (the column
    // shortcut's one sample) [orig: @0x60CBFB..0x60CC2D].
    const int32_t start[3] = {x, y, q16(20)};
    const int32_t end[3] = {x, y, q16(0)};
    int32_t hit[3] = {0, 0, 0};
    CHECK(world::terrain_clip_segment(f.field, start, end, hit));
    CHECK(hit[2] == 1016105);
}

// A locked quadrant wraps the +1 tap to its own column 0; an unlocked one
// reads the next quadrant's. Half way across: 128 * (h511 + h_next).
void test_the_seam_lock() {
    Field f;
    f.at(511, 20) = 4000;
    f.at(0, 20) = 100;
    f.at(512, 20) = 300;
    const int32_t x = q16(511) + 0x8000;
    const int32_t y = -q16(20);
    CHECK(terrain::height_field_height_bilinear_fixed(f.field, x, y) == 128 * (4000 + 300));
    f.field.locks.set(0, true, false);
    CHECK(terrain::height_field_height_bilinear_fixed(f.field, x, y) == 128 * (4000 + 100));
}

// An empty cell and no grid read 0 [orig: @0x606804..0x606807].
void test_empty() {
    Field f;
    f.sectors.fill(0);
    CHECK(terrain::height_field_height_bilinear_fixed(f.field, q16(10), -q16(20)) == 0);
    CHECK(terrain::height_field_height_bilinear_fixed(terrain::TerrainHeightField{}, 0, 0) == 0);
}

} // namespace

int main() {
    test_the_terms_round();
    test_the_seam_lock();
    test_empty();
    if (failures == 0) std::puts("terrain_bilinear_fixed: OK");
    return failures == 0 ? 0 : 1;
}
