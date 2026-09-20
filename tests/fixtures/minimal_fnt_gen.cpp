// Guard for the editor's generated bitmap font (engine/editor/blank/blank_font_art.h):
// the font the blank factories emit under every hardcoded boot font name
// [orig: HUD_InitAllFonts @ 0x51ee20] and the menu_style.mns DEF_FONTNAME_*
// names. Always-on: build -> validate -> write -> parse -> re-write must be
// byte-stable, and the label glyphs the authored menus use must have pixels.
//
// The same builder mints the committed FNT test fixtures fixtures/fnt/
// synth_1page.fnt and synth_3page.fnt (one and three pages; the three-page
// form spreads the glyph slots over its pages). They are byte-compared every
// run; `--write` (re)writes them. No retail font is carried.
#include <editor/blank/blank_font_art.h>

#include "common/test_paths.h"
#include <formats/fnt/fnt.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/file_io.h"

using namespace opennova::fnt;
namespace minimal_fnt = opennova::editor::blank_font;

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

bool write_bytes(const fnt_font_t &font, std::vector<uint8_t> &bytes) {
	const size_t size = fnt_calculate_file_size(font.num_pages);
	bytes.assign(size, 0);
	size_t written = 0;
	return fnt_write(&font, bytes.data(), bytes.size(), &written) == FNT_OK && written == size;
}

using test_io::read_file;

int guard_fixture(const std::string &path, uint32_t pages, bool write_mode) {
	fnt_font_t font{};
	if (!expect(minimal_fnt::build_font_pages(&font, pages) == FNT_OK, "build_font_pages")) return 1;
	std::vector<uint8_t> bytes;
	const bool wrote = write_bytes(font, bytes);
	fnt_free(&font);
	if (!expect(wrote, "fnt_write")) return 1;
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed), (path + " missing; run with --write").c_str())) return 1;
	if (test_io::is_lfs_pointer(committed)) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return 0;
	}
	return expect(committed == bytes, (path + " differs from the builder output; regenerate with --write").c_str()) ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
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
	std::vector<uint8_t> bytes;
	failures += !expect(write_bytes(font, bytes), "fnt_write");
	fnt_font_t reparsed{};
	failures += !expect(fnt_parse(bytes.data(), bytes.size(), &reparsed) == FNT_OK, "fnt_parse");
	std::vector<uint8_t> rewritten;
	failures += !expect(write_bytes(reparsed, rewritten) && rewritten == bytes,
	                    "font is not byte-stable across a round-trip");
	fnt_free(&reparsed);
	fnt_free(&font);

	// The three-page form spreads glyph slots over its pages and still draws.
	fnt_font_t three{};
	failures += !expect(minimal_fnt::build_font_pages(&three, 3) == FNT_OK && fnt_validate(&three) == FNT_OK,
	                    "three-page font builds and validates");
	failures += !expect(fnt_get_glyph(&three, 'A')->page == 0 && fnt_get_glyph(&three, 'B')->page == 1 &&
	                            fnt_get_glyph(&three, 'C')->page == 2,
	                    "glyph slots spread over the pages (slot % pages)");
	failures += !expect(glyph_has_pixels(three, 'B') && glyph_has_pixels(three, 'C'), "art lands on every page");
	fnt_free(&three);

	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/fnt";
	failures += guard_fixture(dir + "/synth_1page.fnt", 1, write_mode);
	failures += guard_fixture(dir + "/synth_3page.fnt", 3, write_mode);

	if (failures == 0) std::printf("OK: minimal generated font valid + byte-reproducible; FNT fixtures match\n");
	return failures == 0 ? 0 : 1;
}
