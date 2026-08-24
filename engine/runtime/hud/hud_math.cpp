#include "hud/hud_math.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace opennova::hud {

// [orig: Viewport_ScaleToVirtualCoords @0x5d2b20]
double scale_axis(double design, double surface, double design_extent) {
	return std::floor((design * surface + design_extent * 0.5) / design_extent);
}

double pixel_delta_to_design(double delta, double surface, double design_extent) {
	if (surface <= 0.0) return 0.0;
	return delta * design_extent / surface;
}

// [orig: draw_hud_ammo_indicator @0x599af9; draw-stance @0x599fc0] Exact
// integer translation, including the elapsed-0 u16 underflow quirk (elapsed 0
// reads fully decayed — one 62 Hz tick of latency).
int fade_decay(int elapsed_ticks, int ramp_ticks) {
	if (ramp_ticks <= 0) return 0;
	const int e = std::clamp(elapsed_ticks, 0, ramp_ticks);
	const int frac =
			(((static_cast<int64_t>(e) << 16) / ramp_ticks) - 1) & 0xFFFF;
	return 255 - ((frac >> 8) & 0xFF);
}

int fade_flash_alpha(int elapsed_ticks, int ramp_ticks, int base_alpha,
		int max_alpha) {
	return std::min(base_alpha + fade_decay(elapsed_ticks, ramp_ticks),
			max_alpha);
}

// [orig: draw-stance @0x599fc2..0x59a2e3]
int stance_current_alpha(int elapsed_ticks, int ramp_ticks, int base_alpha) {
	return std::min(base_alpha + fade_decay(elapsed_ticks, ramp_ticks), 255);
}

int stance_prev_alpha(int elapsed_ticks, int ramp_ticks) {
	return fade_decay(elapsed_ticks, ramp_ticks) >> 2;
}

// [orig: HUD_DrawStanceIndicator @0x59a00a]
int32_t stance_scale_q16(int frame0_w, int frame0_h) {
	const int m = std::max(frame0_w, frame0_h);
	return m > 0 ? 0x800000 / m : 0;
}

// [orig: @0x59a023]
int stance_scaled_dim(int dim, int32_t q16) {
	return static_cast<int>(
			(static_cast<int64_t>(q16) * dim + 0x8000) >> 16);
}

// [orig: @0x59a02a..0x59a07e] 127+ centers at 0.
int stance_center_axis(int scaled_dim) {
	return scaled_dim >= 127 ? 0 : (128 - scaled_dim) / 2;
}

// [orig: HUD_DrawHealthBar @0x5a2e50]
int health_color_band_fp16(int32_t ratio_fp16) {
	if (ratio_fp16 > 0xC000) return 0;
	if (ratio_fp16 > 0x6FFF) return 1;
	return 2;
}

// [orig: Chat_AddDebugMessage @0x4987f0 — @0x51f216 life, @0x49894e stagger]
int message_expire_tick(int now_ticks, int prev_expire, bool has_prev) {
	const int expire = now_ticks + kMessageLifeTicks;
	if (!has_prev) return expire;
	return std::max(expire, prev_expire + kMessageExpiryStagger);
}

// [orig: HUD_DrawTextRightAligned_HalfBright @0x580850 —
// (color >> 1) & 0x7F7F7F | 0xFF000000]
uint32_t half_bright_argb(uint32_t argb) {
	return ((argb >> 1) & 0x7F7F7Fu) | 0xFF000000u;
}

// [orig: HUD_DrawTextHalfBrightF @0x580720]
uint32_t half_bright_keep_alpha(uint32_t argb) {
	return (argb & 0xFF000000u) + ((argb >> 1) & 0x7F7F7Fu);
}

// [orig: hud_draw_weapon_ammo_and_name @0x593a33..0x593ab0]
std::string format_ammo(int clip, int reserve, int capacity) {
	if (reserve == -1 || capacity == -1) return std::string();
	if (clip != -1 && capacity >= 2) {
		return std::to_string(clip) + "/" + std::to_string(reserve);
	}
	return std::to_string(reserve);
}

// [orig: @0x593b36..0x593bf5] align 0 = left, 1 = right; other alignments
// take no nudge.
int weapon_name_x_nudge(bool narrow_surface, int align) {
	if (!narrow_surface) return 0;
	if (align == 0) return -4;
	if (align == 1) return 4;
	return 0;
}

// [orig: draw_hud_ammo_indicator @0x599b9c..0x599bc1]
int round_icon_count(int clip, int reserve, int capacity, int divisor) {
	int n = capacity == 1 ? reserve : clip;
	if (divisor > 1) n = (n + 1) / divisor;
	return std::min(n, kMaxRoundIcons);
}

// [orig: HUD_BuildEntityInfo @0x4b85ef — hudInfo+52 += clip when def+88 == 1;
// the -1 sentinels (no clip / infinite) never fold]
int folded_reserve(int clip, int reserve, int capacity) {
	if (capacity == 1 && clip >= 0 && reserve >= 0) return reserve + clip;
	return reserve;
}

// [orig: HUD_DrawWaypointNameAndDistance @0x5947e5..0x594836 — 2D fixed sqrt,
// the >>16 truncation to whole meters]
int waypoint_distance_m(double dx, double dz) {
	return static_cast<int>(std::sqrt(dx * dx + dz * dz));
}

// [orig: HUD_DrawWeaponHeatBar spans @0x5997a1..0x59981f]
int heat_fill_span(int extent_px, int heat) {
	const int h = std::clamp(heat, 0, 0xFFFF);
	return (extent_px * h + 0x8000) >> 16;
}

bool heat_bar_is_horizontal(double width, double height) {
	return height <= width;
}

// [orig: HUD_DrawPowerThrowChargeBar curve @0x5998ad — 1/93 = flt_7CD390,
// fld1 clamp; full through the 31-tick tap window]
int32_t power_throw_progress_fp16(int held_ticks) {
	if (held_ticks < kPowerThrowTapTicks) return 0x10000;
	const int64_t ramped =
			(static_cast<int64_t>(held_ticks - kPowerThrowTapTicks) << 16) /
			kPowerThrowRampTicks;
	return static_cast<int32_t>(std::min<int64_t>(ramped, 0x10000));
}

// [orig: fill @0x599964]
int power_fill_span(int32_t progress_fp16, int extent_px) {
	return static_cast<int>(
			(static_cast<int64_t>(progress_fp16) * extent_px + 0x8000) >> 16);
}

// [orig: LoadingScreen_UpdateAndPresent @ 0x586c3f; the catch-up max is the
// D-LOADSCR-1 cadence adaptation]
int loading_bar_step(int displayed, int reported) {
	const int lead_cap = std::min(reported + 10, 100);
	return std::clamp(std::max(displayed + 1, reported), 0, lead_cap);
}

// [orig: the fill arithmetic @ 0x5d4c40 — the original's integer divide]
LoadingBarSpan loading_bar_fill_span(int x, int w, int displayed) {
	int fill_right = x + 4 + displayed * (w + 2) / 100;
	fill_right = std::min(fill_right, x + w + 4) - 1;
	return {x + 3, fill_right};
}

// [orig: HUD_DrawCrosshair @0x592b07..0x592bf5 — the HIWORD fov truncation and
// the >>16 both survive; the 2^31/180 factors cancel between spread and fov]
double crosshair_spread_px_fp16(int32_t spread_fp16, double fov_deg,
		double screen_w) {
	const int fov_i = static_cast<int>(fov_deg);
	if (fov_i <= 0) return 0.0;
	return static_cast<double>(
			static_cast<int64_t>(
					static_cast<double>(spread_fp16) * screen_w / fov_i) >>
			16);
}

// [orig: @0x592b07..0x592b28] Wrapping i32 adds with the two arithmetic SAR
// recoil terms.
int32_t crosshair_total_spread_fp16(int32_t error_fp16, int32_t recoil_pitch_bam,
		int32_t weapon_weight_spread_bam) {
	uint32_t acc = static_cast<uint32_t>(error_fp16) +
			static_cast<uint32_t>(recoil_pitch_bam >> 7);
	acc += static_cast<uint32_t>(weapon_weight_spread_bam >> 7);
	int32_t out;
	static_assert(sizeof(out) == sizeof(acc), "i32 reinterpret");
	std::memcpy(&out, &acc, sizeof(out));
	return out;
}

// [orig: @0x592b37..0x592b84]
int crosshair_error_row(int stance, bool scoped) {
	return std::clamp(stance, 0, 2) + (scoped ? 3 : 0);
}

// [orig: gate @0x592afa]
bool crosshair_should_draw(bool aimed_shot_available, bool keep_while_aimed) {
	return !aimed_shot_available || keep_while_aimed;
}

// [orig: HUD_DrawEntityLabel @0x5a3eeb..0x5a3f18 — v30 = 192*(dist_m-50)/250
// clamped to [0,192]; the (color & 0xFFFFFF) - ((v30+1)<<24) borrow leaves
// alpha = 255 - v30]
int friendly_tag_alpha(int32_t dist_q16) {
	const int dist_m = dist_q16 / 0x10000;
	int fade = 192 * (dist_m - 50) / 250;
	fade = std::clamp(fade, 0, 192);
	return 255 - fade;
}

// [orig: @0x5a3fc3 mode 2 always; @0x5a3fcf mode 1 iff dist < 0x12C0000;
// modes 0/3 never draw the text form]
bool friendly_tag_text_visible(int mode, int32_t dist_q16) {
	if (mode == kFriendlyTagModeFull) return true;
	return mode == kFriendlyTagModeFarBrief && dist_q16 < kFriendlyTagTextCutQ16;
}

// [orig: the MMX block @0x5a3e98..0x5a3ebf — punpcklbw(c,c) >> 1 (~c*128.5)
// paddusw (level << 6) per 16-bit lane, >> 8, packuswb: each channel
// saturates at c/2 + level/4]
uint32_t friendly_tag_speaking_blend(uint32_t argb, int level255) {
	const uint32_t lvl = static_cast<uint32_t>(std::clamp(level255, 0, 255));
	uint32_t out = argb & 0xFF000000u;
	for (int shift = 0; shift <= 16; shift += 8) {
		const uint32_t c = (argb >> shift) & 0xFFu;
		// The duplicated-byte lane is c*257; >>1 then +level<<6 then >>8.
		const uint32_t lane = std::min<uint32_t>(
				((c * 257u) >> 1) + (lvl << 6), 0xFFFFu);
		out |= std::min<uint32_t>(lane >> 8, 0xFFu) << shift;
	}
	return out;
}

uint32_t friendly_tag_revive_pulse(uint32_t argb, int frame_counter) {
	// [orig: @0x5a3dfb..0x5a3e6d]
	uint32_t t = (static_cast<uint32_t>(frame_counter) - 8u) & 0x3Fu;
	if (t > 0x20u) t = 0x3Fu - t;
	uint32_t out = argb & 0xFF000000u;
	for (int shift = 0; shift <= 16; shift += 8) {
		const uint32_t c = (argb >> shift) & 0xFFu;
		// `add bl, cl` — the channel byte wraps like retail's byte add.
		const uint32_t lifted = (c + (((255u - c) * t) >> 5)) & 0xFFu;
		out |= lifted << shift;
	}
	return out;
}

// [orig: g_fallbackPeopleNames @0x840a78 — the 36 compiled-in name strings,
// verbatim including the double-space rank padding; count @0x840a0c]
static const char *const kFallbackPeopleNames[] = {
	"PFC  Mitchell", "PVT  Neibauer", "PFC  Daly", "SPC  Berg",
	"PVT  Spence", "PFC  Gordon", "SGT  Taylor", "PVT  Browning",
	"SGT  Wyatt", "CPL  King", "PFC  Herrell", "SSG  Smith",
	"SPC  Jones", "PFC  Mathis", "SGT  Berg", "PFC  Hargrove",
	"CPL  Draper", "SFC  King", "SGT  Cleveland", "SSG  Whalen",
	"SPC  Street", "PVT  West", "PFC  Garcia", "CPL  Martinez",
	"SGT  Brown", "SSG  Alvarez", "SFC  Fedoroff", "SGT  Cooper",
	"PFC  Bennett", "PFC  Davison", "SGT  White", "SGT  Travis",
	"SSG  McLean", "SFC  Santiago", "SGT  Bertsch", "SGT  Barber",
};
inline constexpr int kFallbackPeopleNameCount = 36;

// [orig: @0x5a4047..0x5a40cd — name[0] = '^' (0x5E), then the table entry at
// encoded_id % count; the id is the (pool << 12) | slot the walk derives]
std::string friendly_tag_fallback_name(uint16_t encoded_entity_id) {
	std::string out = "^";
	out += kFallbackPeopleNames[encoded_entity_id % kFallbackPeopleNameCount];
	return out;
}

// [orig: HUD_InitAllFonts @0x51ee20 — the sprintf'd names per width branch
// @0x51ee8a..0x51ef0e; the retail fixed-point (w<<16)/divisor lands in the
// slot as a float @0x58045d, so the float division is the same value modulo
// the 16.16 truncation]
HudLabelFontChoice hud_label_font_choice(int surface_w) {
	// The large slot is one file at the over-800 divisor for every width
	// [orig: g_hudLabelFontLarge = Impac22b.fnt "over 800" @0x51ee20].
	const float large_scale = static_cast<float>(surface_w) / 800.0f;
	if (surface_w > 800) {
		return {"Arial16n.fnt", "Arial16b.fnt", "Impac22b.fnt", "Impac38b.fnt",
				static_cast<float>(surface_w) / 1024.0f, large_scale, 2};
	}
	if (surface_w > 640) {
		return {"Arial14n.fnt", "Arial14b.fnt", "Impac22b.fnt", "Impac38b.fnt",
				static_cast<float>(surface_w) / 800.0f, large_scale, 1};
	}
	return {"Arial14n.fnt", "Arial12b.fnt", "Impac22b.fnt", "Impac38b.fnt",
			static_cast<float>(surface_w) / 640.0f, large_scale, 0};
}

} // namespace opennova::hud
