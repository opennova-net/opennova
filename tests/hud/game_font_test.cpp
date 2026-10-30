// The game font's glyph advance (runtime/hud/game_font.h): every byte from the space up selects
// its own glyph record and advances by that record's width plus the font's (spacing - 1) pad, the
// space included, so a space advances by its own record, never a share of the font's height, and
// an empty record by the pad alone, however small; a decoded string reaches the record of its
// code-page byte, and the three retail control bytes measure nothing
// [orig: CGameFont_MeasureText @ 0x674e70 - the tab as the space @ 0x675039, the control bytes
//  @ 0x675051, the record walk @ 0x675053..0x675093; CGameFont_DrawText @ 0x6752c0].
#include "common/test_font.h"

#include <base/io/cp1252.h>
#include <formats/fnt/fnt.h>
#include <runtime/hud/game_font.h>

#include <cstdint>
#include <cstdio>
#include <string>

using opennova::fnt::FNT_FIRST_CHAR;
using opennova::fnt::fnt_font_t;
using opennova::hud::GameFont;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::fprintf(stderr, "FAIL: %s\n", msg);                           \
			++failures;                                                        \
		}                                                                      \
	} while (0)

// The record of `byte` set `width` x `height` texels.
void set_glyph(fnt_font_t &font, uint8_t byte, int width, int height) {
	opennova::fnt::fnt_glyph_t &glyph = font.glyphs[byte - FNT_FIRST_CHAR];
	glyph.uv.u0 = 0.0f;
	glyph.uv.v0 = 0.0f;
	glyph.uv.u1 = static_cast<float>(width) / 256.0f;
	glyph.uv.v1 = static_cast<float>(height) / 256.0f;
}

int measure_width(const GameFont &gf, const std::string &text) {
	int w = 0;
	int h = 0;
	gf.measure(text.c_str(), 1.0f, 1.0f, &w, &h);
	return w;
}

// The space is a glyph like any other: a 5-texel space in a font of 8-texel glyphs and spacing 2
// advances floor(5 + 1 + 0.5) = 6, whatever its 20-texel height; "x x" is 9 + 6 + 9 less the
// trailing pad, 23, and a tab with no tab width measures as the space.
void test_space_advances_by_its_own_record() {
	fnt_font_t font = test_font::uniform_test_font();
	set_glyph(font, ' ', 5, 20);
	GameFont gf;
	gf.set_font(&font);
	CHECK(measure_width(gf, " ") == 5, "a space measures its own record's width");
	CHECK(measure_width(gf, "x x") == 23, "the space between two glyphs advances 6");
	CHECK(measure_width(gf, "x\tx") == 23, "a tab with no tab width advances as the space");
	CHECK(gf.char_width(' ', 1.0f) == 6, "the space's extent carries the pad");
}

// An empty record advances by the pad alone: at spacing 0 a 0-texel space steps the pen
// floor(0 - 1 + 0.5) = -1, so "x x" is 7 - 1 + 7 = 13 and the measure 14; nothing raises it.
void test_empty_record_advances_by_the_pad() {
	fnt_font_t font = test_font::uniform_test_font();
	font.glyph_spacing = 0;
	set_glyph(font, ' ', 0, 0);
	GameFont gf;
	gf.set_font(&font);
	CHECK(measure_width(gf, "xx") == 15, "two 8-texel glyphs at spacing 0: 7 + 7, measured 15");
	CHECK(measure_width(gf, "x x") == 14, "an empty space steps the pen back one");
	CHECK(gf.char_width(' ', 1.0f) == -1, "the empty record's extent is the pad");
}

// Decoded text draws by its code-page byte (D-FNT-4): the HUD encodes U+201C to 0x93, whose own
// record (12 texels here) it measures; U+20AC encodes to 0x80, a control byte that measures
// nothing, as do 0x7F and 0x81.
void test_decoded_text_selects_its_bytes_record() {
	fnt_font_t font = test_font::uniform_test_font();
	set_glyph(font, 0x93, 12, 16);
	GameFont gf;
	gf.set_font(&font);
	uint8_t quote = 0;
	CHECK(opennova::cp1252_encode_codepoint(0x201C, quote) && quote == 0x93,
			"U+201C is the code page's byte 0x93");
	CHECK(measure_width(gf, std::string(1, static_cast<char>(quote))) == 12,
			"byte 0x93 measures its own record");
	uint8_t euro = 0;
	CHECK(opennova::cp1252_encode_codepoint(0x20AC, euro) && euro == 0x80,
			"U+20AC is the code page's byte 0x80");
	CHECK(measure_width(gf, std::string("x") + static_cast<char>(euro) + "\x7f\x81x") ==
					measure_width(gf, "xx"),
			"0x7F, 0x80 and 0x81 measure nothing");
}

} // namespace

int main() {
	test_space_advances_by_its_own_record();
	test_empty_record_advances_by_the_pad();
	test_decoded_text_selects_its_bytes_record();
	if (failures != 0) {
		std::fprintf(stderr, "game_font: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("game_font: all cases passed\n");
	return 0;
}
