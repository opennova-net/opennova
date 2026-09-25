#include <runtime/hud/scope_circle_mask.h>

#include <cmath>

#include <base/io/bam.h>
#include <formats/def/def.h>
#include <runtime/renderer/aspect_ratio.h>

namespace opennova::hud {

namespace {

// g_bam_sin_table_q22 @0x31bfbc0 and the cosine view of it, off_849934 =
// &g_bam_sin_table_q22[256]: 1024 Q22 entries per revolution plus the 257-entry
// tail the cosine view needs, read as `table[index] * (1 / 4194304)`
// (io::bam_table_sin / io::bam_table_cos)
// [orig: the `* 0.00000023841858` pairs @0x5d18cd / @0x5d18e7].
inline double sin_q22_unit(int index) {
	return io::bam_table_sin(index);
}

inline double cos_q22_unit(int index) {
	return io::bam_table_cos(index);
}

// The x87 `_ftol2_sse` the drawer runs every cross endpoint through: truncate
// toward zero, then `fild` the integer straight back
// [orig: the eight ftol/fild pairs @0x5d121f..0x5d1298].
inline float ftol_back(double v) {
	return static_cast<float>(static_cast<int32_t>(v));
}

// One 7-vertex spoke: `inner`/`outer` are its two break points ON the spoke
// axis, and the half-thickness spreads across that axis.
void build_arm(const ScopeCircleMaskGeometry &g, bool horizontal, float inner,
		float outer, ScopeMaskVertex *out) {
	const float t = g.arm_half_thickness;
	const float cx = g.center_x;
	const float cy = g.center_y;
	// v0 = the exact viewport centre [orig: @0x5d1392/@0x5d1398].
	out[0] = {cx, cy, kScopeCrosshairCenterColor};
	if (horizontal) {
		// [orig: the left spoke @0x5d13ae..0x5d1435, the right @0x5d14c4..0x5d153b]
		out[1] = {inner, cy - t, kScopeCrosshairEdgeColor};
		out[2] = {inner, cy, kScopeCrosshairAxisColor};
		out[3] = {inner, cy + t, kScopeCrosshairEdgeColor};
		out[4] = {outer, cy - t, kScopeCrosshairEdgeColor};
		out[5] = {outer, cy, kScopeCrosshairAxisColor};
		out[6] = {outer, cy + t, kScopeCrosshairEdgeColor};
	} else {
		// [orig: the up spoke @0x5d143a..0x5d14bf, the down @0x5d1540..0x5d15b7]
		out[1] = {cx - t, inner, kScopeCrosshairEdgeColor};
		out[2] = {cx, inner, kScopeCrosshairAxisColor};
		out[3] = {cx + t, inner, kScopeCrosshairEdgeColor};
		out[4] = {cx - t, outer, kScopeCrosshairEdgeColor};
		out[5] = {cx, outer, kScopeCrosshairAxisColor};
		out[6] = {cx + t, outer, kScopeCrosshairEdgeColor};
	}
}

// The spoke's index block [orig: the 18 WORD stores @0x5d11bf..0x5d1218].
constexpr uint16_t kArmIndices[kScopeCrosshairArmIndices] = {
	0, 1, 2, 0, 2, 3, 2, 1, 4, 2, 4, 5, 3, 2, 5, 3, 5, 6,
};

// The tick diamond's index block [orig: the 12 WORD stores @0x5d15c7..0x5d1626].
constexpr uint16_t kTickIndices[kScopeGridTickIndices] = {
	0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 1,
};

} // namespace

ScopedViewOverlay scoped_view_overlay(bool binoculars_view_active, bool sighted,
		bool scoped) {
	// [orig: Render_ProcessMainSceneFrame @0x5caae1 (binoculars) ->
	//  @0x5caaf8 (the Sighted byte) -> @0x5cab06 (the Scoped byte) ->
	//  @0x5cab26 (neither)]
	if (binoculars_view_active) return ScopedViewOverlay::kBinocularMask;
	if (sighted) return ScopedViewOverlay::kSightedCard;
	if (scoped) return ScopedViewOverlay::kScopedCardWithCircleMask;
	return ScopedViewOverlay::kEntityMarkers;
}

bool scoped_selector_from_def(uint32_t weapon_flags, uint32_t weapon_flags2) {
	// [orig: `Flags & 1` @0x4dcc80 -> the Inset split @0x5ca2be
	//  (`Def->Field0C & 0x200` takes the Inset byte instead) -> @0x5ca2c7]
	return (weapon_flags & def::DEF_WEAPON_FLAG_SCOPED) != 0 &&
			(weapon_flags2 & def::DEF_WEAPON_FLAG2_INSET) == 0;
}

bool sighted_selector_from_def(uint32_t weapon_flags, bool slot_switching_from) {
	// [orig: Player_IsVehicleGunnerScoped @0x4dcd30 — `Flags & 2`,
	//  g_weaponScopeActive, and MountSlot.currentAction != SWITCHFROM (7);
	//  the byte set @0x5ca2d5]
	return (weapon_flags & def::DEF_WEAPON_FLAG_SIGHTED) != 0 && !slot_switching_from;
}

namespace {

// The reticle cross and the cardinal grid about the geometry's centre, at its
// scales. [orig: draw_minimap_crosshair_and_grid @0x5d1160]
void append_crosshair_and_grid(const ScopeCircleMaskGeometry &g, ScopeCircleMask &out) {
	// --- the reticle cross ------------------------------------------------
	// A = scale_x * ring_size, B = scale_y * ring_size; every endpoint is
	// truncated to an integer pixel before it is submitted.
	// [orig: @0x5d11bd (A) / @0x5d1235 (B); the eight ftol'd endpoints
	//  @0x5d121f..0x5d1298]
	const double a = static_cast<double>(g.scale_x) * static_cast<double>(g.ring_size);
	const double b = static_cast<double>(g.scale_y) * static_cast<double>(g.ring_size);
	const double cx = static_cast<double>(g.center_x);
	const double cy = static_cast<double>(g.center_y);
	const double inner_a = a * static_cast<double>(kScopeCrosshairInnerFraction);
	const double outer_a = a * static_cast<double>(kScopeCrosshairOuterFraction);
	const double inner_b = b * static_cast<double>(kScopeCrosshairInnerFraction);
	const double outer_b = b * static_cast<double>(kScopeCrosshairOuterFraction);
	out.crosshair.resize(static_cast<size_t>(kScopeCrosshairArms) * kScopeCrosshairArmVertices);
	// Retail's submit order: left, up, right, down.
	build_arm(g, true, ftol_back(cx - inner_a), ftol_back(cx - outer_a),
			out.crosshair.data());
	build_arm(g, false, ftol_back(cy - inner_b), ftol_back(cy - outer_b),
			out.crosshair.data() + kScopeCrosshairArmVertices);
	build_arm(g, true, ftol_back(cx + inner_a), ftol_back(cx + outer_a),
			out.crosshair.data() + 2 * kScopeCrosshairArmVertices);
	build_arm(g, false, ftol_back(cy + inner_b), ftol_back(cy + outer_b),
			out.crosshair.data() + 3 * kScopeCrosshairArmVertices);
	out.crosshair_indices.reserve(
			static_cast<size_t>(kScopeCrosshairArms) * kScopeCrosshairArmIndices);
	for (int arm = 0; arm < kScopeCrosshairArms; ++arm) {
		for (int i = 0; i < kScopeCrosshairArmIndices; ++i) {
			out.crosshair_indices.push_back(static_cast<uint16_t>(
					kArmIndices[i] + arm * kScopeCrosshairArmVertices));
		}
	}

	// --- the cardinal grid ------------------------------------------------
	// Four directions x four ticks, each a diamond of half-size
	// arm_half_thickness around (centre + step * tick_spacing). The offsets are
	// plain screen pixels: neither scale_x nor scale_y touches them.
	// [orig: @0x5d1653..0x5d171d; the switch arms @0x5d167c — 0 = +X, 1 = -X,
	//  2 = +Y, 3 = -Y]
	const float t = g.arm_half_thickness;
	out.grid.reserve(static_cast<size_t>(kScopeGridDirections) *
			kScopeGridTicksPerDirection * kScopeGridTickVertices);
	for (int dir = 0; dir < kScopeGridDirections; ++dir) {
		for (int step = 1; step <= kScopeGridTicksPerDirection; ++step) {
			const float offset = g.tick_spacing * static_cast<float>(step);
			float x = g.center_x;
			float y = g.center_y;
			switch (dir) {
				case 0: x = g.center_x + offset; break;
				case 1: x = g.center_x - offset; break;
				case 2: y = g.center_y + offset; break;
				default: y = g.center_y - offset; break;
			}
			out.grid.push_back({x, y, kScopeGridTickCenterColor});
			out.grid.push_back({x + t, y, kScopeGridTickEdgeColor});
			out.grid.push_back({x, y + t, kScopeGridTickEdgeColor});
			out.grid.push_back({x - t, y, kScopeGridTickEdgeColor});
			out.grid.push_back({x, y - t, kScopeGridTickEdgeColor});
		}
	}
	const int tick_count = kScopeGridDirections * kScopeGridTicksPerDirection;
	out.grid_indices.reserve(static_cast<size_t>(tick_count) * kScopeGridTickIndices);
	for (int tick = 0; tick < tick_count; ++tick) {
		for (int i = 0; i < kScopeGridTickIndices; ++i) {
			out.grid_indices.push_back(static_cast<uint16_t>(
					kTickIndices[i] + tick * kScopeGridTickVertices));
		}
	}
}

} // namespace

ScopeCircleMaskGeometry scope_circle_mask_geometry(int32_t x0, int32_t y0,
		int32_t x1, int32_t y1, int32_t screen_width, int aspect_mode) {
	ScopeCircleMaskGeometry g;
	// [orig: `(dword_24C1430 + dword_24C1428) >> 1` @0x5d17cc;
	//  `(dword_24C142C + dword_24C1434) >> 1` @0x5d17e1 — an ARITHMETIC shift
	//  of the summed rect edges, not a divide]
	const int32_t cx = (x1 + x0) >> 1;
	const int32_t cy = (y0 + y1) >> 1;
	g.center_x = static_cast<float>(cx);
	g.center_y = static_cast<float>(cy);
	// [orig: `scaleX = (double)cx / (double)cy * 0.75` @0x5d17f5]
	g.scale_x = cy != 0 ? static_cast<float>(static_cast<double>(cx) /
										   static_cast<double>(cy) * 0.75)
						: 0.0f;
	// [orig: `scaleY = 3.0 / (sub_58A920() * 4.0)` @0x5d1811; sub_58A920
	//  @0x58a920 returns flt_8409EC, the selected H/W ratio
	//  Render_SetAspectRatioMode @0x58d870 stores]
	const float ratio = renderer::aspect_height_over_width(aspect_mode,
			static_cast<float>(x1 - x0 + 1), static_cast<float>(y1 - y0 + 1));
	g.scale_y = ratio != 0.0f ? 3.0f / (ratio * 4.0f) : 0.0f;
	// [orig: `((h) >> 3) + ((h) >> 1)` @0x5d1830]
	const int32_t height = y1 - y0;
	const int32_t ring_size = (height >> 3) + (height >> 1);
	g.ring_size = static_cast<float>(ring_size);
	// [orig: `0.70999998 * ring_size` @0x5d184d; `ring_size * 1.5` @0x5d1857]
	g.radius_inner = 0.71f * g.ring_size;
	g.radius_outer = g.ring_size * 1.5f;
	// [orig: `(W + W) * flt_7D00A8` @0x5d12ad..0x5d12b5 and
	//  `(W * flt_7C44B4) * flt_7D00A8` @0x5d160a..0x5d1635, flt_7D00A8 =
	//  0.0015625f = 1/640, flt_7C44B4 = 10.0f]
	const float w = static_cast<float>(screen_width);
	g.arm_half_thickness = (w + w) * 0.0015625f;
	g.tick_spacing = (w * 10.0f) * 0.0015625f;
	return g;
}

ScopeCircleMask build_scope_circle_mask(int32_t x0, int32_t y0, int32_t x1,
		int32_t y1, int32_t screen_width, bool draw_crosshair, int aspect_mode) {
	ScopeCircleMask out;
	out.geometry = scope_circle_mask_geometry(x0, y0, x1, y1, screen_width, aspect_mode);
	const ScopeCircleMaskGeometry &g = out.geometry;

	// --- the ring ---------------------------------------------------------
	// Segment s takes table index 16*s; the inner vertex carries the lighter
	// colour, the outer the darker, and the pair alternates down one triangle
	// strip. [orig: the write pattern @0x5d18c2..0x5d1c9e]
	out.ring.reserve(kScopeRingVertexCount);
	for (int s = 0; s <= kScopeRingSegments; ++s) {
		const int index = s * kScopeRingTableStep;
		const double c = cos_q22_unit(index);
		const double sn = sin_q22_unit(index);
		// [orig: `x = cos * r * scaleX + cx` @0x5d18f6;
		//  `y = cy - sin * r * scaleY` @0x5d190d]
		const auto place = [&](float radius, uint32_t argb) {
			const double r = static_cast<double>(radius);
			ScopeMaskVertex v;
			v.x = static_cast<float>(c * r * static_cast<double>(g.scale_x) +
					static_cast<double>(g.center_x));
			v.y = static_cast<float>(static_cast<double>(g.center_y) -
					sn * r * static_cast<double>(g.scale_y));
			v.argb = argb;
			return v;
		};
		out.ring.push_back(place(g.radius_inner, kScopeRingInnerColor));
		out.ring.push_back(place(g.radius_outer, kScopeRingOuterColor));
	}
	// The strip expanded to a list: 128 triangles over the 130 vertices.
	out.ring_indices.reserve(static_cast<size_t>(kScopeRingVertexCount - 2) * 3u);
	for (int i = 0; i + 2 < kScopeRingVertexCount; ++i) {
		out.ring_indices.push_back(static_cast<uint16_t>(i));
		out.ring_indices.push_back(static_cast<uint16_t>(i + 1));
		out.ring_indices.push_back(static_cast<uint16_t>(i + 2));
	}

	if (draw_crosshair)
		append_crosshair_and_grid(g, out);
	return out;
}

ScopeCircleMask build_nvg_lens_reticle(int32_t x0, int32_t y0, int32_t x1,
		int32_t y1, int32_t screen_width) {
	ScopeCircleMask out;
	out.geometry = scope_circle_mask_geometry(x0, y0, x1, y1, screen_width);
	// The lens passes its own ring size and centre -- the same shift pair and
	// sums as the mask's -- and unit scales. [orig: draw_minimap_compass_border
	//  `fld1; fst [scaleY]; fstp [scaleX]` @0x5d27a1..0x5d27b6]
	out.geometry.scale_x = 1.0f;
	out.geometry.scale_y = 1.0f;
	append_crosshair_and_grid(out.geometry, out);
	return out;
}

} // namespace opennova::hud
