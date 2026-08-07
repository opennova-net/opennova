#pragma once

#include <cstdint>
#include <string>

// The HUD view-helper math cluster (the ENG-4/FNT pattern: math + constants
// native, draw/Font blits stay shell-side). Every function is the exact
// witnessed policy its shell caller cites; the GDScript Hud* helpers delegate
// here and keep only CanvasItem work. Witness record:
// docs/interface/hud-re.md.

namespace opennova::hud {

// ---------------------------------------------------------------------------
// hudpos.def design-space scaling [orig: Viewport_ScaleToVirtualCoords
// @0x5d2b20]: every HUD element authored in a fixed 1024x768 virtual space
// scales onto the real surface with round-to-nearest.

inline constexpr double kDesignWidth = 1024.0;
inline constexpr double kDesignHeight = 768.0;

// One axis: floor((design * surface + extent/2) / extent) — the original's
// (x*surface_w + 512)/1024 rounding.
double scale_axis(double design, double surface, double design_extent);
// An output-pixel delta back into design units (0 on a degenerate surface);
// integer pixel deltas commute with scale_axis' rounding.
double pixel_delta_to_design(double delta, double surface, double design_extent);

// ---------------------------------------------------------------------------
// The ALPHAFADE ramp [orig: alphafade parse @0x5a086c — 2.55 (percent ->
// 0..255 alpha) and 62.0 (seconds -> ticks); decay @0x599af9/@0x599fc0]:
// fade = 255 - u8(u16((elapsed<<16)/ticks - 1) >> 8), elapsed-0 wrap quirk
// included (one 62 Hz tick of latency).

inline constexpr double kPercentToAlpha = 2.55;
inline constexpr double kSecondsToTicks = 62.0;

int fade_decay(int elapsed_ticks, int ramp_ticks);
// The ammo/clip flash: base + decay clamped by the ALPHAFADE max
// [orig: draw_hud_ammo_indicator @0x599af9].
int fade_flash_alpha(int elapsed_ticks, int ramp_ticks, int base_alpha,
		int max_alpha);
// The stance cross-fade pair: current = min(base + decay, 255); the previous
// frame ghosts at decay >> 2 (the witnessed (fade<<22) alpha-byte trick)
// [orig: draw-stance @0x599fc2..0x59a2e3].
int stance_current_alpha(int elapsed_ticks, int ramp_ticks, int base_alpha);
int stance_prev_alpha(int elapsed_ticks, int ramp_ticks);

// ---------------------------------------------------------------------------
// The stance indicator's shared Q16 scale from FRAME 0's dims
// [orig: HUD_DrawStanceIndicator @0x599f10 — 0x800000/max(w,h) @0x59a00a,
// dim scale @0x59a023, 128-box centering @0x59a02a..0x59a07e (none at 127+)].

int32_t stance_scale_q16(int frame0_w, int frame0_h);
int stance_scaled_dim(int dim, int32_t q16);
int stance_center_axis(int scaled_dim);

// ---------------------------------------------------------------------------
// The health bar's color thresholds on the witnessed 16.16 ratio
// [orig: HUD_DrawHealthBar @0x5a2e50 — > 0xC000 good, > 0x6FFF mid, else
// bad]. Returns 0 good / 1 mid / 2 bad.

int health_color_band_fp16(int32_t ratio_fp16);

// ---------------------------------------------------------------------------
// The message feed's tick policy [orig: HUD_DisplayTriggeredText @0x51f190 ->
// Chat_AddDebugMessage @0x4987f0 — 930-tick life @0x51f216, the >=186-tick
// expiry stagger vs the previous line @0x49894e].

inline constexpr int kMessageLifeTicks = 930;
inline constexpr int kMessageExpiryStagger = 186;
inline constexpr int kMessageTextMax = 119;    // [orig: @0x49884e 120-byte slots]
inline constexpr int kMessageSlotCount = 40;   // [orig: Chat_RebuildDisplayBuffers @0x498bd0]

// The pushed line's expiry: now + life, floored to prev_expire + stagger when
// a previous line exists.
int message_expire_tick(int now_ticks, int prev_expire, bool has_prev);

// ---------------------------------------------------------------------------
// Half-bright text color [orig: (color >> 1) & 0x7F7F7F | 0xFF000000] on the
// packed 0xAARRGGBB form.

uint32_t half_bright_argb(uint32_t argb);

// ---------------------------------------------------------------------------
// The ammo counter's text fold [orig: hud_draw_weapon_ammo_and_name @0x5939d0,
// @0x593a33..0x593ab0]: "clip/reserve" for a magazine weapon (clip valid,
// capacity >= 2), plain "reserve" otherwise, empty on the -1 sentinels; and
// the weapon-name x nudge on surfaces 640 wide or narrower (-4 left / +4
// right) [orig: @0x593b36..0x593bf5].

std::string format_ammo(int clip, int reserve, int capacity);
int weapon_name_x_nudge(bool narrow_surface, int align);

// ---------------------------------------------------------------------------
// The clip indicator's round-icon count [orig: draw_hud_ammo_indicator
// @0x599b9c..0x599bc1]: the magazine count (the carried pool for capacity-1
// weapons), ceil-divided by a >1 rounds-per-icon divisor, capped at 40.

inline constexpr int kMaxRoundIcons = 40; // [orig: @0x599bbf]
int round_icon_count(int clip, int reserve, int capacity, int divisor);

// ---------------------------------------------------------------------------
// The crosshair spread projection [orig: HUD_DrawCrosshair
// @0x592b07..0x592bf5]: pixel = (spread_16.16 * screen_w / int(fov_deg)) >> 16
// (the 2^31/180 degree factors cancel); the instability sum wraps int32 with
// the two arithmetic >>7 recoil terms [orig: @0x592b07..0x592b28]; the ERROR
// row = stance (0 prone / 1 crouch / 2 stand) + 3 when scoped
// [orig: @0x592b37..0x592b84]; aimed shots hide the reticle unless the
// vehicle/gunner leg keeps it [orig: gate @0x592afa]. Taper 0.1
// [orig: HUD_DrawCrosshairCornerQuad @0x590f50 all cases].

inline constexpr double kCrosshairTaper = 0.1;

double crosshair_spread_px_fp16(int32_t spread_fp16, double fov_deg,
		double screen_w);
int32_t crosshair_total_spread_fp16(int32_t error_fp16, int32_t recoil_pitch_bam,
		int32_t weapon_weight_spread_bam);
int crosshair_error_row(int stance, bool scoped);
bool crosshair_should_draw(bool aimed_shot_available, bool keep_while_aimed);

} // namespace opennova::hud
