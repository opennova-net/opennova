#pragma once

// Per-cell foliage placement, ported from jodemo.exe
// Foliage_BuildPatchData@0x005C0240. Generates up to FOLIAGE_CELL_CAP
// instances on a 6x6 candidate grid inside a 16-world-unit cell, rejecting
// candidates outside the caller's L-infinity view radius or failing the
// foliagemap slot mask.

#include <array>
#include <cstdint>
#include <functional>

#include "foliage/foliage.h"

namespace opennova::foliage {

// Engine constants. Do not change; these are byte-exact from the decomp.
constexpr int FOLIAGE_CELL_CAP = 21;               // 0x5C080D cap
constexpr int FOLIAGE_CANDIDATES_PER_CELL = 36;    // 6x6 grid; v41 = 0..35
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
// slot_mask_at: returns a bitmask of which foliage slots (bit 0..3) are
// permitted at the given world-fixed point, analogous to
// Terrain_GetFoliageMapValue@0x005C65E0.
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
	// FoliageDef attribute flags. Foliage_BuildPatchData only consumes FORCE_ON.
	uint8_t attrib_flags[FOLIAGE_MAX_DEFS] = {};
	// Per-slot quad half-width. Engine source: flt_15F9154[17 * slot].
	float quad_half_width[FOLIAGE_MAX_DEFS] = {1.0f, 1.0f, 1.0f, 1.0f};
	// Per-slot color modes parsed from the .trn. The final retail render emitter
	// still needs a verified anchor; placement keeps these in config for downstream
	// render parity.
	int color_lower[FOLIAGE_MAX_DEFS] = {0, 0, 0, 0};
	int color_upper[FOLIAGE_MAX_DEFS] = {0, 0, 0, 0};
};

struct PlacementInstance {
	// Position: 16.16 fixed world x, z (engine coordinate convention).
	Fixed16_16 world_x_fixed = 0;
	Fixed16_16 world_z_fixed = 0;
	// World y in 16.16 fixed. The engine consumes the corner/midpoint heights;
	// this center height is retained for the Godot MultiMesh adapter.
	Fixed16_16 world_y_fixed = 0;

	// Rotation in radians.
	float rotation_radians = 0.0f;

	// 4 corner heights (TL, TR, BL, BR) and 4 midpoint heights (top, bottom,
	// left, right) as 16.16 fixed. Foliage_BuildPatchData samples these before
	// computing the four patch_control values below.
	Fixed16_16 corner_y_fixed[4] = {};
	Fixed16_16 midpoint_y_fixed[4] = {};

	// Four height-derived patch control values written by
	// Foliage_BuildPatchData@0x005C0240 at 0x5C07E8..0x5C07FC. These are not
	// final colors; the still-unrecovered render emitter sub_5C1790 appears to
	// consume them as vertex-shader constants/control data.
	float patch_control[4] = {};
};

struct PlacementResult {
	std::array<PlacementInstance, FOLIAGE_CELL_CAP> instances{};
	int count = 0;
};

// Pack/unpack the 32-bit cell key used by the dispatcher's LRU.
// Layout: (x_fixed & 0x7FFF0000) | ((z_fixed >> 16) & 0x7FFF)
// Note the 15-bit z loses the top bit; the engine relies on this
// (Foliage_RenderAtPosition@0x005C1940, 0x5C1A4B).
uint32_t pack_cell_key(Fixed16_16 cell_x_fixed, Fixed16_16 cell_z_fixed) noexcept;

// Deterministic per-cell placement matching Foliage_BuildPatchData@0x005C0240.
//
//   slot_index:       foliage def slot (0..3)
//   cell_key:         packed 32-bit cell key (see pack_cell_key)
//   view_center_x/z:  16.16 fixed world coords of the visibility origin
//   view_radius:      16.16 fixed L-infinity radius; candidates outside reject
//
// Returns the populated result. Count is in [0, FOLIAGE_CELL_CAP].
PlacementResult place_cell(int slot_index,
                           uint32_t cell_key,
                           Fixed16_16 view_center_x,
                           Fixed16_16 view_center_z,
                           int32_t view_radius,
                           const PlacementConfig &config,
                           const PlacementSamplers &samplers) noexcept;

} // namespace opennova::foliage
