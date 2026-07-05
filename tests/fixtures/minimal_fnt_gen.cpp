// Guard for the minimal set's generated bitmap font (minimal_fnt_builder.h):
// the font the packaging step emits under every hardcoded boot font name
// [orig: HUD_InitAllFonts @ 0x51ee20] and the menu_style.mns DEF_FONTNAME_*
// names. Always-on (the font is deterministic and generated at package time,
// never committed): build -> validate -> write -> parse -> re-write must be
// byte-stable, and the label glyphs the authored menus use must have pixels.
#include "minimal_fnt_builder.h"

#include <fnt/fnt.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

// True if any texel inside the glyph's UV rect is set.
bool glyph_has_pixels(const fnt_font_t &font, char ch) {
	const fnt_glyph_t *g = fnt_get_glyph(&font, static_cast<uint8_t>(ch));
	if (!g) return false;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	fnt_uv_to_pixels(&g->uv, &x0, &y0, &x1, &y1);
	const uint8_t *page = fnt_get_page_data_const(&font, g->page);
	if (!page) return false;
	for (int y = y0; y < y1; ++y)
		for (int x = x0; x < x1; ++x)
			if (page[(y * (int)FNT_TEXTURE_WIDTH + x) * (int)FNT_TEXTURE_CHANNELS + 3])
				return true;
	return false;
}

} // namespace

int main() {
	int failures = 0;

	fnt_font_t font{};
	if (!expect(minimal_fnt::build_font(&font) == FNT_OK, "build_font")) return 1;
	failures += !expect(fnt_validate(&font) == FNT_OK, "fnt_validate on the built font");
	failures += !expect(font.num_pages == 1, "one 256x256 page");

	// Every character the authored menu labels use must actually draw.
	for (const char ch : std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.")) {
		char msg[64];
		std::snprintf(msg, sizeof(msg), "glyph '%c' has pixels", ch);
		failures += !expect(glyph_has_pixels(font, ch), msg);
	}
	failures += !expect(glyph_has_pixels(font, 'a'), "lowercase maps to uppercase art");
	failures += !expect(!glyph_has_pixels(font, ' '), "space stays blank");

	// Byte-stable across write -> parse -> write.
	const size_t size = fnt_calculate_file_size(font.num_pages);
	std::vector<uint8_t> bytes(size);
	size_t written = 0;
	failures += !expect(fnt_write(&font, bytes.data(), bytes.size(), &written) == FNT_OK &&
	                        written == size,
	                    "fnt_write");
	fnt_font_t reparsed{};
	failures += !expect(fnt_parse(bytes.data(), bytes.size(), &reparsed) == FNT_OK, "fnt_parse");
	std::vector<uint8_t> rewritten(size);
	failures += !expect(fnt_write(&reparsed, rewritten.data(), rewritten.size(), &written) == FNT_OK &&
	                        rewritten == bytes,
	                    "font is not byte-stable across a round-trip");
	fnt_free(&reparsed);
	fnt_free(&font);

	if (failures == 0) std::printf("OK: minimal generated font valid + byte-reproducible\n");
	return failures == 0 ? 0 : 1;
}
