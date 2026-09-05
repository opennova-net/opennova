#pragma once

// Witnessed .mnu widget-geometry math (the layout half of the menu system —
// ADR 0028: math native, Control-tree writes stay shell-side). Everything is
// parameters-in/struct-out: texture DIMENSIONS and measured TEXT EXTENTS come
// from the embedder (the shell owns fonts and texture loading), the witnessed
// rect solves live here.
//
// [orig: the frame pass CUIElement_DrawFrame @ 0x64a210 over the
// init_border_materials @ 0x646f70 tile grid; the three-stage POSITION parse
// tail in CUIElement_ParseXMLDefinition @ 0x648120 ending in
// adjust_rect_to_text_size @ 0x6575f0 (via the shared text-widget parse
// @ 0x657c30 and the edit override @ 0x661d10); the spin up/down child
// windows CSpinListWnd_CreateUpDownChildren @ 0x64b8b0; the ITEM color parse
// CUISpinList_ParseXMLDefinition @ 0x64bd10 + CSpinListWnd_Render @ 0x64b220.]

#include <array>
#include <cstdint>
#include <string>

#include <formats/mnu/mnu.h>

namespace opennova::mnu {

// --- Frame border + fill [orig: CUIElement_DrawFrame @ 0x64a210] -------------

// One tile of the SIZE x SIZE stencil grid (canonically 4*SIZE on a side):
// row 0 = TL / top edge / TR (+ the fill tile at column 3), row 1 = left /
// right edges (columns 0 and 2), row 2 = BL / bottom edge / BR
// [orig: init_border_materials @ 0x646f70 slices these UV rects].
struct FrameTileRect {
	int x = 0;
	int y = 0;
	int size = 0;
};
FrameTileRect frame_tile_rect(int size, int col, int row);

// The tile size: the authored STENCIL attr when positive, else texture_width/4
// (the canonical 4x4 grid).
int frame_stencil_tile_size(int authored_size, int texture_width);

// One of the 8 border pieces: which tile it draws and where it hangs. The
// anchors pick which window edges the piece follows (0 = leading, 1 =
// trailing); the offsets are the element-local geometry — the border hangs
// OUTSIDE the window rect by SIZE, pulled back in by the authored STENCIL
// INSETX/INSETY [orig: left - SIZE + INSETX; the trailing edges sit at
// -inset - 1 against their anchor].
struct FrameBorderPiece {
	const char *name = "";
	int tile_col = 0;
	int tile_row = 0;
	float anchor_left = 0.0f;
	float anchor_right = 0.0f;
	float anchor_top = 0.0f;
	float anchor_bottom = 0.0f;
	float off_left = 0.0f;
	float off_top = 0.0f;
	float off_right = 0.0f;
	float off_bottom = 0.0f;
};
std::array<FrameBorderPiece, 8> frame_border_layout(int size, int insetx,
		int insety);

// --- The three-stage POSITION solve ------------------------------------------

struct RectEdges {
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;
};

// Stage 1+2 [orig: the POSITION branch @ 0x648120 + the parse tail]: missing
// edges read 0; a degenerate axis (right <= left / bottom <= top) falls back
// to the largest appearance image (max_image_w/h, measured by the embedder —
// per image the width contribution is the texture width, the height the
// authored HEIGHT attr when present else the texture height).
RectEdges position_rect(bool has_left, int left, bool has_top, int top,
		bool has_right, int right, bool has_bottom, int bottom,
		int max_image_w, int max_image_h);

// The per-image height contribution to that fallback.
int appearance_extent_height(bool has_height, int height, int texture_height);

// The widget families whose parse ends in the text-extent adjustment
// [orig: the shared static/button text-widget parse @ 0x657c30 vtable slot
// and the edit override @ 0x661d10].
bool window_type_is_text_sized(WindowType t);

// Stage 3 [orig: adjust_rect_to_text_size @ 0x6575f0]: a still-degenerate
// axis sizes from the measured string, with the authored point as the anchor
// the JUSTIFY/VJUSTIFY flags align to ("center" -> centred on it, "right"/
// "bottom" -> trailing edge stays at it, anything else -> leading edge).
// Case-insensitive, matching the parse.
RectEdges adjust_rect_to_text_size(const RectEdges &rect, int text_w,
		int text_h, const std::string &justify, const std::string &vjustify);

// --- Spin up/down child rects [orig: @ 0x64b8b0] -----------------------------

// SPINUP/SPINDOWN POSITION is parent-relative to the spinlist; a missing far
// edge sizes from the appearance extents (the three-stage fallback), and an
// extent-less button takes the witnessed nominal 16 x 12 arrow box.
RectEdges spin_button_rect(bool has_left, int left, bool has_top, int top,
		bool has_right, int right, bool has_bottom, int bottom,
		int extent_w, int extent_h);

// --- ITEM color swatches -----------------------------------------------------

// type="color" reads the element text as base-16 RRGGBB forced opaque
// [orig: CUISpinList_ParseXMLDefinition @ 0x64bd10 (wcstoul base 16) +
// CSpinListWnd_Render @ 0x64b220 (color | 0xFF000000)].
std::uint32_t item_color_argb(const std::string &hex_text);

}  // namespace opennova::mnu
