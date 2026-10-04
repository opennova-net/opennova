#include <runtime/hud/hud_minimap.h>
#include <base/io/fixed.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <runtime/hud/hud_declutter.h>
#include <runtime/hud/hud_math.h>
#include <runtime/hud/hud_minimap_view.h>
#include <base/io/bam.h>

namespace opennova::hud {

// The view transform and clip helpers are shared with the leg TUs
// (hud_minimap_view.h).
namespace minimap_detail {

constexpr double kPi = io::kPi;
// BAM16 to radians [orig: flt_7C7988 = 9.58738019e-05 = 2*pi/65536]
constexpr double kBam16ToRadians = (2.0 * kPi) / io::kFp16OneD;
constexpr int kCircleSegments = 32; // [orig: ring step 0x8000000 BAM @0x5a5f40 vertex loop]
// World-per-pixel divisor [orig: flt_7D2290 = 200.0 @0x5a6501 scale setup]
constexpr float kZoomDivisor = 200.0f;
// Terrain tile cover bound factor [orig: flt_7C6F9C = 0.8 @0x6071C0 head]
constexpr float kTerrainBoundFactor = 0.8f;
// Terrain tint: the map pass hands 0xD0606060 to the decal renderer, which
// forces the alpha byte opaque, and the map decal pipeline's output stage
// QUADRUPLES texture x diffuse — the reference captures measure the map
// interior at exactly 0x60 x 4 = 1.5058 x texture per channel, saturating.
// A 1x canvas cannot express a >1 modulate, so the port emits the doubled
// color (0xC0 per channel) and the device leg draws the terrain a second
// time on an additive child item: min(2 x 0.7529t, 1) == min(1.5058t, 1).
// The enable_fog_pass resubmission at these call sites is the separate
// depthspin shore pass, not the brightness source (see below).
// [orig: 0xD0606060 @0x5a6679; color | 0xFF000000 @0x6071C0;
//  capture-measured 2.00/2.02/2.04 vs a single 0.7529 pass]
constexpr uint32_t kTerrainTint = 0xFFC0C0C0u;
// The shore pass remaps each sector's local 0..1 UV into one of depthspin's
// four 128px quadrants. The 127/256 scale and 130/256 second-half offset are
// literal float constants in the post-base-draw vertex loop.
// [orig: flt_7DF19C / flt_7DF198 @0x6077A2..0x6077E6]
constexpr float kDepthspinUvScale = 127.0f / 256.0f;
constexpr float kDepthspinUvOffset = 130.0f / 256.0f;
// The alpha-tested shore material replaces the completed terrain pixel. The
// Canvas device splits retail's terrain x1.5058 output across two items, so
// this is the capture-measured final color submitted above both of them.
// [orig: fog-pass diffuse @0x607834; JOTAC 00TRa synchronized capture]
constexpr uint32_t kDepthspinColor = 0xFF16476Bu;
// The bit0 backing disc carries NO colour: the rect quad (z 0.1) and the
// 33-vertex fan (z 0.999996) submit diffuse 0 through stock shader #2 (mode
// 0x221: SRCALPHA/INVSRCALPHA, SELECTARG2 diffuse colour and alpha), pass
// 0x600000 (ztest always, zwrite on) — invisible; it only lays the depth the
// later z-tested legs crop against. The port bakes that crop into those legs'
// geometry (the 32-gon clips) and emits no backing geometry.
// [orig: CGfxDevice_SetQuadDiffuse(0) @0x5a6068; SetQuadDepth(flt_7C69F4)
//  @0x5a6556; dword_272E830 = 0 + flt_7D9F80 z @0x5a659d; the 33-vertex
//  loop @0x5a6364 region -> DrawIndexedPrimitive @0x5a6645; shader #2 =
//  sub_676D50(dev, 0, 545) @0x6780AF; pass 0x600000 @0x5a6527]
// Compass ring spans the map radius x1.25 [orig: flt_7C6F18 @0x59c9df]
constexpr float kCompassScale = 1.25f;
// Retail samples only the centered 90% of COMPRING. Cropping five percent
// from each edge removes authored transparent padding, so the visible ring
// reaches 1/0.9 farther than a full-texture sample on the same quad.
// [orig: HUD_DrawCompassIndicator @0x59ca8a — UV center 0.5 +/-
//  flt_7D93A8 (0.45)]
constexpr float kCompassUvMin = 0.05f;
constexpr float kCompassUvMax = 0.95f;
// The 300-wu grid cell and its half, Q16. Column rules sit on the -150
// lattice (cells span [k*300-150, k*300+150) — the same fold the player
// grid formula carries), rows on the plain 300 lattice.
// [orig: 19660800 snaps + the unk_960000 fold in the @0x5a5f40 grid branch;
//  HUD_DrawPlayerGridLabel @0x59cb40 x - 150 wu]
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
// The spinmap disc sits INSIDE its rect by (mask >> 8) & 2 DESIGN pixels —
// two when bit9 is set (the corner map), none on the big map — taken through
// the same integer scaler every other hudpos coordinate uses, on the WIDTH
// axis, even though the radius itself comes from the half-HEIGHT
// [orig: the literal edi = 2 @0x5a60d8; the inset compute
//  (mask >> 8) & edi @0x5a64cd..0x5a64d1 scaled through
//  Viewport_ScaleToVirtualCoords @0x5d2b20; the disc-radius subtract
//  @0x5a6512..0x5a651d]. At 1920 wide that scales to exactly 4 px, which is why the
// earlier live probe read it as a constant four; at other widths it is not
// (1280 -> 3, 2560 -> 5), so a constant only matched the machine it was
// observed on.
// TSDicon.tga is a 30-cell vertical strip of square icon cells. Retail
// uploads the source at its authored resolution (stock JO ships 16x480,
// JOTAC's RevX02 authors 64x1920), box-generates its mip chain, and
// Render_TiledImageStrip derives the half-texel inset from the loaded
// tile's PHYSICAL dimensions — so the input carries the device-measured
// size (HudMinimapInput::icon_strip_*). The badges commonly draw near the
// 16px mip level.
// [orig: Texture_LoadFromFile_0 @0x59e060 -> GTexture_CreateFromPixelData_0
//  @0x6876c0; Render_TiledImageStrip @0x67b540 — uv_half_texel =
//  0.5 / (double)tile_dim]
constexpr int kIconStripCells = 30;
// WPIndctr.tga is a 4-cell vertical strip: 0 up-triangle (waypoint above),
// 1 down-triangle (below), 2 circle (level), 3 blank. The altitude state
// picks the frame. [orig: HUD_LoadAllTextures WPIndctr.tga -> 0x27231A8;
//  frame = `extra` from HUD_UpdateWaypointAltitudeColor @0x590970]
constexpr int kWpIndicatorCells = 4;
// Every pointer drawer receives a 10-design-pixel span after Y-axis viewport
// scaling [orig: renderFlags = 10 through Viewport_ScaleToVirtualCoords
//  @0x5a60dd/@0x5a64c5; the span arg @0x5a64df/@0x5a7885].
constexpr int kPointerSpanDesignPx = 10;

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

// The pass's content mask. The corner map runs the embedder's mask; modes 2/3
// take the view builder's: the glow byte picks 0xAF937 / 0xAE937 (bit12),
// params+7 == 0 clears bits 11/14/17, params+6 == 0 clears bit17, then bit16
// is set and cleared again for the fullscreen mode (and mode 4).
// The M cycle passes no block, which is every byte on. Compass (bit9 && bit6)
// and bit18 are absent — the big map draws neither; bit12 keys the grid leg.
// [orig: HUD_BuildMapOverlayView @0x5a7e8a..0x5a7ed0 / @0x5a7ef5]
uint32_t effective_flags(const HudMinimapInput &input) {
	// The corner map and the mode-4 windowed views (hud_map_view.h
	// command_map_mask / kDeathMapMask) carry their caller's mask.
	if (input.map_mode != 2 && input.map_mode != 3) return input.flags;
	uint32_t flags = input.big_map_params.glow ? 0xAF937u : 0xAE937u;
	if (!input.big_map_params.labels) flags &= 0xFFFDB7FFu;
	if (!input.big_map_params.player_waypoints) flags &= ~0x20000u;
	flags |= 0x10000u;
	if (input.map_mode == 3) flags &= ~0x10000u;
	return flags;
}

// The authored rect through the hudpos design-space scaler, then the retail
// setup: bit16 squares the rect on its half-height about the float centre
// (x1 = trunc(cx - hh), x2 = trunc(cx + hh)); the scale divides the zoom by
// the (squared) WIDTH x 200; the disc radius var_354 is the half-height less
// the bit9-gated scaleX(2) inset; rotation = heading (+180 on the mission
// attrib) - 90 degrees, folded to BAM16.
// [orig: HUD_DrawMapOverlay @0x5a63b5..0x5a642e (rect + bit16 squaring),
//  @0x5a64c5..0x5a651d (inset + scale = [ctx+0x58] / (w * flt_7D2290));
//  Render_TerrainDecal tail @0x607ac1..0x607b13 (the live twin of
//  MapView_SetTransform @0x607130)]
MapView make_view(const HudMinimapInput &input, uint32_t flags) {
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
	int x1 = static_cast<int>(scale_axis(in_x1, input.surface_w, kDesignWidth));
	int x2 = static_cast<int>(scale_axis(in_x2, input.surface_w, kDesignWidth));
	const int y1 = static_cast<int>(scale_axis(in_y1, input.surface_h,
			kDesignHeight));
	const int y2 = static_cast<int>(scale_axis(in_y2, input.surface_h,
			kDesignHeight));
	const float cx = (static_cast<float>(x1) + static_cast<float>(x2)) * 0.5f;
	const float half_h = (static_cast<float>(y2) - static_cast<float>(y1)) * 0.5f;
	if ((flags & 0x10000u) != 0) {
		// [orig: @0x5a6411..0x5a642e — fsub/fadd hh about cx, ftol2 truncation]
		x1 = static_cast<int>(cx - half_h);
		x2 = static_cast<int>(cx + half_h);
	}
	view.rect_w = static_cast<float>(x2 - x1);
	view.rect_h = static_cast<float>(y2 - y1);
	view.px_x1 = static_cast<float>(x1);
	view.px_y1 = static_cast<float>(y1);
	view.px_x2 = static_cast<float>(x2);
	view.px_y2 = static_cast<float>(y2);
	view.center_x = static_cast<float>(static_cast<int>(cx));
	view.center_y = static_cast<float>(static_cast<int>(
			(static_cast<float>(y1) + static_cast<float>(y2)) * 0.5f));
	// The compass texture quad hangs off the uninset half-height at x1.25;
	// the disc (stencil, pointer radius) subtracts scaleX((mask >> 8) & 2).
	view.base_radius = std::max(0.0f, half_h);
	view.disc_radius = std::max(0.0f,
			view.base_radius - static_cast<float>(scale_axis(
					static_cast<double>((flags >> 8) & 2u), input.surface_w,
					kDesignWidth)));
	// Modes 2/3 zoom from the big-map value the radar keys adjust while a
	// map mode is up. [orig: dword_B76490 read @0x5a804b]
	const int32_t zoom = std::clamp(
			input.map_mode != 0 ? input.big_zoom_q16 : input.zoom_q16,
			kSpinmapZoomMin, kSpinmapZoomMax);
	// World-per-pixel = zoom / (rect_width_px x 200), the width taken AFTER the
	// bit16 squaring. The live completed-pass probe observes 25559 /
	// (281 * 200) = 0.45478648 on 00TRa at 1920x1080 (the squared width equals
	// the height there). [orig: fild w; fmul flt_7D2290; fdivr [ebp+58h]
	//  @0x5a6501]
	view.scale = static_cast<float>(zoom) /
			std::max(1.0f, view.rect_w * kZoomDivisor);
	// The windowed views hand their own scale in (hud_map_view.h): the CMAP
	// computes the same zoom / (width x 200), the DEATH window its handler's.
	if (input.map_mode == 4) view.scale = input.window_scale;
	// Modes 2/3 rotate from the fixed 0x40000000 base — north-up after the
	// -90 fold — instead of the player heading.
	// [orig: HUD_BuildMapOverlayView entity_ref = 0x40000000]
	uint32_t angle_bam = input.map_mode != 0
			? 0x40000000u
			: static_cast<uint32_t>(input.player_heading_bam);
	view.heading_bam = static_cast<int32_t>(angle_bam);
	if (input.flip_180) angle_bam += 0x80000000u; // [orig: g_MapYaw180 @0x2723EB0]
	view.base_angle_bam = angle_bam;
	if (input.map_mode == 4)
		view.base_angle_bam += static_cast<uint32_t>(input.window_marker_angle_bias_bam);
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
			inv / io::kFp16One;
	const float ly = static_cast<float>(world_y_q16 - input.player_y) *
			inv * -io::kInvFp16One;
	out_x = view.center_x + lx * view.cos_a - ly * view.sin_a;
	out_y = view.center_y + lx * view.sin_a + ly * view.cos_a;
}

// The identity-rotation twin: the same centre and scale with cos 1, sin 0 —
// what the label legs pass instead of the map rotation.
// [orig: Terrain_FixedPointToWorldFloat @0x607060 with fld1/fldz args
//  @0x5a75b8 / @0x5a775f / @0x5a4e5c / @0x5a4f4f]
void view_project_unrotated(const MapView &view, const HudMinimapInput &input,
		int32_t world_x_q16, int32_t world_y_q16, float &out_x, float &out_y) {
	const float inv = 1.0f / std::max(view.scale, 1e-6f);
	const float lx = static_cast<float>(world_x_q16 - input.player_x) *
			inv / io::kFp16One;
	const float ly = static_cast<float>(world_y_q16 - input.player_y) *
			inv * -io::kInvFp16One;
	out_x = view.center_x + lx;
	out_y = view.center_y + ly;
}

// World-unit length to pixels [orig: Terrain_FixedPointToNormalizedFloat
// @0x607110 — fixed / scale / 65536].
float view_length_px(const MapView &view, float world_units) {
	return world_units / std::max(view.scale, 1e-6f);
}

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

// Icon cell in the TSDicon vertical strip (30 square cells; physical size
// from the loaded asset).
void marker_uv(const HudMinimapInput &input, uint8_t icon, float &u0,
		float &v0, float &u1, float &v1) {
	const int cell = std::min<int>(icon, kIconStripCells - 1);
	// Retail offsets both strip endpoints by half a source texel of the
	// tile's stored PHYSICAL dimensions. The right and bottom coordinates
	// intentionally reach half a texel past the cell; the device's clamp
	// sampler holds the final edge texel.
	// [orig: Render_TiledImageStrip @0x67b540 — 0.5 / tile_dim insets,
	//  uv_right = w/w + 0.5/w, rows in source pixels]
	const float strip_w = std::max(1.0f, input.icon_strip_w_px);
	const float strip_h = std::max(1.0f, input.icon_strip_h_px);
	const float cell_px = strip_h / kIconStripCells;
	u0 = 0.5f / strip_w;
	u1 = (strip_w + 0.5f) / strip_w;
	v0 = (static_cast<float>(cell) * cell_px + 0.5f) / strip_h;
	v1 = (static_cast<float>(cell + 1) * cell_px + 0.5f) / strip_h;
}

// Ordinary TSDicon blips submit the raw team color to the strip renderer,
// whose texture stage is MODULATE2X. Canvas modulates only once, so fold the
// missing output stage into the diffuse RGB. Alpha is not doubled.
// [orig: Minimap_DrawBlip @0x597f48..0x597f73 ->
//  Render_DrawIconStripCell_Debug @0x67bae0; TSS MODULATE2X]
uint32_t marker_modulate2x_color(uint32_t argb) {
	const auto doubled = [](uint32_t channel) {
		return std::min(channel * 2u, 255u);
	};
	return (argb & 0xFF000000u) |
			(doubled((argb >> 16) & 0xFFu) << 16) |
			(doubled((argb >> 8) & 0xFFu) << 8) |
			doubled(argb & 0xFFu);
}

// Clip a segment to the viewport rect (Liang-Barsky). The line legs draw
// through ztest-always passes (0x200000 / 0x300000), so only the rect
// viewport crops them, never the disc mask. Returns false when fully outside.
// [orig: SetViewport(rect) @0x5a64a8; the line passes @0x599412 /
//  @0x5a6ac3 / @0x5a6f47]
bool clip_segment_rect(const MapView &view, float &x0, float &y0,
		float &x1, float &y1) {
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

// The footprint draw submits the entity-team-color fills, cropped by the disc
// mask when it survives and by the viewport rect otherwise. Retail also
// builds 0x80000000 boundary vertices inside Render_CollisionWireframe, but
// completed-pass captures show those lines contribute no visible stroke;
// submitting them through Godot's ordinary alpha line pass produced the
// black outlines absent from retail.
// [orig: Render_CollisionWireframe @0x596800; flush @0x596780]
void emit_footprint(const MapView &view, const HudMinimapInput &input,
		const HudMinimapFootprint &footprint, Polygon &poly, Polygon &scratch,
		HudMapPass &pass, bool disc) {
	// Whole-footprint reject on the feed-time bounding circle: a mission's
	// far-side buildings must not cost triangle math every frame.
	{
		float bx = 0.0f, by = 0.0f;
		view_project(view, input, footprint.bound_x_q16, footprint.bound_y_q16,
				bx, by);
		const float radius_px = view_length_px(view,
				static_cast<float>(footprint.bound_radius_q16) / io::kFp16One);
		if (!disc) {
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
		clip_rect(poly, scratch, view.px_x1, view.px_y1, view.px_x2,
				view.px_y2);
		if (disc) {
			clip_circle32(poly, scratch, view.center_x, view.center_y,
					view.disc_radius, view.disc_radius);
		}
		emit_fan(poly, footprint.fill_argb, pass.overlays);
	}
}

// The 64-frame triangle phase: ((frame - 8) & 0x3F), folded to 63 - phase
// above 0x20 [orig: Render_MinimapSlotBlip @0x5be3f4; HUD_DrawMapOverlay
// @0x5a708f; HUD_DrawEntityLabelsAndMarkers @0x5a4b97].
uint32_t pulse_phase(int ticks) {
	uint32_t phase = static_cast<uint32_t>(ticks - 8) & 0x3Fu;
	if (phase > 0x20u) phase = 63u - phase;
	return phase;
}

// channel += phase * (255 - channel) >> 5 per RGB byte (the byte add wraps).
// [orig: @0x5be410..0x5be444; @0x5a711c..0x5a7150]
uint32_t pulse_toward_white(uint32_t argb, uint32_t phase) {
	uint32_t out = argb & 0xFF000000u;
	for (int shift = 0; shift <= 16; shift += 8) {
		const uint32_t c = (argb >> shift) & 0xFFu;
		const uint32_t p = c + ((phase * (255u - c)) >> 5);
		out |= (p & 0xFFu) << shift;
	}
	return out;
}

// The 64-frame triangle color pulse toward white
// [orig: Render_MinimapSlotBlip @0x5be3f4 — phase=((frame-8)&0x3F,
// fold >0x20 to 63-phase), channel += phase*(255-channel)>>5].
uint32_t pulse_color(uint32_t argb, int ticks) {
	return pulse_toward_white(argb, pulse_phase(ticks));
}

} // namespace minimap_detail

using namespace minimap_detail;

void hud_icon_strip_cell_uv(const HudMinimapInput &input, uint8_t icon,
		float &u0, float &v0, float &u1, float &v1) {
	marker_uv(input, icon, u0, v0, u1, v1);
}

uint32_t hud_icon_strip_modulate2x_color(uint32_t argb) {
	return marker_modulate2x_color(argb);
}

int32_t spinmap_zoom_step(int32_t zoom_q16, int direction) {
	// direction 0 never reaches here: HudMapControl::zoom_step owns the reset
	// (the mission-scaled spawn zoom), so a constant-default branch would be
	// dead — and wrong post mission scaling.
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
	if (input.map_mode != 0) {
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
	pass.clip_x1 = pass.clip_y1 = pass.clip_x2 = pass.clip_y2 = 0.0f;
	pass.clear.clear();
	pass.terrain.clear();
	pass.terrain_water.clear();
	pass.overlays.clear();
	pass.sprites.clear();
	pass.geom.clear();
	pass.lines_under.clear();
	pass.lines.clear();
	pass.labels.clear();
	pass.ring_tris.clear();
	pass.ring_tris_before_sprite = 0;
}

} // namespace

bool project_spinmap_point(const HudMinimapInput &input, int32_t world_x,
		int32_t world_y, bool clamp_to_edge, float &out_x, float &out_y) {
	const MapView view = make_view(input, effective_flags(input));
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
	const uint32_t flags = effective_flags(input);
	const MapView view = make_view(input, flags);
	out.visible = true;
	out.center_x = view.center_x;
	out.center_y = view.center_y;
	out.radius_x = view.disc_radius;
	out.radius_y = view.disc_radius;
	out.clip_x1 = view.px_x1;
	out.clip_y1 = view.px_y1;
	out.clip_x2 = view.px_x2;
	out.clip_y2 = view.px_y2;
	MapCompile c{input, view, flags, out, clip_a_, clip_b_, geom_a_, geom_b_,
			footprint_index_, false, {}, false, tracked_color_, tracked_alpha_,
			{}, 10};
	c.span = static_cast<int32_t>(scale_axis(kPointerSpanDesignPx,
			input.surface_h, kDesignHeight));
	// HUD_SetTrackedEntityTarget restamps ED0 at every set.
	// [orig: @0x59D0EF..0x59D0FF]
	if (input.overlays != nullptr &&
			input.overlays->tracked.serial != tracked_serial_) {
		tracked_serial_ = input.overlays->tracked.serial;
		tracked_color_ = input.overlays->tracked.set_color;
	}

	// bit0 lays the invisible depth mask (see kTerrainTint's neighbour note);
	// it is not geometry here. bit9 picks whether the terrain crops to it.
	const bool mask_laid = (flags & 0x1u) != 0;
	const bool terrain_disc_crop = mask_laid && (flags & 0x200u) != 0;
	bool terrain_drawn = false;

	// Terrain: 512-unit sector tiles over the covered disc, sampled through
	// the TRN routing table. Terrain rows run on NEGATED mission Y. The tile
	// pass is UNMASKED — every mode draws it. The caller always pushes
	// enable_fog_pass=1; bit9 only selects the adjacent use_alt_blend argument.
	// [orig: HUD_DrawMapOverlay tests bit9 @0x5a6670, pushes enable_fog_pass=1
	//  @0x5a6677, pushes use_alt_blend=1/0 @0x5a6684/@0x5a6696, then calls
	//  Render_TerrainDecal @0x5a66a5; its water leg gates only on that
	//  enable_fog_pass value and generated vertices @0x6079DC. Bound =
	//  diag*0.8*scale, tile snap 0x2000000 Q16, row index
	//  (-0x1000000 - y)>>25, sector =
	//  g_TerrainSectorGrid[16*(row&0xF)+(col&0xF)] - 1]
	// Every tile clips to the rect (Vertex_ClipTriangleAndEmitVertices
	// @0x688e30); bit9's pass 0x500000 (zwrite off, ztest on) additionally
	// crops it to the disc mask, pass 0x600000 (ztest always, zwrite on)
	// overwrites the mask instead [orig: id = use_alt_blend ? 0x500000 :
	// 0x600000 @0x6071F7; CGfxShader_ApplyPass @0x683221..0x6832be].
	if (input.terrain.present &&
			input.terrain.sector_count > 0 && input.terrain.sector_rows > 0) {
		const float player_x = static_cast<float>(input.player_x) / io::kFp16One;
		const float player_z = -static_cast<float>(input.player_y) / io::kFp16One;
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
				// A missing sector (grid id <= 0) is NOT skipped: the tile still
				// draws, every UV zero and the texture Colormap0
				// (tile_offset_x = max(sector, 0)) — the colormap's corner
				// texel stretched over the tile; its water pass samples
				// depthspin at UV 0 as well (no quadrant offset).
				// [orig: Render_TerrainDecal @0x60744C (UVs 0 @0x60749A /
				//  @0x6074F9 / @0x607559 / @0x60759B), @0x6075D9
				//  tile_offset_x, @0x6077CF water-UV break]
				const bool missing_sector = !sector.valid;
				const float wx0 = static_cast<float>(
						sx * terrain::COORDS_SECTOR_SIZE);
				const float wx1 = wx0 + terrain::COORDS_SECTOR_SIZE;
				// Mission y from terrain z: y = -z.
				const float wy0 = -static_cast<float>(
						sz * terrain::COORDS_SECTOR_SIZE);
				const float wy1 = -static_cast<float>(
						(sz + 1) * terrain::COORDS_SECTOR_SIZE);
				float x[4], y[4];
				view_project(view, input, io::float_to_fp16_16(wx0),
						io::float_to_fp16_16(wy0), x[0], y[0]);
				view_project(view, input, io::float_to_fp16_16(wx1),
						io::float_to_fp16_16(wy0), x[1], y[1]);
				view_project(view, input, io::float_to_fp16_16(wx1),
						io::float_to_fp16_16(wy1), x[2], y[2]);
				view_project(view, input, io::float_to_fp16_16(wx0),
						io::float_to_fp16_16(wy1), x[3], y[3]);
				// Retail binds Colormap0..3 as independent clamp textures. The
				// device stores them in one atlas, so a half-texel inset reproduces
				// the independent edge clamp without sampling the next quadrant.
				const float atlas_px =
						static_cast<float>(std::max(1, input.terrain.atlas_px));
				const float cell_px =
						static_cast<float>(std::max(1, input.terrain.cell_px));
				constexpr float inset = 0.5f;
				const float rect_u = static_cast<float>(sector.quadrant_x);
				const float rect_v = static_cast<float>(sector.quadrant_z);
				const float u0 = (rect_u + inset) / atlas_px;
				const float v0 = (rect_v + inset) / atlas_px;
				const float u1 = missing_sector ? u0
						: (rect_u + cell_px - inset) / atlas_px;
				const float v1 = missing_sector ? v0
						: (rect_v + cell_px - inset) / atlas_px;
				clip_a_.assign({{x[0], y[0], u0, v0}, {x[1], y[1], u1, v0},
						{x[2], y[2], u1, v1}, {x[3], y[3], u0, v1}});
				clip_rect(clip_a_, clip_b_, view.px_x1, view.px_y1,
						view.px_x2, view.px_y2);
				if (terrain_disc_crop) {
					clip_circle32(clip_a_, clip_b_, view.center_x,
							view.center_y, view.disc_radius,
							view.disc_radius);
				}
				const size_t tris_before = out.terrain.size();
				emit_fan(clip_a_, kTerrainTint, out.terrain);
				terrain_drawn = terrain_drawn || out.terrain.size() > tris_before;

				// Retail's unconditional fog-pass leg redraws the identical clipped
				// tile with depthspin UVs. Sector ids 2/4 select the lower half; 3/4 the
				// right half. The transparent mask texture replaces retail's
				// ADDSIGNED/alpha-test cutout while preserving the same shoreline.
				if (input.terrain.water_present) {
					const float water_u0 = sector.quadrant_x != 0
							? kDepthspinUvOffset : 0.0f;
					const float water_v0 = sector.quadrant_z != 0
							? kDepthspinUvOffset : 0.0f;
					const float water_u1 = missing_sector ? 0.0f
							: water_u0 + kDepthspinUvScale;
					const float water_v1 = missing_sector ? 0.0f
							: water_v0 + kDepthspinUvScale;
					clip_a_.assign({{x[0], y[0], water_u0, water_v0},
							{x[1], y[1], water_u1, water_v0},
							{x[2], y[2], water_u1, water_v1},
							{x[3], y[3], water_u0, water_v1}});
					clip_rect(clip_a_, clip_b_, view.px_x1, view.px_y1,
							view.px_x2, view.px_y2);
					if (terrain_disc_crop) {
						clip_circle32(clip_a_, clip_b_, view.center_x,
								view.center_y, view.disc_radius,
								view.disc_radius);
					}
					emit_fan(clip_a_, kDepthspinColor, out.terrain_water);
				}
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
	//  on g_HUDLabelFontLarge; rule color unk_FFFF7F + ctx[+0x38] alpha
	//  @0x5a6722, label color unk_FFFF7F + ctx[+0x40] alpha @0x5a68a9 —
	//  HUD_BuildMapOverlayView seeds the quartet (alpha, +0x38, +0x3C,
	//  +0x40) = 128/64/96/128 for modes 1-3 and 255/128/255/255 for the
	//  unported mode 4 @0x5a7e2f..0x5a7e44/@0x5a7f92]
	if ((flags & 0x1000u) != 0) {
		const int32_t origin_x = input.grid_origin_present
				? kGridCellQ16 * (input.grid_origin_x / kGridCellQ16) : 0;
		const int32_t origin_y = input.grid_origin_present
				? kGridCellQ16 * (input.grid_origin_y / kGridCellQ16) : 0;
		// The grid draws only on the M-map ctx (bit12), whose seeded
		// alphas are 64 for the rules and 128 for the labels/readout — 128 and
		// 255 on the CMAP window's mode-4 ctx.
		const bool window = input.map_mode == 4;
		const uint32_t rule_color = window ? 0x80FFFF7Fu : 0x40FFFF7Fu;
		const uint32_t label_color = window ? 0xFFFFFF7Fu : 0x80FFFF7Fu;
		const float half_world_q16 = std::max(view.rect_w, view.rect_h) *
				0.5f * view.scale * io::kFp16One;
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
						sx, view.px_y2, rule_color});
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
				label.color = label_color;
				label.align = 0;
				label.font = 1;
				label.clip = 1;
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
						view.px_x2, sy, rule_color});
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
				label.color = label_color;
				label.align = 0;
				label.font = 1;
				label.clip = 1;
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
		//  g_HUDLabelFontLarge at (x - 50, y - 25), unk_FFFF7F + ctx alpha]
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
			label.color = label_color;
			label.align = 0;
			label.font = 1;
			label.clip = 1;
			out.labels.push_back(label);
		}
	}

	// bit9 set: the terrain pass left the mask for the later z-tested legs;
	// bit9 clear: its zwrite overwrote the mask wherever a tile landed.
	c.disc_crop = mask_laid && (terrain_disc_crop || !terrain_drawn);

	// The marker banks, then the legs in retail pass order.
	// [orig: HUD_DrawMapOverlay @0x5a6d17..0x5a789b]
	if (flags & 0x2u) {
		footprint_index_.clear();
		if (input.footprints != nullptr) {
			footprint_index_.reserve(input.footprints->size());
			for (const HudMinimapFootprint &footprint : *input.footprints)
				footprint_index_.push_back(&footprint);
		}
		render_all_layers(c);
	}
	if (flags & 0x4u) draw_objective_tethers(c);
	if (input.overlays != nullptr && input.overlays->game_type == 0x10010 &&
			(flags & 0x8000u) != 0)
		draw_route_lines(c);
	if (flags & 0x4000u) draw_zone_waypoint_labels(c);
	if (flags & 0x8u) draw_zone_letters(c);
	if (flags & 0x810u) draw_pool3_walk(c);
	if (flags & 0x20000u) draw_player_waypoints(c);
	if (flags & 0x20u) draw_entity_labels_and_markers(c);
	if (flags & 0x80000u) draw_tracked_callout(c);
	// The nearest-FARP chevron: an idle or lit flash timer 14 and a FARP the
	// HUD info build found this frame [orig: @0x5a77f8..0x5a7834].
	if ((flags & 0x80u) != 0 && hud_item_flash_shown(input.item_flash[14]) &&
			input.farp_present)
		draw_farp_pointer(c);

	uint32_t waypoint_state_color = 0;
	int waypoint_nub_frame = 3; // blank
	if (input.waypoint_present) {
		// [orig: HUD_UpdateWaypointAltitudeColor @0x590970 — waypoint z - player z vs +-0x20000 (2.0 wu)]
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
	// The waypoint pointer: suppressed by any drawn tether, blinking with
	// flash timer 5 (idle or lit draws) [orig: @0x5a7844..0x5a7893 —
	//  !HIBYTE(v142) @0x5a7853, dword_2723D0C @0x5a7859].
	if (input.waypoint_present && (flags & 0x100u) && !c.tether_drawn &&
			hud_item_flash_shown(input.item_flash[5]))
		draw_waypoint_pointer(c, waypoint_state_color);

	// Altitude nub: the WPIndctr frame drawn just above the rect top edge,
	// nudged -8/+8 design px for above/below, in the raw state color.
	// [orig: bit20 leg @0x5a79b1..0x5a7a10 — y_base = y1 - rect_h/32, half
	//  10 & shift 8 through Viewport_ScaleToVirtualCoords,
	//  CEffect_Begin_Debug(WPIndctr handle, rect, g_WaypointAltitudeColor, extra)]
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
	//  HUD_DrawPlayerGridLabel @0x59cb40 — origin snapped to 300 wu cells, column letters from
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

	// The pointer race's distance label, then the tracked callout's — both
	// after the viewport restore [orig: @0x5a7a53..0x5a7ae4 /
	//  @0x5a7aec..0x5a7b8d].
	if (flags & 0x40000u) draw_race_label(c);
	if (flags & 0x80000u) draw_tracked_label(c);

	// Compass ring: counter-rotates so its north marker points at world
	// north, spanning the rect x1.25. The gate needs bit9 AND bit6 — the
	// big-map masks carry neither.
	// [orig: the @0x5a5f40 walk — (ctx & 0x200) && (ctx & 0x40) around
	//  HUD_DrawCompassIndicator @0x59c900; retail rotation
	//  (0x3FFFFFC0 - yaw) >> 16, size x flt_7C6F18 = 1.25]. The draw list is
	//  consumed in a +Y-down canvas, whose positive visual rotation is the
	//  opposite of retail's matrix convention, so negate that BAM delta here.
	if ((flags & 0x200u) != 0 && (flags & 0x40u) != 0) {
		// The bit-10 legs first: the dmgslice marks, then the threat ring,
		// both unclipped and under the compass [orig: `test eax, 400h`
		// @0x5a790a -> @0x5a791c..0x5a7928, then HUD_DrawCompassIndicator
		// @0x5a7931].
		if ((flags & 0x400u) != 0) hud_minimap_emit_radar(input, flags, out);
		HudMapSprite compass;
		compass.center_x = view.center_x;
		compass.center_y = view.center_y;
		// Compass quad = the uninset rect half-height x1.25. The completed-pass
		// retail probe observes 175.625 px while the backing fan is 136.5 px
		// at 1920x1080, so these are deliberately distinct operands.
		compass.half_w = view.base_radius * kCompassScale;
		compass.half_h = view.base_radius * kCompassScale;
		const uint32_t rot_bam =
				static_cast<uint32_t>(input.player_heading_bam) - 0x3FFFFFC0u;
		compass.rotation_rad = static_cast<float>(
				static_cast<double>(rot_bam >> 16) * kBam16ToRadians);
		compass.u0 = kCompassUvMin;
		compass.v0 = kCompassUvMin;
		compass.u1 = kCompassUvMax;
		compass.v1 = kCompassUvMax;
		compass.color = 0xFFFFFFFFu;
		compass.texture = 1;
		compass.layer = 5;
		// compring.tga is a colour-mode HUD texture: its material 0x651 runs
		// MODULATE2X(TEXTURE, DIFFUSE) on the device over the white diffuse
		// (renderer::hud_color_material_argb; D-HUD-49)
		// [orig: HUD_LoadAllTextures @0x59DDA0 — compring through sub_591750 in
		//  colour mode; RenderState_DecodeModeColorStage @0x6814BE..0x6814CA].
		compass.modulate2x = true;
		out.sprites.push_back(compass);
	}
}

} // namespace opennova::hud
