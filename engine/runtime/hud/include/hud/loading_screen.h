#pragma once

#include <cstdint>

// The mission loading screen's witnessed spec: the GAMETYPE -> LoadingText
// key policy and the layout/appearance block the shell's LoadingScreen draws
// from (the ENG-4/FNT pattern: policy + constants native, the CanvasItem
// draws stay shell-side). The smoothing/fill arithmetic already lives in
// hud_math.h (loading_bar_step / loading_bar_fill_span).
// [orig: background + text compositing render_loading_screen @ 0x521d10;
//  session text provider HUD_GetLoadingScreenTextByGameType @ 0x51f300;
//  throttle + creep + bar draw LoadingScreen_UpdateAndPresent @ 0x586be0;
//  bar primitive draw_progress_bar_0 @ 0x5d4c40]
// Witness record: docs/interface/loading-screen-re.md.

namespace opennova::hud {

// The LoadingText key for a numeric session game type, or "" for an unknown
// type (the original leaves the line empty)
// [orig: HUD_GetLoadingScreenTextByGameType @ 0x51f300, switch
// @ 0x51f30b-0x51f3a6; COOP masks bit 17 so both COOP variants match].
inline const char *loading_gametype_text_key(uint32_t game_type) {
	if (game_type == 0x00000u) {
		return "LTGT_DM";
	}
	if (game_type == 0x10000u) {
		return "LTGT_TDM";
	}
	if ((game_type & 0xFFFDFFFFu) == 0x10020u) {
		return "LTGT_COOP";
	}
	switch (game_type) {
		case 0x10001u:
			return "LTGT_TKOTH";
		case 0x00001u:
			return "LTGT_KOTH";
		case 0x90002u:
			return "LTGT_SD";
		case 0x10002u:
			return "LTGT_AD";
		case 0x10004u:
			return "LTGT_CTF";
		case 0x10008u:
			return "LTGT_FB";
		case 0x10010u:
			return "LTGT_AAS";
		case 0x50010u:
			return "LTGT_CAC";
		default:
			return "";
	}
}

// Top text band, in image space (the original composites text INTO the
// 800x600 background surface before stretching) [orig: rect (21, 29, right,
// 500) @ 0x52200f; right edge 661 with a custom background, 782 with the
// stock one @ 0x521ec4].
inline constexpr int kLoadingBandLeft = 21;
inline constexpr int kLoadingBandTop = 29;
inline constexpr int kLoadingBandRightCustom = 661;
inline constexpr int kLoadingBandRightStock = 782;
inline constexpr int kLoadingBandBottom = 500;

// Server-message block, as fractions of the image size [orig: doubles 0.02
// @ 0x7D01A0, 0.98 @ 0x7D0198, 0.87 @ 0x7D0190, 0.9 @ 0x7C4878].
inline constexpr double kLoadingMsgXFrac = 0.02;
inline constexpr double kLoadingMsgRightFrac = 0.98;
inline constexpr double kLoadingMsgLabelYFrac = 0.87;
inline constexpr double kLoadingMsgBodyYFrac = 0.9;

// The message label color, RGB (the body restores white)
// [orig: "<c80E0FF>%s:\r\n<cFFFFFF>" @ 0x7D01A8].
inline constexpr uint32_t kLoadingMsgLabelRgb = 0x80E0FFu;

// Progress bar, in the 1024x768 virtual overlay space (hud_math.h
// kDesignWidth/kDesignHeight) [orig: x=368 y=732 w=286 h=15
// @ 0x586c78-0x586c90, scaled via Viewport_ScaleToVirtualCoords @ 0x5d2b20].
inline constexpr int kLoadingBarX = 368;
inline constexpr int kLoadingBarY = 732;
inline constexpr int kLoadingBarW = 286;
inline constexpr int kLoadingBarH = 15;

// Bar colors: the layered border draws black/gray/black [orig: 0, 0xC0C0C0,
// 0 @ 0x5d4c40]; the fill is the red override [orig: 0xFFEB0000 @ 0x586cd0].
inline constexpr uint32_t kLoadingBarBorderGray = 0xC0C0C0u;
inline constexpr uint32_t kLoadingBarFillArgb = 0xFFEB0000u;

// Redraw throttle [orig: GetTickCount() - last >= 100 @ 0x586c24].
inline constexpr int kLoadingPresentIntervalMs = 100;

} // namespace opennova::hud
