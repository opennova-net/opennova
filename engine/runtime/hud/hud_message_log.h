#pragma once

#include <cstdint>

namespace opennova::hud {

// THE "RECENT MESSAGES" HISTORY WINDOW — retail's J-key panel.
//
// It is a SECOND VIEW over the same two message rings the HUD feeds already
// draw: one stdbox holding sixteen rows per column, the CHAT ring down the left
// and the SYSTEM ring down the right. Unlike the feeds it applies NO expiry
// gate — it lists the ring history, so lines that have already faded off the
// HUD are still here.
// [orig: HUD_DrawMessageLog @0x5B9D70 (IDB-renamed 2026-08-21, ex the kong
//  misnomer `draw_credits_scroll`), called from Server_DrawStatusScreen
//  @0x50A2D0 (the call @0x50B21F) when the `OldMessages` toggle
//  g_showMessageLog @0x24C18C0 is set (`xor g_showMessageLog, 1` @0x49B55A in
//  Input_HandleActionBinding, jumptable case 29; cleared by
//  Game_InitRespawnState @0x49939A; catalog action 56 "OldMessages", default
//  VK 0x4A = 'J').]
//
// GEOMETRY IS DERIVED FROM THE SURFACE WIDTH, not from design-space constants,
// which is why these are functions rather than a table. Retail computes each
// value in SCREEN pixels from the width and then pushes the box corners back
// through its own inverse transform [orig: Viewport_ScreenToVirtual @0x5D2C70].
// Note the mixed shifts and divides: the line step divides by 640 while the
// others shift by 10 (i.e. divide by 1024). That asymmetry is witnessed — the
// step does not scale with the same denominator as the columns.

inline constexpr int kMessageLogRows = 16; // [orig: the ring walk @0x5B9E8A..0x5B9F1A — entry -= 128 from unk_B405BC down to byte_B3FDBC]
// The stdbox corners in design space [orig: literals 8 and 0x3f8 @0x5b9e1e/22].
inline constexpr float kMessageLogBoxX1 = 8.0f;
inline constexpr float kMessageLogBoxX2 = 1016.0f;
// The box grows 0x20 beyond the text block at both ends
// [orig: `outY -= 0x20` @0x5B9E42 / `bottom += 0x20` @0x5B9E46].
inline constexpr float kMessageLogBoxPadY = 32.0f;
// The title rides just inside the box corner [orig: +0xf, +2 @0x51efe1/e8].
inline constexpr float kMessageLogTitleDx = 15.0f;
inline constexpr float kMessageLogTitleDy = 2.0f;

// Per-row line step, SCREEN px [orig: (width * 0xc) / 0x280 @0x5B9DAB — /640, not /1024].
inline int message_log_step_px(int surface_w) {
	return (surface_w * 12) / 640;
}
// The panel's top, SCREEN px [orig: (width * 0x78) >> 10 @0x5B9DA3..0x5B9DED].
//
// Retail adds two more terms here, both provably zero, so they are not carried:
// fixedZ @0x24C18F4 has exactly ONE writer in the image and it stores a zeroed
// register [orig: Renderer_SetDisplayModeWithFallback @0x587370 — the store
// @0x587622 with edi zeroed @0x58761A], and the chat box table's Y[1]
// (dword_28E51FC) is never authored.
inline int message_log_top_px(int surface_w) {
	return (surface_w * 120) >> 10;
}
// The first text row sits below the panel top [orig: 10 * width / 1024 @0x5B9DE1].
inline int message_log_text_top_px(int surface_w) {
	return message_log_top_px(surface_w) + ((surface_w * 10) >> 10);
}
// The CHAT column, left-aligned [orig: 32 * width / 1024 @0x5B9DBC..0x5B9DC8, + 2 @0x5B9E7E].
inline int message_log_chat_x_px(int surface_w) {
	return ((surface_w * 32) >> 10) + 2;
}
// The SYSTEM column, right-aligned [orig: (width * 0x3de) >> 10 with align flag 1 @0x5B9EC0..0x5B9F02].
inline int message_log_system_x_px(int surface_w) {
	return (surface_w * 990) >> 10;
}

// Screen px -> the 1024x768 design space: retail's own inverse, with the
// half-dimension term providing the rounding
// [orig: Viewport_ScreenToVirtual @0x5D2C70].
inline float message_log_to_design_x(int px, int surface_w) {
	if (surface_w <= 0) return static_cast<float>(px);
	return static_cast<float>(px * 1024 + surface_w / 2) /
			static_cast<float>(surface_w);
}
inline float message_log_to_design_y(int px, int surface_h) {
	if (surface_h <= 0) return static_cast<float>(px);
	return static_cast<float>(px * 768 + surface_h / 2) /
			static_cast<float>(surface_h);
}

// Where a ring row lands within the window.
//
// The ring walk paints row 15 FIRST and steps down to row 0, and the sinks put
// the NEWEST line in row 0 — so the oldest of the shown set is the TOP line and
// each row below it is newer. A history shorter than 16 leaves the TOP rows
// blank, exactly as the empty ring slots do; it does not bottom-align.
//
// Given a history of `count` lines, returns the index into that history for
// window row `row`, or -1 when the row is one of the leading blanks.
inline int message_log_row_source(int row, int count) {
	if (row < 0 || row >= kMessageLogRows) return -1;
	const int shown = count < kMessageLogRows ? count : kMessageLogRows;
	const int pad = kMessageLogRows - shown;
	if (row < pad) return -1;
	// The newest `shown` lines, oldest first.
	return (count - shown) + (row - pad);
}

} // namespace opennova::hud
