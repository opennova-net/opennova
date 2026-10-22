// The height mip chain's 64 x 64 level, read bilinearly: the ground the arty barrage spawns its
// shells 700 units above (D-NET-NEW-L4-23). The mip build stores each finest cell's max raw16 of
// its 3 x 3 texels plus 127, shifted down by 7, as a byte (a peak past 32640 wraps), and each
// coarser level keeps its four children's largest byte; the sampler reads the 64 x 64 level at
// the position less 16 units on x and plus 16 on y, the tile and its +1 neighbours weighted by
// the low eight bits of the shifted words' 4096ths, the sum halved.
// The values are the cited disassembly's arithmetic worked outside the port (a full 512-level
// build up the chain, then the four-tap sum).
// [orig: Foliage_SampleDensityBilinear @0x606200 - the centring @0x60620D / @0x606219, the
//  empty return @0x606264, the weights @0x606275 / @0x606280, the tiles @0x6062AF..0x6062C8,
//  the sum @0x6062D1..0x606318 halved @0x60631B; Terrain_BuildMipchain @0x6056D0 - the rows @0x605740..0x60575D, the byte
//  @0x60589B..0x6058A7, the level list @0x6058EE, its stores @0x6059D0..0x6059DC]
#include <runtime/terrain_query/height_field.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

namespace {

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

// The flat field: every finest cell's byte is (4000 + 127) >> 7 = 32, so every tile reads 32 and
// the sample is 32 * 65536 / 2, 16 units (the raw 4000 is 15.6 units: the chain's max rounds up).
void test_a_flat_field() {
    auto f = std::make_unique<Field>();
    CHECK(terrain::height_field_mip_max_bilinear_fixed(f->field, 300 * 65536 + 0x8000,
            -(300 * 65536 + 0x4000)) == 1048576);
}

// Two peaks. Texel (40, 72) at 20000 is in the finest cells c 19..20, r 35..36: byte 157 in tile
// (2, 4). Texel (48, 64) at 40000 is in the cells c 23..24, r 31..32, tiles (2..3, 3..4): its
// byte (40000 + 127) >> 7 = 313 is stored as 57, so tile (2, 4) keeps 157 and tile (3, 4) reads
// 57. At x 52.25, y -86.5 the centred words are x 36.25 (tile 2, weight 0x44) and atlas z 70.5
// (tile 4, weight 0x68): (104 * 188 * 32 + 152 * 188 * 157 + 68 * 104 * 32 + 68 * 152 * 57) >> 1.
void test_the_peaks_and_the_byte() {
    auto f = std::make_unique<Field>();
    f->at(40, 72) = 20000;
    f->at(48, 64) = 40000;
    const int32_t x = 52 * 65536 + 0x4000;
    const int32_t y = -(86 * 65536 + 0x8000);
    CHECK(terrain::height_field_mip_max_bilinear_fixed(f->field, x, y) == 2963776);
    // Without the centring the same words would read tile (3, 5): the 16 units move the taps.
    CHECK(terrain::height_field_mip_max_bilinear_fixed(f->field, x + 16 * 65536,
            y - 16 * 65536) != 2963776);
}

// An empty sector cell and no grid read 0 [orig: @0x606264].
void test_an_empty_cell() {
    auto f = std::make_unique<Field>();
    f->sectors[0] = 0;
    CHECK(terrain::height_field_mip_max_bilinear_fixed(f->field, 52 * 65536, -(86 * 65536)) == 0);
    f->field.layout.sector_grid = nullptr;
    CHECK(terrain::height_field_mip_max_bilinear_fixed(f->field, 52 * 65536, -(86 * 65536)) == 0);
}

} // namespace

int main() {
    test_a_flat_field();
    test_the_peaks_and_the_byte();
    test_an_empty_cell();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS terrain_mip_max\n");
    return 0;
}
