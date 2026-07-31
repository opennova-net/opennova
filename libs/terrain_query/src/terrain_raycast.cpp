#include "terrain/terrain_raycast.h"

// Structural translation of the retail heightmap raycast chain; see the
// header for the model and the axis-convention / editor-guard notes.
// [orig: Terrain_RaycastHeightmapLoRes @ 0x60cb80,
//        Terrain_RaycastHeightmapHiRes_0 @ 0x60e710]

namespace opennova::terrain {

namespace {

// x86 abs idiom (cdq/xor/sub): wraps INT32_MIN to itself rather than UB.
inline int32_t abs32(int32_t v) {
	return v < 0 ? static_cast<int32_t>(0u - static_cast<uint32_t>(v)) : v;
}

// Resolve a BILINEAR sample to a surface height for the shortcut / confirm /
// refine paths. kHeight -> the height; kEmpty -> 0, matching retail's
// bilinear sampler over an empty (null-tile) cell [orig:
// Terrain_SampleHeightBilinear @ 0x6067b0, empty cell -> the height-0 plane].
// kOutOfExtent -> no surface (returns false): the editor-guard divergence —
// retail can't sample out-of-extent (its cell clamp-to-edge always yields
// data); see the header note.
inline bool resolve_surface_bilinear(const TerrainRaycastSampler &sampler,
                                     int32_t world_x, int32_t world_y, int32_t *out_height) {
	const TerrainRaycastSample s = sampler.bilinear(sampler.ctx, world_x, world_y);
	switch (s.kind) {
		case TerrainRaycastSample::kHeight:
			*out_height = s.height_1616;
			return true;
		case TerrainRaycastSample::kEmpty:
			*out_height = 0;
			return true;
		case TerrainRaycastSample::kOutOfExtent:
		default:
			return false;
	}
}

// The refine compares the ray point against the surface each step; over
// kOutOfExtent there is no surface (editor guard), which resolves as
// "bottomless" (INT32_MIN): the point never reads as below it, so the
// back-walk stops and the forward-walk/bisection push toward real terrain.
inline int32_t resolve_refine_height(const TerrainRaycastSampler &sampler,
                                     int32_t world_x, int32_t world_y) {
	int32_t h = 0;
	if (!resolve_surface_bilinear(sampler, world_x, world_y, &h)) {
		return INT32_MIN;
	}
	return h;
}

} // namespace

// [orig: Terrain_RaycastHeightmapLoRes @ 0x60cb80] retail returns 0 = HIT,
// 1 = CLEAR; ported as bool hit. Retail's null-atlas early-out (no terrain
// loaded -> return HIT without writing the hit out [orig: @ 0x60ccf7]) is not
// ported: data presence lives behind the sampler seam, so an embedder without
// terrain decides its own answer before calling.
bool terrain_raycast_march(const TerrainRaycastSampler &sampler,
                           const int32_t start[3], const int32_t end[3],
                           int32_t out_hit[3], int32_t out_step[3]) {
	const int32_t dx = end[0] - start[0];
	const int32_t dy = end[1] - start[1];
	const int32_t dz = end[2] - start[2];
	const int32_t abs_dx = abs32(dx);
	const int32_t abs_dy = abs32(dy);

	// Column-shortcut gate — the witnessed operator shapes kept [orig:
	// @ 0x60cbe4..0x60cbf9]: with abs_dx != 0 the march is taken as soon as
	// abs_dx >= 4096; with abs_dx == 0 the abs_dy < 4096 test only runs when
	// abs_dy != 0 (both-zero takes an early path straight into the shortcut).
	// Net predicate: both deltas under 4096 (1/16 world unit).
	bool column_shortcut;
	if (abs_dx != 0) {
		column_shortcut = abs_dx < TERRAIN_RAYCAST_COLUMN_MAX_DELTA &&
				abs_dy < TERRAIN_RAYCAST_COLUMN_MAX_DELTA;
	} else if (abs_dy != 0) {
		column_shortcut = abs_dy < TERRAIN_RAYCAST_COLUMN_MAX_DELTA;
	} else {
		column_shortcut = true; // both-zero early path [orig: @ 0x60cbe8]
	}

	if (column_shortcut) {
		// ONE bilinear sample at the start column [orig: @ 0x60cbfb].
		int32_t h = 0;
		if (!resolve_surface_bilinear(sampler, start[0], start[1], &h)) {
			// Editor-guard divergence: a column probe beyond the authored
			// extent has no terrain to cross -> CLEAR, outs untouched.
			return false;
		}
		// Witnessed order: the hit out (terrain height as z, unlike the
		// march's ray z) and the zeroed step vector are written BEFORE the
		// hit/clear decision — even when this path returns CLEAR
		// [orig: @ 0x60cc12..0x60cc2d].
		if (out_hit) {
			out_hit[0] = start[0];
			out_hit[1] = start[1];
			out_hit[2] = h;
		}
		if (out_step) {
			out_step[0] = 0;
			out_step[1] = 0;
			out_step[2] = 0;
		}
		// HIT iff the segment crosses (or touches) the surface [orig:
		// @ 0x60cc33..0x60cc52]: both endpoints strictly above -> CLEAR;
		// both strictly below -> CLEAR. The latter is the WITNESSED
		// ASYMMETRY, ported verbatim: a fully-buried column probe reports
		// CLEAR, while the march path hits at its FIRST sample when starting
		// below ground.
		if (h < start[2] && h < end[2]) {
			return false; // both endpoints above the surface
		}
		if (h > start[2] && h > end[2]) {
			return false; // both endpoints below the surface (asymmetry)
		}
		return true;
	}

	// Step normalization [orig: @ 0x60cc72..0x60cce4]: inv = floor(2^32 /
	// max(|dx|, |dy|)) — an unsigned 64-bit divide of 0x100000000 by the
	// int32 delta (the gate guarantees max_delta >= 4096, so inv fits int32
	// and the divide is nonzero); the witnessed tie shape picks abs_dy when
	// abs_dx <= abs_dy. Per-axis step = (inv * delta + 0x8000) >> 16 on the
	// signed 64-bit product — ~1.0 world unit along the major axis. The y
	// step comes from the WORLD dy with no negation (retail computes it in
	// the V-flipped axis and negates it back downstream; one axis convention
	// throughout here — see the header; the two differ only when the
	// product's low 16 bits are exactly 0x8000).
	const int32_t max_delta = (abs_dx <= abs_dy) ? abs_dy : abs_dx;
	const int32_t inv = static_cast<int32_t>(0x100000000ULL / static_cast<uint32_t>(max_delta));
	const int32_t step_x = static_cast<int32_t>(
			(static_cast<int64_t>(inv) * dx + TERRAIN_RAYCAST_STEP_ROUND_BIAS) >> 16);
	const int32_t step_y = static_cast<int32_t>(
			(static_cast<int64_t>(inv) * dy + TERRAIN_RAYCAST_STEP_ROUND_BIAS) >> 16);
	const int32_t step_z = static_cast<int32_t>(
			(static_cast<int64_t>(inv) * dz + TERRAIN_RAYCAST_STEP_ROUND_BIAS) >> 16);

	int32_t cur_x = start[0];
	int32_t cur_y = start[1];
	int32_t cur_z = start[2];
	int32_t remaining = TERRAIN_RAYCAST_MARCH_BUDGET; // [orig: @ 0x60ccef]

	// Retail splits the walk into a tiled loop (coarse point test + bilinear
	// confirm [orig: @ 0x60cd92..0x60ce95]) and a null-tile loop (height-0
	// floor test only [orig: @ 0x60cea0..0x60cf4c]), switching on the cell
	// re-resolve at 512-unit crossings (frac & 0xFE000000 [orig: @ 0x60ce20]).
	// The cell resolve lives behind the sampler seam here, so the two loops
	// collapse into one per-sample kind dispatch with the identical
	// per-sample order: test at the current position, then decrement the
	// budget, advance, and CLEAR once the budget is spent
	// [orig: @ 0x60cdf6..0x60ce10].
	for (;;) {
		const TerrainRaycastSample coarse = sampler.point(sampler.ctx, cur_x, cur_y);
		switch (coarse.kind) {
			case TerrainRaycastSample::kHeight: {
				// Coarse point test, then bilinear confirm; both comparisons
				// include equality [orig: @ 0x60cdca..0x60cdec].
				if (coarse.height_1616 >= cur_z) {
					int32_t h = 0;
					if (resolve_surface_bilinear(sampler, cur_x, cur_y, &h) && h >= cur_z) {
						// HIT: the ray position, with z = the RAY z (not the
						// terrain height), plus the per-sample step vector
						// [orig: the shared hit epilogue @ 0x60cf5d..0x60cf8e;
						// retail stores the steps to Terrain_LastRayStep*
						// only when a hit out was requested — de-globalized
						// to an independent nullable out here].
						if (out_hit) {
							out_hit[0] = cur_x;
							out_hit[1] = cur_y;
							out_hit[2] = cur_z;
						}
						if (out_step) {
							out_step[0] = step_x;
							out_step[1] = step_y;
							out_step[2] = step_z;
						}
						return true;
					}
				}
				break;
			}
			case TerrainRaycastSample::kEmpty: {
				// The null-tile floor: an authored-empty cell hits as soon as
				// the ray z reaches the height-0 plane, tested at the current
				// position before advancing [orig: @ 0x60cea0..0x60cea6; the
				// hit flows into the same step-storing epilogue @ 0x60cf5d].
				if (cur_z <= 0) {
					if (out_hit) {
						out_hit[0] = cur_x;
						out_hit[1] = cur_y;
						out_hit[2] = cur_z;
					}
					if (out_step) {
						out_step[0] = step_x;
						out_step[1] = step_y;
						out_step[2] = step_z;
					}
					return true;
				}
				break;
			}
			case TerrainRaycastSample::kOutOfExtent:
			default:
				// Editor-guard divergence (see the header): retail clamps the
				// cell to the grid edge and keeps testing [orig: the OOB
				// masks @ 0x31a0010/0x319fc0c, clamp @ 0x60cd50..0x60cd62];
				// our editor shells report OOB and the march continues with NO
				// terrain test and NO height-0 floor. Same deliberate guard
				// class as coords_editor_options vs coords_runtime_options
				// (terrain/coords.h).
				break;
		}
		remaining -= inv;   // [orig: @ 0x60cdf6]
		cur_x += step_x;    // [orig: @ 0x60cdfe..0x60ce06]
		cur_y += step_y;
		cur_z += step_z;
		if (remaining <= 0) {
			return false; // budget spent = the major-axis extent walked [orig: @ 0x60ce10]
		}
	}
}

// [orig: Terrain_RaycastHeightmapHiRes_0 @ 0x60e710] retail returns 0 = HIT,
// 1 = CLEAR; ported as bool hit.
bool terrain_raycast_refined(const TerrainRaycastSampler &sampler,
                             const int32_t start[3], const int32_t end[3],
                             int32_t out_hit[3]) {
	int32_t step[3] = { 0, 0, 0 };
	// The coarse march runs first; CLEAR propagates [orig: @ 0x60e723].
	if (!terrain_raycast_march(sampler, start, end, out_hit, step)) {
		return false;
	}
	// No hit out requested -> nothing to refine [orig: @ 0x60e733..0x60e735].
	if (!out_hit) {
		return true;
	}
	// Skip-refine guard — WITNESSED ODD FORM, ported verbatim [orig:
	// @ 0x60e74d..0x60e757]: the refine is skipped iff (step_x == 0 &&
	// step_y != 0 && step_z != 0). The column shortcut's zeroed steps
	// therefore DO enter the refine (a harmless no-op walk — every phase
	// moves by zero). Rationale unknown; an open follow-up in the B0 record.
	if (step[0] == 0 && step[1] != 0 && step[2] != 0) {
		return true;
	}

	int32_t cur_x = out_hit[0];
	int32_t cur_y = out_hit[1];
	int32_t cur_z = out_hit[2];
	// Refine steps = the march per-sample step, arithmetic >> 2 (quarter-unit
	// steps) [orig: @ 0x60e769..0x60e778]. Retail negates the stored y step
	// first (the Terrain_LastRayStepY global holds the V-flipped axis); this
	// core stores world-axis steps, so NO negation here (see the header).
	int32_t rstep_x = step[0] >> TERRAIN_RAYCAST_REFINE_STEP_SHIFT;
	int32_t rstep_y = step[1] >> TERRAIN_RAYCAST_REFINE_STEP_SHIFT;
	int32_t rstep_z = step[2] >> TERRAIN_RAYCAST_REFINE_STEP_SHIFT;

	// Phase 1: back-walk while the ray point is strictly BELOW the surface
	// [orig: @ 0x60e784..0x60e7b7]. Counter semantics ported verbatim: sample
	// first; if below, loop { step back; test the counter's OLD value — if it
	// was 0, break; resample } while below. A counter-terminated walk
	// therefore moves 9 times (8 resamples) — the witnessed shape.
	int32_t h = resolve_refine_height(sampler, cur_x, cur_y);
	if (cur_z < h) {
		int32_t counter = TERRAIN_RAYCAST_REFINE_WALK_BUDGET;
		for (;;) {
			cur_x -= rstep_x;
			cur_y -= rstep_y;
			cur_z -= rstep_z;
			if (counter-- == 0) { // OLD-value test [orig: @ 0x60e79e..0x60e7a9]
				break;
			}
			h = resolve_refine_height(sampler, cur_x, cur_y);
			if (cur_z >= h) {
				break;
			}
		}
	}
	// Phase 2: forward-walk while strictly ABOVE, same shape
	// [orig: @ 0x60e7bb..0x60e7f7].
	h = resolve_refine_height(sampler, cur_x, cur_y);
	if (cur_z > h) {
		int32_t counter = TERRAIN_RAYCAST_REFINE_WALK_BUDGET;
		for (;;) {
			cur_x += rstep_x;
			cur_y += rstep_y;
			cur_z += rstep_z;
			if (counter-- == 0) { // OLD-value test [orig: @ 0x60e7de..0x60e7e9]
				break;
			}
			h = resolve_refine_height(sampler, cur_x, cur_y);
			if (cur_z <= h) {
				break;
			}
		}
	}
	// Phase 3: exactly 8 bisection iterations [orig: @ 0x60e7f9..0x60e834]:
	// sample; at-or-above the surface -> add the step, below -> subtract;
	// THEN halve each step component (arithmetic >> 1).
	for (int32_t i = TERRAIN_RAYCAST_REFINE_BISECT_ITERATIONS; i != 0; --i) {
		h = resolve_refine_height(sampler, cur_x, cur_y);
		if (cur_z >= h) {
			cur_x += rstep_x;
			cur_y += rstep_y;
			cur_z += rstep_z;
		} else {
			cur_x -= rstep_x;
			cur_y -= rstep_y;
			cur_z -= rstep_z;
		}
		rstep_x >>= 1;
		rstep_y >>= 1;
		rstep_z >>= 1;
	}
	// The refined point [orig: @ 0x60e836..0x60e840].
	out_hit[0] = cur_x;
	out_hit[1] = cur_y;
	out_hit[2] = cur_z;
	return true;
}

// [orig: Terrain_RaycastHeightmapHiRes @ 0x60c760] — the LOS variant; see the
// header note for the shape and the tile-cache quantization difference. The
// hit-point/step outs the original optionally writes are dropped: every LOS
// caller (sound occlusion @ 0x53b0bc/0x53b110, the three-ray visibility probe
// @ 0x610f1f) passes a null hit out and consumes only the boolean.
bool terrain_raycast_los_clear(const TerrainRaycastSampler &sampler,
                               const int32_t start[3], const int32_t end[3]) {
	// END-point precheck: the bilinear surface above the END z is an immediate
	// HIT [orig: @ 0x60c7f8 via Terrain_GetHeightAtPosition @ 0x606720].
	int32_t h = 0;
	if (resolve_surface_bilinear(sampler, end[0], end[1], &h) && h > end[2]) {
		return false;
	}

	const int32_t dx = end[0] - start[0];
	const int32_t dy = end[1] - start[1];
	const int32_t dz = end[2] - start[2];
	const int32_t abs_dx = abs32(dx);
	const int32_t abs_dy = abs32(dy);

	// Short segment (both axes under 2.0u): the START point decides
	// [orig: @ 0x60c82e-0x60c86f].
	if (abs_dx < 0x20000 && abs_dy < 0x20000) {
		return !(resolve_surface_bilinear(sampler, start[0], start[1], &h) && h > start[2]);
	}

	// The texel march: one 4.0-unit heightmap texel per step along the major
	// axis [orig: @ 0x60c872-0x60c8fd — scale = 2^34 / maxDelta, per-axis step
	// (scale * delta + 0x8000) >> 16 on the signed product, budget 0x10000
	// losing scale per sample].
	const int32_t max_delta = abs_dx > abs_dy ? abs_dx : abs_dy;
	const int32_t scale = static_cast<int32_t>(0x400000000LL / max_delta);
	const int32_t step_x = static_cast<int32_t>(
			(static_cast<int64_t>(scale) * dx + TERRAIN_RAYCAST_STEP_ROUND_BIAS) >> 16);
	const int32_t step_y = static_cast<int32_t>(
			(static_cast<int64_t>(scale) * dy + TERRAIN_RAYCAST_STEP_ROUND_BIAS) >> 16);
	const int32_t step_z = static_cast<int32_t>(
			(static_cast<int64_t>(scale) * dz + TERRAIN_RAYCAST_STEP_ROUND_BIAS) >> 16);

	int32_t x = start[0];
	int32_t y = start[1];
	int32_t z = start[2];
	int32_t remaining = TERRAIN_RAYCAST_MARCH_BUDGET;
	for (;;) {
		const TerrainRaycastSample s = sampler.point(sampler.ctx, x, y);
		if (s.kind == TerrainRaycastSample::kHeight) {
			// pointHeight >= rayZ is the hit [orig: @ 0x60c9c7].
			if (s.height_1616 >= z) return false;
		} else if (s.kind == TerrainRaycastSample::kEmpty) {
			// The null-tile height-0 floor [orig: @ 0x60ca71-0x60ca77].
			if (z <= 0) return false;
		}
		// kOutOfExtent: no terrain, no floor (editor guard) — march on.
		remaining -= scale;
		x += step_x;
		y += step_y;
		z += step_z;
		if (remaining <= 0) return true; // budget CLEAR [orig: @ 0x60c9e7]
	}
}

} // namespace opennova::terrain
