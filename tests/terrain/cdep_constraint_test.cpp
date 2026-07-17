// CDEP per-block range constraint, raw16 kernel (libs/terrain/cdep_constraint).
//
// Ports terrain_editor_cdep_constraint.gd into C++, correcting the float-domain
// clamp to the raw16 integer domain so the post-clamp quantized range can never
// exceed 32767 -- the limit the CDEP encoder and the bake guard
// (cdep_ranges_valid) actually test against.

#include "terrain/cdep_constraint.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace opennova::terrain;

static int g_fail = 0;

static void check(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        g_fail = 1;
    }
}

// Worst per-block quantized raw range across the whole grid.
static int max_block_range_raw(const std::vector<float> &h, int width, int height) {
    int worst = 0;
    int bpr = width / CDEP_BLOCK_WIDTH;
    for (int z = 0; z < height; ++z) {
        for (int bx = 0; bx < bpr; ++bx) {
            int lo = 65535, hi = 0;
            for (int x = bx * CDEP_BLOCK_WIDTH; x < (bx + 1) * CDEP_BLOCK_WIDTH; ++x) {
                int r = cdep_height_to_raw16(h[static_cast<size_t>(z) * width + x]);
                if (r < lo) lo = r;
                if (r > hi) hi = r;
            }
            if (hi - lo > worst) worst = hi - lo;
        }
    }
    return worst;
}

static int block0_row0_max_raw(const std::vector<float> &h, int) {
    int hi = 0;
    for (int x = 0; x < CDEP_BLOCK_WIDTH; ++x) {
        int r = cdep_height_to_raw16(h[x]);
        if (r > hi) hi = r;
    }
    return hi;
}

static int block0_row0_min_raw(const std::vector<float> &h, int) {
    int lo = 65535;
    for (int x = 0; x < CDEP_BLOCK_WIDTH; ++x) {
        int r = cdep_height_to_raw16(h[x]);
        if (r < lo) lo = r;
    }
    return lo;
}

int main() {
    // 1. raw16 quantization fixtures: truncation toward zero, clamped to uint16.
    //    Matches NovaTerrainData::get_depth_raw16 and the bake guard exactly.
    check(cdep_height_to_raw16(0.0) == 0, "0.0 -> 0");
    check(cdep_height_to_raw16(1.0) == 256, "1.0 -> 256");
    check(cdep_height_to_raw16(255.996) == 65534, "255.996 -> 65534 (truncation, not 65535)");
    check(cdep_height_to_raw16(256.0) == 65535, "256.0 -> clamped 65535");
    check(cdep_height_to_raw16(-1.0) == 0, "negative -> 0");

    // Invalid grids are safe no-ops across the public constraint entry points.
    CdepRect one_block{0, 0, CDEP_BLOCK_WIDTH, 1};
    float dummy = 0.0f;
    check(cdep_clamp_blocks_in_rect(&dummy, CDEP_BLOCK_WIDTH, 0, one_block) == 0,
          "zero-height rect clamp is a no-op");
    check(cdep_clamp_blocks_in_rect(nullptr, CDEP_BLOCK_WIDTH, 1, one_block) == 0,
          "null rect clamp is a no-op");
    check(cdep_count_violations(nullptr, CDEP_BLOCK_WIDTH, 1) == 0,
          "null violation count is zero");
    check(cdep_clamp_all_violations(nullptr, CDEP_BLOCK_WIDTH, 1) == 0,
          "null whole-grid clamp is a no-op");

    const int W = 512;  // 2 blocks per row
    const int H = 4;
    std::vector<float> grid(static_cast<size_t>(W) * H, 0.0f);

    // 2. A flat grid has no violations.
    check(cdep_count_violations(grid.data(), W, H) == 0, "flat grid: no violations");

    // 3. The latent-bug case from the audit: block 0 / row 0 with a quantized
    //    min of raw 28 and one pixel far above the range limit. The old float
    //    clamp anchored on the float min (0.11328) + 127.99609375, whose float32
    //    ceiling quantizes to raw 32796 -> range 32768 (one past the limit). The
    //    raw kernel anchors on the quantized min (28) -> ceiling raw 32795 ->
    //    range exactly 32767.
    for (int x = 0; x < CDEP_BLOCK_WIDTH; ++x)
        grid[x] = 0.11328f;  // int(0.11328 * 256) = 28  (whole block's floor)
    grid[1] = 200.0f;        // int(200 * 256)     = 51200 -> range 51172 >> 32767
    check(cdep_count_violations(grid.data(), W, H) == 1, "one over-range block detected");

    CdepRect rect{0, 0, 256, 1};
    int clamped = cdep_clamp_blocks_in_rect(grid.data(), W, H, rect);
    check(clamped == 1, "clamp_blocks_in_rect reports one block clamped");

    int lo = block0_row0_min_raw(grid, W);
    int hi = block0_row0_max_raw(grid, W);
    check(lo == 28, "anchor (quantized block min) preserved at raw 28");
    check(hi - lo <= CDEP_MAX_BLOCK_RANGE_RAW, "post-clamp raw range within limit");
    check(hi - lo == CDEP_MAX_BLOCK_RANGE_RAW, "post-clamp raw range is exactly anchor + 32767");
    check(cdep_count_violations(grid.data(), W, H) == 0, "no violations after clamp");

    // 4. Clip rect honored: a violation outside the rect is left untouched.
    std::fill(grid.begin(), grid.end(), 0.0f);
    grid[static_cast<size_t>(2) * W + 300] = 0.11328f;  // block 1, row 2
    grid[static_cast<size_t>(2) * W + 301] = 200.0f;    // violation in block 1, row 2
    CdepRect block0_only{0, 0, 256, H};
    int c2 = cdep_clamp_blocks_in_rect(grid.data(), W, H, block0_only);
    check(c2 == 0, "clamp limited to rect leaves the out-of-rect block alone");
    check(cdep_count_violations(grid.data(), W, H) == 1, "out-of-rect violation still present");

    // 5. clamp_all_violations heals the whole grid and reports the count.
    int healed = cdep_clamp_all_violations(grid.data(), W, H);
    check(healed == 1, "clamp_all_violations reports one block healed");
    check(cdep_count_violations(grid.data(), W, H) == 0, "clamp_all leaves no violations");
    check(max_block_range_raw(grid, W, H) <= CDEP_MAX_BLOCK_RANGE_RAW,
          "every block within the raw limit after clamp_all");

    if (g_fail) {
        std::fprintf(stderr, "cdep_constraint_test FAILED\n");
        return 1;
    }
    std::printf("OK: cdep_constraint raw16 kernel\n");
    return 0;
}
