#pragma once

// The retail terrain segment raycast (ENG-3 B1) — a faithful structural
// translation of the Jointops.exe heightmap raycast chain, Godot-agnostic,
// running over a caller-supplied world-space height sampler so the editor
// host (live Image sampling) and the runtime host (baked CPT atlas) adopt the
// same core. Witness record: docs/terrain/terrain-re.md §"Runtime terrain
// queries (ENG-3 B0)".
//
//   * terrain_raycast_march   [orig: Terrain_RaycastHeightmapLoRes @ 0x60cb80]
//     — the coarse march: ~1.0-world-unit steps along the major axis, all
//     math int32 16.16 fixed point; per sample a coarse POINT height test
//     confirmed by a BILINEAR sample; near-vertical segments take the
//     "column shortcut" (one bilinear sample, segment-crosses-surface test).
//   * terrain_raycast_refined [orig: Terrain_RaycastHeightmapHiRes_0 @ 0x60e710]
//     — the march plus a three-phase refine at quarter-unit steps (back-walk
//     while below, forward-walk while above, 8-iteration bisection); final
//     precision ~(1/4)/2^8 world unit along the ray.
//
// Axis convention: retail passes (x, y, z) with z = the HEIGHT axis, all
// 16.16 fixed-point world units; this port keeps that argument order. Retail
// internally remaps its working coords at entry (x += 0x8000, y -> 0x8000 - y:
// half-texel bias + heightmap V flip) and inverts the remap around every
// bilinear call and hit write [orig: 0x60cb9c..0x60cbb2, 0x60cdde,
// 0x60cf66..0x60cf78]; the bias/flip belong to the retail atlas substrate,
// which lives behind TerrainRaycastSampler here, so this core marches in
// world axes throughout. Consequences, called out where they land:
//   - the per-sample y step is computed from the world dy with NO negation
//     (retail stores the V-flipped y step in Terrain_LastRayStepY @ 0x319a29c
//     and negates it back in the refine);
//   - the coarse point sample retail takes at the biased coordinate (i.e. the
//     NEAREST texel of the world coordinate) is the host point callback's
//     policy — the core passes unbiased world coordinates to both callbacks.
// The step vector retail publishes through the Terrain_LastRayStep* globals
// (@ 0x319a298..a0, written on hit when a hit out was requested) is
// de-globalized into the out_step parameter / an internal local.
//
// EDITOR-GUARD DIVERGENCE (deliberate, same class as coords_editor_options
// vs coords_runtime_options in terrain/coords.h): retail clamps an
// out-of-extent sector-grid cell to the grid edge, so terrain continues
// forever [orig: the OOB masks @ 0x31a0010/0x319fc0c, the ~(cell >> 31)
// clamp-to-edge @ 0x60cd50..0x60cd62]. Our editor hosts instead report
// kOutOfExtent from the sampler and the core marches on WITHOUT any terrain
// test there — no hit, and no height-0 floor. A D-TERRAIN row gets minted at
// the B1b record update if the divergence is observable in a shipped surface.

#include <cstdint>

namespace opennova::terrain {

// [orig: Terrain_RaycastHeightmapLoRes @ 0x60cb80, gate @ 0x60cbe4..0x60cbf9]
// The column-shortcut threshold: both |dx| and |dy| under 4096 (1/16 world
// unit) take the single-sample column test instead of the march.
constexpr int32_t TERRAIN_RAYCAST_COLUMN_MAX_DELTA = 4096;
// [orig: @ 0x60ccb4/0x60cccc/0x60cce4] per-axis step rounding bias:
// step = (inv * delta + 0x8000) >> 16 on the signed 64-bit product.
constexpr int32_t TERRAIN_RAYCAST_STEP_ROUND_BIAS = 0x8000;
// [orig: @ 0x60ccef] the march budget: remaining starts at 0x10000 and loses
// inv = floor(2^32 / max_delta) per sample, i.e. the sample budget equals the
// major-axis extent in world units.
constexpr int32_t TERRAIN_RAYCAST_MARCH_BUDGET = 0x10000;
// [orig: Terrain_RaycastHeightmapHiRes_0 @ 0x60e710, @ 0x60e769..0x60e778]
// Refine steps = the march per-sample step, arithmetic >> 2 (quarter-unit).
constexpr int32_t TERRAIN_RAYCAST_REFINE_STEP_SHIFT = 2;
// [orig: @ 0x60e77c / 0x60e7bb] the back-walk / forward-walk counters. The
// witnessed budget test is on the counter's OLD value after each move
// (postfix), so a counter-terminated walk moves 9 times with 8 resamples.
constexpr int32_t TERRAIN_RAYCAST_REFINE_WALK_BUDGET = 8;
// [orig: @ 0x60e7f9..0x60e834] exactly eight bisection iterations.
constexpr int32_t TERRAIN_RAYCAST_REFINE_BISECT_ITERATIONS = 8;

// One height sample from the host.
//   kHeight      — height_1616 carries the terrain height (16.16 world units).
//   kEmpty       — an authored-empty sector (retail: a null tile). The march
//                  applies the height-0 floor there [orig: the null-tile loop
//                  @ 0x60cea0..0x60cf4c] and bilinear consumers resolve it to
//                  height 0 [orig: Terrain_SampleHeightBilinear @ 0x6067b0,
//                  empty cell -> 0 — the height-0 plane].
//   kOutOfExtent — beyond the authored sector grid (editor hosts; see the
//                  divergence note above): no terrain, no floor.
// height_1616 is ignored for kEmpty/kOutOfExtent.
struct TerrainRaycastSample {
	enum Kind {
		kHeight,
		kEmpty,
		kOutOfExtent,
	};
	Kind kind = kHeight;
	int32_t height_1616 = 0;
};

// The caller-supplied height source. Two callbacks because retail's march
// uses a coarse POINT sample (a single floor-texel read of the biased
// coordinate = the nearest texel of the world coordinate [orig: @ 0x60cdca])
// and confirms with the BILINEAR sampler [orig: Terrain_SampleHeightBilinear
// @ 0x6067b0]; the shortcut and the refine sample bilinear only. A host may
// map both callbacks to bilinear; the structural translation calls point for
// the coarse test. Both callbacks receive unbiased 16.16 world coordinates
// and must be non-null; ctx is passed through verbatim.
struct TerrainRaycastSampler {
	TerrainRaycastSample (*point)(void *ctx, int32_t world_x_1616, int32_t world_y_1616) = nullptr;
	TerrainRaycastSample (*bilinear)(void *ctx, int32_t world_x_1616, int32_t world_y_1616) = nullptr;
	void *ctx = nullptr;
};

// The coarse segment march [orig: Terrain_RaycastHeightmapLoRes @ 0x60cb80].
// start/end are (x, y, z = height) in 16.16 world units. Returns true on HIT
// (retail returns 0 = HIT, 1 = CLEAR; the polarity is flipped to bool here).
//
// On a march HIT: out_hit (nullable) receives the sample position with z =
// the RAY z at the hit sample, NOT the terrain height (the refine sharpens
// it), and out_step (nullable) receives the per-sample step vector [orig:
// stores to Terrain_LastRayStep* @ 0x319a298..a0]. On a march CLEAR neither
// out is written. The COLUMN SHORTCUT (both |dx| and |dy| < 4096) writes
// out_hit = (start x, start y, terrain height) and out_step = (0, 0, 0)
// BEFORE deciding hit/clear — i.e. even when it returns CLEAR — matching the
// witnessed order [orig: @ 0x60cc12..0x60cc2d]; its hit test is
// segment-crosses-surface, where a fully-buried segment reports CLEAR (the
// witnessed asymmetry: the march path instead hits at its first sample when
// starting below ground).
bool terrain_raycast_march(const TerrainRaycastSampler &sampler,
                           const int32_t start[3], const int32_t end[3],
                           int32_t out_hit[3], int32_t out_step[3]);

// March + refine [orig: Terrain_RaycastHeightmapHiRes_0 @ 0x60e710]. Runs
// terrain_raycast_march; on CLEAR returns false. On HIT with out_hit
// non-null, refines the hit along the ray at quarter-unit steps (back-walk /
// forward-walk / 8-iteration bisection) and writes the refined (x, y, z) to
// out_hit — unless the witnessed odd skip guard fires (see the source).
// With out_hit null the refine is skipped entirely [orig: @ 0x60e733].
bool terrain_raycast_refined(const TerrainRaycastSampler &sampler,
                             const int32_t start[3], const int32_t end[3],
                             int32_t out_hit[3]);

} // namespace opennova::terrain
