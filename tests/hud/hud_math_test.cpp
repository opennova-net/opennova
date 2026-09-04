// The HUD view-helper math cluster (engine/runtime/hud): the hudpos.def
// design-space scaling [orig: Viewport_ScaleToVirtualCoords @0x5d2b20], the
// ALPHAFADE decay with its elapsed-0 wrap quirk [orig: @0x599af9/@0x599fc0],
// the stance Q16 scale + 128-box centering [orig: @0x599f10], the health color
// bands [orig: @0x5a2e50], the message tick policy [orig: @0x51f216/@0x49894e],
// half-bright [orig: @0x580850], the ammo text fold + name nudge
// [orig: @0x5939d0], the round-icon count [orig: @0x599b9c], and the crosshair
// spread projection/sum/row/gate [orig: @0x592640].

#include <runtime/hud/hud_math.h>

#include <cstdio>

namespace {

namespace hud = opennova::hud;

int failures = 0;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

} // namespace

int main() {
	// Design-space scaling: 1024x768 identity surface maps through the
	// round-to-nearest; a 2048-wide surface doubles with the +512 rounding.
	expect(hud::scale_axis(100.0, 1024.0, hud::kDesignWidth) == 100.0,
			"identity surface maps 1:1");
	expect(hud::scale_axis(3.0, 2048.0, hud::kDesignWidth) == 6.0,
			"doubled surface doubles the axis");
	expect(hud::scale_axis(1.0, 1500.0, hud::kDesignWidth) == 1.0,
			"round-to-nearest floors (1*1500+512)/1024");
	expect(hud::pixel_delta_to_design(10.0, 0.0, hud::kDesignWidth) == 0.0,
			"degenerate surface yields zero delta");

	// The ALPHAFADE decay: elapsed 1 is fully lit, the ramp end is 0, and
	// elapsed 0 reads fully decayed (the witnessed u16 underflow quirk).
	expect(hud::fade_decay(1, 62) == 251,
			"one tick in: 255 - ((65536/62 - 1) >> 8)");
	expect(hud::fade_decay(62, 62) == 0, "ramp end: decayed");
	expect(hud::fade_decay(0, 62) == 0, "elapsed 0 underflows to decayed");
	expect(hud::fade_decay(5, 0) == 0, "zero ramp disables");
	expect(hud::fade_flash_alpha(1, 62, 100, 200) == 200,
			"the flash clamps at the ALPHAFADE max");
	expect(hud::stance_current_alpha(1, 62, 100) == 255,
			"stance current clamps at 255");
	expect(hud::stance_prev_alpha(1, 62) == 251 >> 2,
			"the previous frame ghosts at quarter fade (the <<22 trick)");

	// Stance Q16 scale + centering.
	expect(hud::stance_scale_q16(64, 32) == 0x800000 / 64,
			"the shared factor comes from the larger frame-0 dim");
	expect(hud::stance_scale_q16(0, 0) == 0, "degenerate frame 0");
	expect(hud::stance_scaled_dim(64, 0x800000 / 64) == 128,
			"a max-dim frame scales to the 128 box");
	expect(hud::stance_center_axis(128) == 0, "127+ centers at 0");
	expect(hud::stance_center_axis(64) == 32, "(128-64)/2 centering");

	// Health color bands on the witnessed 16.16 thresholds.
	expect(hud::health_color_band_fp16(0xC001) == 0, "above 0xC000 = good");
	expect(hud::health_color_band_fp16(0xC000) == 1, "0xC000 itself = mid");
	expect(hud::health_color_band_fp16(0x7000) == 1, "above 0x6FFF = mid");
	expect(hud::health_color_band_fp16(0x6FFF) == 2, "0x6FFF itself = bad");

	// The message tick policy: 930-tick life, >=186-tick stagger.
	expect(hud::message_expire_tick(100, 0, false) == 1030,
			"a first line lives 930 ticks");
	expect(hud::message_expire_tick(100, 1000, true) == 1186,
			"a crowded push floors to prev + 186");
	expect(hud::message_expire_tick(1000, 100, true) == 1930,
			"a late push keeps its own 930 life");

	// Half-bright.
	expect(hud::half_bright_argb(0x80FFFFFFu) == 0xFF7F7F7Fu,
			"(color >> 1) & 0x7F7F7F | opaque");
	expect(hud::half_bright_argb(0xFF204060u) == 0xFF102030u,
			"channels halve independently");

	// The ammo text fold + the narrow-surface name nudge.
	expect(hud::format_ammo(12, 60, 30) == "12/60", "magazine weapons pair");
	expect(hud::format_ammo(-1, 5, 1) == "5", "capacity-1 folds to reserve");
	expect(hud::format_ammo(3, 9, 1) == "9",
			"capacity < 2 shows the reserve even with a clip");
	expect(hud::format_ammo(0, -1, 30).empty(), "reserve sentinel: nothing");
	expect(hud::format_ammo(0, 5, -1).empty(), "capacity sentinel: nothing");
	expect(hud::weapon_name_x_nudge(true, 0) == -4, "narrow left nudges -4");
	expect(hud::weapon_name_x_nudge(true, 1) == 4, "narrow right nudges +4");
	expect(hud::weapon_name_x_nudge(false, 0) == 0, "wide surfaces never nudge");
	expect(hud::weapon_name_x_nudge(true, 2) == 0, "center never nudges");

	// The round-icon count.
	expect(hud::round_icon_count(17, 60, 30, 1) == 17, "clip count, no divisor");
	expect(hud::round_icon_count(-1, 7, 1, 1) == 7, "capacity-1 counts reserve");
	expect(hud::round_icon_count(9, 0, 30, 2) == 5, "divisor ceil-divides");
	expect(hud::round_icon_count(200, 0, 30, 1) == hud::kMaxRoundIcons,
			"capped at 40");

	// The crosshair spread projection + sum + row + gate.
	expect(hud::crosshair_spread_px_fp16(2 << 16, 80.9, 800.0) == 20.0,
			"(spread * screen / int(fov)) >> 16, fov truncates");
	expect(hud::crosshair_spread_px_fp16(1 << 16, 0.0, 800.0) == 0.0,
			"non-positive fov yields zero");
	expect(hud::crosshair_total_spread_fp16(1000, 256, 128) == 1000 + 2 + 1,
			"the two recoil terms ride arithmetic >>7");
	expect(hud::crosshair_total_spread_fp16(0, -256, 0) == -2,
			"SAR keeps negative recoil negative");
	expect(hud::crosshair_error_row(0, false) == 0, "prone row");
	expect(hud::crosshair_error_row(2, true) == 5, "scoped stand row +3");
	expect(hud::crosshair_error_row(7, false) == 2, "stance clamps to 2");
	expect(hud::crosshair_should_draw(false, false), "unaimed draws");
	expect(!hud::crosshair_should_draw(true, false), "aimed hides");
	expect(hud::crosshair_should_draw(true, true), "the gunner leg keeps it");

	// The capacity-1 reserve fold [orig: HUD_BuildEntityInfo @0x4b85ef].
	expect(hud::folded_reserve(1, 5, 1) == 6, "capacity-1 folds the chamber");
	expect(hud::folded_reserve(3, 9, 30) == 9, "magazines never fold");
	expect(hud::folded_reserve(-1, 5, 1) == 5, "clip sentinel never folds");
	expect(hud::folded_reserve(1, -1, 1) == -1, "reserve sentinel never folds");

	// The waypoint distance [orig: @0x5947e5..0x594836 — truncated meters].
	expect(hud::waypoint_distance_m(3.0, 4.0) == 5, "2D hypotenuse");
	expect(hud::waypoint_distance_m(10.9, 0.0) == 10, "truncates, never rounds");
	expect(hud::waypoint_distance_m(0.0, 0.0) == 0, "zero at the marker");

	// The heat bar [orig: HUD_DrawWeaponHeatBar spans @0x5997a1..0x59981f].
	expect(hud::heat_fill_span(100, 0x8000) == 50, "half heat fills half");
	expect(hud::heat_fill_span(100, 0x20000) == 100,
			"heat clamps at the 0xFFFF gauge top");
	expect(hud::heat_fill_span(100, 0) == 0, "cold bar fills nothing");
	expect(hud::heat_bar_is_horizontal(10.0, 4.0), "wide rects fill sideways");
	expect(!hud::heat_bar_is_horizontal(4.0, 10.0), "tall rects fill upward");
	expect(hud::heat_bar_is_horizontal(4.0, 4.0), "square takes the wide leg");

	// The PowerThrow windup curve [orig: @0x5998ad; fill @0x599964].
	expect(hud::power_throw_progress_fp16(0) == 0x10000,
			"the tap window throws at full power");
	expect(hud::power_throw_progress_fp16(30) == 0x10000,
			"full through the 31st held tick");
	expect(hud::power_throw_progress_fp16(31) == 0,
			"the ramp restarts at zero after the tap window");
	expect(hud::power_throw_progress_fp16(31 + 46) == 32415,
			"(held - 31) / 93 in 16.16");
	expect(hud::power_throw_progress_fp16(31 + 93) == 0x10000, "ramp clamps at 1");
	expect(hud::power_throw_progress_fp16(500) == 0x10000, "held past full stays full");
	expect(hud::power_fill_span(0x10000, 80) == 80, "full fill spans the bar");
	expect(hud::power_fill_span(0x8000, 80) == 40, "half fill rounds the span");

	// The loading-bar geometry [orig: @ 0x5d4c40 fill].
	{
		const hud::LoadingBarSpan full = hud::loading_bar_fill_span(10, 100, 100);
		expect(full.left == 13 && full.right == 113,
				"full fill clamps to the track minus the 1px inset");
		const hud::LoadingBarSpan empty = hud::loading_bar_fill_span(10, 100, 0);
		expect(empty.right <= empty.left, "0%% draws an empty fill");
	}

	// Friendly tags (D-HUD-20) [orig: HUD_DrawEntityLabel @0x5a39b0].
	expect(hud::friendly_tag_alpha(50 << 16) == 255, "no fade at 50 m");
	expect(hud::friendly_tag_alpha(10 << 16) == 255, "fade clamps below 50 m");
	expect(hud::friendly_tag_alpha(175 << 16) == 159,
			"192*(175-50)/250 = 96 off the midpoint");
	expect(hud::friendly_tag_alpha(300 << 16) == 63, "fade floor at 300 m");
	expect(hud::friendly_tag_alpha(1000 << 16) == 63, "fade clamps past 300 m");
	expect(hud::friendly_tag_text_visible(hud::kFriendlyTagModeFull, 500 << 16),
			"FULL always draws the text form");
	expect(hud::friendly_tag_text_visible(hud::kFriendlyTagModeFarBrief,
				   (300 << 16) - 1),
			"FARBRIEF draws text under 300 m");
	expect(!hud::friendly_tag_text_visible(hud::kFriendlyTagModeFarBrief,
				   300 << 16),
			"FARBRIEF cuts the text at 300 m");
	expect(!hud::friendly_tag_text_visible(hud::kFriendlyTagModeBrief, 100 << 16),
			"BRIEF never draws the text form");
	expect(!hud::friendly_tag_text_visible(hud::kFriendlyTagModeOff, 100 << 16),
			"OFF never draws the text form");
	expect(hud::friendly_tag_fallback_name(24) == "^SGT  Brown",
			"the '^' + table entry (the capture's SGT Brown at 24)");
	expect(hud::friendly_tag_fallback_name(36) == "^PFC  Mitchell",
			"the table wraps modulo 36");
	expect(hud::friendly_tag_fallback_name(4120) == "^CPL  Draper",
			"pool-encoded ids index mod 36 (4120 %% 36 = 16)");
	// The speaking blend: each channel saturates at c/2 + level/4, alpha kept.
	expect(hud::friendly_tag_speaking_blend(0xFF00FF00u, 0) == 0xFF007F00u,
			"level 0 halves each channel");
	expect(hud::friendly_tag_speaking_blend(0xFF00FF00u, 255) == 0xFF3FBF3Fu,
			"level 255 lifts every channel by ~level/4");
	expect(hud::half_bright_keep_alpha(0xC005FA0Du) == 0xC0027D06u,
			"the text fold halves RGB and keeps the fade alpha");

	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_math: all checks passed\n");
	return 0;
}
