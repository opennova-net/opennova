#pragma once

// The SP epilog cine's text through the HUD label fonts' glyph runs (world/epilog_cine.h owns the
// nodes, their schedule and their values): a TextFade's lines and an EpilogCounter's row, laid out
// in screen pixels over the cine viewport as the game's drawers place them (D-LOADSCR-2's epilog
// screen).

#include <runtime/hud/game_font.h>
#include <runtime/world/epilog_cine.h>

#include <cstdint>
#include <string>

namespace opennova::hud {

// The cine viewport, g_CineViewportRect's left, top, right and bottom: the whole screen.
struct CineViewport {
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;
};

// A TextFade's lines in the large label slot (`font` at `scale`, the slot's W / 800): its box
// (x, y, width, height in the cine's 1024 x 768 space) over the viewport, x / 1024 * width + left
// and y / 768 * height + top, each sum truncated, and the text word-wrapped in it from the box's
// top, each line centred on the box's middle (HUD_DrawWrappedText's mode 4). `color` is the
// node's (alpha byte << 24) + its colour, which the wrapper halves keeping the alpha
// (half_bright_keep_alpha). `text` is the line in the game's code page. Every epilog build names
// font -2, the large slot.
// [orig: CCineEventTextFade_Draw @0x573020 - the box @0x573041..0x573093 (flt_7D68F4 1/1024,
//  flt_7D68F8 1/768), the alpha @0x57309d..0x5730b4, the font -2 @0x573130, HUD_DrawWrappedText
//  (text, slot, x, y, x + w, y + h, 4, colour, 0) @0x573175; HUD_DrawWrappedText @0x580C00 - the
//  colour halved keeping its alpha @0x580c99..0x580ca8, mode 4's x = x0 + (width >> 1) with the
//  centred draw @0x580dd8..0x580def]
GameFontRun cine_text_fade_run(const GameFont &font, float scale, const std::string &text,
		int32_t x, int32_t y, int32_t box_width, int32_t box_height, uint32_t color,
		const CineViewport &view);

// An EpilogCounter's row in the large label slot: the label left at label_x, the value
// right-aligned at value_x and, when the count-up column runs, its count right-aligned at count_x,
// all on row_y, each in the cine's 1024 x 768 space over the viewport, truncated, half-bright with
// the alpha forced (the counters do not fade). Empty `value` or `count` draws nothing there.
// [orig: CineEventEpilogCounter_Draw @0x573440 - the columns @0x573460..0x5734d3,
//  HUD_DrawTextLeft_HalfBright(&g_HUDLabelFontLarge, x, y, label, -1, 0) @0x5734de, the value's
//  "%ld" / "%ld/%ld" @0x57351b / @0x573506 and HUD_DrawTextRightAligned_HalfBright @0x57353c, the
//  count's "%ld" @0x573567 and its draw @0x573588; HUD_DrawTextLeft_HalfBright @0x5804C0 - the
//  colour halved, alpha forced @0x5804D8..0x5804F0]
GameFontRun cine_epilog_counter_run(const GameFont &font, float scale, const std::string &label,
		const std::string &value, const std::string &count, int32_t label_x, int32_t value_x,
		int32_t count_x, int32_t row_y, const CineViewport &view);

// The count-up column's text: the count as "%ld" while the column runs (its target or its rate
// other than -1), else none [orig: CineEventEpilogCounter_Draw @0x573554 / @0x573567].
std::string cine_epilog_count_text(int32_t points_target, int32_t points_rate, int32_t points);

// A cine node's text run: a TextFade's lines over `text` (its resolved line), an EpilogCounter's
// row with `text` its label (the value and the count from the node); any other node none.
GameFontRun cine_event_text_run(const world::CineEvent &e, const std::string &text,
		const GameFont &font, float scale, const CineViewport &view);

} // namespace opennova::hud
