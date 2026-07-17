// The retail terrain segment raycast port (libs/terrain_query/terrain_raycast):
// pins the Terrain_RaycastHeightmapLoRes march [orig: @ 0x60cb80] and the
// Terrain_RaycastHeightmapHiRes_0 refine [orig: @ 0x60e710] structural
// translations against hand-computed fixed-point values over synthetic
// samplers (a uniform plane, an authored-empty field, an out-of-extent
// field). Witness record: docs/terrain/terrain-re.md §"Runtime terrain
// queries (ENG-3 B0)".

#include "terrain/terrain_raycast.h"

#include <cstdio>

using namespace opennova::terrain;

static int g_fail = 0;

static void check(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        g_fail = 1;
    }
}

static void check_eq(int32_t got, int32_t want, const char *msg) {
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s (got 0x%08X want 0x%08X)\n", msg,
                     static_cast<uint32_t>(got), static_cast<uint32_t>(want));
        g_fail = 1;
    }
}

// One world unit in 16.16.
static const int32_t U = 0x10000;

// A uniform synthetic field: every sample reports the same kind (and height,
// for kHeight), with call counting so tests can pin the witnessed sample
// cadence (point for the coarse march test, bilinear for confirms/shortcut/
// refine).
struct UniformField {
    TerrainRaycastSample::Kind kind = TerrainRaycastSample::kHeight;
    int32_t height = 0; // 16.16
    int point_calls = 0;
    int bilinear_calls = 0;
};

static TerrainRaycastSample uniform_point(void *ctx, int32_t, int32_t) {
    UniformField *f = static_cast<UniformField *>(ctx);
    ++f->point_calls;
    TerrainRaycastSample s;
    s.kind = f->kind;
    s.height_1616 = f->height;
    return s;
}

static TerrainRaycastSample uniform_bilinear(void *ctx, int32_t, int32_t) {
    UniformField *f = static_cast<UniformField *>(ctx);
    ++f->bilinear_calls;
    TerrainRaycastSample s;
    s.kind = f->kind;
    s.height_1616 = f->height;
    return s;
}

static TerrainRaycastSampler uniform_sampler(UniformField &f) {
    TerrainRaycastSampler s;
    s.point = &uniform_point;
    s.bilinear = &uniform_bilinear;
    s.ctx = &f;
    return s;
}

// A ridge field for the LOS march: height 6u over x in [16u, 32u), else 0 —
// terrain that rises BETWEEN endpoints, invisible to the endpoint prechecks.
struct RidgeField {
    int point_calls = 0;
    int bilinear_calls = 0;
};

static TerrainRaycastSample ridge_sample(int32_t x) {
    TerrainRaycastSample s;
    s.kind = TerrainRaycastSample::kHeight;
    s.height_1616 = (x >= 16 * 0x10000 && x < 32 * 0x10000) ? 6 * 0x10000 : 0;
    return s;
}

static TerrainRaycastSample ridge_point(void *ctx, int32_t x, int32_t) {
    ++static_cast<RidgeField *>(ctx)->point_calls;
    return ridge_sample(x);
}

static TerrainRaycastSample ridge_bilinear(void *ctx, int32_t x, int32_t) {
    ++static_cast<RidgeField *>(ctx)->bilinear_calls;
    return ridge_sample(x);
}

static TerrainRaycastSampler ridge_sampler(RidgeField &f) {
    TerrainRaycastSampler s;
    s.point = &ridge_point;
    s.bilinear = &ridge_bilinear;
    s.ctx = &f;
    return s;
}

// Reference step normalization, written independently of the implementation:
// inv = floor(2^32 / max_delta) (unsigned 64-bit divide), per-axis step =
// (inv * delta + 0x8000) >> 16 on the signed 64-bit product
// [orig: Terrain_RaycastHeightmapLoRes @ 0x60cb80, @ 0x60cc72..0x60cce4].
static int32_t ref_inv(int32_t max_delta) {
    return static_cast<int32_t>(0x100000000ULL / static_cast<uint32_t>(max_delta));
}

static int32_t ref_step(int32_t inv, int32_t delta) {
    return static_cast<int32_t>((static_cast<int64_t>(inv) * delta + 0x8000) >> 16);
}

int main() {
    // ---------------------------------------------------------------- (a)
    // Step normalization exactness + the sample budget = the major-axis
    // extent in world units [orig: @ 0x60cc72..0x60ccef, budget @ 0x60ce10].
    {
        // Descending ray onto a plane at 10 units: observe the stored steps.
        UniformField f;
        f.height = 10 * U;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { 0, 0, 12 * U };
        const int32_t end[3] = { 16 * U, 5 * U, 4 * U };
        int32_t hit[3] = { 0, 0, 0 };
        int32_t step[3] = { 0, 0, 0 };
        check(terrain_raycast_march(s, start, end, hit, step), "(a) descending ray hits");
        const int32_t inv = ref_inv(16 * U);
        check_eq(inv, 0x1000, "(a) inv literal for a 16-unit major extent");
        check_eq(step[0], ref_step(inv, 16 * U), "(a) step_x == reference formula");
        check_eq(step[1], ref_step(inv, 5 * U), "(a) step_y == reference formula");
        check_eq(step[2], ref_step(inv, -8 * U), "(a) step_z == reference formula");
        check_eq(step[0], 0x10000, "(a) step_x literal (~1.0 unit along the major axis)");
        check_eq(step[1], 0x5000, "(a) step_y literal");
        check_eq(step[2], -0x8000, "(a) step_z literal");
    }
    {
        // Non-power-of-two major extent (3 units): the flooring of inv shows
        // in the step literals (major step 0xFFFF, just under 1.0).
        UniformField f;
        f.height = 1 * U;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { 0, 0, 2 * U };
        const int32_t end[3] = { 3 * U, 1 * U, 0 };
        int32_t hit[3] = { 0, 0, 0 };
        int32_t step[3] = { 0, 0, 0 };
        check(terrain_raycast_march(s, start, end, hit, step), "(a) 3-unit ray hits");
        const int32_t inv = ref_inv(3 * U);
        check_eq(inv, 0x5555, "(a) inv literal for a 3-unit major extent");
        check_eq(step[0], 0xFFFF, "(a) 3-unit step_x literal");
        check_eq(step[1], 0x5555, "(a) 3-unit step_y literal");
        check_eq(step[2], -0xAAAA, "(a) 3-unit step_z literal");
        check_eq(step[0], ref_step(inv, 3 * U), "(a) 3-unit step_x == reference");
        check_eq(step[1], ref_step(inv, 1 * U), "(a) 3-unit step_y == reference");
        check_eq(step[2], ref_step(inv, -2 * U), "(a) 3-unit step_z == reference");
    }
    {
        // Sample budget: a CLEAR 16-unit ray takes exactly 16 coarse point
        // samples (remaining 0x10000 loses inv per sample, checked AFTER the
        // sample [orig: @ 0x60cdf6..0x60ce10]) and never confirms.
        UniformField f;
        f.height = -10 * U; // plane far below the ray
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { 0, 0, 0 };
        const int32_t end[3] = { 16 * U, 0, 0 };
        int32_t hit[3] = { 0x13572468, 0x13572468, 0x13572468 };
        int32_t step[3] = { 0x13572468, 0x13572468, 0x13572468 };
        check(!terrain_raycast_march(s, start, end, hit, step), "(a) low plane ray is CLEAR");
        check_eq(f.point_calls, 16, "(a) 16-unit extent takes 16 point samples");
        check_eq(f.bilinear_calls, 0, "(a) coarse-fail never confirms");
        // The march path writes NOTHING on CLEAR [orig: the return-1 paths
        // never touch hit_point].
        check_eq(hit[0], 0x13572468, "(a) CLEAR march leaves out_hit untouched");
        check_eq(step[0], 0x13572468, "(a) CLEAR march leaves out_step untouched");
    }
    {
        // Inexact budget division: a 3-unit extent floors inv (0x5555), so
        // remaining hits zero only after a 4th sample — the witnessed
        // arithmetic, not a unit-count idealization.
        UniformField f;
        f.height = -10 * U;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { 0, 0, 0 };
        const int32_t end[3] = { 3 * U, 0, 0 };
        check(!terrain_raycast_march(s, start, end, nullptr, nullptr), "(a) 3-unit CLEAR");
        check_eq(f.point_calls, 4, "(a) floored inv gives a 3-unit ray 4 samples");
    }

    // ---------------------------------------------------------------- (b)
    // A descending march hit carries the RAY z at the first confirming
    // sample, not the terrain height [orig: hit epilogue @ 0x60cf5d..0x60cf78
    // writes cur_z]; march and refined agree on x/y within one step.
    const int32_t kPlane = 10 * U; // 0xA0000
    int32_t march_hit_b[3] = { 0, 0, 0 };
    {
        UniformField f;
        f.height = kPlane;
        TerrainRaycastSampler s = uniform_sampler(f);
        // start z 12.25 units so no sample lands exactly on the plane.
        const int32_t start[3] = { 0, 0, 0xC4000 };
        const int32_t end[3] = { 16 * U, 0, 0x44000 };
        int32_t step[3] = { 0, 0, 0 };
        check(terrain_raycast_march(s, start, end, march_hit_b, step), "(b) march hits");
        // steps (0x10000, 0, -0x8000); first sample with plane >= ray z is
        // k = 5: cur_z = 0xC4000 - 5 * 0x8000 = 0x9C000.
        check_eq(march_hit_b[0], 5 * U, "(b) hit x = the k=5 sample");
        check_eq(march_hit_b[1], 0, "(b) hit y");
        check_eq(march_hit_b[2], 0x9C000, "(b) hit z = the RAY z at the hit sample");
        check(march_hit_b[2] != kPlane, "(b) hit z is NOT the terrain height");

        UniformField f2;
        f2.height = kPlane;
        TerrainRaycastSampler s2 = uniform_sampler(f2);
        int32_t refined[3] = { 0, 0, 0 };
        check(terrain_raycast_refined(s2, start, end, refined), "(b) refined hits");
        const int32_t dx = refined[0] - march_hit_b[0];
        check(dx <= 0x10000 && dx >= -0x10000, "(b) refined x within one step of the march x");
        check_eq(refined[1], march_hit_b[1], "(b) refined y agrees (zero-dy ray)");

        // ------------------------------------------------------------ (c)
        // Refined convergence to the surface along z: within
        // (0x10000/4)/2^8 + 1 LSB = 65 of the plane for a crossing ray
        // [orig: quarter steps @ 0x60e769..0x60e778, 8-iteration bisection
        // @ 0x60e7f9..0x60e834].
        const int32_t err = refined[2] - kPlane;
        check(err <= 65 && err >= -65, "(c) refined z within (1/4)/2^8 + 1 LSB of the plane");
        // Deterministic hand-trace of the ported semantics (2 back-steps,
        // no forward steps, 8 bisections): a regression anchor.
        check_eq(refined[0], 0x48080, "(c) refined x hand-traced golden");
        check_eq(refined[2], 0x9FFC0, "(c) refined z hand-traced golden");
    }

    // ---------------------------------------------------------------- (d)
    // The column shortcut [orig: gate @ 0x60cbe4..0x60cbf9, body @ 0x60cbfb..
    // 0x60cc52]: one bilinear sample at the start column; hit iff the segment
    // crosses/touches the surface; hit z = the TERRAIN height; the outs are
    // written BEFORE the hit/clear decision (even on CLEAR); step out zeroed.
    const int32_t kColX = 7 * U + 0x1234;
    const int32_t kColY = 3 * U + 0x777;
    {
        // Crossing: HIT at (start x, start y, plane height).
        UniformField f;
        f.height = kPlane;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { kColX, kColY, 12 * U };
        const int32_t end[3] = { kColX + 0x800, kColY + 0xF00, 8 * U };
        int32_t hit[3] = { 0, 0, 0 };
        int32_t step[3] = { 1, 2, 3 };
        check(terrain_raycast_march(s, start, end, hit, step), "(d) crossing column HITs");
        check_eq(hit[0], kColX, "(d) column hit x = start x");
        check_eq(hit[1], kColY, "(d) column hit y = start y");
        check_eq(hit[2], kPlane, "(d) column hit z = the TERRAIN height");
        check_eq(step[0], 0, "(d) column step x zeroed");
        check_eq(step[1], 0, "(d) column step y zeroed");
        check_eq(step[2], 0, "(d) column step z zeroed");
        check_eq(f.bilinear_calls, 1, "(d) exactly ONE bilinear sample");
        check_eq(f.point_calls, 0, "(d) no coarse point samples");
    }
    {
        // Fully above -> CLEAR; the witnessed quirk: the outs are written
        // anyway [orig: @ 0x60cc12..0x60cc2d runs before the decision].
        UniformField f;
        f.height = kPlane;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { kColX, kColY, 20 * U };
        const int32_t end[3] = { kColX + 0x800, kColY, 12 * U };
        int32_t hit[3] = { 0, 0, 0 };
        int32_t step[3] = { 1, 2, 3 };
        check(!terrain_raycast_march(s, start, end, hit, step), "(d) fully-above column CLEAR");
        check_eq(hit[2], kPlane, "(d) CLEAR column still wrote the hit out (witnessed order)");
        check_eq(step[2], 0, "(d) CLEAR column still zeroed the step out");
    }
    {
        // FULLY-BURIED -> CLEAR: the witnessed asymmetry [orig: @ 0x60cc33..
        // 0x60cc52] — contrast with the march case below.
        UniformField f;
        f.height = kPlane;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { kColX, kColY, 4 * U };
        const int32_t end[3] = { kColX + 0x800, kColY, 8 * U };
        check(!terrain_raycast_march(s, start, end, nullptr, nullptr),
              "(d) fully-buried column CLEAR (witnessed asymmetry)");
    }
    {
        // Zero-length segment (both deltas 0: the early gate path
        // [orig: @ 0x60cbe8]) touching the surface exactly -> HIT.
        UniformField f;
        f.height = kPlane;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { kColX, kColY, kPlane };
        const int32_t end[3] = { kColX, kColY, kPlane };
        check(terrain_raycast_march(s, start, end, nullptr, nullptr),
              "(d) zero-delta touching segment HITs (early gate path)");
    }
    {
        // The contrasting march case: a shallow ray STARTING below ground
        // hits at its FIRST sample (coarse+confirm at the start position).
        UniformField f;
        f.height = kPlane;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { 0, 0, 4 * U };
        const int32_t end[3] = { 16 * U, 0, 8 * U };
        int32_t hit[3] = { 0, 0, 0 };
        check(terrain_raycast_march(s, start, end, hit, nullptr),
              "(d) march starting below ground HITs");
        check_eq(hit[0], start[0], "(d) below-ground march hit x = start");
        check_eq(hit[1], start[1], "(d) below-ground march hit y = start");
        check_eq(hit[2], start[2], "(d) below-ground march hit z = start ray z");
        check_eq(f.point_calls, 1, "(d) hit at the FIRST sample");
    }
    {
        // Gate boundary: |dx| == 4096 marches (a single sample: the budget
        // spends 0x100000 per sample); |dx| == 4095 takes the shortcut.
        UniformField f;
        f.height = -10 * U;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { 0, 0, 20 * U };
        const int32_t end_march[3] = { 4096, 0, 20 * U };
        check(!terrain_raycast_march(s, start, end_march, nullptr, nullptr), "(d) 4096 CLEAR");
        check_eq(f.point_calls, 1, "(d) |dx| == 4096 marches (1-sample budget)");
        check_eq(f.bilinear_calls, 0, "(d) |dx| == 4096 no shortcut sample");

        UniformField f2;
        f2.height = -10 * U;
        TerrainRaycastSampler s2 = uniform_sampler(f2);
        const int32_t end_col[3] = { 4095, 0, 12 * U };
        check(!terrain_raycast_march(s2, start, end_col, nullptr, nullptr),
              "(d) 4095 fully-above CLEAR");
        check_eq(f2.point_calls, 0, "(d) |dx| == 4095 takes the shortcut");
        check_eq(f2.bilinear_calls, 1, "(d) |dx| == 4095 one bilinear sample");
    }
    {
        // The shortcut's zeroed steps DO enter the refine — the guard skips
        // only (x==0 && y!=0 && z!=0) — as a harmless no-op walk
        // [orig: @ 0x60e74d..0x60e757]: result unchanged, and the refine's
        // 10 bilinear samples (1 back-walk probe + 1 forward-walk probe + 8
        // bisections) happen on top of the shortcut's 1.
        UniformField f;
        f.height = kPlane;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { kColX, kColY, 12 * U };
        const int32_t end[3] = { kColX + 0x800, kColY + 0xF00, 8 * U };
        int32_t hit[3] = { 0, 0, 0 };
        check(terrain_raycast_refined(s, start, end, hit), "(d) refined column HITs");
        check_eq(hit[0], kColX, "(d) refined column hit x unchanged (no-op walk)");
        check_eq(hit[1], kColY, "(d) refined column hit y unchanged");
        check_eq(hit[2], kPlane, "(d) refined column hit z unchanged");
        check_eq(f.bilinear_calls, 11, "(d) refine entered: 1 shortcut + 10 no-op walk samples");
    }

    // ---------------------------------------------------------------- (e)
    // The empty-cell height-0 floor [orig: the null-tile loop @ 0x60cea0..
    // 0x60cf4c]: over kEmpty the march hits exactly when the ray z reaches 0,
    // and the hit flows through the same step-storing epilogue.
    {
        UniformField f;
        f.kind = TerrainRaycastSample::kEmpty;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { 0, 0, 2 * U };
        const int32_t end[3] = { 16 * U, 0, -6 * U };
        int32_t hit[3] = { 0, 0, 0 };
        int32_t step[3] = { 0, 0, 0 };
        check(terrain_raycast_march(s, start, end, hit, step), "(e) empty-floor HIT");
        // step_z = -0x8000: cur_z reaches 0 exactly at k = 4.
        check_eq(hit[0], 4 * U, "(e) empty-floor hit x");
        check_eq(hit[1], 0, "(e) empty-floor hit y");
        check_eq(hit[2], 0, "(e) empty-floor hit z = ray z crossing 0");
        check_eq(step[0], 0x10000, "(e) empty-floor hit stores the step vector");
        check_eq(step[2], -0x8000, "(e) empty-floor hit step z");
        check_eq(f.point_calls, 5, "(e) samples k=0..4");
        check_eq(f.bilinear_calls, 0, "(e) the floor test never confirms");
    }
    {
        // Above the floor for the whole extent -> CLEAR.
        UniformField f;
        f.kind = TerrainRaycastSample::kEmpty;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { 0, 0, 2 * U };
        const int32_t end[3] = { 16 * U, 0, 2 * U };
        check(!terrain_raycast_march(s, start, end, nullptr, nullptr), "(e) above-floor CLEAR");
        check_eq(f.point_calls, 16, "(e) marches the full budget over empty cells");
    }
    {
        // Column shortcut over an empty cell: bilinear resolves to the
        // height-0 plane [orig: Terrain_SampleHeightBilinear @ 0x6067b0,
        // empty cell -> 0], so a crossing of z=0 HITs at height 0.
        UniformField f;
        f.kind = TerrainRaycastSample::kEmpty;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { kColX, kColY, 1 * U };
        const int32_t end[3] = { kColX, kColY, -1 * U };
        int32_t hit[3] = { 1, 2, 3 };
        check(terrain_raycast_march(s, start, end, hit, nullptr), "(e) empty column crossing HITs");
        check_eq(hit[2], 0, "(e) empty column hit z = the height-0 plane");
    }

    // ---------------------------------------------------------------- (f)
    // Out-of-extent (the editor-guard divergence; retail instead clamps the
    // cell to the grid edge [orig: @ 0x60cd50..0x60cd62]): no terrain, no
    // height-0 floor — never a hit.
    {
        UniformField f;
        f.kind = TerrainRaycastSample::kOutOfExtent;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { 0, 0, 2 * U };
        const int32_t end[3] = { 16 * U, 0, -6 * U };
        int32_t hit[3] = { 0x13572468, 0, 0 };
        check(!terrain_raycast_march(s, start, end, hit, nullptr),
              "(f) OOB march never hits (no height-0 floor)");
        check_eq(f.point_calls, 16, "(f) OOB marches the full budget");
        check_eq(hit[0], 0x13572468, "(f) OOB CLEAR leaves out_hit untouched");
    }
    {
        // Column shortcut over OOB: no terrain to cross -> CLEAR, outs
        // untouched (retail cannot reach this: its clamp always yields data).
        UniformField f;
        f.kind = TerrainRaycastSample::kOutOfExtent;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { kColX, kColY, 1 * U };
        const int32_t end[3] = { kColX, kColY, -1 * U };
        int32_t hit[3] = { 0x13572468, 0, 0 };
        int32_t step[3] = { 0x13572468, 0, 0 };
        check(!terrain_raycast_march(s, start, end, hit, step), "(f) OOB column CLEAR");
        check_eq(hit[0], 0x13572468, "(f) OOB column leaves out_hit untouched");
        check_eq(step[0], 0x13572468, "(f) OOB column leaves out_step untouched");
        check_eq(f.bilinear_calls, 1, "(f) OOB column sampled once");
    }

    // ---------------------------------------------------------------- (g)
    // The odd skip-refine guard [orig: @ 0x60e74d..0x60e757]: a pure y+z
    // diagonal stores step (0, y, z) -> the refine is SKIPPED and the
    // refined result equals the unrefined march hit; a nearby x!=0 ray
    // refines.
    {
        UniformField fm;
        fm.height = kPlane;
        TerrainRaycastSampler sm = uniform_sampler(fm);
        const int32_t start[3] = { 5 * U, 0, 0xC4000 };
        const int32_t end[3] = { 5 * U, 16 * U, 0x44000 }; // dx == 0
        int32_t march_hit[3] = { 0, 0, 0 };
        int32_t march_step[3] = { 0, 0, 0 };
        check(terrain_raycast_march(sm, start, end, march_hit, march_step), "(g) diagonal march hits");
        check_eq(march_step[0], 0, "(g) step x == 0");
        check(march_step[1] != 0 && march_step[2] != 0, "(g) step y/z != 0");

        UniformField fr;
        fr.height = kPlane;
        TerrainRaycastSampler sr = uniform_sampler(fr);
        int32_t refined[3] = { 0, 0, 0 };
        check(terrain_raycast_refined(sr, start, end, refined), "(g) diagonal refined hits");
        check_eq(refined[0], march_hit[0], "(g) guard skips: refined x == march x");
        check_eq(refined[1], march_hit[1], "(g) guard skips: refined y == march y");
        check_eq(refined[2], march_hit[2], "(g) guard skips: refined z == march z (still the ray z)");
        check_eq(fr.bilinear_calls, fm.bilinear_calls,
                 "(g) guard skips: no refine samples happened");

        // The contrasting x != 0 ray refines: z converges to the plane.
        const int32_t end2[3] = { 7 * U, 16 * U, 0x44000 }; // dx = 2 units
        UniformField fm2;
        fm2.height = kPlane;
        TerrainRaycastSampler sm2 = uniform_sampler(fm2);
        int32_t march_hit2[3] = { 0, 0, 0 };
        check(terrain_raycast_march(sm2, start, end2, march_hit2, nullptr), "(g) x!=0 march hits");
        UniformField fr2;
        fr2.height = kPlane;
        TerrainRaycastSampler sr2 = uniform_sampler(fr2);
        int32_t refined2[3] = { 0, 0, 0 };
        check(terrain_raycast_refined(sr2, start, end2, refined2), "(g) x!=0 refined hits");
        check(refined2[2] != march_hit2[2], "(g) x!=0 ray DID refine (z moved)");
        const int32_t err = refined2[2] - kPlane;
        check(err <= 65 && err >= -65, "(g) x!=0 refined z converges to the plane");
        check(fr2.bilinear_calls > fm2.bilinear_calls, "(g) x!=0 refine sampled");
    }

    // --- terrain_raycast_los_clear [orig: Terrain_RaycastHeightmapHiRes @ 0x60c760] ---
    {
        // (h) END under the bilinear surface -> immediate HIT, no march.
        // [orig: @ 0x60c7f8]
        UniformField f;
        f.height = 4 * U;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { 0, 0, 8 * U };
        const int32_t end[3] = { 64 * U, 0, 2 * U };
        check(!terrain_raycast_los_clear(s, start, end), "(h) end under surface hits");
        check(f.point_calls == 0, "(h) no march ran");
    }
    {
        // (i) Short segment (both axes under 2.0u): the START point decides.
        // [orig: @ 0x60c82e-0x60c86f]
        UniformField f;
        f.height = 4 * U;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t above_a[3] = { 0, 0, 8 * U };
        const int32_t above_b[3] = { U, U, 8 * U };
        check(terrain_raycast_los_clear(s, above_a, above_b), "(i) short both-above clear");
        check(f.point_calls == 0, "(i) short path never point-samples");
        const int32_t below_a[3] = { 0, 0, 2 * U };
        check(!terrain_raycast_los_clear(s, below_a, above_b), "(i) short start-under hits");
    }
    {
        // (j) The march sees terrain that rises BETWEEN clear endpoints: a
        // level 2u ray across the 6u ridge hits; an 8u ray clears. The march
        // uses POINT samples only [orig: @ 0x60c9c7 — no bilinear confirm].
        RidgeField f;
        TerrainRaycastSampler s = ridge_sampler(f);
        const int32_t lo_a[3] = { 0, 0, 2 * U };
        const int32_t lo_b[3] = { 64 * U, 0, 2 * U };
        const int bilinear_before = 0;
        check(!terrain_raycast_los_clear(s, lo_a, lo_b), "(j) ridge blocks the low ray");
        check(f.point_calls > 0, "(j) the march point-sampled");
        check(f.bilinear_calls - bilinear_before == 1, "(j) bilinear only at the END precheck");
        RidgeField f2;
        TerrainRaycastSampler s2 = ridge_sampler(f2);
        const int32_t hi_a[3] = { 0, 0, 8 * U };
        const int32_t hi_b[3] = { 64 * U, 0, 8 * U };
        check(terrain_raycast_los_clear(s2, hi_a, hi_b), "(j) high ray clears the ridge");
    }
    {
        // (k) The witnessed asymmetry: the long-march path never prechecks the
        // START — a start under the kEmpty height-0 floor hits at the first
        // sample [orig: the null-tile loop @ 0x60ca71-0x60ca77], while the END
        // precheck resolves kEmpty as the height-0 plane.
        UniformField f;
        f.kind = TerrainRaycastSample::kEmpty;
        TerrainRaycastSampler s = uniform_sampler(f);
        const int32_t start[3] = { 0, 0, -U };
        const int32_t end[3] = { 64 * U, 0, 8 * U };
        check(!terrain_raycast_los_clear(s, start, end), "(k) start under the empty floor hits");
        UniformField f2;
        f2.kind = TerrainRaycastSample::kEmpty;
        TerrainRaycastSampler s2 = uniform_sampler(f2);
        const int32_t start2[3] = { 0, 0, U };
        check(terrain_raycast_los_clear(s2, start2, end), "(k) above the empty floor clears");
    }

    if (g_fail) {
        std::fprintf(stderr, "terrain_raycast_test: FAILED\n");
        return 1;
    }
    std::printf("terrain_raycast_test: OK\n");
    return 0;
}
