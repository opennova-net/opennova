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

namespace opennova {
struct Title; // base/title/title.h
} // namespace opennova

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

// The progress bar's layered rects in surface pixels (left, top, width,
// height) on a `surface_w` x `surface_h` surface at `displayed` percent: the
// bar's virtual placement and extent each scaled in integers with retail's
// round-half-up (hud_math.h design_to_screen_x/y), then the black outer
// (w + 6, h + 6), the gray (inset 1), the black track (inset 2) and the fill
// (left x + 3, top y + 3, height h, its right edge loading_bar_fill_span's; a
// width <= 0 draws no fill) [orig: LoadingScreen_UpdateAndPresent @
// 0x586c78..0x586cac (the placement and the extent through
// Viewport_ScaleToVirtualCoords @ 0x5d2b20);
// HUD_DrawProgressBar_0 @ 0x5d4c40 -- the clears @ 0x5d4cb2 / @ 0x5d4ce2 /
// @ 0x5d4d0f, the fill's right edge @ 0x5d4d57..0x5d4d64, its inset clear
// @ 0x5d4dbc..0x5d4e33].
struct LoadingBarRect {
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
};
struct LoadingBarRects {
	LoadingBarRect outer;
	LoadingBarRect gray;
	LoadingBarRect track;
	LoadingBarRect fill;
};
LoadingBarRects loading_bar_rects(int surface_w, int surface_h, int displayed);

// Bar colors: the layered border draws black/gray/black [orig: 0, 0xC0C0C0,
// 0 @ 0x5d4c40]; the fill is the red override [orig: 0xFFEB0000 @ 0x586cd0].
inline constexpr uint32_t kLoadingBarBorderGray = 0xC0C0C0u;
inline constexpr uint32_t kLoadingBarFillArgb = 0xFFEB0000u;

// Redraw throttle: GetTickCount() - last >= 100, the difference read signed
// [orig: LoadingScreen_UpdateAndPresent `sub eax, [esi+1Ch]; cmp eax, 64h; jge`
//  @0x586C10..0x586C1A; the gate @ 0x586c24].
inline constexpr int kLoadingPresentIntervalMs = 100;

// Redraw + present when the window-pump interval elapses or a real checkpoint
// changes. Retail stores the caller's checkpoint in this[8] @ 0x586c32 and a
// separate liveness value in this[9] @ 0x586c2f, advanced at @ 0x586c3f.
// OpenNova intentionally omits that autonomous displayed-value creep so the
// bar never claims work the pipeline has not reached.
inline bool loading_present_due(int elapsed_ms, bool reported_changed) {
	return elapsed_ms >= kLoadingPresentIntervalMs || reported_changed;
}

// What a present draws: the screen and its bar alone. It clears, draws the screen and the
// bar, and presents; no world pass, no particle pass and no other render target runs in it
// (the shell's present holds every offscreen target for its forced draw, D-NET-1270).
// The join's waits draw no
// more: the sync-tail wait's pass and the world stream's 0x0D / 0x10 records present this
// screen at its stored checkpoint, the game-start wait's pass draws nothing. So no particle a
// load's warm drew is drawn again until the mission's own frames: the device side retires a
// reset effect scene's submissions (D-NET-1077).
// [orig: LoadingScreen_UpdateAndPresent @0x586be0 - Clear @0x586c5a, the screen @0x586c64,
//  the bar @0x586cd0, Present @0x586d53; LoadingScreen_PresentStoredProgress @0x586d70 (slot
//  15 of the screen's vtable with this[8], the checkpoint the present stores @0x586c32);
//  NapiClient_SendPlayerSyncAndWaitForSyncTail @0x42cb20 - the call @0x42cb79;
//  NapiNPClientMsg_0x00D @0x432c40 - the call @0x433236; NapiNPClientMsg_0x010 @0x433400 -
//  the call @0x43342b; NapiClient_WaitForGameStart @0x42cc10 - the pass @0x42cc67..0x42ccbf]
//
// The loaders inside a mission start present the screen at its stored checkpoint as they go,
// each call gated by the 100 ms rule above, so a long loader keeps the screen live: the effect
// system's load past each loose `ptl\*.ptl` it parses, past each loose gore-set file, and once
// past the archive walk; its atlas build at its start, past the size probe of every frame and
// at the top of each frame's place-and-decode (the shell's effect world presents past each
// loose file it parses and once past its walk, and its atlas device at its start, past each
// frame it decodes into the build and past its walk, GameWorld::present_stored_load_progress).
// [orig: CEffectSystem_Init @0x5f6070 - LoadingScreen_PresentStoredProgress @0x586d70 called
//  @0x5f630b (the loose .ptl leg), @0x5f6451 (the loose gore leg), @0x5f6582 (past the
//  archive walk); CParticleManager_BuildTextureAtlases @0x5e8db0 - the calls @0x5e8dfb (its
//  start), @0x5e8e0c (past the size probes, CParticleManager_ProbeAllTextureSizes called
//  @0x5e8e02) and @0x5e8ef5 (each
//  frame's iteration, ahead of its CTextureData_LoadTGA call @0x5e9087);
//  CEffectSystem_BuildTextureAtlases @0x5f65b0 <- Game_StartMission @0x524360 (the call
//  @0x5249b9). The terrain's loaders present the same way between their steps
//  (Terrain_Init @0x60fbe0 - @0x60fcd2 / @0x60fd23 / @0x60fd3a; PolyTrn_LoadTerrainConfig
//  @0x60e3d0 - @0x60e5d9 / @0x60e648 / @0x60e663 / @0x60e677 / @0x60e6b8) and the model
//  reload between its pools (Game_ReloadEntityModelsAndCallbacks @0x522830 - @0x522943 /
//  @0x522956 / @0x522abb / @0x522c01); the shell presents there at its stage steps and per
//  model (mission/mission_load_plan.h).]

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
// font (the title's large slot, hud_large_label_font: Impac22b.fnt at the
// (screen_w << 16) / 800 slot scale in Jointops.exe), through the
// half-bright text fold (hud_math.h half_bright_argb), color alternating on
// GetTickCount() bit 0x200 — a 512 ms two-color pulse, white / light red
// [orig: fetch @ 0x520975; HUD_DrawTextAtVirtualPos(ctx, 512, 730, 0, text,
//  g_HUDLabelFontLarge, color, mode=2 centered) @ 0x5209da; blink select
//  @ 0x5209b0-0x5209be; font slot Impac22b.fnt @ HUD_InitAllFonts 0x51ef4e;
//  centered dispatch HUD_DrawTextCentered_HalfBright @ 0x580680].
inline constexpr const char *kSplashContinueTextKey = "LT_Continue";
inline constexpr int kSplashContinueX = 512;
inline constexpr int kSplashContinueY = 730;
inline constexpr uint32_t kSplashContinueColorOn = 0xFFFFFFFFu;
inline constexpr uint32_t kSplashContinueColorOff = 0xFFFF8080u;
inline constexpr int kSplashBlinkMaskMs = 0x200;

// The continue line laid out as the splash draws it over a `surface_w` x `surface_h` display, `text` in
// the game's code page: the design point through HUD_DrawTextAtVirtualPos's integer rounding, ((512 * W
// + 512) / 1024, (730 * H + 384) / 768) [orig: HUD_DrawTextAtVirtualPos @ 0x5d3ec0], its mode 2 the
// centred half-bright drawer, which halves the phase's colour, its alpha forced opaque, and lays the run
// out centred at the title's large slot's scale (W / 800 in Jointops.exe)
// [orig: HUD_DrawTextAligned_HalfBrightNoFlags @ 0x5d2ece ->
// HUD_DrawTextCentered_HalfBright @ 0x580680 -> CGameFont_DrawText @ 0x6752c0, flags 1 @ 0x58069d;
// the slot @ HUD_InitAllFonts 0x51ef62 (hud_label_font_choice)]. The quads carry the halved colour, the
// raw diffuse the font page's material doubles on the device (kFontPageMaterialWord, MODULATE2X), so
// the line reads at the phase's colour, less the fold's low bit: white at 254, the red phase at (254,
// 128, 128) (docs/interface/loading-screen-re.md D-LOADSCR-10). Not the session block's text: the
// loading screen blends that into the background's pixels on the CPU, texel times colour over the
// pixel by the texel's alpha, no material [orig: Render_LoadingScreen, its two fonts aimed at the
// background's pixels @ 0x521f1a..0x521f42 / 0x521f86..0x521fa8 -> Render_DrawWrappedTextBlockEx @
// 0x580eb0 -> CGameFont_RenderText @ 0x675e40 -> CGameFont_DrawBlendedGlyph @ 0x6744a0].
GameFontRun splash_continue_run(const GameFont &font, const char *text, int surface_w, int surface_h,
		bool phase_on, const Title &title);

// The continue line's phase at a GetTickCount() reading: on (white) while bit 0x200 is set
// [orig: Game_ShowStartMissionSplash - the blink select @ 0x5209b0-0x5209be].
inline bool splash_blink_on(uint32_t tick_ms) {
	return (tick_ms & static_cast<uint32_t>(kSplashBlinkMaskMs)) != 0;
}

// The arrow quad's size over a `surface_w` x `surface_h` display: the art's sides times the
// display over 800 x 600, each an integer division [orig: Game_ShowStartMissionSplash - the
// width (art_w * screen_w) / 800 @ 0x520876..0x52089d, the height (art_h * screen_h) / 600
// @ 0x520889..0x5208ae, the signed magic divides]. Its top-left is the live cursor [orig:
// xLeft / yTop @ 0x3342e48 / 0x3342e4c, the cursor position Input_DispatchMouseEvent
// @ 0x761470 writes, scaled to the display @ 0x520920-0x520942].
struct SplashArrowSize {
	int width = 0;
	int height = 0;
};
inline SplashArrowSize splash_arrow_size(int art_w, int art_h, int surface_w, int surface_h) {
	return {art_w * surface_w / kSplashArrowScaleBaseW, art_h * surface_h / kSplashArrowScaleBaseH};
}

// The splash's input loop as states: raised over the held background (the key queue flushed at
// entry, so a press made during the blocking load never dismisses it), it takes input until a
// queued key, autorepeat included, or a mouse button exits, the live button mask read each pass
// so a held button exits too; it then presents one background-only frame, no text and no
// arrow, before the caller releases the loading screen's effect. Motion and key releases never
// exit, and the START_MISSION sound never gates it [orig: Game_ShowStartMissionSplash
// @ 0x520820 - Input_ResetKeyQueue @ 0x52085b, the exit test reading the button mask (the
// WindowProc's button bits, g_MouseState.rawWParam @ 0x3342e50) and the key queue @ 0x520a2d,
// Input_DequeueKeyEvent @ 0x520a36, the loop's draw @ 0x520993-0x520a28,
// the closing present @ 0x520a48-0x520a79; the caller's release @ 0x525d45].
class StartMissionSplash {
public:
	enum class State : uint8_t { kNone, kActive, kClosing };

	State state() const { return state_; }
	// Raised; false when one is up already.
	bool raise() {
		if (state_ != State::kNone) return false;
		state_ = State::kActive;
		closing_frames_ = 0;
		return true;
	}
	// A key press (an autorepeat one too) or a button press: the loop's exit, its closing frame
	// owed where frames present (`presents` false: no renderer to wait on, the release on the
	// next frame). False when the splash takes no input.
	bool exit(bool presents) {
		if (state_ != State::kActive) return false;
		state_ = State::kClosing;
		closing_frames_ = presents ? 1 : 0;
		return true;
	}
	// One display frame, `mouse_mask` the live button mask: an active splash exits on a held
	// button; a closing one spends its background-only frame, then releases. True on the frame
	// it releases (the owner's dismissal).
	bool frame(uint32_t mouse_mask, bool presents) {
		if (state_ == State::kActive) {
			if (mouse_mask != 0u) exit(presents);
			return false;
		}
		if (state_ != State::kClosing) return false;
		if (closing_frames_ > 0) {
			--closing_frames_;
			return false;
		}
		state_ = State::kNone;
		return true;
	}
	// The frame draws the continue line and the arrow (the loop's frames), else the background
	// alone (the closing present).
	bool draws_overlay() const { return state_ == State::kActive; }

private:
	State state_ = State::kNone;
	int closing_frames_ = 0;
};

// --- the wrapped text block (loading_screen.cpp) --------------------------------
// The original composites its text blocks with Render_DrawWrappedTextBlockEx
// @ 0x580eb0: a line breaker and a line placer over one width measure, the
// font's (CGameFont_GetTextExtent @ 0x675290; loading_draw_text_block below),
// so the rules take it as a callback: the pixel extent of a code-page string
// in the font the block draws in.
using TextExtent = std::function<float(const std::string &text)>;

// The three bytes the breaker treats structurally: a space is the wrap point it
// remembers, a carriage return is the hard break, and a line feed is only
// swallowed when it trails a break [orig: the 32 / 13 / 10 tests @0x580f88,
// @0x580fdf and @0x581128].
inline constexpr unsigned char kTextSpace = 32;
inline constexpr unsigned char kTextCarriageReturn = 13;
inline constexpr unsigned char kTextLineFeed = 10;

// The line breaker: the wrapped lines of `text` measured against `max_width`,
// byte by byte, break characters consumed. Empty for no measure, empty text or
// a zero box.
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
// space; a line that measures empty advances HALF a line, and is still placed
// (at `left`) when it holds bytes, which the drawer walks for their tags; drawing
// stops once the next line would pass `bottom` unless `top == bottom`, which
// disables the vertical clip.
TextBlock layout_text_block(const TextExtent &extent, int line_height, const std::string &text,
		int left, int top, int right, int bottom, TextBlockAlign align, int skip_lines = 0);

// One of the session block's text boxes, in the background's image space (the text composites
// into the image before it stretches): the wrapped block's rect and alignment, and its colour.
struct LoadingTextBox {
	int x = 0;
	int y = 0;
	int width = 0;
	int bottom = 0;
	TextBlockAlign align = TextBlockAlign::kLeft;
	uint32_t argb = 0xFFFFFFFFu;
};

// The session block over an `image_w` x `image_h` background whose band ends at `band_right`
// (loading_band_right): the title, the mission and the game type share the band, left-, centre-
// and right-aligned, all white [orig: Render_LoadingScreen @ 0x521fe1-0x522123 - the alignments
// 3 / 4 / 5 @ 0x52200f / 0x52202e / 0x522050, folded @ 0x58106c / 0x581071; <cFFFFFF>
// @ 0x51f42d]; the server message's label (its key's text and a colon, kLoadingMsgLabelRgb) and
// body (white) at the image fractions, each box to the image's bottom [orig: the label
// "<c80E0FF>%s:\r\n<cFFFFFF>" @ 0x7D01A8; the fractions' doubles @ 0x7D01A0 / 0x7D0198 /
// 0x7D0190 / 0x7C4878]. The message draws only over a non-empty server text [orig: the
// CUSTOMTEXT gate @ 0x52205f].
struct LoadingSessionBoxes {
	LoadingTextBox title;
	LoadingTextBox mission;
	LoadingTextBox game_type;
	LoadingTextBox message_label;
	LoadingTextBox message_body;
};
LoadingSessionBoxes loading_session_boxes(int image_w, int image_h, int band_right);
// The message label's text: the key's text and the format's colon.
inline std::string loading_message_label_text(const std::string &label) { return label + ":"; }

// --- the session block's composite (loading_screen.cpp) -------------------------
// Render_LoadingScreen draws the session block INTO the background's surface before the
// stretch, each glyph blended on the CPU (D-LOADSCR-2, FIXED 2026-10-27) [orig:
// Render_LoadingScreen @ 0x521f1a / @ 0x521f86 -> Render_DrawWrappedTextBlockEx @ 0x580eb0 ->
// CGameFont_RenderText @ 0x675e40 -> CGameFont_DrawBlendedGlyph @ 0x6744a0].

// The background the block composites into: RGBA8 texels, row-major, `width` x `height` (the
// image's own size).
struct LoadingSurface {
	uint8_t *rgba = nullptr;
	int width = 0;
	int height = 0;
};

// The fonts' tab-stop widths (font+0x168): 120 for the small font, 0 (a tab draws as a space)
// for the large [orig: CGameFont_SetTabWidth @ 0x6741c0 - push 78h @ 0x521f32, the call
// @ 0x521f42 (Arials18.fnt); @ 0x521fa4 / @ 0x521fae (Arial22.fnt)].
inline constexpr int kLoadingFontSmallTabWidth = 120;
inline constexpr int kLoadingFontLargeTabWidth = 0;

// One glyph blended into the surface with its top-left at (x, y): the glyph's texel rect, its
// width the u-extent times 256 and its rows the v-extent times 256 plus one (the drawer's row
// loop runs one past the extent), each truncated, from the page's texel at (u0, v0) times 256;
// each texel's alpha A weighs the colour times the texel against the surface, per channel
// ((256 - A) * d * 256 + A * c * t) >> 16 for red and green and ((256 - A) * d + A * ((c * t)
// >> 8)) >> 8 for blue. The surface's alpha byte stays (the game's blend writes 0 there, which
// the stretch never reads). Texels off the page and pixels off the surface are skipped.
// [orig: CGameFont_DrawBlendedGlyph @ 0x6744a0 - the width @0x6744c6, the height
//  @0x6744d6, the source @0x674526..0x67454e, the rows + 1 @0x67455c, the blend
//  @0x674588..0x674613]
void loading_blend_glyph(LoadingSurface &surface, const opennova::fnt::fnt_font_t &font,
		const opennova::fnt::fnt_glyph_t &glyph, int x, int y, uint32_t color);

// CGameFont_RenderText: `text` drawn into the surface from the anchor (x, y), one pass per
// texture page over each line ('\n' ends one, '\r' is skipped), each line centred on x (`flags`
// 1) or ending at it (2) by the font's measure; each glyph at the walk's cursor truncated, the
// cursor stepping floor(cursor + the u-extent times 256 + (spacing - 1) + 0.5); '\\' escapes the
// next byte, '<' tags through the shared parser (unless the state disables them), a tab steps
// to the next stop from the anchor (a space with no stop width), control bytes and 0x7F..0x81
// draw nothing; bold draws again one right, underline writes the colour across the glyph's
// width on the row under it; lines step by the space glyph's v-extent times 256. `state`
// carries the styles, the colour and the tab width in and out, the last pass's.
// [orig: CGameFont_RenderText @ 0x675e40 - the page loop @0x675f14..0x67604b, the alignment
//  @0x675f86..0x675ff1, the walk @0x6760be..0x676273, the tab @0x6760dc..0x676145, the
//  advance @0x67618b..0x6761b2, the draws @0x6761ce..0x676254, the line step
//  @0x67600f..0x676024, the state out @0x676051..0x67609c; CGameFont_ReadLine @ 0x676480]
void loading_render_text(LoadingSurface &surface, const opennova::fnt::fnt_font_t &font,
		int tab_width, const std::string &text, float x, float y, uint32_t flags,
		GameFontState &state);

// Render_DrawWrappedTextBlockEx over `box` into the surface: the lines broken and placed by
// layout_text_block over the font's measure (with its tab width) and the 'I' extent's height as
// the pitch, each drawn by loading_render_text at the box's left, its centre (flags 1) or its
// right (2), in one state seeded with the box's colour and the tab width.
// [orig: Render_DrawWrappedTextBlockEx @ 0x580eb0 - the 'I' extent @0x580f2b, the state
//  @0x580f4b..0x580f6b, the measure @0x580fa9, the anchors @0x5810ab / @0x581094, the draws
//  @0x581059 / @0x5810ce]
void loading_draw_text_block(LoadingSurface &surface, const opennova::fnt::fnt_font_t &font,
		int tab_width, const std::string &text, const LoadingTextBox &box);

// The session block composited into the surface (loading_session_boxes over its size): the title,
// the mission and the game type in the large font; the server message's label and body in the
// small one where the message is not empty. Code-page text; a null font draws nothing.
// [orig: Render_LoadingScreen @ 0x521fe1-0x522123]
void composite_loading_session_text(LoadingSurface &surface,
		const opennova::fnt::fnt_font_t *large, const opennova::fnt::fnt_font_t *small,
		const std::string &title, const std::string &mission, const std::string &game_type,
		const std::string &label, const std::string &custom_text, int band_right);

} // namespace opennova::hud
