// Terrain-grounded movement (Phase 1) tests:
//   * the portable terrain/height_field.h samplers (the three NovaTerrainData::get_height*
//     bodies, now shared) over a synthetic X-ramp atlas,
//   * calc_average_ground_height (Entity_CalcAverageGroundHeight @0x457230): the 5-tap
//     weighted average, the >= center clamp, and the worldY water clamp, by hand math,
//   * AiSystem grounding: apply_ground_clamp drives pos[2] off the terrain, the AI tick
//     wires it in, and a null field leaves Z untouched.
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <vector>

#include "terrain/height_field.h"
#include "world/ai.h"
#include "world/world.h"

using namespace opennova::world;
using opennova::terrain::TerrainHeightField;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static bool approx(float a, float b) { return std::fabs(a - b) < 1e-4f; }

namespace {

constexpr int kDim = 512;

// A square atlas whose height ramps with X (raw16 = x * 16, uniform in Z), so
// height_units(x) = x*16/256 = x/16. Uniform in Z keeps the N/S taps equal to the
// center, so the 5-tap hand math stays clean. All 16x16 sector cells map to id 1
// (quadrant offset 0,0), so world->source is the identity within [0,512).
struct RampField {
    std::vector<uint16_t> heightmap;
    std::vector<int> sector_grid;
    TerrainHeightField field;

    RampField() : heightmap(kDim * kDim), sector_grid(256, 1) {
        for (int z = 0; z < kDim; ++z)
            for (int x = 0; x < kDim; ++x)
                heightmap[z * kDim + x] = static_cast<uint16_t>(x * 16);
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
        field.layout.origin_x = 0;
        field.layout.origin_y = 0;
    }
};

// Engine 16.16-fixed position helper.
constexpr int32_t fx(double units) { return static_cast<int32_t>(units * 65536.0); }

} // namespace

int main() {
    RampField rf;
    const TerrainHeightField &f = rf.field;

    // ---- samplers: exact at integer columns, bilinear between, /256 scale ----
    // Square (no-remap) bilinear: world (x,z) indexes the atlas directly.
    CHECK(approx(opennova::terrain::height_field_height_bilinear(f, 100.0f, 0.0f), 6.25f));   // 100*16/256
    CHECK(approx(opennova::terrain::height_field_height_bilinear(f, 100.5f, 0.0f), 6.28125f)); // (1600+1616)/2/256
    // world-remap nearest + bilinear agree at an integer column.
    CHECK(approx(opennova::terrain::height_field_height_world(f, 200.0f, 0.0f), 12.5f));
    CHECK(approx(opennova::terrain::height_field_height_world_bilinear(f, 200.0f, 0.0f), 12.5f));
    CHECK(approx(opennova::terrain::height_field_height_world_bilinear(f, 300.0f, 0.0f), 18.75f));

    // Empty sector cell -> invalid -> 0 (the negative-sentinel contract).
    {
        RampField empty;
        for (int &c : empty.sector_grid) c = 0;
        empty.field.layout.sector_grid = empty.sector_grid.data();
        CHECK(opennova::terrain::height_field_height_world_bilinear(empty.field, 100.0f, 0.0f) == 0.0f);
    }
    // Null field -> 0.
    {
        TerrainHeightField bad;
        CHECK(opennova::terrain::height_field_height_world_bilinear(bad, 0.0f, 0.0f) == 0.0f);
    }

    // ---- calc_average_ground_height: 5-tap weighted average (hand math) ----
    // [orig: Entity_CalcAverageGroundHeight @0x457230] At engine (X=100,Y=0): center/N/S = 6.25u
    // (409600), E(x=105) = 6.5625u (430080), W(x=95) = 5.9375u (389120). max = 430080.
    // result = (409600+409600+430080+389120 + 2*(409600 + 2*430080)) / 10 = 417792.
    {
        const int32_t pos[3] = {fx(100), 0, 0};
        GroundClearance gc{};
        const int32_t g = calc_average_ground_height(f, pos, 0x50000, gc);
        CHECK(g == 417792);

        // radius 0 -> center only (6.25u = 409600).
        const int32_t c = calc_average_ground_height(f, pos, 0, gc);
        CHECK(c == 409600);

        // def alive offset is added on top.
        GroundClearance gco{};
        gco.alive_offset = 1000;
        CHECK(calc_average_ground_height(f, pos, 0, gco) == 409600 + 1000);

        // water clamp: worldY above the ground floor wins, but only with physics.
        TerrainHeightField wf = f;
        wf.has_water = true;
        wf.water_y = fx(50); // 3276800, well above the 417792 ground
        GroundClearance gcw{};
        gcw.has_physics = true;
        CHECK(calc_average_ground_height(wf, pos, 0x50000, gcw) == fx(50));
        gcw.has_physics = false; // no physics -> no water clamp
        CHECK(calc_average_ground_height(wf, pos, 0x50000, gcw) == 417792);

        // invalid field -> INT32_MIN sentinel.
        TerrainHeightField bad;
        CHECK(calc_average_ground_height(bad, pos, 0x50000, gc) == INT32_MIN);
    }

    // ---- AiSystem::apply_ground_clamp drives pos[2] = ground + stand_offset ----
    {
        AiSystem sys;
        sys.terrain = &f;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.pos[0] = fx(100);
        e.pos[1] = 0;
        e.pos[2] = fx(9999); // start floating
        sys.apply_ground_clamp(e);
        // ground 417792 + stand 0x50000 (327680) = 745472.
        CHECK(e.pos[2] == 745472);
        CHECK(e.brain.f[AiBrain::kWorkPosZ] == 745472);

        // Slope-tracking: a different column gives a different grounded Z.
        e.pos[0] = fx(300);
        e.pos[2] = 0;
        sys.apply_ground_clamp(e);
        CHECK(e.pos[2] > 745472); // x=300 is higher up the ramp than x=100
    }

    // ---- null terrain leaves Z untouched ----
    {
        AiSystem sys; // terrain == nullptr (default)
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.pos[2] = fx(1234);
        sys.apply_ground_clamp(e);
        CHECK(e.pos[2] == fx(1234)); // unchanged
    }

    // ---- the AI tick wires grounding (apply_ground_clamp runs last, so it's authoritative) ----
    {
        World w;
        AiSystem sys;
        sys.terrain = &f;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = 0; // benign default state (nullsub handlers)
        e.pos[0] = fx(200);
        e.pos[1] = 0;
        e.pos[2] = fx(9999); // floating
        TickContext ctx{};
        ctx.is_authority = true;
        sys.tick(w, ctx);
        // ground at x=200: center/N/S 12.5u(819200), E(205)=839680, W(195)=798720, max=839680.
        // result=(819200*3? no: N+S+E+W=819200+819200+839680+798720)=3276800; +2*(819200+2*839680)=4997120
        //   => 8273920/10 = 827392; +327680 = 1155072.
        CHECK(e.pos[2] == 1155072);
    }

    if (failures == 0) std::printf("ground_height_test: OK\n");
    else std::printf("ground_height_test: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
