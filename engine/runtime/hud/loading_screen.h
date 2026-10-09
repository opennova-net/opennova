#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <runtime/hud/game_font.h>

// The mission loading screen's witnessed spec: the GAMETYPE -> LoadingText
// key policy and the layout/appearance block the shell's LoadingScreen draws
// from (the ENG-4/FNT pattern: policy + constants native, the CanvasItem
// draws stay shell-side). The fill arithmetic already lives in hud_math.h
// (loading_bar_fill_span).
// [orig: background + text compositing Render_LoadingScreen @ 0x521d10;
//  session text provider HUD_GetLoadingScreenTextByGameType @ 0x51f300;
//  throttle + retail creep + bar draw LoadingScreen_UpdateAndPresent @ 0x586be0;
//  bar primitive HUD_DrawProgressBar_0 @ 0x5d4c40]
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

// Redraw + present when the window-pump interval elapses or a real checkpoint
// changes. Retail stores the caller's checkpoint in this[8] @ 0x586c32 and a
// separate liveness value in this[9] @ 0x586c2f, advanced at @ 0x586c3f.
// OpenNova intentionally omits that autonomous displayed-value creep so the
// bar never claims work the pipeline has not reached.
inline bool loading_present_due(int elapsed_ms, bool reported_changed) {
	return elapsed_ms >= kLoadingPresentIntervalMs || reported_changed;
}

// The composited resources: the stock background when the mission has no
// sidecar image [orig: "loadscrn.pcx" @ 0x521e20], the two text fonts
// [orig: "Arials18.fnt" @ 0x521eec, "Arial22.fnt" @ 0x521f5a], and the
// server-message label's literal fallback when the gametext table misses
// [orig: GameText_GetStringWithFallback("LoadingText", "LT_SERVERMSG", ...)
// @ 0x522074].
inline constexpr const char *kLoadingFallbackImage = "loadscrn.pcx";
inline constexpr const char *kLoadingFontSmall = "Arials18.fnt";
inline constexpr const char *kLoadingFontLarge = "Arial22.fnt";
inline constexpr const char *kLoadingServerMessageLabelKey = "LT_SERVERMSG";
inline constexpr const char *kLoadingServerMessageLabelFallback =
		"Message from Game Server";

// <mission>.bms -> <mission>.pcx: the sidecar image name for a mission file
// — the file part cut at its FIRST '.', then "pcx" appended: the extension is
// removed from the last '.', and the replace that follows swaps from the
// first one left ("op.v2.bms" -> "op.v2" -> "op.pcx") [orig:
// Path_RemoveExtension @ 0x521d66 + Path_ReplaceOrAppendExtension(path, "pcx")
// @ 0x521dab, its first-dot scan @ 0x53c7c4; resolution is case-insensitive
// through the VFS].
inline std::string loading_sidecar_image_name(const std::string &mission_file) {
	const size_t slash = mission_file.find_last_of("/\\");
	std::string base = slash == std::string::npos ? mission_file
													: mission_file.substr(slash + 1);
	const size_t dot = base.find('.');
	if (dot != std::string::npos) base.erase(dot);
	return base + ".pcx";
}

// The SP start-mission splash at the end of a single-player load with a
// custom (sidecar) background: the held loading-screen background stays up,
// a blinking "press any key" line replaces the finished bar, and the mouse
// cursor arrow is drawn by the splash itself while any fresh key event or a
// mouse button dismisses it (the key queue is flushed at entry so presses
// made during the blocking load do not skip it; the START_MISSION sound is
// fire-and-forget and never dismisses)
// [orig: Game_ShowStartMissionSplash @ 0x520820 — queue flush
//  Input_ResetKeyQueue @ 0x760e00, exit test input_mask/@ 0x520a2d +
//  Input_DequeueKeyEvent @ 0x520a36; caller gate @ 0x525d38].

// The arrow quad is the menu cursor art, top-left at the live cursor
// position, sized tga_dims * (backbuffer / 800x600)
// [orig: "newarow1.tga" @ 0x520871; quad size @ 0x52089d/0x5208ae].
inline constexpr const char *kSplashArrowImage = "newarow1.tga";
inline constexpr int kSplashArrowScaleBaseW = 800;
inline constexpr int kSplashArrowScaleBaseH = 600;

// The one-shot sound set [orig: SoundBank_FindSetByNameAnyBank("START_MISSION")
// @ 0x5208f7].
inline constexpr const char *kSplashSoundSet = "START_MISSION";

// The continue line: gametext LoadingText/LT_Continue, drawn CENTERED at
// virtual (512, 730) of the 1024x768 overlay space in the large HUD label
// font (Impac22b.fnt at the (screen_w << 16) / 800 slot scale), through the
// half-bright text fold (hud_math.h half_bright_argb), color alternating on
// GetTickCount() bit 0x200 — a 512 ms two-color pulse, white / light red
// [orig: fetch @ 0x520975; HUD_DrawTextAtVirtualPos(ctx, 512, 730, 0, text,
//  g_HUDLabelFontLarge, color, mode=2 centered) @ 0x5209da; blink select
//  @ 0x5209b0-0x5209be; font slot Impac22b.fnt @ HUD_InitAllFonts 0x51ef4e;
//  centered dispatch HUD_DrawTextCentered_HalfBright @ 0x580680].
inline constexpr const char *kSplashContinueTextKey = "LT_Continue";
inline constexpr const char *kSplashContinueFont = "Impac22b.fnt";
inline constexpr int kSplashContinueFontScaleBaseW = 800;
inline constexpr int kSplashContinueX = 512;
inline constexpr int kSplashContinueY = 730;
inline constexpr uint32_t kSplashContinueColorOn = 0xFFFFFFFFu;
inline constexpr uint32_t kSplashContinueColorOff = 0xFFFF8080u;
inline constexpr int kSplashBlinkMaskMs = 0x200;

// The continue line laid out as the splash draws it over a `surface_w` x `surface_h` display, `text` in
// the game's code page: the design point through HUD_DrawTextAtVirtualPos's integer rounding, ((512 * W
// + 512) / 1024, (730 * H + 384) / 768) [orig: HUD_DrawTextAtVirtualPos @ 0x5d3ec0], its mode 2 the
// centred half-bright drawer, which halves the phase's colour, its alpha forced opaque, and lays the run
// out centred at the large slot's scale, W / 800 [orig: sub_5D2EA0 @ 0x5d2ece ->
// HUD_DrawTextCentered_HalfBright @ 0x580680 -> CGameFont_DrawText @ 0x6752c0, flags 1 @ 0x58069d;
// the slot @ HUD_InitAllFonts 0x51ef62 (hud_label_font_choice)]. The quads carry the halved colour, the
// raw diffuse the font page's material doubles on the device (kFontPageMaterialWord, MODULATE2X), so
// the line reads at the phase's colour, less the fold's low bit: white at 254, the red phase at (254,
// 128, 128) (docs/interface/loading-screen-re.md D-LOADSCR-10). Not the session block's text: the
// loading screen blends that into the background's pixels on the CPU, texel times colour over the
// pixel by the texel's alpha, no material [orig: Render_LoadingScreen, its two fonts aimed at the
// background's pixels @ 0x521f1a..0x521f42 / 0x521f86..0x521fa8 -> render_draw_wrapped_text_block_ex @
// 0x580eb0 -> CGameFont_RenderText @ 0x675e40 -> CGameFont_DrawBlendedGlyph @ 0x6744a0].
GameFontRun splash_continue_run(const GameFont &font, const char *text, int surface_w, int surface_h,
		bool phase_on);

// --- the wrapped text block (loading_screen.cpp) --------------------------------
// The original composites its text blocks with Render_DrawWrappedTextBlockEx
// @ 0x580eb0: a line breaker and a line placer over one width measure. The
// measure is the embedder's (today the FontFile view of the .fnt; D-LOADSCR-2
// carries the CGameFont metric residual), so the rules take it as a callback:
// the pixel extent of a UTF-8 string in the font the block draws in.
using TextExtent = std::function<float(const std::string &text)>;

// The three bytes the breaker treats structurally: a space is the wrap point it
// remembers, a carriage return is the hard break, and a line feed is only
// swallowed when it trails a break [orig: the 32 / 13 / 10 tests @0x580f88,
// @0x580fdf and @0x581128].
inline constexpr unsigned char kTextSpace = 32;
inline constexpr unsigned char kTextCarriageReturn = 13;
inline constexpr unsigned char kTextLineFeed = 10;

// The line breaker: the wrapped lines of `text` measured against `max_width`,
// break characters consumed. Empty for no measure, empty text or a zero box.
std::vector<std::string> wrap_text_lines(const TextExtent &extent, const std::string &text,
		int max_width);

// The alignment fold: retail's 4 = centred on left + width/2, 5 = right-aligned
// on rect_right, anything else left [orig: @0x5810ab / @0x581094 / @0x58107f].
enum class TextBlockAlign : uint8_t { kLeft = 0, kCenter = 1, kRight = 2 };

// One laid-out line: the slice to paint and the top-left the alignment resolved
// it to.
struct TextBlockLine {
	std::string text;
	float x = 0.0f;
	float y = 0.0f;
};

// The laid-out block: every line the drawer will paint, already placed, plus
// `stopped_at` -- 0 when the whole string was consumed, otherwise the 1-based
// count of lines processed when the box ran out of vertical room.
struct TextBlock {
	std::vector<TextBlockLine> lines;
	int stopped_at = 0;
};

// The line placer over the breaker. `line_height` is the font's line pitch
// (retail: the 'I' character's own extent @0x580f2b); lines before
// `skip_lines` are consumed without being placed and without costing vertical
// space; a line that measures empty advances HALF a line; drawing stops once
// the next line would pass `bottom` unless `top == bottom`, which disables the
// vertical clip.
TextBlock layout_text_block(const TextExtent &extent, int line_height, const std::string &text,
		int left, int top, int right, int bottom, TextBlockAlign align, int skip_lines = 0);

} // namespace opennova::hud
