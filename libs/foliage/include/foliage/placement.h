#pragma once

// Retail FAR foliage candidate placement. Generates up to FAR_CELL_CAP
// placements on a 6x6 grid inside a 16-world-unit cell. Source geometry and
// terrain bending belong to far_mesh_emitter.h; FAR does not synthesize or
// ground-fit a quad. [orig: generate_foliage_instances_0 @ 0x5ffdd0]

#include <array>
#include <cstdint>
#include <functional>

#include "foliage/foliage.h"

namespace opennova::foliage {

// Engine constants. Do not change; these are byte-exact from the decomp.
constexpr int FOLIAGE_CANDIDATES_PER_CELL = 36;    // 6x6 grid; v41 = 0..35
constexpr int FAR_CELL_CAP = FOLIAGE_CANDIDATES_PER_CELL;  // FAR has no accepted-count early cap
constexpr int FOLIAGE_CELL_GRID = 6;
constexpr float FOLIAGE_CANDIDATE_STEP = 2.5999999f;       // 0x5C033A
constexpr float FOLIAGE_CANDIDATE_BASE = 1.0f;
constexpr float FOLIAGE_RNG_FRAC_SCALE = 0.00002746582f;   // 1/36394.67...
constexpr float FOLIAGE_ROTATION_SCALE = 0.000095873722f;  // 0x5C03E1

// 16.16 fixed-point helpers.
using Fixed16_16 = int32_t;
constexpr float FIXED_SCALE = 65536.0f;
constexpr float FIXED_TO_FLOAT = 1.0f / 65536.0f;

// Sampler callbacks, decoupled from the concrete FoliageMap / heightmap container.
// path_blocked: returns true if the given world-fixed point has a blocker
// (Terrain_IsNearAmbientSource in the decomp). Our port currently returns false
// unconditionally.
// slot_mask_at: returns a bitmask of permitted foliage slots (bit 0..3).
// FAR supplies the raw surface-map byte at (x,-z); MODEL supplies its
// foliage-map-derived mask at (x,z).
// height_at: returns 16.16-fixed world height at the given 16.16 world coords,
// analogous to Terrain_SampleHeightBilinear@0x005C6770.
using PathBlockedFn = std::function<bool(Fixed16_16 wx, Fixed16_16 wz, int32_t range)>;
using SlotMaskFn = std::function<uint32_t(Fixed16_16 wx, Fixed16_16 wz)>;
using HeightFn = std::function<Fixed16_16(Fixed16_16 wx, Fixed16_16 wz)>;

struct PlacementSamplers {
	PathBlockedFn path_blocked;
	SlotMaskFn slot_mask_at;
	HeightFn height_at;
};

struct PlacementConfig {
	// Retail FAR placement consumes only FORCE_ON from the def attributes.
	uint8_t attrib_flags[FOLIAGE_MAX_DEFS] = {};
};

struct PlacementInstance {
	// Position: 16.16 fixed world x, z (engine coordinate convention).
	Fixed16_16 world_x_fixed = 0;
	Fixed16_16 world_z_fixed = 0;

	// Rotation in radians.
	float rotation_radians = 0.0f;
};

struct PlacementResult {
	std::array<PlacementInstance, FAR_CELL_CAP> instances{};
	int count = 0;
};

// Pack/unpack the 32-bit cell key used by the dispatcher's LRU.
// Layout: (x_fixed & 0x7FFF0000) | ((z_fixed >> 16) & 0x7FFF)
// Note the 15-bit z loses the top bit; the engine relies on this
// (Foliage_RenderAtPosition@0x005C1940, 0x5C1A4B).
uint32_t pack_cell_key(Fixed16_16 cell_x_fixed, Fixed16_16 cell_z_fixed) noexcept;

// Deterministic retail FAR per-cell candidate placement.
//
//   slot_index:       foliage def slot (0..3)
//   cell_key:         packed 32-bit cell key (see pack_cell_key)
//   view_center_x/z:  16.16 fixed world coords of the visibility origin
//   view_radius:      16.16 fixed L-infinity radius; candidates outside reject
//
// Returns the populated result. Count is in [0, FAR_CELL_CAP]. Unlike the
// MODEL tier's separate MODEL_TILE_CAP=21, all 36 FAR candidates may survive.
PlacementResult place_cell(int slot_index,
                           uint32_t cell_key,
                           Fixed16_16 view_center_x,
                           Fixed16_16 view_center_z,
                           int32_t view_radius,
                           const PlacementConfig &config,
                           const PlacementSamplers &samplers) noexcept;

} // namespace opennova::foliage
