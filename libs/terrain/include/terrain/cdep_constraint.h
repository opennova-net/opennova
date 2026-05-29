#ifndef OPENNOVA_TERRAIN_CDEP_CONSTRAINT_H
#define OPENNOVA_TERRAIN_CDEP_CONSTRAINT_H

#include <algorithm>
#include <cstdint>
#include <cstddef>

// CDEP per-block range enforcement. Ported from the editor's
// terrain_editor_cdep_constraint.gd, corrected to operate in the raw16 integer
// domain.
//
// CDEP packs each heightmap row into blocks of 256 pixels with a 4-bit
// bits_per_delta field, so each block's (max - min) must fit in 15 bits as a
// uint16 (<= 32767). Editor heights are stored FORMAT_RF (float); the CDEP
// encoder (libs/cpt) and the bake guard
// (terrain_editor_document.cdep_ranges_valid) both test the range AFTER
// quantizing to raw16 via int(height * 256). The old GDScript clamp worked in
// float units (min_f + 32767/256.0) and, because float32 rounding of that
// ceiling can bump the quantized result up by one, could leave a baked raw
// range of 32768 -- one past the encoder's limit (a latent bug; the .gd header
// claimed a ULP margin the code never applied). Anchoring on the *quantized*
// block minimum here makes the post-clamp raw range provably <= 32767.

namespace opennova::terrain {

constexpr int CDEP_BLOCK_WIDTH = 256;
constexpr int CDEP_MAX_BLOCK_RANGE_RAW = 32767;

// raw16 quantization, identical to NovaTerrainData::get_depth_raw16 and the
// bake guard: truncate height*256 toward zero, clamp to [0, 65535].
inline uint16_t cdep_height_to_raw16(double height) {
    int raw = static_cast<int>(height * 256.0);
    if (raw < 0) raw = 0;
    if (raw > 65535) raw = 65535;
    return static_cast<uint16_t>(raw);
}

// Inverse: raw16 back to the editor float height. Exact in float32 for every
// raw16 input (raw/256 = raw * 2^-8, at most 16 significant bits).
inline float cdep_raw16_to_height(uint16_t raw) {
    return static_cast<float>(raw / 256.0);
}

// Half-open pixel rectangle [x0, x1) x [z0, z1).
struct CdepRect {
    int x0;
    int z0;
    int x1;
    int z1;
};

// Clamp every 256-pixel block intersecting `rect` so its quantized raw range is
// <= 32767, anchored at the block's quantized minimum. `heights` is a row-major
// float grid `width` x `height`; `width` must be a multiple of 256. Returns the
// number of blocks that were clamped.
int cdep_clamp_blocks_in_rect(float *heights, int width, int height, const CdepRect &rect);

// Count blocks in the whole grid whose quantized raw range exceeds the limit.
int cdep_count_violations(const float *heights, int width, int height);

// Clamp every over-range block in the whole grid, keeping the quantized minimum
// as anchor. Returns the number of blocks clamped.
int cdep_clamp_all_violations(float *heights, int width, int height);

}  // namespace opennova::terrain

#endif  // OPENNOVA_TERRAIN_CDEP_CONSTRAINT_H
