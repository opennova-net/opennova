// The HUD view-helper math cluster (engine/runtime/hud): the hudpos.def
// design-space scaling [orig: Viewport_ScaleToVirtualCoords @0x5d2b20], the
// ALPHAFADE decay with its elapsed-0 wrap quirk [orig: @0x599af9/@0x599fc0],
// the stance Q16 scale + 128-box centering [orig: @0x599f10], the health color
// bands [orig: @0x5a2e50], the message tick policy [orig: @0x51f216/@0x49894e],
// half-bright [orig: @0x580850], the ammo text fold + name nudge
// [orig: @0x5939d0], the round-icon count [orig: @0x599b9c], and the crosshair
// spread projection/sum/row/gate [orig: @0x592640].

#include <runtime/hud/hud_math.h>

#include <base/title/title.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

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

	// The displayed clip [orig: HUD_BuildEntityInfo @0x4b8573..0x4b85fa].
	expect(hud::displayed_clip(7, 30) == 7, "a magazine weapon shows its clip");
	expect(hud::displayed_clip(7, -1) == -1, "an infinite weapon reads clip -1");
	expect(hud::displayed_clip(0, 1) == 0, "an empty single-shot shows 0");

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
	// The level is the meter's first stage, 0..0x3FF: its top saturates every lane
	// (`psllw mm1, 6` of 0x3FF is 0xFFC0) [orig: @0x5a3e9d..0x5a3eae].
	expect(hud::friendly_tag_speaking_blend(0x8000FF00u, 0x3FF) == 0x80FFFFFFu,
			"the meter's top level saturates every channel");
	expect(hud::friendly_tag_speaking_blend(0xFF000000u, 0x200) == 0xFF808080u,
			"level 0x200 lifts a black channel to 0x80");
	expect(hud::half_bright_keep_alpha(0xC005FA0Du) == 0xC0027D06u,
			"the text fold halves RGB and keeps the fade alpha");

	// The crosshair spread's pixels, (err_16.16 * screen_w / int(fov_deg)) >> 16, and its sum,
	// ERROR + (entity+0x380 >> 7) + (entity+0x384 >> 7) with arithmetic shifts and the signed
	// 32-bit wrap [orig: HUD_DrawCrosshair @0x592b07..0x592bf5].
	expect(hud::crosshair_spread_px_fp16(0x4000, 80.0, 1024.0) == 3.0,
			"0.25 deg over an 80-deg fov on a 1024-wide screen is 3 px");
	expect(hud::crosshair_spread_px_fp16(0, 80.0, 1024.0) == 0.0, "no error, no spread");
	expect(hud::crosshair_spread_px_fp16(0x10000, 0.0, 1024.0) == 0.0,
			"a fov whose integer part is 0 spreads nothing");
	expect(hud::crosshair_total_spread_fp16(0x4000, 0x80000, 0x40000) == 0x5800,
			"the two carriers shift by 7 before the sum");
	expect(hud::crosshair_total_spread_fp16(0, -1, -129) == -3,
			"negative carriers shift arithmetically, not by truncating division");
	expect(hud::crosshair_total_spread_fp16(0x7FFFFFFF, 128, 0) == INT32_MIN,
			"the sum keeps the signed 32-bit wrap");
	// The health bands over the 16.16 ratio: 0 above 0xC000, 1 down to 0x7000, 2 under it
	// [orig: @0x5a2e50].
	expect(hud::health_color_band_fp16(0xC001) == 0 && hud::health_color_band_fp16(0xC000) == 1 &&
					hud::health_color_band_fp16(0x7000) == 1 &&
					hud::health_color_band_fp16(0x6FFF) == 2,
			"the three health bands");
	// The stance centering from frame 0's scaled dims: none at 127 and up, else (128 - dim) / 2
	// [orig: HUD_DrawStanceIndicator @0x599fed..0x59a07e].
	expect(hud::stance_center_axis(hud::stance_scaled_dim(64, 0x20000)) == 0 &&
					hud::stance_center_axis(hud::stance_scaled_dim(32, 0x20000)) == 32,
			"the 64 x 32 frame centres its minor side alone");
	// ALPHAFADE's percent: 30 and 50 read 76 and 127.
	expect(static_cast<int>(30 * hud::kPercentToAlpha) == 76 &&
					static_cast<int>(50 * hud::kPercentToAlpha) == 127,
			"the percent to alpha factor, 2.55");

	// The label fonts by width, each title's table (Title::hud_fonts): Jointops.exe's three tiers
	// and its Impact pair over 800; dfx2.exe's six (DFX2-39) and its Impact pair over 1600; a title
	// with no table chooses no file
	// [orig: HUD_InitAllFonts @0x51EE20] [orig: @0x51BD70 (dfx2.exe)].
	{
		const opennova::Title &jo = *opennova::title_by_code("jo");
		const opennova::Title &dfx2 = *opennova::title_by_code("dfx2");
		const auto is = [](const char *a, const char *b) { return std::strcmp(a, b) == 0; };
		const hud::HudLabelFontChoice j640 = hud::hud_label_font_choice(640, jo);
		const hud::HudLabelFontChoice j800 = hud::hud_label_font_choice(800, jo);
		const hud::HudLabelFontChoice j1600 = hud::hud_label_font_choice(1600, jo);
		expect(is(j640.normal_fnt, "Arial14n.fnt") && is(j640.bold_fnt, "Arial12b.fnt") &&
						j640.scale == 1.0f && j640.tier == 0,
				"JO: 640 takes the first tier over 640");
		expect(is(j800.bold_fnt, "Arial14b.fnt") && j800.scale == 1.0f && j800.tier == 1,
				"JO: 800 takes the second tier over 800");
		expect(is(j1600.normal_fnt, "Arial16n.fnt") && is(j1600.bold_fnt, "Arial16b.fnt") &&
						j1600.scale == 1600.0f / 1024.0f && j1600.tier == 2,
				"JO: any wider width takes the last tier over 1024");
		expect(is(j1600.large_fnt, "Impac22b.fnt") && is(j1600.impact38_fnt, "Impac38b.fnt") &&
						j1600.large_scale == 2.0f,
				"JO: the Impact pair over 800");
		const hud::HudLabelFontChoice d1024 = hud::hud_label_font_choice(1024, dfx2);
		const hud::HudLabelFontChoice d1280 = hud::hud_label_font_choice(1280, dfx2);
		const hud::HudLabelFontChoice d1600 = hud::hud_label_font_choice(1600, dfx2);
		const hud::HudLabelFontChoice d1920 = hud::hud_label_font_choice(1920, dfx2);
		expect(is(d1024.bold_fnt, "Arial16b.fnt") && d1024.scale == 1.0f && d1024.tier == 2,
				"DFX2: 1024 keeps JO's third tier");
		expect(is(d1280.normal_fnt, "Arial20n.fnt") && is(d1280.bold_fnt, "Arial20b.fnt") &&
						d1280.scale == 1.0f && d1280.tier == 3,
				"DFX2: 1280 takes Arial20 over 1280");
		expect(is(d1600.bold_fnt, "Arial28b.fnt") && d1600.scale == 1.0f && d1600.tier == 4,
				"DFX2: 1600 takes Arial28 over 1600");
		expect(is(d1920.normal_fnt, "Arial32n.fnt") && is(d1920.bold_fnt, "Arial32b.fnt") &&
						d1920.scale == 1.0f && d1920.tier == 5,
				"DFX2: wider takes Arial32 over 1920");
		expect(is(d1600.large_fnt, "Impac44b.fnt") && is(d1600.impact38_fnt, "Impac76b.fnt") &&
						d1600.large_scale == 1.0f &&
						is(hud::hud_large_label_font(dfx2), "Impac44b.fnt"),
				"DFX2: the Impact pair over 1600");
		opennova::Title bare;
		const hud::HudLabelFontChoice none = hud::hud_label_font_choice(1024, bare);
		expect(none.tier == -1 && is(none.bold_fnt, "") && is(none.large_fnt, "") &&
						none.scale == 1.0f && is(hud::hud_large_label_font(bare), ""),
				"a title with no table chooses no file");
	}

	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_math: all checks passed\n");
	return 0;
}
