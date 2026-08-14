#include "hud/hud_minimap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "hud/hud_math.h"
#include <io/bam.h>

namespace opennova::hud {

namespace {

constexpr double kPi = 3.14159265358979323846;
// BAM16 to radians [orig: flt_7C7988 = 9.58738019e-05 = 2*pi/65536]
constexpr double kBam16ToRadians = (2.0 * kPi) / 65536.0;
constexpr int kCircleSegments = 32; // [orig: ring step 0x8000000 BAM @0x5a5f40 vertex loop]
// World-per-pixel divisor [orig: flt_7D2290 = 200.0 @0x5a5f40 scale setup]
constexpr float kZoomHeightDivisor = 200.0f;
// Terrain tile cover bound factor [orig: flt_7C6F9C = 0.8 @0x6071C0 head]
constexpr float kTerrainBoundFactor = 0.8f;
// Terrain tint: the map pass hands 0xD0606060 to the decal renderer, which
// forces the alpha byte opaque, and the map decal pipeline's output stage
// QUADRUPLES texture x diffuse — the reference captures measure the map
// interior at exactly 0x60 x 4 = 1.5058 x texture per channel, saturating.
// A 1x canvas cannot express a >1 modulate, so the port emits the doubled
// color (0xC0 per channel) and the device leg draws the terrain a second
// time on an additive child item: min(2 x 0.7529t, 1) == min(1.5058t, 1).
// The enable_fog_pass resubmission at these call sites is the faint water
// overlay, not the brightness source (see the water-pass note below).
// [orig: 0xD0606060 @0x5a59c8/@0x5a6677; color | 0xFF000000 @0x6071C0;
//  capture-measured 2.00/2.02/2.04 vs a single 0.7529 pass]
constexpr uint32_t kTerrainTint = 0xFFC0C0C0u;
// The witnessed-but-unported water overlay: with enable_fog_pass the decal
// redraws each clipped tile in water blue 0x003F7F at alpha
// (water_height_int + 1) — ~2/255 at JO water heights, below the 8-bit
// visibility floor here. [orig: @0x607834 LABEL_43 block — vert color
// 0x01003F7F + (v55 << 24), v55 = SHIWORD(Env_WaterHeightFixed) clamped 254]
// Backing disc color: the disc pass modulates the struct RGBA through a
// dedicated effect pass; the exact constant is not byte-witnessed, but the
// retail capture corroborates ~75% black (the backing-only zone just past
// the compring band reads ~0.76 alpha over sky).
constexpr uint32_t kBackingColor = 0xC0000000u;
// Special-bank icon quads are a fixed 6px half-extent, resolution-independent
// [orig: draw_billboard_decal size_override = 6.0 @0x5be297; floor 6.0
// @0x597666]
constexpr float kSpecialIconHalfPx = 6.0f;
// Compass ring spans the map radius x1.25 [orig: flt_7C6F18 @0x59c9df]
constexpr float kCompassScale = 1.25f;
// The 300-wu grid cell and its half, Q16. Column rules sit on the -150
// lattice (cells span [k*300-150, k*300+150) — the same fold the player
// grid formula carries), rows on the plain 300 lattice.
// [orig: 19660800 snaps + the unk_960000 fold in the @0x5a5f40 grid branch;
//  HUD_DrawPlayerGridLabel sub_59CB40 x - 150 wu]
constexpr int32_t kGridCellQ16 = 19660800;
constexpr int32_t kGridHalfCellQ16 = 0x960000;
// Row readouts multiply by the FLOAT reciprocal constant, not an exact
// divide — on-rule-line positions round the opposite way under an exact
// division. [orig: flt_7D93B4 = 5.0862631e-08f @0x59cbc3/@0x5a6c67]
constexpr double kGridCellReciprocal =
		static_cast<double>(1.0f / 19660800.0f);
// The compass size base is the SCALED RECT HALF-HEIGHT. The completed-pass
// retail probe observes the backing/stencil fan four physical pixels inside
// that base: 136.5 versus 140.5 at 1920x1080. The independently submitted
// compass quad remains base x1.25 (175.625 px), resolving the two operands.
// [orig: the @0x5a5f40 rect block — fld flt_7C3B94 (0.5), centers
//  (x1+x2)*0.5/(y1+y2)*0.5, radius (y2-y1)*0.5 into the ring vertex loop
//  with flt_7C3610 = 2^-22; rect scaled per axis by
//  Viewport_ScaleToVirtualCoords @0x5d2b20 (x*w/1024, y*h/768, rounded)]
constexpr float kDiscInsetPx = 4.0f;
// TSDicon.tga is a 16x480 vertical strip of 30 16px icon cells (witnessed
// asset; consumed per-index by render_tiled_image_strip @0x67b540).
constexpr int kIconStripCells = 30;
// WPIndctr.tga is a 4-cell vertical strip: 0 up-triangle (waypoint above),
// 1 down-triangle (below), 2 circle (level), 3 blank. The altitude state
// picks the frame. [orig: HUD_LoadAllTextures WPIndctr.tga -> 0x27231A8;
//  frame = `extra` from sub_590970]
constexpr int kWpIndicatorCells = 4;
// TSDicon cells the waypoint state line borrows for its tip: the chevron
// (cell 7) at the clamped edge point while the waypoint projects OUTSIDE
// the clip, the dot (cell 1) at the waypoint itself once inside.
// [orig: HUD_DrawMapTargetPointer @0x599220 — cell arg = lodLevel, default
//  7, switched to 1 on the inside branch @0x59935e; single strip-cell
//  submit @0x59953d]
constexpr int kWaypointTipCellClamped = 7;
constexpr int kWaypointTipCellInside = 1;

// Base-26 grid column letters, A..Z then AA..ZZ, negatives folding back
// from ZZ. [orig: HUD_FormatGridCoordinate @0x598600; 19660800 = 300 wu Q16]
void format_grid_column(char *buffer, size_t size, int32_t grid_value_q16) {
	int idx = grid_value_q16 / 19660800;
	int first_letter_offset = -1;
	if (idx == 0) {
		if (grid_value_q16 < 0) {
			std::snprintf(buffer, size, "%c%c", 675 / 26 + 0 + 'A',
					675 % 26 + 'A');
			return;
		}
		std::snprintf(buffer, size, "%c", idx + 'A');
		return;
	}
	if (idx >= 0) {
		if (idx >= 676) idx %= 676;
	} else {
		idx += 675;
		first_letter_offset = 0;
	}
	if (idx < 0) {
		buffer[0] = '\0';
		return;
	}
	if (idx >= 26) {
		std::snprintf(buffer, size, "%c%c",
				idx / 26 + first_letter_offset + 'A', idx % 26 + 'A');
		return;
	}
	std::snprintf(buffer, size, "%c", idx + 'A');
}

struct MapView {
	float center_x = 0.0f;
	float center_y = 0.0f;
	// The map is a TRUE PIXEL CIRCLE on any surface. The backing/stencil fan
	// is four physical pixels inside the scaled rect half-height; the compass
	// quad uses the uninset base at x1.25.
	float base_radius = 0.0f;
	float disc_radius = 0.0f;
	float rect_w = 0.0f;
	float rect_h = 0.0f;
	// Modes 2/3 clip to the view RECT — the circular stencil belongs to the
	// corner spinmap alone (retail's fullscreen/window map is rectangular;
	// the box clip is clip_triangle_and_emit_vertices @0x688e30).
	bool rect_clip = false;
	float px_x1 = 0.0f;
	float px_y1 = 0.0f;
	float px_x2 = 0.0f;
	float px_y2 = 0.0f;
	float scale = 1.0f; // world units per pixel
	float sin_a = 0.0f;
	float cos_a = 1.0f;
	// The pre-fold view angle (heading or the modes-2/3 north-up base, plus
	// the flip term). Marker sprites rotate relative to THIS, not the player
	// heading — on the north-up map a blip's facing must stay world-stable.
	// [orig: draw_minimap_blip's angle rides the same map transform the view
	//  set up @0x607ac1..0x607b13]
	uint32_t base_angle_bam = 0;
};

// The authored rect through the hudpos design-space scaler, then the retail
// transform: screen centers truncated to ints, scale = zoom / (width * 200),
// rotation = heading (+180 on the mission attrib) - 90 degrees, folded to
// BAM16. [orig: HUD_DrawMapOverlay @0x5a5f40 setup + render_terrain_decal
// tail @0x607ac1..0x607b13 (the live twin of MapView_SetTransform @0x607130)]
MapView make_view(const HudMinimapInput &input) {
	MapView view;
	// Modes 2/3 replace the authored spinmap rect with the builder's
	// literals: the 400x400 window at (20,20) or the full design screen.
	// [orig: HUD_BuildMapOverlayView @0x5a7e10 — 20,20..419,419 /
	//  0,0..1023,767]
	float in_x1 = input.rect_x1, in_y1 = input.rect_y1;
	float in_x2 = input.rect_x2, in_y2 = input.rect_y2;
	if (input.map_mode == 2) {
		in_x1 = 20.0f;
		in_y1 = 20.0f;
		in_x2 = 419.0f;
		in_y2 = 419.0f;
	} else if (input.map_mode == 3) {
		in_x1 = 0.0f;
		in_y1 = 0.0f;
		in_x2 = 1023.0f;
		in_y2 = 767.0f;
	}
	const float x1 = static_cast<float>(scale_axis(in_x1,
			input.surface_w, kDesignWidth));
	const float x2 = static_cast<float>(scale_axis(in_x2,
			input.surface_w, kDesignWidth));
	const float y1 = static_cast<float>(scale_axis(in_y1,
			input.surface_h, kDesignHeight));
	const float y2 = static_cast<float>(scale_axis(in_y2,
			input.surface_h, kDesignHeight));
	view.rect_w = x2 - x1;
	view.rect_h = y2 - y1;
	view.rect_clip = input.map_mode != 0;
	view.px_x1 = x1;
	view.px_y1 = y1;
	view.px_x2 = x2;
	view.px_y2 = y2;
	view.center_x = static_cast<float>(static_cast<int>((x1 + x2) * 0.5f));
	view.center_y = static_cast<float>(static_cast<int>((y1 + y2) * 0.5f));
	// The backing and terrain stencil share the observed four-pixel inset;
	// the compass texture quad hangs off the uninset half-height at x1.25.
	view.base_radius = std::max(0.0f, view.rect_h * 0.5f);
	view.disc_radius = std::max(0.0f, view.base_radius - kDiscInsetPx);
	// Modes 2/3 zoom from the big-map value the radar keys adjust while a
	// map mode is up. [orig: dword_B76490 read @0x5a804b]
	const int32_t zoom = std::clamp(
			input.map_mode != 0 ? input.big_zoom_q16 : input.zoom_q16,
			kSpinmapZoomMin, kSpinmapZoomMax);
	// World-per-pixel = zoom / (rect_height_px x 200). The live completed-pass
	// probe observes 25559 / (281 * 200) = 0.45478648 on 00TRa at 1920x1080.
	// [orig: flt_7D2290 setup @0x5a6501]
	view.scale = static_cast<float>(zoom) /
			std::max(1.0f, view.rect_h * kZoomHeightDivisor);
	// Modes 2/3 rotate from the fixed 0x40000000 base — north-up after the
	// -90 fold — instead of the player heading.
	// [orig: HUD_BuildMapOverlayView entity_ref = 0x40000000]
	uint32_t angle_bam = input.map_mode != 0
			? 0x40000000u
			: static_cast<uint32_t>(input.player_heading_bam);
	if (input.flip_180) angle_bam += 0x80000000u; // [orig: g_mapYaw180 @0x2723EB0]
	view.base_angle_bam = angle_bam;
	angle_bam -= 0x40000000u; // [orig: MapView_SetTransform @0x607144 sub esi, 40000000h]
	const double rad = static_cast<double>(angle_bam >> 16) * kBam16ToRadians;
	view.sin_a = static_cast<float>(std::sin(rad));
	view.cos_a = static_cast<float>(std::cos(rad));
	return view;
}

// Mission Q16 -> map pixels. Mission +Y is negated into screen space before
// the rotation. [orig: Terrain_FixedPointToWorldFloat @0x607060 —
// localX=(x-cx)*inv/65536, localY=(y-cy)*inv*(-1/65536),
// out.x=lx*cos-ly*sin+ctr, out.y=lx*sin+ly*cos+ctr]
void view_project(const MapView &view, const HudMinimapInput &input,
		int32_t world_x_q16, int32_t world_y_q16, float &out_x, float &out_y) {
	const float inv = 1.0f / std::max(view.scale, 1e-6f);
	const float lx = static_cast<float>(world_x_q16 - input.player_x) *
			inv / 65536.0f;
	const float ly = static_cast<float>(world_y_q16 - input.player_y) *
			inv * -1.0f / 65536.0f;
	out_x = view.center_x + lx * view.cos_a - ly * view.sin_a;
	out_y = view.center_y + lx * view.sin_a + ly * view.cos_a;
}

// World-unit length to pixels [orig: Terrain_FixedPointToNormalizedFloat
// @0x607110 — fixed / scale / 65536].
float view_length_px(const MapView &view, float world_units) {
	return world_units / std::max(view.scale, 1e-6f);
}

using Polygon = std::vector<HudMapVertex>;

// Clip `in` against one half-plane into `out` (cleared first). The two
// buffers ping-pong across edges so a whole convex clip runs allocation-free
// once their capacity warms up.
void clip_edge(const Polygon &in, Polygon &out, float nx, float ny, float d) {
	out.clear();
	if (in.empty()) return;
	auto dist = [&](const HudMapVertex &p) { return nx * p.x + ny * p.y - d; };
	auto push_unique = [&out](const HudMapVertex &v) {
		if (!out.empty() && std::fabs(out.back().x - v.x) < 0.001f &&
				std::fabs(out.back().y - v.y) < 0.001f)
			return;
		out.push_back(v);
	};
	HudMapVertex prev = in.back();
	float prev_dist = dist(prev);
	for (const HudMapVertex &cur : in) {
		const float cur_dist = dist(cur);
		const bool prev_inside = prev_dist <= 0.0001f;
		const bool cur_inside = cur_dist <= 0.0001f;
		if (prev_inside != cur_inside) {
			const float t = prev_dist / (prev_dist - cur_dist);
			push_unique({prev.x + (cur.x - prev.x) * t,
					prev.y + (cur.y - prev.y) * t,
					prev.u + (cur.u - prev.u) * t,
					prev.v + (cur.v - prev.v) * t});
		}
		if (cur_inside) push_unique(cur);
		prev = cur;
		prev_dist = cur_dist;
	}
	while (out.size() > 1 && std::fabs(out.front().x - out.back().x) < 0.001f &&
			std::fabs(out.front().y - out.back().y) < 0.001f)
		out.pop_back();
}

// Clip `poly` in place against the view rect, ping-ponging with `scratch`.
void clip_rect(Polygon &poly, Polygon &scratch, float x1, float y1,
		float x2, float y2) {
	clip_edge(poly, scratch, -1.0f, 0.0f, -x1);
	clip_edge(scratch, poly, 1.0f, 0.0f, x2);
	clip_edge(poly, scratch, 0.0f, -1.0f, -y1);
	clip_edge(scratch, poly, 0.0f, 1.0f, y2);
}

// Retail crops the terrain with the stencil laid by the 32-gon backing disc
// (the tris themselves clip only against the rect @0x688e30). Clipping the
// quads against the same 32-gon is this compiler's equivalent; the ring
// vertices match the disc exactly.
void clip_circle32(Polygon &poly, Polygon &scratch, float cx, float cy,
		float rx, float ry) {
	for (int i = 0; i < kCircleSegments && !poly.empty(); ++i) {
		const float a0 = static_cast<float>(2.0 * kPi * i / kCircleSegments);
		const float a1 = static_cast<float>(2.0 * kPi * (i + 1) / kCircleSegments);
		const float x0 = cx + std::cos(a0) * rx;
		const float y0 = cy + std::sin(a0) * ry;
		const float x1 = cx + std::cos(a1) * rx;
		const float y1 = cy + std::sin(a1) * ry;
		const float ex = x1 - x0;
		const float ey = y1 - y0;
		const float nx = ey;
		const float ny = -ex;
		clip_edge(poly, scratch, nx, ny, nx * x0 + ny * y0);
		poly.swap(scratch);
	}
}

void emit_fan(const Polygon &poly, uint32_t color,
		std::vector<HudMapTri> &out) {
	if (poly.size() < 3) return;
	for (size_t i = 1; i + 1 < poly.size(); ++i) {
		// The clipper can leave zero-area slivers on tiles that graze the
		// 32-gon; canvas triangulation rejects them, so drop them here.
		const float ax = poly[i].x - poly[0].x;
		const float ay = poly[i].y - poly[0].y;
		const float bx = poly[i + 1].x - poly[0].x;
		const float by = poly[i + 1].y - poly[0].y;
		if (std::fabs(ax * by - ay * bx) < 0.5f) continue;
		out.push_back({{poly[0].x, poly[0].y, poly[0].u, poly[0].v},
				{poly[i].x, poly[i].y, poly[i].u, poly[i].v},
				{poly[i + 1].x, poly[i + 1].y,
						poly[i + 1].u, poly[i + 1].v}, color});
	}
}

// Icon cell in the TSDicon vertical strip (16x480 = 30 cells).
void marker_uv(uint8_t icon, float &u0, float &v0, float &u1, float &v1) {
	const int cell = std::min<int>(icon, kIconStripCells - 1);
	u0 = 0.0f;
	u1 = 1.0f;
	v0 = static_cast<float>(cell) / kIconStripCells;
	v1 = static_cast<float>(cell + 1) / kIconStripCells;
}

// Clip a segment to the view (rect modes: Liang-Barsky; disc modes: the
// circle intersection). Returns false when fully outside.
bool clip_map_segment(const MapView &view, float &x0, float &y0,
		float &x1, float &y1) {
	if (view.rect_clip) {
		float t0 = 0.0f, t1 = 1.0f;
		const float dx = x1 - x0, dy = y1 - y0;
		const float p[4] = {-dx, dx, -dy, dy};
		const float q[4] = {x0 - view.px_x1, view.px_x2 - x0,
				y0 - view.px_y1, view.px_y2 - y0};
		for (int i = 0; i < 4; ++i) {
			if (p[i] == 0.0f) {
				if (q[i] < 0.0f) return false;
				continue;
			}
			const float r = q[i] / p[i];
			if (p[i] < 0.0f) {
				if (r > t1) return false;
				if (r > t0) t0 = r;
			} else {
				if (r < t0) return false;
				if (r < t1) t1 = r;
			}
		}
		const float nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy;
		const float nx1 = x0 + t1 * dx, ny1 = y0 + t1 * dy;
		x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
		return true;
	}
	const float r = std::max(0.001f, view.disc_radius);
	const float cx = view.center_x, cy = view.center_y;
	const float fx = x0 - cx, fy = y0 - cy;
	const float dx = x1 - x0, dy = y1 - y0;
	const float a = dx * dx + dy * dy;
	const float b = 2.0f * (fx * dx + fy * dy);
	const float c = fx * fx + fy * fy - r * r;
	if (a < 1e-6f) return c <= 0.0f;
	const float disc = b * b - 4.0f * a * c;
	if (disc < 0.0f) return false;
	const float sq = std::sqrt(disc);
	float t0 = (-b - sq) / (2.0f * a);
	float t1 = (-b + sq) / (2.0f * a);
	if (t0 < 0.0f) t0 = 0.0f;
	if (t1 > 1.0f) t1 = 1.0f;
	if (t0 >= t1) return false;
	const float nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy;
	const float nx1 = x0 + t1 * dx, ny1 = y0 + t1 * dy;
	x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
	return true;
}

// The footprint draw submits the entity-team-color fills clipped by the map
// stencil. Retail also builds 0x80000000 boundary vertices inside
// render_collision_wireframe, but completed-pass captures show those lines
// contribute no visible stroke; submitting them through Godot's ordinary
// alpha line pass produced the black outlines absent from retail.
// [orig: render_collision_wireframe @0x596800; flush @0x596780]
void emit_footprint(const MapView &view, const HudMinimapInput &input,
		const HudMinimapFootprint &footprint, Polygon &poly, Polygon &scratch,
		HudMapPass &pass) {
	// Whole-footprint reject on the feed-time bounding circle: a mission's
	// far-side buildings must not cost triangle math every frame.
	{
		float bx = 0.0f, by = 0.0f;
		view_project(view, input, footprint.bound_x_q16, footprint.bound_y_q16,
				bx, by);
		const float radius_px = view_length_px(view,
				static_cast<float>(footprint.bound_radius_q16) / 65536.0f);
		if (view.rect_clip) {
			if (bx + radius_px < view.px_x1 || bx - radius_px > view.px_x2 ||
					by + radius_px < view.px_y1 || by - radius_px > view.px_y2)
				return;
		} else {
			const float dx = bx - view.center_x;
			const float dy = by - view.center_y;
			const float reach = view.disc_radius + radius_px;
			if (dx * dx + dy * dy > reach * reach) return;
		}
	}
	const size_t tri_count = footprint.fill_xy_q16.size() / 6;
	for (size_t t = 0; t < tri_count; ++t) {
		const int32_t *xy = footprint.fill_xy_q16.data() + t * 6;
		poly.resize(3);
		for (int corner = 0; corner < 3; ++corner) {
			view_project(view, input, xy[corner * 2 + 0], xy[corner * 2 + 1],
					poly[static_cast<size_t>(corner)].x,
					poly[static_cast<size_t>(corner)].y);
		}
		if (view.rect_clip) {
			clip_rect(poly, scratch, view.px_x1, view.px_y1, view.px_x2,
					view.px_y2);
		} else {
			clip_circle32(poly, scratch, view.center_x, view.center_y,
					view.disc_radius, view.disc_radius);
		}
		emit_fan(poly, footprint.fill_argb, pass.overlays);
	}
}

// The 64-frame triangle color pulse toward white
// [orig: render_minimap_slot_blip @0x5be3f4 — phase=((frame-8)&0x3F,
// fold >0x20 to 63-phase), channel += phase*(255-channel)>>5].
uint32_t pulse_color(uint32_t argb, int ticks) {
	uint32_t phase = static_cast<uint32_t>(ticks - 8) & 0x3Fu;
	if (phase > 0x20u) phase = 63u - phase;
	uint32_t out = argb & 0xFF000000u;
	for (int shift = 0; shift <= 16; shift += 8) {
		const uint32_t c = (argb >> shift) & 0xFFu;
		const uint32_t p = c + ((phase * (255u - c)) >> 5);
		out |= (p & 0xFFu) << shift;
	}
	return out;
}

} // namespace

int32_t spinmap_zoom_step(int32_t zoom_q16, int direction) {
	if (direction == 0) return kSpinmapZoomDefault;
	// Zoom OUT multiplies the world extent up; IN shrinks it.
	// [orig: case 361 x1.15 max 0x100000 @0x49beaf; case 360 x0.85 min 4096
	//  @0x49bcb0]
	if (direction > 0) {
		const int32_t next = static_cast<int32_t>(
				static_cast<double>(zoom_q16) * 1.15);
		return std::min(next, kSpinmapZoomMax);
	}
	const int32_t next = static_cast<int32_t>(
			static_cast<double>(zoom_q16) * 0.85);
	return std::max(next, kSpinmapZoomMin);
}

namespace {

// The per-marker projection against a prebuilt view (compile hoists the
// view once; the public wrapper below rebuilds it for one-shot callers).
bool project_view_point(const MapView &view, const HudMinimapInput &input,
		int32_t world_x, int32_t world_y, bool clamp_to_edge, float &out_x,
		float &out_y) {
	view_project(view, input, world_x, world_y, out_x, out_y);
	if (view.rect_clip) {
		const bool inside = out_x >= view.px_x1 && out_x <= view.px_x2 &&
				out_y >= view.px_y1 && out_y <= view.px_y2;
		if (inside) return true;
		if (!clamp_to_edge) return false;
		out_x = std::clamp(out_x, view.px_x1, view.px_x2);
		out_y = std::clamp(out_y, view.px_y1, view.px_y2);
		return false;
	}
	const float r = std::max(0.001f, view.disc_radius);
	const float nx = (out_x - view.center_x) / r;
	const float ny = (out_y - view.center_y) / r;
	const float len = std::sqrt(nx * nx + ny * ny);
	if (len <= 1.0f) return true;
	if (!clamp_to_edge) return false;
	out_x = view.center_x + nx * r / len;
	out_y = view.center_y + ny * r / len;
	return false;
}

void reset_pass(HudMapPass &pass) {
	pass.visible = false;
	pass.center_x = 0.0f;
	pass.center_y = 0.0f;
	pass.radius_x = 0.0f;
	pass.radius_y = 0.0f;
	pass.backing.clear();
	pass.terrain.clear();
	pass.overlays.clear();
	pass.sprites.clear();
	pass.lines_under.clear();
	pass.lines.clear();
	pass.labels.clear();
}

} // namespace

bool project_spinmap_point(const HudMinimapInput &input, int32_t world_x,
		int32_t world_y, bool clamp_to_edge, float &out_x, float &out_y) {
	const MapView view = make_view(input);
	return project_view_point(view, input, world_x, world_y, clamp_to_edge,
			out_x, out_y);
}

void hud_minimap_finalize_footprint(HudMinimapFootprint &footprint) {
	// Midpoint-of-extents center + max vertex distance, over both vertex sets.
	int64_t min_x = INT64_MAX, min_y = INT64_MAX;
	int64_t max_x = INT64_MIN, max_y = INT64_MIN;
	auto extend = [&](const std::vector<int32_t> &xy) {
		for (size_t i = 0; i + 1 < xy.size(); i += 2) {
			min_x = std::min<int64_t>(min_x, xy[i]);
			max_x = std::max<int64_t>(max_x, xy[i]);
			min_y = std::min<int64_t>(min_y, xy[i + 1]);
			max_y = std::max<int64_t>(max_y, xy[i + 1]);
		}
	};
	extend(footprint.fill_xy_q16);
	extend(footprint.edge_xy_q16);
	if (min_x > max_x || min_y > max_y) {
		footprint.bound_x_q16 = 0;
		footprint.bound_y_q16 = 0;
		footprint.bound_radius_q16 = 0;
		return;
	}
	footprint.bound_x_q16 = static_cast<int32_t>((min_x + max_x) / 2);
	footprint.bound_y_q16 = static_cast<int32_t>((min_y + max_y) / 2);
	const double half_x = static_cast<double>(max_x - min_x) * 0.5;
	const double half_y = static_cast<double>(max_y - min_y) * 0.5;
	footprint.bound_radius_q16 = static_cast<int32_t>(std::min<double>(
			std::sqrt(half_x * half_x + half_y * half_y), 2147483000.0));
}

void HudMinimapCompiler::compile(const HudMinimapInput &input,
		HudMapPass &out) {
	reset_pass(out);
	if (input.map_mode == 0 &&
			(input.rect_x2 <= input.rect_x1 || input.rect_y2 <= input.rect_y1))
		return;
	// Modes 2/3 both select the big-map content mask 0xAF937 (+bit16 for the
	// windowed mode): the default M-cycle call takes the glow_enabled=1
	// literal; 715063 (0xAE937) belongs to the PARAMETERIZED variant other
	// callers use, not to a mode. Compass (bit9&&bit6) and the at-tip label
	// bit18 are absent — the big map draws neither; bit12 keys the grid leg.
	// [orig: HUD_BuildMapOverlayView @0x5a7e8a..0x5a7ed0 — 719159 default,
	//  715063 when BYTE1(params+4) is clear, |0x10000 then cleared for
	//  modes 3/4]
	const uint32_t flags = input.map_mode != 0
			? (0xAF937u | (input.map_mode == 2 ? 0x10000u : 0u))
			: input.flags;
	const MapView view = make_view(input);
	out.visible = true;
	out.center_x = view.center_x;
	out.center_y = view.center_y;
	out.radius_x = view.disc_radius;
	out.radius_y = view.disc_radius;

	// Backing disc: a 33-vertex/32-triangle fan. The retail ring walks the Q22
	// BAM tables from 0x200000 in 0x8000000 steps — the offset vanishes below
	// the table resolution, so the effective angles are exactly i/32 turns.
	// [orig: @0x5a5f40 vertex loop @0x5a6364 region + DrawIndexedPrimitive
	//  (4, verts, 0x21, indices, 0x60)]
	if (flags & 0x1u) {
		if (view.rect_clip) {
			// The big map's backing fills its rectangle.
			out.backing.push_back({
					{view.px_x1, view.px_y1, 0.0f, 0.0f},
					{view.px_x2, view.px_y1, 0.0f, 0.0f},
					{view.px_x2, view.px_y2, 0.0f, 0.0f}, kBackingColor});
			out.backing.push_back({
					{view.px_x1, view.px_y1, 0.0f, 0.0f},
					{view.px_x2, view.px_y2, 0.0f, 0.0f},
					{view.px_x1, view.px_y2, 0.0f, 0.0f}, kBackingColor});
		} else {
			for (int i = 0; i < kCircleSegments; ++i) {
				const float a0 = static_cast<float>(
						2.0 * kPi * i / kCircleSegments);
				const float a1 = static_cast<float>(2.0 * kPi * (i + 1) /
						kCircleSegments);
				out.backing.push_back({
						{view.center_x, view.center_y, 0.5f, 0.5f},
						{view.center_x + std::cos(a0) * view.disc_radius,
								view.center_y + std::sin(a0) * view.disc_radius,
								0.0f, 0.0f},
						{view.center_x + std::cos(a1) * view.disc_radius,
								view.center_y + std::sin(a1) * view.disc_radius,
								0.0f, 0.0f},
						kBackingColor});
			}
		}
	}

	// Terrain: 512-unit sector tiles over the covered disc, sampled through
	// the TRN routing table. Terrain rows run on NEGATED mission Y. The tile
	// pass is UNMASKED — every mode draws it; bit9 only selects the
	// enable_fog_pass water-overlay variant (witnessed-unported).
	// [orig: render_terrain_decal @0x6071C0 called unconditionally from the
	//  @0x5a5f40 walk (the bit9 test at the call site picks the fog-pass
	//  argument); bound = diag*0.8*scale, tile snap 0x2000000 Q16, row index
	//  (-0x1000000 - y)>>25, sector =
	//  Terrain_SectorGrid[16*(row&0xF)+(col&0xF)] - 1]
	if (input.terrain.present &&
			input.terrain.sector_count > 0 && input.terrain.sector_rows > 0) {
		const float player_x = static_cast<float>(input.player_x) / 65536.0f;
		const float player_z = -static_cast<float>(input.player_y) / 65536.0f;
		const float diag_px = std::sqrt(view.rect_w * view.rect_w +
				view.rect_h * view.rect_h);
		const float bound = diag_px * kTerrainBoundFactor * view.scale;
		const int sx0 = static_cast<int>(std::floor((player_x - bound) /
				terrain::COORDS_SECTOR_SIZE));
		const int sx1 = static_cast<int>(std::floor((player_x + bound) /
				terrain::COORDS_SECTOR_SIZE));
		const int sz0 = static_cast<int>(std::floor((player_z - bound) /
				terrain::COORDS_SECTOR_SIZE));
		const int sz1 = static_cast<int>(std::floor((player_z + bound) /
				terrain::COORDS_SECTOR_SIZE));
		terrain::SectorLayout layout;
		layout.sector_grid = input.terrain.sector_grid.data();
		layout.origin_x = input.terrain.origin_x;
		layout.origin_y = input.terrain.origin_y;
		layout.sector_count = input.terrain.sector_count;
		layout.sector_rows = input.terrain.sector_rows;
		for (int sz = sz0; sz <= sz1; ++sz) {
			for (int sx = sx0; sx <= sx1; ++sx) {
				const terrain::CoordsSectorResolve sector =
						terrain::coords_resolve_sector(layout, sx, sz,
								terrain::coords_runtime_options());
				if (!sector.valid) continue;
				const float wx0 = static_cast<float>(
						sx * terrain::COORDS_SECTOR_SIZE);
				const float wx1 = wx0 + terrain::COORDS_SECTOR_SIZE;
				// Mission y from terrain z: y = -z.
				const float wy0 = -static_cast<float>(
						sz * terrain::COORDS_SECTOR_SIZE);
				const float wy1 = -static_cast<float>(
						(sz + 1) * terrain::COORDS_SECTOR_SIZE);
				const auto q16 = [](float wu) {
					return static_cast<int32_t>(wu * 65536.0f);
				};
				float x[4], y[4];
				view_project(view, input, q16(wx0), q16(wy0), x[0], y[0]);
				view_project(view, input, q16(wx1), q16(wy0), x[1], y[1]);
				view_project(view, input, q16(wx1), q16(wy1), x[2], y[2]);
				view_project(view, input, q16(wx0), q16(wy1), x[3], y[3]);
				// The atlas rect for this cell: the composed per-cell tile
				// atlas when the device baked one (retail's tile cache is
				// keyed per cell), the raw colormap's per-quadrant-id sheet
				// otherwise. The sub-cell inset mirrors retail's 127/256-style
				// anti-bleed on its per-tile textures [orig: UV scale
				// 0.49609375 + offset 0.5078125 @0x6071C0].
				const float atlas_px =
						static_cast<float>(std::max(1, input.terrain.atlas_px));
				const float cell_px =
						static_cast<float>(std::max(1, input.terrain.cell_px));
				const float inset = std::max(0.25f, cell_px / 512.0f);
				const float rect_u = input.terrain.per_cell_atlas
						? sector.cell_col * cell_px
						: static_cast<float>(sector.quadrant_x);
				const float rect_v = input.terrain.per_cell_atlas
						? sector.cell_row * cell_px
						: static_cast<float>(sector.quadrant_z);
				const float u0 = (rect_u + inset) / atlas_px;
				const float v0 = (rect_v + inset) / atlas_px;
				const float u1 = (rect_u + cell_px - inset) / atlas_px;
				const float v1 = (rect_v + cell_px - inset) / atlas_px;
				clip_a_.assign({{x[0], y[0], u0, v0}, {x[1], y[1], u1, v0},
						{x[2], y[2], u1, v1}, {x[3], y[3], u0, v1}});
				if (view.rect_clip) {
					clip_rect(clip_a_, clip_b_, view.px_x1, view.px_y1,
							view.px_x2, view.px_y2);
				} else {
					clip_circle32(clip_a_, clip_b_, view.center_x,
							view.center_y, view.disc_radius,
							view.disc_radius);
				}
				emit_fan(clip_a_, kTerrainTint, out.terrain);
			}
		}
	}

	// The bit12 grid leg: 300-wu rules in pale yellow, the column letters /
	// row numbers on the LARGE label font, and the on-map player readout —
	// all drawn before the marker walk (bit12 routes through this leg and
	// falls into the bank walk; it does not suppress markers). Column rules
	// sit on the ABSOLUTE -150 lattice, so the letter of every gap agrees
	// with the player grid formula everywhere inside it; row rules sit on
	// the plain 300 lattice with a sequential counter (no zero skip).
	// [orig: HUD_DrawMapOverlay @0x5a5f40 grid branch — entered on
	//  ctx bit12 && mode != 1; origin snap from dword_2723EB4; rule start
	//  19660800*(center/19660800 - half) with the unk_960000 column fold;
	//  letters HUD_FormatGridCoordinate(line_x - origin) / rows "%d" from
	//  (start - origin)/19660800 - 1, both HUD_DrawTextCentered_HalfBright
	//  on g_hudLabelFontLarge; line color unk_FFFF7F + ctx alpha]
	if ((flags & 0x1000u) != 0) {
		const int32_t origin_x = input.grid_origin_present
				? kGridCellQ16 * (input.grid_origin_x / kGridCellQ16) : 0;
		const int32_t origin_y = input.grid_origin_present
				? kGridCellQ16 * (input.grid_origin_y / kGridCellQ16) : 0;
		const uint32_t grid_color = 0xFFFFFF7Fu;
		const float half_world_q16 = std::max(view.rect_w, view.rect_h) *
				0.5f * view.scale * 65536.0f;
		const int32_t lo_x = input.player_x -
				static_cast<int32_t>(half_world_q16);
		const int32_t hi_x = input.player_x +
				static_cast<int32_t>(half_world_q16);
		const int32_t lo_y = input.player_y -
				static_cast<int32_t>(half_world_q16);
		const int32_t hi_y = input.player_y +
				static_cast<int32_t>(half_world_q16);
		// Vertical rules on the -150 lattice + the letter centered in the
		// following gap; the letter input is the LINE position minus the
		// snapped origin (the -150 fold rides the lattice itself).
		int32_t wx = kGridCellQ16 * (lo_x / kGridCellQ16) -
				kGridCellQ16 - kGridHalfCellQ16;
		while (wx <= hi_x + kGridCellQ16) {
			float sx = 0.0f, sy = 0.0f;
			view_project(view, input, wx, input.player_y, sx, sy);
			if (sx >= view.px_x1 && sx <= view.px_x2) {
				out.lines_under.push_back({sx, view.px_y1,
						sx, view.px_y2, grid_color});
			}
			float mx = 0.0f, my = 0.0f;
			view_project(view, input, wx + kGridCellQ16 / 2, input.player_y,
					mx, my);
			if (mx >= view.px_x1 && mx <= view.px_x2) {
				HudMapLabel label;
				char column[8] = {};
				format_grid_column(column, sizeof(column), wx - origin_x);
				std::snprintf(label.text, sizeof(label.text), "%s", column);
				label.x = mx;
				label.y = view.px_y1 + 12.0f;
				label.color = grid_color;
				label.align = 0;
				label.font = 1;
				out.labels.push_back(label);
			}
			wx += kGridCellQ16;
		}
		// Horizontal rules + the sequential row number in each gap.
		// [orig: v25 = (start - origin)/19660800 - 1, ++ per row]
		int32_t wy = kGridCellQ16 * (lo_y / kGridCellQ16) - kGridCellQ16;
		while (wy <= hi_y + kGridCellQ16) {
			float sx = 0.0f, sy = 0.0f;
			view_project(view, input, input.player_x, wy, sx, sy);
			if (sy >= view.px_y1 && sy <= view.px_y2) {
				out.lines_under.push_back({view.px_x1, sy,
						view.px_x2, sy, grid_color});
			}
			float mx = 0.0f, my = 0.0f;
			view_project(view, input, input.player_x, wy + kGridCellQ16 / 2,
					mx, my);
			if (my >= view.px_y1 && my <= view.px_y2) {
				HudMapLabel label;
				std::snprintf(label.text, sizeof(label.text), "%d",
						wy / kGridCellQ16 - origin_y / kGridCellQ16 - 1);
				label.x = view.px_x1 + 15.0f;
				label.y = my;
				label.color = grid_color;
				label.align = 0;
				label.font = 1;
				out.labels.push_back(label);
			}
			wy += kGridCellQ16;
		}
		// The on-map player readout "(col,row)" the grid leg closes with:
		// the player-formula column (-150 fold) and the zero-skip row, in
		// the large font at the projected player point minus (50, 25) px.
		// [orig: @0x5a5f40 grid-branch tail — "(%s,%d)" from
		//  HUD_FormatGridCoordinate(ctx+4 - origin - unk_960000) and the
		//  row - 1 (+1 above zero) fold, HUD_DrawTextCentered_HalfBright on
		//  g_hudLabelFontLarge at (x - 50, y - 25), unk_FFFF7F + ctx alpha]
		{
			char column[8] = {};
			format_grid_column(column, sizeof(column),
					input.player_x - kGridHalfCellQ16 - origin_x);
			double row = static_cast<double>(input.player_y) *
					kGridCellReciprocal - static_cast<double>(origin_y) *
					kGridCellReciprocal - 1.0;
			if (row > 0.0) row += 1.0;
			HudMapLabel label;
			std::snprintf(label.text, sizeof(label.text), "(%s,%d)", column,
					static_cast<int>(row));
			label.x = view.center_x - 50.0f;
			label.y = view.center_y - 25.0f;
			label.color = grid_color;
			label.align = 0;
			label.font = 1;
			out.labels.push_back(label);
		}
	}

	// Markers. Within a layer retail draws persistent, then transient, then
	// the special bank; special slots route to layer 3 on flags bit7, low
	// layers otherwise. [orig: MapOverlay_RenderAllByLayer @0x5be590]
	if (flags & 0x2u) {
		footprint_index_.clear();
		if (input.footprints != nullptr) {
			footprint_index_.reserve(input.footprints->size());
			for (const HudMinimapFootprint &footprint : *input.footprints)
				footprint_index_.push_back(&footprint);
		}
		struct Keyed {
			const HudMinimapMarker *m;
			int key;
		};
		std::vector<Keyed> order;
		order.reserve(input.markers.size());
		for (const HudMinimapMarker &marker : input.markers) {
			const bool special = (marker.flags & 0x40u) != 0;
			int layer;
			if (special) {
				layer = (marker.flags & 0x80u) != 0 ? 3 : 0;
			} else {
				layer = hud_minimap_icon_layer(marker.icon);
			}
			int bank_order;
			switch (static_cast<HudMinimapBank>(marker.bank)) {
			case HudMinimapBank::kPersistent: bank_order = 0; break;
			case HudMinimapBank::kTransient: bank_order = 1; break;
			default: bank_order = 2; break;
			}
			order.push_back({&marker, layer * 4 + bank_order});
		}
		std::stable_sort(order.begin(), order.end(),
				[](const Keyed &a, const Keyed &b) { return a.key < b.key; });
		for (const Keyed &keyed : order) {
			const HudMinimapMarker &marker = *keyed.m;
			const bool special = (marker.flags & 0x40u) != 0;
			// Regular markers render from the live entity only.
			// [orig: render_minimap_slot_blip @0x5be4b8 entity[538] gate]
			if (!special && !marker.entity_known) continue;
			// Special slots keep their handle after expiry (the bank floors
			// the lifetime at zero) but the render walk skips them.
			// [orig: MapOverlay_RenderAllByLayer @0x5be794 — lifetime == 0
			//  slots are passed over]
			if (special && marker.remaining_ticks == 0) continue;
			// Footprint-class entities (buildings/zones with marker models)
			// never draw an icon quad — their OOBJ occlusion ground-slice
			// polygons draw instead, clipped like the terrain tiles.
			// [orig: the Building leg @0x597a84 ->
			//  render_collision_wireframe @0x596800]
			if (!special && marker.footprint != 0) {
				const HudMinimapFootprint *footprint = nullptr;
				for (const HudMinimapFootprint *candidate : footprint_index_) {
					if (candidate->handle == marker.handle) {
						footprint = candidate;
						break;
					}
				}
				if (footprint != nullptr) {
					emit_footprint(view, input, *footprint, clip_a_, clip_b_,
							out);
				}
				continue;
			}
			float mx = 0.0f, my = 0.0f;
			if (!project_view_point(view, input, marker.x, marker.y,
					false, mx, my))
				continue;
			if (special && (marker.icon == 253 || marker.icon == 254)) {
				// Pulse markers: rings sized by the slot height, colors
				// pulsing toward white. Icon 254 adds a shrinking ring stack
				// (x0.75 steps). [orig: render_minimap_slot_blip @0x5be25e /
				//  @0x5be267; ring radius = max(arg, projected height)
				//  @0x597357; shrink 49152/65536 @0x5be385]
				const float height_wu =
						static_cast<float>(marker.z) / 65536.0f;
				const float radius = std::max(4.0f,
						view_length_px(view, height_wu));
				const uint32_t pulse = pulse_color(marker.color, input.ticks);
				// Rings emit as the retail 32-segment vertex loop, each
				// segment clipped by the pass stencil like every other map
				// leg — an off-center ring must crop at the disc/rect, not
				// paint over the surrounding HUD.
				// [orig: the ring vertex loop steps 0x8000000 BAM
				//  @0x597320 region; the stencil crop is the same disc]
				const auto emit_ring = [&](float ring_radius,
						uint32_t color) {
					for (int i = 0; i < kCircleSegments; ++i) {
						const float a0 = static_cast<float>(
								2.0 * kPi * i / kCircleSegments);
						const float a1 = static_cast<float>(
								2.0 * kPi * (i + 1) / kCircleSegments);
						float x0 = mx + std::cos(a0) * ring_radius;
						float y0 = my + std::sin(a0) * ring_radius;
						float x1 = mx + std::cos(a1) * ring_radius;
						float y1 = my + std::sin(a1) * ring_radius;
						if (!clip_map_segment(view, x0, y0, x1, y1))
							continue;
						out.lines.push_back({x0, y0, x1, y1, color});
					}
				};
				emit_ring(radius, pulse);
				if (marker.icon == 254) {
					float r = radius;
					for (int i = 0; i < 3; ++i) {
						r *= 0.75f;
						emit_ring(r, 0xFFFFFF00u);
					}
				}
				continue;
			}
			HudMapSprite sprite;
			sprite.center_x = mx;
			sprite.center_y = my;
			if (special) {
				// Fixed 6px half-extent, unrotated.
				// [orig: draw_billboard_decal call @0x5be297, angle 0]
				sprite.half_w = kSpecialIconHalfPx;
				sprite.half_h = kSpecialIconHalfPx;
				sprite.rotation_rad = 0.0f;
			} else {
				// The per-def draw policy: halves from the stamped model
				// bounds when resolved (fallback = the witnessed class
				// table: person 2.0 wu, generic 10.0 wu), the per-branch
				// pixel floor, and rotation ONLY for the rotated classes —
				// upright badges (armory, non-vehicle EWEAPs, cells 6/2,
				// dead persons, ...) hold angle 0.
				// (witness at world::minimap_blip_draw_policy;
				//  [orig: draw_minimap_blip @0x597890 branch table])
				float half_x_wu;
				float half_y_wu;
				if (marker.half_x_q16 > 0 || marker.half_y_q16 > 0) {
					half_x_wu = static_cast<float>(marker.half_x_q16) /
							65536.0f;
					half_y_wu = static_cast<float>(marker.half_y_q16) /
							65536.0f;
				} else {
					const bool person = marker.icon == 3 || marker.icon == 8;
					half_x_wu = half_y_wu = person ? 2.0f : 10.0f;
				}
				const float floor_px = static_cast<float>(
						marker.floor_px != 0 ? marker.floor_px : 6);
				sprite.half_w = std::max(floor_px,
						view_length_px(view, half_x_wu));
				sprite.half_h = std::max(floor_px,
						view_length_px(view, half_y_wu));
				if (marker.rotate != 0) {
					// Blip facing is heading-relative-to-VIEW: the corner
					// map rotates with the player, the M modes hold the
					// north-up base, and the flip term folds in on flipped
					// missions.
					const int32_t relative = opennova::io::bam_sub(
							marker.heading_bam,
							static_cast<int32_t>(view.base_angle_bam));
					sprite.rotation_rad = static_cast<float>(
							-static_cast<double>(
									opennova::io::bam_sar(relative, 16)) *
							kBam16ToRadians);
				} else {
					sprite.rotation_rad = 0.0f;
				}
			}
			sprite.color = marker.color;
			sprite.layer = static_cast<uint8_t>(keyed.key >> 2);
			marker_uv(marker.icon, sprite.u0, sprite.v0, sprite.u1, sprite.v1);
			out.sprites.push_back(sprite);
		}
	}

	// Waypoint state line: a fixed-length bearing pointer from the player
	// (map center) toward the current waypoint, colored by the altitude
	// tricolor and channel-doubled at submit, tipped with a TSDicon cell
	// (chevron ahead, dot behind). The at-tip distance label draws only when
	// SPINMAPWPDISTOFF authored it on.
	// [orig: bit8 leg @0x5a7850..0x5a78a0 -> drawer @0x599220 (line +
	//  sub_67BAE0 tip cell, tip stored to slot[17]/[18], distance to
	//  slot[20]); tricolor sub_590970; bit18 label gate @0x5a7a51..0x5a7ab5]
	uint32_t waypoint_state_color = 0;
	int waypoint_nub_frame = 3; // blank
	if (input.waypoint_present) {
		// [orig: sub_590970 — waypoint z - player z vs +-0x20000 (2.0 wu)]
		const int32_t dz = input.waypoint_z - input.player_z;
		if (dz < -0x20000) {
			waypoint_state_color = 0xFF20407Fu; // below -> blue
			waypoint_nub_frame = 1;
		} else if (dz > 0x20000) {
			waypoint_state_color = 0xFF7F5000u; // above -> orange
			waypoint_nub_frame = 0;
		} else {
			waypoint_state_color = 0xFF007000u; // level -> green
			waypoint_nub_frame = 2;
		}
	}
	if (input.waypoint_present && (flags & 0x100u)) {
		float wx = 0.0f, wy = 0.0f;
		view_project(view, input, input.waypoint_x, input.waypoint_y, wx, wy);
		const float dx = wx - view.center_x;
		const float dy = wy - view.center_y;
		const float len = std::sqrt(dx * dx + dy * dy);
		if (len > 0.001f) {
			const float ux = dx / len;
			const float uy = dy / len;
			// The line tethers to the waypoint, clamped at the content disc;
			// the retail captures show the clamped form at the bezel in BOTH
			// hemispheres. [orig: @0x599220 line submit; the clamp is the
			//  disc stencil]
			float clamp_len = view.disc_radius;
			if (view.rect_clip) {
				// Rect-mode clamp: the nearest border along the bearing.
				clamp_len = 1e9f;
				if (ux > 0.0001f) clamp_len = std::min(clamp_len,
						(view.px_x2 - view.center_x) / ux);
				if (ux < -0.0001f) clamp_len = std::min(clamp_len,
						(view.px_x1 - view.center_x) / ux);
				if (uy > 0.0001f) clamp_len = std::min(clamp_len,
						(view.px_y2 - view.center_y) / uy);
				if (uy < -0.0001f) clamp_len = std::min(clamp_len,
						(view.px_y1 - view.center_y) / uy);
			}
			const bool inside = len <= clamp_len;
			const float line_len = std::min(len, clamp_len);
			const float ex = view.center_x + ux * line_len;
			const float ey = view.center_y + uy * line_len;
			// The state color submits NET-RAW: retail pre-halves it when the
			// device caps carry the 2X-modulate flag, and the vertex submit
			// re-doubles per channel — identity on this 1x canvas.
			// [orig: halve (c >> 1) & 0x7F7F7F under dword_A87064 & 0x20
			//  @0x599397; vertex 2c-saturate @0x5993c6..0x5993f8; the strip
			//  cell takes the same halved color @0x59953d]
			out.lines.push_back({view.center_x, view.center_y, ex, ey,
					waypoint_state_color});
			// ONE strip cell: the dot (cell 1) AT the waypoint while it
			// projects inside the clip, the chevron (cell 7) at the clamped
			// tip — rotated along the bearing — once it leaves.
			// [orig: cell = lodLevel, default 7, switched to 1 on the
			//  inside branch @0x59935e; the single
			//  Render_DrawIconStripCell_Debug submit @0x59953d]
			HudMapSprite tip;
			tip.center_x = ex;
			tip.center_y = ey;
			tip.half_w = kSpecialIconHalfPx;
			tip.half_h = kSpecialIconHalfPx;
			// Cell 7's chevron points up; +90 degrees lays it along the
			// bearing. The inside dot is round and holds angle zero.
			tip.rotation_rad = inside ? 0.0f
					: std::atan2(uy, ux) + static_cast<float>(kPi * 0.5);
			tip.color = waypoint_state_color;
			marker_uv(static_cast<uint8_t>(inside ? kWaypointTipCellInside
					: kWaypointTipCellClamped),
					tip.u0, tip.v0, tip.u1, tip.v1);
			tip.texture = 0;
			tip.layer = 4;
			out.sprites.push_back(tip);
			if ((flags & 0x40000u) &&
					input.waypoint_distance_offset == 0) {
				// The distance label rides a FIXED radial slot along the
				// bearing — the drawer stores center + a constant length
				// into the ctx tip slots and the bit18 leg draws there
				// centered on the BOLD label font in the overlay color (the
				// reference capture puts the slot just past the compass
				// band, ~1.04 x the base radius).
				// [orig: slot store @0x5995c7..0x599616 (ctx[17]/[18]);
				//  label draw @0x5a7a51..0x5a7ab5 ->
				//  HUD_DrawTextCentered_HalfBright(g_hudLabelFontBold)]
				HudMapLabel label;
				label.x = view.center_x + ux * (view.base_radius + 6.0f);
				label.y = view.center_y + uy * (view.base_radius + 6.0f);
				label.color = input.overlay_color;
				label.align = 0;
				if (input.waypoint_distance_m <= 1000) {
					std::snprintf(label.text, sizeof(label.text), "%03dm",
							input.waypoint_distance_m);
				} else {
					std::snprintf(label.text, sizeof(label.text), "%01.2fk",
							static_cast<double>(input.waypoint_distance_m) /
							1000.0);
				}
				out.labels.push_back(label);
			}
		}
	}

	// Altitude nub: the WPIndctr frame drawn just above the rect top edge,
	// nudged -8/+8 design px for above/below, in the raw state color.
	// [orig: bit20 leg @0x5a79b1..0x5a7a10 — y_base = y1 - rect_h/32, half
	//  10 & shift 8 through Viewport_ScaleToVirtualCoords,
	//  CEffect_Begin_Debug(WPIndctr handle, rect, dword_2723D7C, extra)]
	if (input.waypoint_present && (flags & 0x100000u)) {
		const float sx = view.rect_w > 0.0f ? view.rect_w /
				std::max(1.0f, input.rect_x2 - input.rect_x1) : 1.0f;
		const float sy = view.rect_h > 0.0f ? view.rect_h /
				std::max(1.0f, input.rect_y2 - input.rect_y1) : 1.0f;
		const float shift = waypoint_nub_frame == 0 ? -8.0f
				: (waypoint_nub_frame == 1 ? 8.0f : 0.0f);
		const float y_base = (view.center_y - view.rect_h * 0.5f) -
				view.rect_h / 32.0f;
		HudMapSprite nub;
		nub.center_x = view.center_x + shift * sx;
		nub.center_y = y_base - 10.0f * sy;
		nub.half_w = 10.0f * sx;
		nub.half_h = 10.0f * sy;
		nub.rotation_rad = 0.0f;
		nub.color = waypoint_state_color;
		nub.u0 = 0.0f;
		nub.u1 = 1.0f;
		nub.v0 = static_cast<float>(waypoint_nub_frame) / kWpIndicatorCells;
		nub.v1 = static_cast<float>(waypoint_nub_frame + 1) /
				kWpIndicatorCells;
		nub.texture = 3;
		nub.layer = 5;
		out.sprites.push_back(nub);
	}

	// Player grid coordinate label at the authored MAPCOORDS position.
	// [orig: gate @0x5a7a1c — ctx bit9 && !bit12 && dword_27236FC == 0;
	//  sub_59CB40 — origin snapped to 300 wu cells, column letters from
	//  x - 150 wu - origin, row = y/300 - origin/300 - 1 (+1 above zero),
	//  "(%s,%d)" right-aligned in the frame overlay color]
	if ((flags & 0x200u) && !(flags & 0x1000u) &&
			input.map_coords_off == 0) {
		const int32_t origin_x = input.grid_origin_present
				? kGridCellQ16 * (input.grid_origin_x / kGridCellQ16) : 0;
		const int32_t origin_y = input.grid_origin_present
				? kGridCellQ16 * (input.grid_origin_y / kGridCellQ16) : 0;
		char column[8] = {};
		format_grid_column(column, sizeof(column),
				input.player_x - kGridHalfCellQ16 - origin_x);
		double row = static_cast<double>(input.player_y) *
				kGridCellReciprocal -
				static_cast<double>(origin_y) * kGridCellReciprocal - 1.0;
		if (row > 0.0) row += 1.0;
		HudMapLabel label;
		label.x = static_cast<float>(scale_axis(input.map_coords_x,
				input.surface_w, kDesignWidth));
		label.y = static_cast<float>(scale_axis(input.map_coords_y,
				input.surface_h, kDesignHeight));
		label.color = input.overlay_color;
		label.align = 1;
		std::snprintf(label.text, sizeof(label.text), "(%s,%d)", column,
				static_cast<int>(row));
		out.labels.push_back(label);
	}

	// Compass ring: counter-rotates so its north marker points at world
	// north, spanning the rect x1.25. The gate needs bit9 AND bit6 — the
	// big-map masks carry neither.
	// [orig: the @0x5a5f40 walk — (ctx & 0x200) && (ctx & 0x40) around
	//  draw_compass_indicator @0x59c900; rotation (0x3FFFFFC0 - yaw) >> 16,
	//  size x flt_7C6F18 = 1.25]
	if ((flags & 0x200u) != 0 && (flags & 0x40u) != 0) {
		HudMapSprite compass;
		compass.center_x = view.center_x;
		compass.center_y = view.center_y;
		// Compass quad = the uninset rect half-height x1.25. The completed-pass
		// retail probe observes 175.625 px while the backing fan is 136.5 px
		// at 1920x1080, so these are deliberately distinct operands.
		compass.half_w = view.base_radius * kCompassScale;
		compass.half_h = view.base_radius * kCompassScale;
		const uint32_t rot_bam = 0x3FFFFFC0u -
				static_cast<uint32_t>(input.player_heading_bam);
		compass.rotation_rad = static_cast<float>(
				static_cast<double>(rot_bam >> 16) * kBam16ToRadians);
		compass.u0 = 0.0f;
		compass.v0 = 0.0f;
		compass.u1 = 1.0f;
		compass.v1 = 1.0f;
		compass.color = 0xFFFFFFFFu;
		compass.texture = 1;
		compass.layer = 5;
		out.sprites.push_back(compass);
	}
}

} // namespace opennova::hud
