#pragma once

// Per-cell foliage placement, ported from jodemo.exe sub_5C0240@0x5C0240
// ("Terrain_BuildFoliagePatchData"). Generates up to FOLIAGE_CELL_CAP instances
// on a 6×6 candidate grid inside a 16-world-unit cell, rejecting candidates
// outside the caller's L∞ view radius or failing the foliagemap slot mask.
//
// Decomp reference: RE/kong/kong_output_jodemo/terrain.c line 15026.
// Spec reference:   docs/engine_spec_foliage.md §4.

#include <array>
#include <cstdint>
#include <functional>

#include "foliage/foliage.h"

namespace opennova::foliage {

// Engine constants. Do not change; these are byte-exact from the decomp.
constexpr int FOLIAGE_CELL_CAP = 21;          // sub_5C0240 early-break at a3[341] >= 21
constexpr int FOLIAGE_CANDIDATES_PER_CELL = 36; // 6×6 grid; v41 = 0..35
constexpr int FOLIAGE_CELL_GRID = 6;
constexpr float FOLIAGE_CANDIDATE_STEP = 2.5999999f;  // sub_5C0240 @ 0x5c033a
constexpr float FOLIAGE_CANDIDATE_BASE = 1.0f;
constexpr float FOLIAGE_RNG_FRAC_SCALE = 0.00002746582f; // 1/36394.67…
constexpr float FOLIAGE_ROTATION_SCALE = 0.000095873722f;  // sub_5C0240 @ 0x5c03e1

// 16.16 fixed-point helpers.
using Fixed16_16 = int32_t;
constexpr float FIXED_SCALE = 65536.0f;
constexpr float FIXED_TO_FLOAT = 1.0f / 65536.0f;

// Sampler callbacks, decoupled from the concrete FoliageMap / heightmap container.
// path_blocked: returns true if the given world-fixed point has a blocker (matches
// sub_5C6450 in the decomp — analogue for our port's charmap/path check; our port
// currently returns false unconditionally).
// slot_mask_at: returns a bitmask of which foliage slots (bit 0..3) are permitted
// at the given world-fixed point — analogue of sub_5C65E0 reading the foliagemap.
// height_at:   returns 16.16-fixed world height at the given 16.16 world coords —
// analogue of Terrain_SampleHeightBilinear@0x5C6770.
using PathBlockedFn = std::function<bool(Fixed16_16 wx, Fixed16_16 wz, int32_t range)>;
using SlotMaskFn = std::function<uint32_t(Fixed16_16 wx, Fixed16_16 wz)>;
using HeightFn = std::function<Fixed16_16(Fixed16_16 wx, Fixed16_16 wz)>;

struct PlacementSamplers {
	PathBlockedFn path_blocked;
	SlotMaskFn slot_mask_at;
	HeightFn height_at;
};

struct PlacementConfig {
	// FoliageDef attribute flags (currently only FORCE_ON is consumed in placement).
	uint8_t attrib_flags[FOLIAGE_MAX_DEFS] = {};
	// Per-slot quad half-width (engine: flt_15F9154[17*slot]). Controls corner
	// height sample offsets. World units.
	float quad_half_width[FOLIAGE_MAX_DEFS] = {1.0f, 1.0f, 1.0f, 1.0f};
	// Per-slot color_lower / color_upper mode codes parsed from the .trn.
	// Reserved for spec §4.4.8 band blend — placement doesn't read these yet
	// (the blend formula is §7.2 untraced). Plumbed through so callers don't
	// have to churn the config when the formula lands.
	int color_lower[FOLIAGE_MAX_DEFS] = {0, 0, 0, 0};
	int color_upper[FOLIAGE_MAX_DEFS] = {0, 0, 0, 0};
};

struct PlacementInstance {
	// Position: 16.16 fixed world x, z (engine coord convention — z is negated in
	// the per-cell code; consumer converts).
	Fixed16_16 world_x_fixed = 0;
	Fixed16_16 world_z_fixed = 0;
	// World y in 16.16 fixed (ground-snapped centre; corner heights stored separately).
	Fixed16_16 world_y_fixed = 0;

	// Rotation in radians.
	float rotation_radians = 0.0f;

	// 4 corner heights (TL, TR, BL, BR) and 4 midpoint heights (top, bottom, left, right)
	// as 16.16 fixed. For ground-hugging quads / tangent-space construction.
	Fixed16_16 corner_y_fixed[4] = {};
	Fixed16_16 midpoint_y_fixed[4] = {};
};

struct PlacementResult {
	std::array<PlacementInstance, FOLIAGE_CELL_CAP> instances{};
	int count = 0;
};

// Pack/unpack the 32-bit cell key used by the dispatcher's LRU.
// Layout: (x_fixed & 0x7FFF0000) | ((y_fixed >> 16) & 0x7FFF)
// Note the 15-bit y lose the top bit — the engine relies on this (sub_5C1940 @ 0x5c1a4b).
uint32_t pack_cell_key(Fixed16_16 cell_x_fixed, Fixed16_16 cell_z_fixed) noexcept;

// Bounded stand-in for spec §4.4.8 color_lower/color_upper band blend. The
// engine's exact formula is §7.2 untraced; the observable signal is slope
// magnitude (darker on steep ground). Given the Y component of a triangle
// normal, returns a grayscale shade in [SLOPE_SHADE_FLOOR, 1.0]:
//   - flat ground (|normal.y| ~= 1) → 1.0
//   - 45° slope (|normal.y| ~= 0.707) → 0.707
//   - near-vertical (|normal.y| < 0.4) → clamped to 0.4 so foliage stays visible
// Input is taken as absolute value internally so caller doesn't have to flip
// back-facing normals first.
constexpr float FOLIAGE_SLOPE_SHADE_FLOOR = 0.4f;
constexpr float foliage_slope_shade_from_normal_y(float normal_y) noexcept {
	if (normal_y < 0.0f) {
		normal_y = -normal_y;
	}
	if (normal_y < FOLIAGE_SLOPE_SHADE_FLOOR) {
		return FOLIAGE_SLOPE_SHADE_FLOOR;
	}
	if (normal_y > 1.0f) {
		return 1.0f;
	}
	return normal_y;
}

// Deterministic per-cell placement matching sub_5C0240.
//
//   slot_index:       foliage def slot (0..3)
//   cell_key:         packed 32-bit cell key (see pack_cell_key)
//   view_center_x/z:  16.16 fixed world coords of the visibility origin (camera/entity)
//   view_radius:      16.16 fixed L∞ radius; candidates outside are rejected
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
