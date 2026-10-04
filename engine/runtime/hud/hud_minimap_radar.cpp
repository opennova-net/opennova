// The spinmap's content-mask bit-10 legs: the dmgslice sector marks and the
// threat ring, both drawn unclipped just ahead of the compass ring from the
// radar-contact snapshot (HudMinimapRadar; the state and its producers are
// world/radar_contacts.h).
// [orig: HUD_DrawMapOverlay @0x5a78ff..0x5a7931 ->
//  HUD_DrawWeaponDirectionIndicators @0x59c350, HUD_DrawTimerOverlayBox
//  @0x59c7b0 -> HUD_DrawDirectionalIndicatorRing @0x598180]

#include <runtime/hud/hud_minimap.h>

#include <cmath>

#include <base/io/bam.h>
#include <runtime/hud/hud_math.h>

namespace opennova::hud {

namespace {

// The marks' diffuse: `neg al; sbb; and 7EA000h; add 0FF808020h` over the red
// byte — 0xFFFF2020 when the red ring holds the sector, else the olive
// 0xFF808020 [orig: @0x59c469..0x59c47d / @0x59c619..0x59c62d].
constexpr uint32_t kMarkHit = 0xFFFF2020u;
constexpr uint32_t kMarkMiss = 0xFF808020u;
// The mark quad spans the base x1.25 [orig: flt_7C6F18 @0x59c490] and samples
// the centred 90% window rotated about the texture centre [orig: flt_7D93A8 =
// 0.45 @0x59c537, flt_7C3B94 = 0.5].
constexpr float kMarkScale = 1.25f;
constexpr float kMarkUvHalf = 0.45f;
// BAM16 to radians as the float constant, not 2*pi/65536 [orig: flt_7C7988].
constexpr float kBam16ToRadiansF = 9.58738019107841e-05f;
// The mark angle accumulators: 12 marks in 0x15555540 steps from 0xD5555580,
// 24 in 0x0AAAAAA0 steps from 0xCAAAAAE0, each read through its high word
// [orig: @0x59c42f / @0x59c5d8; @0x59c5e9 / @0x59c788; HIWORD @0x59c49c].
constexpr uint32_t kMark12Start = 0xD5555580u;
constexpr uint32_t kMark12Step = 0x15555540u;
constexpr uint32_t kMark24Start = 0xCAAAAAE0u;
constexpr uint32_t kMark24Step = 0x0AAAAAA0u;

// The ring's per-state vertex colours, a 6-dword stride: inner = T[6s + e],
// outer = T[6s + e + 1] for band e — transparent edges, the solid band
// between [orig: the table stores @0x598353..0x5983b9 into var_90..var_54].
constexpr uint32_t kRingColors[18] = {
	0x000F0F0Fu, 0x807F7F7Fu, 0x807F7F7Fu, 0x000F0F0Fu, 0u, 0u,
	0x000F0F0Fu, 0xFFD0D000u, 0xFFD0D000u, 0x000F0F0Fu, 0u, 0u,
	0x00800F0Fu, 0xFFFF4040u, 0xFFFF4040u, 0x00800F0Fu, 0u, 0u,
};
// Segment k's first column and the column step (5 columns span the 30
// degrees) [orig: var_C4 = 0x0ACAA800 @0x5983ca, += 0x15555000 @0x598567;
// ebp += 0x5555400 @0x5984ee].
constexpr uint32_t kRingStart = 0x0ACAA800u;
constexpr uint32_t kRingSegmentStep = 0x15555000u;
constexpr uint32_t kRingColumnStep = 0x05555400u;
constexpr int kRingSegments = 12;
constexpr int kRingBands = 3;
constexpr int kRingColumns = 5;

int32_t chop(double v) {
	return static_cast<int32_t>(v); // _ftol2_sse truncates toward zero
}

// The authored HUDSPINMAP corners through Viewport_ScaleToVirtualCoords, as the
// two drawers fild them [orig: @0x59c3b3/@0x59c3c7; @0x59c7e3/@0x59c7f7].
void scaled_rect(const HudMinimapInput &input, float &x1, float &y1, float &x2, float &y2) {
	x1 = static_cast<float>(scale_axis(input.rect_x1, input.surface_w, kDesignWidth));
	y1 = static_cast<float>(scale_axis(input.rect_y1, input.surface_h, kDesignHeight));
	x2 = static_cast<float>(scale_axis(input.rect_x2, input.surface_w, kDesignWidth));
	y2 = static_cast<float>(scale_axis(input.rect_y2, input.surface_h, kDesignHeight));
}

// One ring of marks. Each lit sector (red or olive) blinks with bit 3 of the
// HUD tick and draws a full base x1.25 quad around the rect centre whose
// texture window turns by the sector's angle; on this +Y-down canvas that is
// the rotated sprite at -theta over the 0.05..0.95 crop.
// [orig: HUD_DrawWeaponDirectionIndicators @0x59c440..0x59c5e1 (12) /
//  @0x59c5f0..0x59c791 (24); the UV block @0x59c531..0x59c5c8]
void emit_marks(const HudMinimapInput &input, const uint8_t *red, const uint8_t *olive,
		int count, uint32_t start, uint32_t step, uint8_t texture, float cx, float cy,
		float base, HudMapPass &out) {
	uint32_t acc = start;
	for (int k = 0; k < count; ++k, acc += step) {
		if (red[k] == 0 && olive[k] == 0) continue;           // @0x59c446..0x59c450
		if ((static_cast<uint32_t>(input.ticks) & 8u) == 0) continue; // @0x59c456
		HudMapSprite mark;
		mark.center_x = cx;
		mark.center_y = cy;
		mark.half_w = base * kMarkScale;
		mark.half_h = base * kMarkScale;
		const double theta = static_cast<double>(acc >> 16) *
				static_cast<double>(kBam16ToRadiansF);
		mark.rotation_rad = static_cast<float>(-theta);
		mark.u0 = 0.5f - kMarkUvHalf;
		mark.v0 = 0.5f - kMarkUvHalf;
		mark.u1 = 0.5f + kMarkUvHalf;
		mark.v1 = 0.5f + kMarkUvHalf;
		// The raw diffuse: the device runs the material's MODULATE2X(TEXTURE,
		// DIFFUSE) colour stage (mode word 0x651, loaded with flags 1617) under
		// the pass [orig: HUD_LoadAllTextures @0x59de25..0x59de42;
		// GfxShader_ApplyPassChecked(tex, 0x1300000) @0x59c484 / @0x59c634].
		mark.color = red[k] != 0 ? kMarkHit : kMarkMiss;
		mark.texture = texture;
		mark.layer = 5;
		out.sprites.push_back(mark);
	}
}

// [orig: HUD_DrawWeaponDirectionIndicators @0x59c350]
void emit_direction_marks(const HudMinimapInput &input, uint32_t flags, HudMapPass &out) {
	// Both slice textures loaded and the NoTracers bit clear
	// [orig: @0x59c359..0x59c378].
	if (!input.radar_slices_loaded || input.radar.rules_no_tracers) return;
	float x1, y1, x2, y2;
	scaled_rect(input, x1, y1, x2, y2);
	// Float centres; the base is the half-height under the fullscreen bit,
	// else the centre's reach from the left edge [orig: @0x59c3cc..0x59c434].
	const float cx = (x1 + x2) * 0.5f;
	const float cy = (y1 + y2) * 0.5f;
	const float base = (flags & 0x10000u) != 0 ? (y2 - y1) * 0.5f : cx - x1;
	const HudMinimapRadar &radar = input.radar;
	emit_marks(input, radar.red12.data(), radar.olive12.data(), 12, kMark12Start, kMark12Step,
			kHudMapSpriteRadar, cx, cy, base, out);
	emit_marks(input, radar.red24.data(), radar.olive24.data(), 24, kMark24Start, kMark24Step,
			kHudMapSpriteRadarNarrow, cx, cy, base, out);
}

// [orig: HUD_DrawTimerOverlayBox @0x59c7b0 -> HUD_DrawDirectionalIndicatorRing
//  @0x598180]
void emit_threat_ring(const HudMinimapInput &input, uint32_t flags, HudMapPass &out) {
	float x1, y1, x2, y2;
	scaled_rect(input, x1, y1, x2, y2);
	// The box: float centre, the fullscreen bit re-spans X on the half-height,
	// and the radius argument is the half-span less a width-scaled 4
	// [orig: @0x59c7fc..0x59c8c2].
	const float cx = (x1 + x2) * 0.5f;
	const float cy = (y1 + y2) * 0.5f;
	if ((flags & 0x10000u) != 0) {
		const float hh = (y2 - y1) * 0.5f;
		x1 = cx - hh;
		x2 = cx + hh;
	}
	const int s4 = static_cast<int>(scale_axis(4.0, input.surface_w, kDesignWidth));
	const float rho0 = (x2 - x1) * 0.5f - static_cast<float>(s4);

	// The ring: segment states. The friendly list that marks state 1 has no
	// writer (dword_27233D4 stays 0), so only the missiles (2) and the red-12
	// damage sectors (2, blinking) reach it [orig: @0x5981d6..0x598265 (the
	// dead friendly walk), @0x598267..0x5982e4, @0x5982e6..0x59831b].
	const HudMinimapRadar &radar = input.radar;
	const uint32_t yaw = static_cast<uint32_t>(input.player_heading_bam);
	int state[kRingSegments] = {};
	for (const HudMinimapRadar::Threat &threat : radar.threats) {
		if (threat.present == 0) continue;
		const uint32_t raw = radar_bearing_raw(
				static_cast<int32_t>(static_cast<uint32_t>(threat.x) -
						static_cast<uint32_t>(radar.local_x)),
				static_cast<int32_t>(static_cast<uint32_t>(radar.local_y) -
						static_cast<uint32_t>(threat.y)),
				-kRadarBearingScale);
		state[radar_sector(raw, yaw, 12)] = 2;
	}
	if (!radar.rules_no_tracers) {
		for (int k = 0; k < kRingSegments; ++k)
			if (radar.red12[static_cast<size_t>(k)] != 0 &&
					(static_cast<uint32_t>(input.ticks) & 8u) != 0)
				state[k] = 2;
	}

	// The four band radii from R = rho0 - scaleX(2) [orig: @0x59831d..0x5983d2].
	const int s2 = static_cast<int>(scale_axis(2.0, input.surface_w, kDesignWidth));
	const float r = rho0 - static_cast<float>(s2);
	const float radii[4] = {r - 1.0f, r, r + static_cast<float>(s2),
			static_cast<float>(static_cast<double>(r) + s2 + 1.0)};
	uint32_t segment_angle = kRingStart;
	for (int k = 0; k < kRingSegments; ++k, segment_angle += kRingSegmentStep) {
		const int s = state[k];
		for (int e = 0; e < kRingBands; ++e) {
			// The outermost/innermost edges push out by 2 per state step; the
			// radii truncate to whole pixels [orig: @0x598490..0x5984d0].
			const int32_t rin = e == 0
					? chop(static_cast<double>(radii[0]) - 2.0 * s)
					: chop(static_cast<double>(radii[e]));
			const int32_t rout = e == 2
					? chop(2.0 * s + static_cast<double>(radii[3]))
					: chop(static_cast<double>(radii[e + 1]));
			const uint32_t inner_color = kRingColors[6 * s + e];
			const uint32_t outer_color = kRingColors[6 * s + e + 1];
			// One 10-vertex strip: inner/outer pairs at 5 columns on the Q22
			// tables, +Y up [orig: @0x59846f..0x598536; strip @0x598552].
			HudMapColorVertex v[2 * kRingColumns];
			uint32_t angle = segment_angle;
			for (int j = 0; j < kRingColumns; ++j, angle += kRingColumnStep) {
				// The truncated Q22 entries (io::bam_table_*) are exact under
				// the float 2^-22 scale; the products run wide, then store.
				const int idx = io::bam_table_index(angle);
				const double c = io::bam_table_cos(idx);
				const double sn = io::bam_table_sin(idx);
				v[2 * j] = {static_cast<float>(cx + rin * c),
						static_cast<float>(cy - rin * sn), inner_color};
				v[2 * j + 1] = {static_cast<float>(cx + rout * c),
						static_cast<float>(cy - rout * sn), outer_color};
			}
			for (int t = 0; t + 2 < 2 * kRingColumns; ++t) {
				if ((t & 1) == 0)
					out.ring_tris.push_back({v[t], v[t + 1], v[t + 2]});
				else
					out.ring_tris.push_back({v[t + 1], v[t], v[t + 2]});
			}
		}
	}
}

} // namespace

uint32_t radar_bearing_raw(int32_t dx, int32_t dy, double scale) {
	const double v = std::atan2(static_cast<double>(dx), static_cast<double>(dy)) * scale;
	return static_cast<uint32_t>(static_cast<int64_t>(v));
}

int radar_sector(uint32_t raw, uint32_t yaw, int sectors) {
	const uint32_t a = 0xF5555800u - raw - yaw;
	return static_cast<int>(((a >> 16) * static_cast<uint32_t>(sectors)) >> 16);
}

void hud_minimap_emit_radar(const HudMinimapInput &input, uint32_t flags, HudMapPass &out) {
	emit_direction_marks(input, flags, out);
	emit_threat_ring(input, flags, out);
	out.ring_tris_before_sprite = out.sprites.size();
}

} // namespace opennova::hud
