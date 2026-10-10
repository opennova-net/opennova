// A glyph sheet made into a font (formats/fnt/fnt_sheet.h), through our own FNT writer and read back
// by fnt_parse: a sheet built in memory (16 x 14 cells of 8 x 10, a few glyphs drawn) made into a font
// under each advance mode, its rect widths, tracking and space, the blank cells, the zero records at
// 0x7F..0x81, the spacing and design-width header words, the pixels copied and their colour (white, or
// the sheet's own under sheet_color: a dark rim texel kept dark); a sheet of large cells packed over
// several pages, and one that needs more than a font holds; the sheet's refusals; sheets of other grids
// (fewer rows from the space, a full 256-byte grid from 0, a grid from a later byte) and the grid's
// refusals. Also the glyph UV from a pixel rect (fnt_pixels_to_uv), the inverse of fnt_uv_to_pixels.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <formats/fnt/fnt.h>
#include <formats/fnt/fnt_sheet.h>

#include "common/test_expect.h"

using namespace opennova::fnt;
using opennova::RgbaImage;

namespace {

constexpr int kCellW = 8, kCellH = 10;

// A transparent sheet of 16 x 14 cells of `cell_w` x `cell_h`.
RgbaImage blank_sheet(int cell_w = kCellW, int cell_h = kCellH) {
	RgbaImage sheet;
	sheet.width = cell_w * kFontSheetColumns;
	sheet.height = cell_h * kFontSheetRows;
	sheet.pixels.assign(size_t(sheet.width) * size_t(sheet.height) * 4, 0);
	return sheet;
}

// Columns [x0, x1] and rows [y0, y1] of byte `byte`'s cell inked, red at `alpha`.
void ink(RgbaImage &sheet, int byte, int x0, int x1, int y0, int y1, uint8_t alpha = 255) {
	const int cell_w = sheet.width / kFontSheetColumns, cell_h = sheet.height / kFontSheetRows;
	const int cx = ((byte - 0x20) % kFontSheetColumns) * cell_w, cy = ((byte - 0x20) / kFontSheetColumns) * cell_h;
	for (int y = y0; y <= y1; ++y)
		for (int x = x0; x <= x1; ++x) {
			uint8_t *p = &sheet.pixels[(size_t(cy + y) * size_t(sheet.width) + size_t(cx + x)) * 4];
			p[0] = 200;
			p[1] = 10;
			p[2] = 10;
			p[3] = alpha;
		}
}

// The test sheet: 'A' five columns (1..5), 'I' one (3), '.' two at half alpha (2..3, its lowest rows),
// 'W' the whole cell, the space's and 0x7F's cells drawn on (neither is read), 0xFF two columns (0..1).
RgbaImage test_sheet() {
	RgbaImage sheet = blank_sheet();
	ink(sheet, 'A', 1, 5, 1, 8);
	ink(sheet, 'I', 3, 3, 1, 8);
	ink(sheet, '.', 2, 3, 8, 9, 128);
	ink(sheet, 'W', 0, 7, 1, 8);
	ink(sheet, ' ', 0, 7, 0, 9);
	ink(sheet, 0x7F, 0, 7, 0, 9);
	ink(sheet, 0xFF, 0, 1, 0, 9);
	return sheet;
}

FontSheetSettings settings_of(FontSheetAdvance advance = FontSheetAdvance::Ink, int tracking = 1, int space = 0) {
	FontSheetSettings settings;
	settings.advance = advance;
	settings.tracking = tracking;
	settings.space = space;
	return settings;
}

// A font made of `sheet` by `settings` and parsed back; false when the sheet is refused.
struct Made {
	fnt_font_t font{};
	std::vector<uint8_t> bytes;
	bool opaque = false;
	std::string why, field;
	~Made() { fnt_free(&font); }
};
bool make(const RgbaImage &sheet, const FontSheetSettings &settings, Made &out) {
	if (!make_font_from_sheet(sheet, settings, out.bytes, out.opaque, out.why, out.field)) return false;
	return fnt_parse(out.bytes.data(), out.bytes.size(), &out.font) == FNT_OK;
}

int width_of(const fnt_font_t &font, int byte) {
	int w = 0, h = 0;
	fnt_get_glyph_size(fnt_get_glyph(&font, uint8_t(byte)), &w, &h);
	return w;
}
int height_of(const fnt_font_t &font, int byte) {
	int w = 0, h = 0;
	fnt_get_glyph_size(fnt_get_glyph(&font, uint8_t(byte)), &w, &h);
	return h;
}
bool zero_record(const fnt_font_t &font, int byte) {
	const fnt_glyph_t *g = fnt_get_glyph(&font, uint8_t(byte));
	return g->page == 0 && g->uv.u0 == 0.0f && g->uv.v0 == 0.0f && g->uv.u1 == 0.0f && g->uv.v1 == 0.0f;
}
// The texel of a glyph's rect at (x, y) within it: its page's RGBA.
const uint8_t *glyph_texel(const fnt_font_t &font, int byte, int x, int y) {
	const fnt_glyph_t *g = fnt_get_glyph(&font, uint8_t(byte));
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	fnt_uv_to_pixels(&g->uv, &x0, &y0, &x1, &y1);
	return fnt_get_page_data_const(&font, g->page) + (size_t(y0 + y) * FNT_TEXTURE_WIDTH + size_t(x0 + x)) * FNT_TEXTURE_CHANNELS;
}

} // namespace

static int test_pixels_to_uv() {
	// Every texel rect a page holds comes back from its UVs as it went in.
	for (int x0 = 0; x0 <= 256; x0 += 17)
		for (int x1 = x0; x1 <= 256; x1 += 23) {
			fnt_uv_t uv{};
			fnt_pixels_to_uv(x0, x1 / 2, x1, x1, &uv);
			int a = -1, b = -1, c = -1, d = -1;
			fnt_uv_to_pixels(&uv, &a, &b, &c, &d);
			TEST_EXPECT(a == x0 && b == x1 / 2 && c == x1 && d == x1);
		}
	fnt_uv_t uv{};
	fnt_pixels_to_uv(64, 128, 192, 256, &uv);
	TEST_EXPECT(uv.u0 == 0.25f && uv.v0 == 0.5f && uv.u1 == 0.75f && uv.v1 == 1.0f);
	return 0;
}

static int test_advance_modes() {
	const RgbaImage sheet = test_sheet();
	{
		// Ink (the fallback), tracking 1, the space a quarter of the cell's height: (10 + 2) / 4 = 3.
		Made made;
		TEST_EXPECT(make(sheet, FontSheetSettings(), made));
		const fnt_font_t &font = made.font;
		TEST_EXPECT(!made.opaque && font.num_pages == 1 && font.design_width == 800 && font.glyph_spacing == 0);
		TEST_EXPECT(width_of(font, 'A') == 6 && width_of(font, 'I') == 2 && width_of(font, '.') == 3 && width_of(font, 'W') == 9);
		TEST_EXPECT(width_of(font, ' ') == 3 && width_of(font, 'B') == 3 && width_of(font, 0xFF) == 3);
		// One height for the font: every printing glyph the cell's.
		for (int byte = 0x20; byte <= 0xFF; ++byte) {
			if (byte >= 0x7F && byte <= 0x81) {
				TEST_EXPECT(zero_record(font, byte));
				continue;
			}
			TEST_EXPECT(height_of(font, byte) == kCellH);
		}
		// The ink copied from its first column, its alpha as drawn, its colour white (fnt_write); the
		// tracking column and the space clear.
		for (int x = 0; x < 5; ++x) {
			TEST_EXPECT(glyph_texel(font, 'A', x, 0)[3] == 0 && glyph_texel(font, 'A', x, 1)[3] == 255 &&
			            glyph_texel(font, 'A', x, 8)[3] == 255 && glyph_texel(font, 'A', x, 9)[3] == 0);
			TEST_EXPECT(glyph_texel(font, 'A', x, 4)[0] == 255 && glyph_texel(font, 'A', x, 4)[1] == 255);
		}
		TEST_EXPECT(glyph_texel(font, 'A', 5, 4)[3] == 0);
		TEST_EXPECT(glyph_texel(font, '.', 0, 9)[3] == 128 && glyph_texel(font, '.', 1, 8)[3] == 128 && glyph_texel(font, '.', 2, 9)[3] == 0);
		for (int x = 0; x < 3; ++x)
			for (int y = 0; y < kCellH; ++y) TEST_EXPECT(glyph_texel(font, ' ', x, y)[3] == 0);
		// Packed in byte order from the page's gutter: the space first at (1, 1), the next to its right.
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		fnt_uv_to_pixels(&fnt_get_glyph(&font, ' ')->uv, &x0, &y0, &x1, &y1);
		TEST_EXPECT(x0 == int(FNT_PACK_PAD) && y0 == int(FNT_PACK_PAD));
		fnt_uv_to_pixels(&fnt_get_glyph(&font, '!')->uv, &x0, &y0, &x1, &y1);
		TEST_EXPECT(x0 == int(FNT_PACK_PAD) + 3 + int(FNT_PACK_PAD) && y0 == int(FNT_PACK_PAD));
		// The header's words: the file's +4 the design width, its +12 the spacing.
		TEST_EXPECT(made.bytes[4] == 0x20 && made.bytes[5] == 0x03 && made.bytes[12] == 0);
	}
	{
		// Left: from the cell's left edge; tracking 2.
		Made made;
		TEST_EXPECT(make(sheet, settings_of(FontSheetAdvance::Left, 2), made));
		TEST_EXPECT(width_of(made.font, 'A') == 8 && width_of(made.font, 'I') == 6 && width_of(made.font, '.') == 6 &&
		            width_of(made.font, 'W') == 10 && width_of(made.font, 'B') == 3);
		// The left bearing kept: the glyph's first column the cell's, clear.
		TEST_EXPECT(glyph_texel(made.font, 'A', 0, 4)[3] == 0 && glyph_texel(made.font, 'A', 1, 4)[3] == 255);
	}
	{
		// Cell: every printing glyph the cell's width and its tracking, the space and the blank cells too;
		// space ignored (a space wider than a cell refuses nothing).
		Made made;
		TEST_EXPECT(make(sheet, settings_of(FontSheetAdvance::Cell, 5, 12), made));
		for (int byte = 0x20; byte <= 0xFF; ++byte)
			TEST_EXPECT((byte >= 0x7F && byte <= 0x81) ? zero_record(made.font, byte) : width_of(made.font, byte) == kCellW + 5);
		// The tracking's columns clear, past the cell's.
		TEST_EXPECT(glyph_texel(made.font, 'W', kCellW - 1, 4)[3] == 255 && glyph_texel(made.font, 'W', kCellW, 4)[3] == 0);
		TEST_EXPECT(glyph_texel(made.font, 'I', 3, 4)[3] == 255 && glyph_texel(made.font, 'I', 2, 4)[3] == 0);
		// The space's cell, drawn on, is never read: it draws blank here too.
		for (int x = 0; x < kCellW; ++x)
			for (int y = 0; y < kCellH; ++y) TEST_EXPECT(glyph_texel(made.font, ' ', x, y)[3] == 0);
	}
	{
		// Tracking 0 and 3; a space of 5; the spacing and the design width written.
		Made made;
		FontSheetSettings settings = settings_of(FontSheetAdvance::Ink, 0, 5);
		settings.spacing = -3;
		settings.design_width = 1024;
		TEST_EXPECT(make(sheet, settings, made));
		TEST_EXPECT(width_of(made.font, 'A') == 5 && width_of(made.font, 'I') == 1 && width_of(made.font, ' ') == 5 &&
		            width_of(made.font, 'Z') == 5);
		TEST_EXPECT(made.font.glyph_spacing == -3 && made.font.design_width == 1024);
		TEST_EXPECT(made.bytes[4] == 0x00 && made.bytes[5] == 0x04 && made.bytes[12] == 0xFD && made.bytes[15] == 0xFF);
		Made wide;
		TEST_EXPECT(make(sheet, settings_of(FontSheetAdvance::Ink, 3), wide) && width_of(wide.font, 'A') == 8);
	}
	{
		// A space wider than a cell is the space setting's fault.
		Made made;
		TEST_EXPECT(!make(sheet, settings_of(FontSheetAdvance::Ink, 1, 9), made) && made.field == "space");
	}
	{
		// sheet_color: false writes every texel white, its alpha kept; true keeps the sheet's own colour,
		// so 'A''s dark rim texel (its first inked column's second row) stays dark.
		RgbaImage rimmed = test_sheet();
		uint8_t *rim = &rimmed.pixels[(size_t(2 * kCellH + 1) * size_t(rimmed.width) + size_t(kCellW + 1)) * 4];
		rim[0] = rim[1] = rim[2] = 0;
		rim[3] = 255;
		FontSheetSettings keep;
		keep.sheet_color = true;
		Made white, kept;
		TEST_EXPECT(make(rimmed, FontSheetSettings(), white) && make(rimmed, keep, kept));
		const uint8_t *was_rim = glyph_texel(white.font, 'A', 0, 1);
		TEST_EXPECT(was_rim[0] == 255 && was_rim[1] == 255 && was_rim[2] == 255 && was_rim[3] == 255);
		const uint8_t *dark = glyph_texel(kept.font, 'A', 0, 1);
		TEST_EXPECT(dark[0] == 0 && dark[1] == 0 && dark[2] == 0 && dark[3] == 255);
		const uint8_t *red = glyph_texel(kept.font, 'A', 1, 1);
		TEST_EXPECT(red[0] == 200 && red[1] == 10 && red[2] == 10 && red[3] == 255);
		TEST_EXPECT(glyph_texel(kept.font, '.', 0, 9)[3] == 128 && width_of(kept.font, 'A') == width_of(white.font, 'A'));
		// Only the pages differ.
		TEST_EXPECT(std::equal(white.bytes.begin(), white.bytes.begin() + FNT_TOTAL_HEADER, kept.bytes.begin()));
	}
	return 0;
}

static int test_pages_and_refusals() {
	{
		// 30 x 60 cells, every one inked whole: 220 glyphs of 31 x 60 and the space of 15 (the three
		// non-printing bytes none), seven to a shelf and four shelves to a page, so eight pages, byte
		// order kept.
		RgbaImage sheet = blank_sheet(30, 60);
		for (int byte = 0x20; byte <= 0xFF; ++byte) ink(sheet, byte, 0, 29, 0, 59);
		Made made;
		TEST_EXPECT(make(sheet, FontSheetSettings(), made));
		TEST_EXPECT(made.font.num_pages == 8);
		TEST_EXPECT(fnt_get_glyph(&made.font, ' ')->page == 0 && fnt_get_glyph(&made.font, 0xFF)->page == 7);
		TEST_EXPECT(made.bytes.size() == fnt_calculate_file_size(8));
		uint32_t page = 0;
		for (int byte = 0x20; byte <= 0xFF; ++byte) {
			if (byte >= 0x7F && byte <= 0x81) continue;
			const fnt_glyph_t *g = fnt_get_glyph(&made.font, uint8_t(byte));
			TEST_EXPECT(g->page >= page && width_of(made.font, byte) == (byte == ' ' ? 15 : 31) && height_of(made.font, byte) == 60);
			page = g->page;
		}
		// Every texel drawn opaque and none clear: the opaque case.
		RgbaImage full = blank_sheet(2, 3);
		for (size_t i = 3; i < full.pixels.size(); i += 4) full.pixels[i] = 255;
		Made boxes;
		TEST_EXPECT(make(full, FontSheetSettings(), boxes) && boxes.opaque && width_of(boxes.font, 'A') == 3);
	}
	{
		// 60 x 100 cells inked whole take eight glyphs a page: more than the sixteen pages a font holds.
		RgbaImage sheet = blank_sheet(60, 100);
		for (int byte = 0x20; byte <= 0xFF; ++byte) ink(sheet, byte, 0, 59, 0, 99);
		Made made;
		TEST_EXPECT(!make(sheet, FontSheetSettings(), made) && made.why.find("pages") != std::string::npos && made.field.empty());
	}
	{
		// Sides that do not divide into the grid; a cell taller than a page holds; a glyph wider than one
		// (its tracking's fault where its columns fit, else the sheet's).
		Made made;
		RgbaImage odd = blank_sheet();
		odd.width += 1;
		odd.pixels.resize(size_t(odd.width) * size_t(odd.height) * 4);
		TEST_EXPECT(!make(odd, FontSheetSettings(), made) && made.why.find("16 columns") != std::string::npos);
		Made tall;
		TEST_EXPECT(!make(blank_sheet(1, 255), FontSheetSettings(), tall) && tall.why.find("255 texels tall") != std::string::npos);
		Made fits;
		TEST_EXPECT(make(blank_sheet(1, 254), settings_of(FontSheetAdvance::Cell, 0), fits) && height_of(fits.font, 'A') == 254 &&
		            fits.font.num_pages == 2);
		RgbaImage wide = blank_sheet(254, 4);
		ink(wide, 'M', 0, 253, 0, 3);
		Made tracked;
		TEST_EXPECT(!make(wide, FontSheetSettings(), tracked) && tracked.field == "tracking" &&
		            tracked.why.find("0x4D") != std::string::npos);
		Made untracked;
		TEST_EXPECT(make(wide, settings_of(FontSheetAdvance::Ink, 0), untracked) && width_of(untracked.font, 'M') == 254);
		Made monospace;
		TEST_EXPECT(!make(blank_sheet(255, 4), settings_of(FontSheetAdvance::Cell), monospace) && monospace.field.empty());
		// A cell a page holds whose tracking takes it past one: the tracking's fault.
		Made cell_tracked;
		TEST_EXPECT(!make(blank_sheet(254, 4), settings_of(FontSheetAdvance::Cell), cell_tracked) &&
		            cell_tracked.field == "tracking");
		Made cell_fits;
		TEST_EXPECT(make(blank_sheet(254, 4), settings_of(FontSheetAdvance::Cell, 0), cell_fits) &&
		            width_of(cell_fits.font, 'A') == 254);
		// A tracking under 0, under each advance: refused, the tracking's fault (a rect narrower than the
		// columns copied into it would ink its neighbours, or run off its page). Cells wider than a page
		// stay refused whatever the tracking.
		for (const FontSheetAdvance advance : {FontSheetAdvance::Ink, FontSheetAdvance::Left, FontSheetAdvance::Cell}) {
			Made negative;
			TEST_EXPECT(!make(wide, settings_of(advance, -2), negative) && negative.field == "tracking" &&
			            negative.why.find("0 or more") != std::string::npos);
		}
		Made huge;
		TEST_EXPECT(!make(blank_sheet(300, 1), settings_of(FontSheetAdvance::Cell, -250), huge) && huge.field == "tracking");
		Made too_wide;
		TEST_EXPECT(!make(blank_sheet(300, 1), settings_of(FontSheetAdvance::Cell, 0), too_wide) && too_wide.field.empty());
	}
	return 0;
}

// A transparent sheet of `columns` x `rows` cells of 8 x 10, and the cell of `byte` (from `first`) inked
// over columns [x0, x1].
static RgbaImage grid_sheet(int columns, int rows) {
	RgbaImage sheet;
	sheet.width = kCellW * columns;
	sheet.height = kCellH * rows;
	sheet.pixels.assign(size_t(sheet.width) * size_t(sheet.height) * 4, 0);
	return sheet;
}
static void grid_ink(RgbaImage &sheet, int columns, int first, int byte, int x0, int x1) {
	const int cell = byte - first;
	const int cx = (cell % columns) * kCellW, cy = (cell / columns) * kCellH;
	for (int y = 1; y <= 8; ++y)
		for (int x = x0; x <= x1; ++x) sheet.pixels[(size_t(cy + y) * size_t(sheet.width) + size_t(cx + x)) * 4 + 3] = 255;
}

static int test_grids() {
	{
		// Six rows from the space (0x20..0x7F): 'A' read from its cell, the bytes no cell holds blanks of
		// the space's width.
		RgbaImage sheet = grid_sheet(16, 6);
		grid_ink(sheet, 16, 0x20, 'A', 1, 5);
		grid_ink(sheet, 16, 0x20, '~', 0, 6);
		FontSheetSettings settings;
		settings.rows = 6;
		Made made;
		TEST_EXPECT(make(sheet, settings, made));
		TEST_EXPECT(width_of(made.font, 'A') == 6 && width_of(made.font, '~') == 8);
		TEST_EXPECT(width_of(made.font, 0xE9) == 3 && width_of(made.font, 0xFF) == 3 && zero_record(made.font, 0x80));
		TEST_EXPECT(glyph_texel(made.font, 'A', 0, 4)[3] == 255);
		// The same sheet read as the default grid's 14 rows is refused: its height does not divide.
		Made fourteen;
		TEST_EXPECT(!make(sheet, FontSheetSettings(), fourteen) && fourteen.why.find("14 rows") != std::string::npos);
	}
	{
		// A full 256-byte grid from 0: the cells below the space are not read, 'A' is the 65th cell, 0xFF the last.
		RgbaImage sheet = grid_sheet(16, 16);
		grid_ink(sheet, 16, 0, 0x05, 0, 7); // a control's cell, never read
		grid_ink(sheet, 16, 0, 'A', 2, 4);
		grid_ink(sheet, 16, 0, 0xFF, 0, 1);
		FontSheetSettings settings;
		settings.rows = 16;
		settings.first = 0;
		Made made;
		TEST_EXPECT(make(sheet, settings, made));
		TEST_EXPECT(width_of(made.font, 'A') == 4 && width_of(made.font, 0xFF) == 3 && width_of(made.font, 'B') == 3);
	}
	{
		// A grid from a later byte (8 columns by 2 rows from 0xC0): the bytes before it blanks.
		RgbaImage sheet = grid_sheet(8, 2);
		grid_ink(sheet, 8, 0xC0, 0xC1, 0, 4);
		FontSheetSettings settings;
		settings.columns = 8;
		settings.rows = 2;
		settings.first = 0xC0;
		Made made;
		TEST_EXPECT(make(sheet, settings, made));
		TEST_EXPECT(width_of(made.font, 0xC1) == 6 && width_of(made.font, 'A') == 3 && width_of(made.font, 0xD0) == 3);
	}
	{
		// The grid's refusals: no columns, rows past 256, a first byte past 0xFF; each names its setting.
		const RgbaImage sheet = grid_sheet(16, 14);
		FontSheetSettings settings;
		settings.columns = 0;
		Made none;
		TEST_EXPECT(!make(sheet, settings, none) && none.field == "columns");
		settings = FontSheetSettings();
		settings.rows = 300;
		Made many;
		TEST_EXPECT(!make(sheet, settings, many) && many.field == "rows");
		settings = FontSheetSettings();
		settings.first = 256;
		Made past;
		TEST_EXPECT(!make(sheet, settings, past) && past.field == "first");
	}
	return 0;
}

int main() {
	if (test_pixels_to_uv()) return 1;
	if (test_advance_modes()) return 1;
	if (test_pages_and_refusals()) return 1;
	if (test_grids()) return 1;
	std::printf("fnt_sheet: ok\n");
	return 0;
}
