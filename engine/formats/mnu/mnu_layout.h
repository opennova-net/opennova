#pragma once

// Witnessed .mnu widget-geometry math (the layout half of the menu system —
// ADR 0028: math native, Control-tree writes stay shell-side). Everything is
// parameters-in/struct-out: texture DIMENSIONS and measured TEXT EXTENTS come
// from the embedder (the shell owns fonts and texture loading), the witnessed
// rect solves live here.
//
// [orig: the frame pass CUIElement_DrawFrame @ 0x64a210 over the
// CUIElement_InitBorderMaterials @ 0x646f70 tile grid; the three-stage POSITION parse
// tail in CUIElement_ParseXMLDefinition @ 0x648120 ending in
// CStaticWnd_AdjustRectToTextSize @ 0x6575f0 (via the shared text-widget parse
// CUIButtonWidget_ParseXMLAttributes @ 0x657c30); the ITEM color parse
// CUISpinList_ParseXMLDefinition @ 0x64bd10 + CSpinListWnd_Render @ 0x64b220;
// the APPEARANCE COLOR / OUTLINE value, CRT_wcstoxl base 16 @ 0x648562.]

#include <cstddef>
#include <cstdint>
#include <string>

#include <formats/mnu/mnu.h>

namespace opennova::mnu {

// --- Frame border + fill [orig: CUIElement_DrawFrame @ 0x64a210] -------------

// One tile of the SIZE x SIZE stencil grid (canonically 4*SIZE on a side):
// row 0 = TL / top edge / TR (+ the fill tile at column 3), row 1 = left /
// right edges (columns 0 and 2), row 2 = BL / bottom edge / BR
// [orig: CUIElement_InitBorderMaterials @ 0x646f70 slices these UV rects].
struct FrameTileRect {
	int x = 0;
	int y = 0;
	int size = 0;
};
FrameTileRect frame_tile_rect(int size, int col, int row);

// The tile size: the authored STENCIL attr when positive, else texture_width/4
// (the canonical 4x4 grid).
int frame_stencil_tile_size(int authored_size, int texture_width);

// Where the 8 border pieces hang is device geometry, the scaled cell and insets in
// floats: the menu compiler's (runtime/menu MenuFrameCompiler::emit_frame).

// --- The three-stage POSITION solve ------------------------------------------

struct RectEdges {
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;
};

// Stage 1+2 [orig: the POSITION branch @ 0x648120 + the parse tail @ 0x649736]:
// missing edges read 0; a degenerate axis (right <= left / bottom <= top) falls
// back to the largest appearance image (max_image_w/h: the ImageExtents below).
RectEdges position_rect(bool has_left, int left, bool has_top, int top,
		bool has_right, int right, bool has_bottom, int bottom,
		int max_image_w, int max_image_h);

// The largest IMAGE extents a window's APPEARANCE rows reach as they are parsed
// [orig: CUIElement_ParseXMLDefinition @ 0x6485cd..0x648634]: every IMAGE row
// counts, including one a later row of its state replaces; the width is the
// texture's; the height is the row's HEIGHT when it is not -1 (the attribute's
// absence), else the texture's; both maxima compare unsigned (`jbe`), so a
// HEIGHT of 0 adds nothing and a negative one other than -1 is a huge maximum.
// A texture that did not load measures 0.
struct ImageExtents {
	uint32_t width = 0;
	uint32_t height = 0;
	void add(int texture_width, int texture_height, int height_attribute);
};

// The widget families whose parse ends in the text-extent adjustment: every
// type whose parse chain runs the STATIC parse [orig:
// CUIButtonWidget_ParseXMLAttributes @ 0x657c30 calls CStaticWnd_AdjustRectToTextSize
// @ 0x658079; CButtonWnd_ParseTooltipXML @ 0x658170 again @ 0x658324]: STATIC,
// BUTTON, EDIT, MULTILINE_EDIT, RADIO, CHECKBOX, SPINLIST, LIST, LAN_LIST,
// TABLE, COMBOBOX. The base-only types (SCROLL, MARQUEE_WND, GLB_TABLE, GOPHER,
// the generic window, RADIOEDIT itself) are never text sized.
bool window_type_is_text_sized(WindowType t);

// Stage 3 [orig: CStaticWnd_AdjustRectToTextSize @ 0x6575f0]: a still-degenerate
// axis sizes from the measured string, with the authored point as the anchor
// the JUSTIFY/VJUSTIFY flags align to ("center" -> centred on it, "right"/
// "bottom" -> trailing edge stays at it, anything else -> leading edge).
// Case-insensitive, matching the parse.
RectEdges adjust_rect_to_text_size(const RectEdges &rect, int text_w,
		int text_h, const std::string &justify, const std::string &vjustify);

// --- Colors -----------------------------------------------------------------

// A color the parse reads with wcstoul(text, 16) [orig: CRT_wcstoxl @ 0x76e93b
// through the APPEARANCE COLOR / OUTLINE arm @ 0x648562 and the spin ITEM
// @ 0x64bd10]: leading whitespace, an optional sign, an optional 0x, then hex
// digits up to the first other character (a partly valid value keeps its valid
// prefix); no digit reads 0 ('#' is not accepted); a value past 32 bits
// saturates to 0xFFFFFFFF; '-' negates. The result is the 0xAARRGGBB word as it
// stands, so six digits leave alpha 0.
std::uint32_t color_value(const std::string &text);

// Whether color_value reads the whole text [orig: CRT_wcstoxl @ 0x76e93b through the
// APPEARANCE COLOR / OUTLINE arm @ 0x648562 and the FONT colours @ 0x648d14..0x648e64]:
// leading blanks, a sign, 0x, then hex digits to the end (trailing blanks read nothing
// more); false when no digit is read or another character stops it. `digits_read`, when
// given, is how many hex digits the read took.
bool color_reads_whole(const std::string &text, size_t *digits_read = nullptr);

// The 0xAARRGGBB word as the text color_value reads back whole: eight upper-case hex digits, no
// prefix ("FF808080"). What a writer puts in a COLOR / OUTLINE / FONT colour.
std::string color_text(std::uint32_t word);

// type="color" on a spin ITEM: the same word forced opaque [orig:
// CSpinListWnd_Render @ 0x64b220 (color | 0xFF000000)].
std::uint32_t item_color_argb(const std::string &hex_text);

}  // namespace opennova::mnu
