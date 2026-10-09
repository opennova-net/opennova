// A glyph sheet made into the .fnt the game reads (fnt_sheet.h). Tooling, not a port: the sheet's
// grid, the advance rules and the packing are authoring policy; the FNT facts it is made to are the
// reader's (fnt.h), each cited where it binds.
#include <formats/fnt/fnt_sheet.h>

#include <algorithm>
#include <cstdio>

namespace opennova::fnt {

namespace {

std::string hex_byte(int byte) {
	char text[8];
	std::snprintf(text, sizeof(text), "0x%02X", byte & 0xFF);
	return text;
}

} // namespace

bool make_font_from_sheet(const RgbaImage &sheet, const FontSheetSettings &settings, std::vector<uint8_t> &out,
                          bool &opaque, std::string &why, std::string &field) {
	out.clear();
	opaque = false;
	field.clear();
	const std::string sides = std::to_string(sheet.width) + " x " + std::to_string(sheet.height);
	if (sheet.empty() || sheet.width % kFontSheetColumns != 0 || sheet.height % kFontSheetRows != 0) {
		why = "the sheet is " + sides + ": its width divides into 16 columns and its height into 14 rows of equal cells";
		return false;
	}
	const int cell_w = sheet.width / kFontSheetColumns;
	const int cell_h = sheet.height / kFontSheetRows;
	if (cell_h > kFontSheetGlyphMax) {
		why = "a cell of the sheet is " + std::to_string(cell_h) + " texels tall: a glyph is at most " +
		      std::to_string(kFontSheetGlyphMax) + " (a page's 256 less the packer's gutter at each edge)";
		return false;
	}
	const bool proportional = settings.advance != FontSheetAdvance::Cell;
	const int space = settings.space > 0 ? settings.space : std::max(1, (cell_h + 2) / 4);
	if (proportional && space > cell_w) {
		why = "the space is " + std::to_string(space) + " texels wide and a cell " + std::to_string(cell_w) +
		      ": the space is no wider than a cell";
		field = "space";
		return false;
	}
	const auto alpha = [&](int x, int y) { return sheet.pixels[(size_t(y) * size_t(sheet.width) + size_t(x)) * 4 + 3]; };
	opaque = true;
	for (size_t i = 3; i < sheet.pixels.size() && opaque; i += 4) opaque = sheet.pixels[i] != 0;

	// Each glyph's width, and the columns of its cell copied into it (from `from`, `copy` of them: its
	// tracking, and a blank glyph whole, the page's clear texels).
	struct Glyph {
		int from = 0;
		int copy = 0;
	};
	std::vector<Glyph> glyphs(FNT_GLYPH_COUNT);
	std::vector<fnt_pack_size_t> sizes(FNT_GLYPH_COUNT);
	for (uint32_t i = 0; i < FNT_GLYPH_COUNT; ++i) {
		const int byte = int(FNT_FIRST_CHAR + i);
		sizes[i] = {0, 0};
		// The all-zero record: the game neither measures nor draws the byte [orig: CGameFont_MeasureText @
		// 0x674e70; CGameFont_DrawText @ 0x6752c0].
		if (fnt_byte_is_nonprinting(uint8_t(byte))) continue;
		const int cell_x = int(i % kFontSheetColumns) * cell_w, cell_y = int(i / kFontSheetColumns) * cell_h;
		int first = -1, last = -1;
		for (int x = 0; x < cell_w; ++x)
			for (int y = 0; y < cell_h; ++y)
				if (alpha(cell_x + x, cell_y + y) != 0) {
					if (first < 0) first = x;
					last = x;
					break;
				}
		Glyph &glyph = glyphs[i];
		int width = 0;
		if (byte == 0x20 || first < 0) {
			// The space (its cell never read: it draws blank), and a cell drawn empty: a blank of the
			// space's width, or the cell's in a monospaced font.
			width = proportional ? space : cell_w;
		} else if (!proportional) {
			glyph = {cell_x, cell_w};
			width = cell_w;
		} else if (settings.advance == FontSheetAdvance::Ink) {
			glyph = {cell_x + first, last - first + 1};
			width = glyph.copy + settings.tracking;
		} else {
			glyph = {cell_x, last + 1};
			width = glyph.copy + settings.tracking;
		}
		if (width > kFontSheetGlyphMax) {
			// Its tracking's fault where its own columns fit; the sheet's where its cells are wider than a page holds.
			const bool tracked = proportional && glyph.copy <= kFontSheetGlyphMax;
			why = "byte " + hex_byte(byte) + "'s glyph is " + std::to_string(width) + " texels wide" +
			      (tracked ? " with its tracking" : "") + ": a glyph is at most " + std::to_string(kFontSheetGlyphMax);
			if (tracked) field = "tracking";
			return false;
		}
		sizes[i] = {uint32_t(width), uint32_t(cell_h)};
	}

	std::vector<fnt_pack_rect_t> rects(FNT_GLYPH_COUNT);
	uint32_t pages = 0;
	const fnt_error_t packed = fnt_pack_shelf(sizes.data(), sizes.size(), rects.data(), &pages);
	if (packed == FNT_ERR_INVALID_PAGE_COUNT) {
		why = "the glyphs of " + std::to_string(cell_h) + "-texel cells take more than the " + std::to_string(FNT_MAX_PAGES) +
		      " pages of 256 x 256 a font holds";
		return false;
	}
	fnt_font_t font{};
	fnt_error_t error = packed;
	if (error == FNT_OK) error = fnt_init_blank(&font, pages, settings.spacing);
	if (error != FNT_OK) {
		why = std::string("the font cannot be made: ") + fnt_error_string(error);
		return false;
	}
	// The header's +4 word, which the game draws the font at 800 / it of its texels by [orig:
	// GameFont_LoadFromBlob @ 0x674740].
	font.design_width = settings.design_width;
	font.keep_page_rgb = settings.sheet_color ? 1 : 0;
	for (uint32_t i = 0; i < FNT_GLYPH_COUNT; ++i) {
		const fnt_pack_rect_t &rect = rects[i];
		if (rect.width == 0) continue;
		const Glyph &glyph = glyphs[i];
		const int cell_y = int(i / kFontSheetColumns) * cell_h;
		uint8_t *page = fnt_get_page_data(&font, rect.page);
		for (int y = 0; y < cell_h; ++y) {
			const uint8_t *from = &sheet.pixels[(size_t(cell_y + y) * size_t(sheet.width) + size_t(glyph.from)) * 4];
			uint8_t *to = page + (size_t(rect.y + uint32_t(y)) * FNT_TEXTURE_WIDTH + rect.x) * FNT_TEXTURE_CHANNELS;
			std::copy(from, from + size_t(glyph.copy) * 4, to);
		}
		fnt_glyph_t &record = font.glyphs[i];
		record.page = rect.page;
		fnt_pixels_to_uv(int(rect.x), int(rect.y), int(rect.x + rect.width), int(rect.y + rect.height), &record.uv);
	}
	out.assign(fnt_calculate_file_size(font.num_pages), 0);
	size_t written = 0;
	error = fnt_write(&font, out.data(), out.size(), &written);
	fnt_free(&font);
	if (error != FNT_OK || written != out.size()) {
		out.clear();
		why = std::string("the font cannot be written: ") + fnt_error_string(error);
		return false;
	}
	return true;
}

} // namespace opennova::fnt
