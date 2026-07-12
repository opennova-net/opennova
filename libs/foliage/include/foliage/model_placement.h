#pragma once

// The foliage MODEL tier: per-tile 3DI-cluster placement around sector
// entities, ported from retail Jointops.exe. Retail runs TWO foliage tiers -
// the deterministic FAR candidate placements (placement.h) at range, and this
// NEAR tier
// stamping the def graphic's full 3DI geometry in clusters around visible
// sector entities. Witness record: docs/foliage/foliage-re.md §The model tier.
//
// [orig: Foliage_GenerateModelTileInstances @ 0x600980 (the per-tile
// generator), Foliage_UpdateModelTiles @ 0x601f50 (the quadrant walk / key
// form), Terrain_RenderSectorEntitiesBySide @ 0x5c7d50 (the driver: per
// visible sector entity, depth >= 38, radius 0x40000, alpha ref curve)]

#include <array>
#include <cstdint>

#include "foliage/foliage.h"
#include "foliage/placement.h"

namespace opennova::foliage {

// Engine constants - byte-exact from the retail decomp. Do not change.
constexpr int MODEL_TILE_CAP = 21;             // 21 accepted per tile; the draw buffers are sized for exactly 21 [orig: Foliage_InitModelTileBuffers @ 0x5ffcd0]
constexpr int MODEL_CANDIDATES_PER_TILE = 36;  // 6x6 grid [orig: Foliage_GenerateModelTileInstances @ 0x600980]
constexpr int MODEL_TILE_GRID = 6;
constexpr float MODEL_CANDIDATE_STEP = 2.6f;           // constant @ 0x7D8E50
constexpr float MODEL_CANDIDATE_MARGIN = 1.0f;         // grid margin inside the 16u tile
constexpr float MODEL_RNG_FRAC_SCALE = 1.8f / 65536.0f;  // constant @ 0x7DE9CC (jitter 0..1.8)
constexpr float MODEL_YAW_SCALE =
    6.28318530717958647692f / 65536.0f;  // 2*pi/65536, constant @ 0x7CD4DC

// Per-axis candidate radius around the anchor entity, 16.16 fixed (+-4.0 u,
// Chebyshev). FIXED in retail: the single driver call site passes 0x40000
// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d50 ->
// Foliage_UpdateModelTiles(slot, entityPos, 0x40000, alphaRef)].
constexpr int32_t MODEL_CANDIDATE_RADIUS = 0x40000;
// Path/spacing reject range, 16.16 fixed (2.0 u), same family as the FAR
// tier [orig: sub_606490(x, -z, 0x20000) in Foliage_GenerateModelTileInstances].
constexpr int32_t MODEL_PATH_SPACING = 0x20000;
// Footprint = 0.75 * Chebyshev bound radius [orig: Foliage_DefTable_Footprint
// @ 0x316207C, filled by Foliage_LoadDefAssets @ 0x601260].
constexpr float MODEL_FOOTPRINT_SCALE = 0.75f;

// Tile walk constants [orig: Foliage_UpdateModelTiles @ 0x601f50].
constexpr Fixed16_16 MODEL_QUADRANT_OFFSET = 0x80000;  // +-8.0 u around the anchor
constexpr Fixed16_16 MODEL_TILE_SIZE = 0x100000;       // 16.0 u
constexpr Fixed16_16 MODEL_TILE_SNAP_MASK =
    static_cast<Fixed16_16>(0xFFF00000);  // aligns to 16 u

// Alpha-test reference curve [orig: Terrain_RenderSectorEntitiesBySide
// @ 0x5c7d50]: ref = clamp(int(4096.0 / (dist_units + 1)), 8, 128) where
// dist_units = anchor-to-camera distance >> 16 (integer world units).
// Near entities get a TIGHT silhouette (128), far ones a fat one (8,
// compensating mip alpha erosion). flt_7C6F90 = 4096.0.
constexpr float MODEL_ALPHA_REF_NUMERATOR = 4096.0f;
constexpr int MODEL_ALPHA_REF_MIN = 8;
constexpr int MODEL_ALPHA_REF_MAX = 128;

// Per-def model-tier config. Mirrors the witnessed def table
// (Foliage_DefTable_* @ 0x3162060): attrib flags from the module def copy
// (record byte +532; bit 0 = FOLIAGE_ATTRIB_FORCE_ON skips the path/spacing
// gate) and the precomputed footprint (0.75 * Chebyshev bound radius).
struct ModelPlacementConfig {
	uint8_t attrib_flags[FOLIAGE_MAX_DEFS] = {};
	float footprint[FOLIAGE_MAX_DEFS] = {1.0f, 1.0f, 1.0f, 1.0f};
};

// One accepted model instance. Corner order k = 0..3 with local footprint
// coords (A, B) = (k&1 ? +F : -F, k&2 ? +F : -F); the yaw rotation lives
// entirely in the corner geometry (the model itself stamps UPRIGHT, yaw-only)
// [orig: Foliage_GenerateModelTileInstances @ 0x600980].
struct ModelInstance {
	// Candidate center, world 16.16 fixed.
	Fixed16_16 center_x_fixed = 0;
	Fixed16_16 center_z_fixed = 0;
	// Yaw in radians (draw * 2*pi/65536).
	float yaw_radians = 0.0f;
	// The 4 rotated footprint corners, world 16.16 fixed.
	Fixed16_16 corner_x_fixed[4] = {};
	Fixed16_16 corner_z_fixed[4] = {};
	// Corner ground heights, world units (height_at / 65536).
	float corner_height[4] = {};
	// The 8-sample biquadratic fold (E_A, T_A, E_B, T_B): per edge the sag
	// D_edge = h_mid - (h_cornerA + h_cornerB)/2, folded to
	// E_A = (D_-A + D_+A)/2, T_A = D_+A - E_A (same for B). Consumed by the
	// grid-placement VS sag polynomial (model_ground_height below).
	float fold_e_a = 0.0f;
	float fold_t_a = 0.0f;
	float fold_e_b = 0.0f;
	float fold_t_b = 0.0f;
};

struct ModelTileResult {
	std::array<ModelInstance, MODEL_TILE_CAP> instances{};
	int count = 0;
};

// One walked quadrant tile: packed key + the snapped 16u tile origin the key
// encodes. The tile covers [snap, snap+16]^2 in world units.
struct ModelTileRef {
	uint32_t key = 0;
	Fixed16_16 snap_x_fixed = 0;
	Fixed16_16 snap_z_fixed = 0;
};

// Pack the model-tile key from the SNAPPED (16u-aligned) tile origin:
//   key = (snapX & 0x7FFF0000) | (((snapZ + 0x100000) >> 16) & 0x7FFF)
// keyHigh = snapX in world units; keyLow = snapZ units + 16 - the +16
// compensates the negated local B axis (candidates run keyLow - localB), so
// the tile still covers [snap, snap+16]^2
// [orig: Foliage_UpdateModelTiles @ 0x601f50].
uint32_t pack_model_tile_key(Fixed16_16 snap_x_fixed, Fixed16_16 snap_z_fixed) noexcept;

// The 4 quadrant tiles for an anchor: the 16u cells (snap mask 0xFFF00000)
// overlapping anchor +-0x80000 (+-8 u) on each axis
// [orig: Foliage_UpdateModelTiles @ 0x601f50].
std::array<ModelTileRef, 4> model_quadrant_tiles(Fixed16_16 anchor_x_fixed,
                                                 Fixed16_16 anchor_z_fixed) noexcept;

// Deterministic per-tile model placement
// [orig: Foliage_GenerateModelTileInstances @ 0x600980].
//
//   slot_index:             foliage def slot (0..3)
//   tile_key:               packed tile key (see pack_model_tile_key)
//   anchor_x/z_fixed:       16.16 fixed world coords of the anchor entity
//   candidate_radius_fixed: per-axis Chebyshev gate radius, 16.16 fixed. The
//                           retail walk always passes MODEL_CANDIDATE_RADIUS
//                           (0x40000); it is an argument in the original.
//
// PRNG: state = (key & 0x1FF01FF) + ROL32(0xA55B1EED, key & 0x1F); step
// state = ROL32(state + ROL32(state, 11), 4) ^ 1, value = (u16)state; three
// draws per candidate (jitter A, jitter B, yaw). Candidate axis form:
// worldX = (keyHigh + localA) * 65536, worldZ = (keyLow - localB) * 65536 -
// the local B axis runs NEGATIVE world Z. Gates in order: per-axis
// |world - anchor| <= radius; path spacing path_blocked(x, -z, 0x20000)
// skipped on FOLIAGE_ATTRIB_FORCE_ON; foliage-map mask
// (1 << slot) & slot_mask_at(x, z) [orig: Foliage_SampleFoliageMapMask
// @ 0x606620]. Cap MODEL_TILE_CAP accepted per tile.
ModelTileResult generate_model_tile_instances(int slot_index,
                                              uint32_t tile_key,
                                              Fixed16_16 anchor_x_fixed,
                                              Fixed16_16 anchor_z_fixed,
                                              int32_t candidate_radius_fixed,
                                              const ModelPlacementConfig &config,
                                              const PlacementSamplers &samplers) noexcept;

// The witnessed alpha-test reference curve for a given anchor distance in
// integer world units [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d50]:
// clamp(int(4096.0 / (dist_units + 1)), 8, 128).
int model_alpha_ref(int32_t dist_units) noexcept;

// The instance base height the host transform carries: the bilinear ground
// height at the instance CENTER = corner average + sag(0,0) = E_A + E_B
// (bilinear weights at (0.5, 0.5) are 0.25 each).
float model_instance_hbase(const ModelInstance &inst) noexcept;

// Reference implementation of the grid-placement VS ground fit, in the
// ENGINE axis convention: x_n runs along the local A axis (0 at A = -F,
// 1 at A = +F), z_n along B. Bilinear weights
// w = ((1-x_n)(1-z_n), x_n(1-z_n), (1-x_n)z_n, x_n*z_n) over corners k=0..3,
// plus the sag polynomial with u = 2x_n - 1, v = 2z_n - 1:
//   sag = (1-v^2)(E_A + u*T_A) + (1-u^2)(E_B + v*T_B)
// Exact at the 4 corners AND reproduces each edge-midpoint sample exactly
// [orig: Foliage_GridPlacementVS, assembled in
// Terrain_CreateFoliageVertexShaders @ 0x5ff630].
float model_ground_height(const ModelInstance &inst, float x_n, float z_n) noexcept;

} // namespace opennova::foliage
