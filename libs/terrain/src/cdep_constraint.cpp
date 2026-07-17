#include "terrain/cdep_constraint.h"

namespace opennova::terrain {

namespace {

// Quantized [min, max] of the 256 pixels of block `bx` in row `z`.
void block_min_max_raw(const float *heights, int width, int bx, int z, int &lo, int &hi) {
    const float *row = heights + static_cast<size_t>(z) * width;
    int x_lo = bx * CDEP_BLOCK_WIDTH;
    int x_hi = x_lo + CDEP_BLOCK_WIDTH;
    lo = 65535;
    hi = 0;
    for (int x = x_lo; x < x_hi; ++x) {
        int r = cdep_height_to_raw16(row[x]);
        if (r < lo) lo = r;
        if (r > hi) hi = r;
    }
}

// Clamp one block top-down, anchored at its quantized minimum. Returns true if
// the block was over-range (and thus modified). A pixel is lowered when its
// quantized value exceeds the ceiling, and is set to the float whose
// quantization is exactly the ceiling raw (raw/256 is exact in float32), so the
// re-read range is provably <= 32767. (When a block is over-range its min raw
// is <= 32768, so ceiling_raw <= 65535 and never needs its own clamp.)
bool clamp_block(float *heights, int width, int bx, int z) {
    int lo, hi;
    block_min_max_raw(heights, width, bx, z, lo, hi);
    if (hi - lo <= CDEP_MAX_BLOCK_RANGE_RAW)
        return false;
    int ceiling_raw = lo + CDEP_MAX_BLOCK_RANGE_RAW;
    float ceiling = cdep_raw16_to_height(static_cast<uint16_t>(ceiling_raw));
    float *row = heights + static_cast<size_t>(z) * width;
    int x_lo = bx * CDEP_BLOCK_WIDTH;
    int x_hi = x_lo + CDEP_BLOCK_WIDTH;
    for (int x = x_lo; x < x_hi; ++x) {
        if (cdep_height_to_raw16(row[x]) > ceiling_raw)
            row[x] = ceiling;
    }
    return true;
}

}  // namespace

int cdep_clamp_blocks_in_rect(float *heights, int width, int height, const CdepRect &rect) {
    if (!heights || width < CDEP_BLOCK_WIDTH || height <= 0 ||
        rect.x1 - rect.x0 <= 0 || rect.z1 - rect.z0 <= 0)
        return 0;
    int blocks_per_row = width / CDEP_BLOCK_WIDTH;
    if (blocks_per_row <= 0)
        return 0;
    int bx_lo = std::clamp(rect.x0 / CDEP_BLOCK_WIDTH, 0, blocks_per_row - 1);
    int bx_hi = std::clamp((rect.x1 - 1) / CDEP_BLOCK_WIDTH, 0, blocks_per_row - 1);
    int z_lo = std::clamp(rect.z0, 0, height - 1);
    int z_hi = std::clamp(rect.z1, 0, height);
    int clamped = 0;
    for (int z = z_lo; z < z_hi; ++z)
        for (int bx = bx_lo; bx <= bx_hi; ++bx)
            if (clamp_block(heights, width, bx, z))
                ++clamped;
    return clamped;
}

int cdep_count_violations(const float *heights, int width, int height) {
    if (!heights || width < CDEP_BLOCK_WIDTH || height <= 0)
        return 0;
    int blocks_per_row = width / CDEP_BLOCK_WIDTH;
    int count = 0;
    for (int z = 0; z < height; ++z)
        for (int bx = 0; bx < blocks_per_row; ++bx) {
            int lo, hi;
            block_min_max_raw(heights, width, bx, z, lo, hi);
            if (hi - lo > CDEP_MAX_BLOCK_RANGE_RAW)
                ++count;
        }
    return count;
}

int cdep_clamp_all_violations(float *heights, int width, int height) {
    if (!heights || width < CDEP_BLOCK_WIDTH || height <= 0)
        return 0;
    int blocks_per_row = width / CDEP_BLOCK_WIDTH;
    int clamped = 0;
    for (int z = 0; z < height; ++z)
        for (int bx = 0; bx < blocks_per_row; ++bx)
            if (clamp_block(heights, width, bx, z))
                ++clamped;
    return clamped;
}

}  // namespace opennova::terrain
