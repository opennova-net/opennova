// The minimal set's from-scratch bitmap font — a self-authored 5x7 stroke font
// rendered 2x into one 256x256 RGBA page and emitted through libs/fnt fnt_write.
// The menu boot loads a HARDCODED font set (Arial12b/14n/14b/16n/16b,
// Impac22b, Impac38b) [orig: HUD_InitAllFonts @ 0x51ee20]; the stylesheet's
// DEF_FONTNAME_* keys name fonts too (menu_style.mns). All of them are served
// by this one generated glyph set — a null font slot draws nothing, which is
// exactly the black Startup screen the minimal set showed without it.
// Glyph data is authored here (no external font, no retail bytes).
#ifndef OPENNOVA_TESTS_MINIMAL_FNT_BUILDER_H
#define OPENNOVA_TESTS_MINIMAL_FNT_BUILDER_H

#include <fnt/fnt.h>

#include <cstdint>
#include <cstring>

namespace minimal_fnt {

// 5 columns x 7 rows per glyph; '#' = opaque white pixel. Uppercase-only —
// lowercase input maps to uppercase at lookup. Enough coverage for menu labels
// (A-Z, 0-9, and the punctuation the authored menus/strings use).
struct GlyphArt {
	char ch;
	const char *rows[7];
};

inline const GlyphArt *glyph_art_table(size_t *count) {
	static const GlyphArt kArt[] = {
	    {'A', {" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
	    {'B', {"#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "}},
	    {'C', {" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "}},
	    {'D', {"#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "}},
	    {'E', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"}},
	    {'F', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "}},
	    {'G', {" ### ", "#   #", "#    ", "# ###", "#   #", "#   #", " ### "}},
	    {'H', {"#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
	    {'I', {" ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
	    {'J', {"  ###", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  "}},
	    {'K', {"#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"}},
	    {'L', {"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"}},
	    {'M', {"#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"}},
	    {'N', {"#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"}},
	    {'O', {" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
	    {'P', {"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "}},
	    {'Q', {" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"}},
	    {'R', {"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"}},
	    {'S', {" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "}},
	    {'T', {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "}},
	    {'U', {"#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
	    {'V', {"#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "}},
	    {'W', {"#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"}},
	    {'X', {"#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"}},
	    {'Y', {"#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "}},
	    {'Z', {"#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"}},
	    {'0', {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "}},
	    {'1', {"  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
	    {'2', {" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"}},
	    {'3', {" ### ", "#   #", "    #", "  ## ", "    #", "#   #", " ### "}},
	    {'4', {"   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "}},
	    {'5', {"#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "}},
	    {'6', {" ### ", "#    ", "#    ", "#### ", "#   #", "#   #", " ### "}},
	    {'7', {"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "}},
	    {'8', {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "}},
	    {'9', {" ### ", "#   #", "#   #", " ####", "    #", "    #", " ### "}},
	    {'.', {"     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "}},
	    {',', {"     ", "     ", "     ", "     ", " ##  ", " ##  ", "#    "}},
	    {':', {"     ", " ##  ", " ##  ", "     ", " ##  ", " ##  ", "     "}},
	    {'!', {"  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  "}},
	    {'?', {" ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  "}},
	    {'-', {"     ", "     ", "     ", "#####", "     ", "     ", "     "}},
	    {'_', {"     ", "     ", "     ", "     ", "     ", "     ", "#####"}},
	    {'/', {"    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    "}},
	    {'(', {"   # ", "  #  ", " #   ", " #   ", " #   ", "  #  ", "   # "}},
	    {')', {" #   ", "  #  ", "   # ", "   # ", "   # ", "  #  ", " #   "}},
	    {'\'', {"  #  ", "  #  ", "     ", "     ", "     ", "     ", "     "}},
	    {'"', {" # # ", " # # ", "     ", "     ", "     ", "     ", "     "}},
	    {'&', {" ##  ", "#  # ", "#  # ", " ##  ", "# # #", "#  # ", " ## #"}},
	    {'%', {"##  #", "##  #", "   # ", "  #  ", " #   ", "#  ##", "#  ##"}},
	    {'+', {"     ", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "     "}},
	    {'=', {"     ", "     ", "#####", "     ", "#####", "     ", "     "}},
	    {'*', {"     ", "# # #", " ### ", "#####", " ### ", "# # #", "     "}},
	    {'<', {"   # ", "  #  ", " #   ", "#    ", " #   ", "  #  ", "   # "}},
	    {'>', {" #   ", "  #  ", "   # ", "    #", "   # ", "  #  ", " #   "}},
	};
	*count = sizeof(kArt) / sizeof(kArt[0]);
	return kArt;
}

// Cell layout on the single 256x256 page: 16 columns x 14 rows of 16x16 cells,
// one per glyph 32..255. The 5x7 art renders 2x (10x14) at cell offset (1, 1);
// the glyph UV rect spans 12x16 so text gets a 2px advance gap.
const uint32_t kCellSize = 16;
const uint32_t kCellsPerRow = FNT_TEXTURE_WIDTH / kCellSize; // 16
const uint32_t kGlyphUvWidth = 12;

inline const GlyphArt *find_art(char ch) {
	if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
	size_t count = 0;
	const GlyphArt *table = glyph_art_table(&count);
	for (size_t i = 0; i < count; ++i)
		if (table[i].ch == ch) return &table[i];
	return nullptr;
}

// Build the minimal font: one page, every glyph slot gets a cell-aligned UV
// rect; drawable characters get their 2x stroke art, space stays blank, and
// anything else renders as a hollow box (visibly "missing", never invisible).
inline fnt_error_t build_font(fnt_font_t *font) {
	const fnt_error_t rc = fnt_init_blank(font, 1, 0);
	if (rc != FNT_OK) return rc;

	uint8_t *page = fnt_get_page_data(font, 0);
	std::memset(page, 0, FNT_TEXTURE_SIZE);

	for (uint32_t i = 0; i < FNT_GLYPH_COUNT; ++i) {
		const uint32_t cell_x = (i % kCellsPerRow) * kCellSize;
		const uint32_t cell_y = (i / kCellsPerRow) * kCellSize;
		const char ch = static_cast<char>(FNT_FIRST_CHAR + i);
		const GlyphArt *art = (ch == ' ') ? nullptr : find_art(ch);
		const bool hollow_box = (ch != ' ' && art == nullptr && ch < 127);

		for (uint32_t row = 0; row < 7; ++row) {
			for (uint32_t col = 0; col < 5; ++col) {
				bool on = false;
				if (art) {
					on = art->rows[row][col] == '#';
				} else if (hollow_box) {
					on = row == 0 || row == 6 || col == 0 || col == 4;
				}
				if (!on) continue;
				// 2x2 white block per art pixel.
				for (uint32_t dy = 0; dy < 2; ++dy) {
					for (uint32_t dx = 0; dx < 2; ++dx) {
						const uint32_t px = cell_x + 1 + col * 2 + dx;
						const uint32_t py = cell_y + 1 + row * 2 + dy;
						uint8_t *p = page + (py * FNT_TEXTURE_WIDTH + px) * FNT_TEXTURE_CHANNELS;
						p[0] = p[1] = p[2] = p[3] = 0xFF;
					}
				}
			}
		}

		fnt_glyph_t &g = font->glyphs[i];
		g.page = 0;
		g.uv.u0 = static_cast<float>(cell_x) / FNT_TEXTURE_WIDTH;
		g.uv.v0 = static_cast<float>(cell_y) / FNT_TEXTURE_HEIGHT;
		g.uv.u1 = static_cast<float>(cell_x + kGlyphUvWidth) / FNT_TEXTURE_WIDTH;
		g.uv.v1 = static_cast<float>(cell_y + kCellSize) / FNT_TEXTURE_HEIGHT;
	}
	return FNT_OK;
}

} // namespace minimal_fnt

#endif // OPENNOVA_TESTS_MINIMAL_FNT_BUILDER_H
