#pragma once

#include <cstdint>
#include <base/io/tick_rate.h>
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
inline constexpr double kSecondsToTicks = io::kTicksPerSecondInt;

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
// The SYSTEM feed's witnessed geometry [orig: HUD_DrawConsoleMessages
// @0x59ad30]: only three ring rows are walked per channel (the system walk
// @0x59ae5e..0x59aebf) and consecutive lines step 18 DESIGN px (the 0x12
// store @0x59ad97, scaled through Viewport_ScaleToVirtualCoords @0x5d2b20).
inline constexpr int kFeedVisibleRows = 3;
inline constexpr int kFeedLineStepDesign = 18;
inline constexpr int kMessageTextMax = 119;    // [orig: @0x49884e 120-byte slots]
inline constexpr int kMessageSlotCount = 40;   // [orig: Chat_RebuildDisplayBuffers @0x498bd0]

// The pushed line's expiry: now + life, floored to prev_expire + stagger when
// a previous line exists.
int message_expire_tick(int now_ticks, int prev_expire, bool has_prev);

// ---------------------------------------------------------------------------
// Half-bright text color [orig: (color >> 1) & 0x7F7F7F | 0xFF000000] on the
// packed 0xAARRGGBB form.

uint32_t half_bright_argb(uint32_t argb);

// The alpha-PRESERVING half-bright fold the friendly-tag text rides — the
// distance fade must survive the transform
// [orig: HUD_DrawTextHalfBrightF @0x580720 —
//  (color & 0xFF000000) + ((color >> 1) & 0x7F7F7F)].
uint32_t half_bright_keep_alpha(uint32_t argb);

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
// The capacity-1 reserve fold [orig: HUD_BuildEntityInfo @0x4b85ef —
// hudInfo+52 += clip when def+88 == 1]: the chambered round joins the
// displayed reserve (the ammo text and the round icons both read the folded
// count).

int folded_reserve(int clip, int reserve, int capacity);

// ---------------------------------------------------------------------------
// The waypoint distance label [orig: HUD_DrawWaypointNameAndDistance
// @0x5947e5..0x594836]: horizontal-only (the mission X/Y plane), fixed sqrt
// truncated to whole meters (the sar-16 read).

int waypoint_distance_m(double dx, double dz);

// ---------------------------------------------------------------------------
// The weapon heat bar [orig: HUD_DrawWeaponHeatBar @0x599700 — gate
// hudInfo+60 @0x59970a; spans @0x5997a1..0x59981f]: heat is 0..0xFFFF, the
// fill span is the rounded 16.16 fraction of the bar extent, and the fill
// axis follows the authored rect (wide -> horizontal left-to-right, tall ->
// vertical bottom-up).

int heat_fill_span(int extent_px, int heat);
bool heat_bar_is_horizontal(double width, double height);

// ---------------------------------------------------------------------------
// The PowerThrow windup curve [orig: HUD_DrawPowerThrowChargeBar @0x599830 —
// curve @0x5998ad (full through the first 31 held ticks — the tap window
// throws at full power — then restarting at 0 and climbing (held-31)/93 to
// 1); fill @0x599964 = (progress_fp16 * extent + 0x8000) >> 16].

inline constexpr int kPowerThrowTapTicks = 31;
inline constexpr int kPowerThrowRampTicks = 93;

int32_t power_throw_progress_fp16(int held_ticks);
int power_fill_span(int32_t progress_fp16, int extent_px);

// ---------------------------------------------------------------------------
// The loading-bar fill arithmetic [orig: @ 0x5d4c40 — right edge =
// displayed * (w + 2) / 100 + x + 4, clamped to the track, then the final
// 1px inset]. Progress itself is an exact mission-stage checkpoint.

struct LoadingBarSpan {
	int left = 0;
	int right = 0; // right <= left draws an empty fill
};
LoadingBarSpan loading_bar_fill_span(int x, int w, int displayed);

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

// ---------------------------------------------------------------------------
// Friendly tags (D-HUD-20) [orig: HUD_DrawEntityLabel @0x5a39b0, called by
// HUD_DrawFriendlyTagsPass @0x5a4480]. Modes cycle 0 off / 1 text under 300 m
// ("FARBRIEF") / 2 text always ("FULL", the boot default @0x4a7fed) / 3 tick
// marks ("BRIEF") [orig: input action case 30 @0x49b573].

enum class FriendlyTagMode : int {
	kOff = 0,
	kFarBrief = 1,  // text under 300 m
	kFull = 2,      // text always
	kBrief = 3,     // tick marks
};
inline constexpr int kFriendlyTagModeOff = static_cast<int>(FriendlyTagMode::kOff);
inline constexpr int kFriendlyTagModeFarBrief = static_cast<int>(FriendlyTagMode::kFarBrief);
inline constexpr int kFriendlyTagModeFull = static_cast<int>(FriendlyTagMode::kFull);
inline constexpr int kFriendlyTagModeBrief = static_cast<int>(FriendlyTagMode::kBrief);
inline constexpr int kFriendlyTagModeCount = 4;
// Boot default FULL, process-lifetime like retail's global
// [orig: g_friendlyTagsMode @0x24C18C4; default @0x4a7fed].
inline constexpr FriendlyTagMode kFriendlyTagModeDefault = FriendlyTagMode::kFull;
// The cycle 0 -> 1 -> 2 -> 3 -> 0 [orig: input action case 30 @0x49b573].
inline FriendlyTagMode next_friendly_tag_mode(FriendlyTagMode mode) {
	return static_cast<FriendlyTagMode>((static_cast<int>(mode) + 1) % kFriendlyTagModeCount);
}

// The minimum draw distance (0.5 u) [orig: @0x5a3b0c] and the mode-1 text
// cutoff (300.0 u) [orig: @0x5a3fcf], both 16.16 world units.
inline constexpr int32_t kFriendlyTagMinDistQ16 = 0x8000;
inline constexpr int32_t kFriendlyTagTextCutQ16 = 0x12C0000;

// The distance alpha: 255 at <= 50 m fading to 63 at >= 300 m
// [orig: @0x5a3eeb..0x5a3f18 — alpha byte = 255 - clamp(192*(m-50)/250, 0, 192)].
int friendly_tag_alpha(int32_t dist_q16);

// Whether the text form draws for this mode/distance [orig: @0x5a3fc3..0x5a3fcf].
bool friendly_tag_text_visible(int mode, int32_t dist_q16);

// The speaking-entity pulse: each channel saturates at c/2 + level/4
// [orig: the MMX blend @0x5a3e98..0x5a3ebf against g_audioOutLevelStage1;
// the speaking entity is g_voicePlaybackEntity @0xC6EC38, stamped at scripted
// positional voice start @0x4ece03]. Alpha byte passes through.
uint32_t friendly_tag_speaking_blend(uint32_t argb, int level255);

// The medic-request pulse on a downed teammate's light-blue tag: a 64-frame
// triangle wave on the HUD frame counter lifts every RGB channel toward white
// and back — `t = (frame - 8) & 0x3F; if (t > 0x20) t = 0x3F - t;
// c += ((255 - c) * t) >> 5` per channel, alpha untouched
// [orig: HUD_DrawEntityLabel @0x5a3dfb..0x5a3e6d over g_hudColorLightBlue].
uint32_t friendly_tag_revive_pulse(uint32_t argb, int frame_counter);

// The two g_hudColorTable entries the downed legs read [orig:
// HUD_InitTeamColorTable @0x51f26d table[3] light blue, @0x51f295 table[8]
// gray; read @0x5a3deb / @0x5a3e77].
inline constexpr uint32_t kFriendlyTagDownedLightBlue = 0xFF80A0FFu;
inline constexpr uint32_t kFriendlyTagDownedGray = 0xFFA0A0A0u;

// The unnamed-entity fallback: a literal '^' + the compiled-in 36-name table
// indexed by the pool-encoded entity id [orig: @0x5a4047..0x5a40cd;
// g_fallbackPeopleNames @0x840a78, count @0x840a0c].
std::string friendly_tag_fallback_name(uint16_t encoded_entity_id);

// The overlay label-font selection by surface width: the Arial pair and the
// slot scale the friendly-tag / attach-label draws ride
// [orig: HUD_InitAllFonts @0x51ee20 — width <= 640: Arial14n/Arial12b over
// 640; <= 800: Arial14n/Arial14b over 800; > 800: Arial16n/Arial16b over
// 1024; scale = (screenWidth<<16)/divisor @0x51ef26, stored as the slot's
// float pair by HUD_LoadFontIntoSlot @0x580400].
struct HudLabelFontChoice {
	const char *normal_fnt;
	const char *bold_fnt;
	// The single-file large slot the big-map grid labels ride
	// [orig: HUD_InitAllFonts — g_hudLabelFontLarge @0xB4C3A0 = Impac22b.fnt
	//  at the over-800 scale for every width].
	const char *large_fnt;
	// The Impact38 slot the end-round overlay / death screen / disconnect
	// dialog draw with, loaded at the SAME over-800 scale as the large slot
	// [orig: HUD_InitAllFonts — g_hudLabelFontImpact38 @0xB4C3AC = Impac38b.fnt
	//  @0x51ef7b..0x51ef94, the scale register shared with Impac22b].
	const char *impact38_fnt = nullptr;
	float scale = 1.0f;
	float large_scale = 1.0f;
	int tier = 0; // 0 <= 640 / 1 <= 800 / 2 > 800 — stable id for reload checks
};
HudLabelFontChoice hud_label_font_choice(int surface_w);

} // namespace opennova::hud
