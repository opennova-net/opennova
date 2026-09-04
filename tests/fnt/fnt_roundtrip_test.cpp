// FNT reader/writer over the synthetic fixtures fixtures/fnt/synth_1page.fnt
// and synth_3page.fnt (tests/fixtures/minimal_fnt_gen.cpp): header/page
// parsing, the multi-page glyph lookup, the D-FNT-1/2 design-width
// contract, and a deterministic blank-font write/reload.
#include <formats/fnt/fnt.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova::fnt;

#ifndef OPENNOVA_SOURCE_DIR
#define OPENNOVA_SOURCE_DIR "."
#endif

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

std::vector<uint8_t> read_file(const std::string &path) {
	std::ifstream f(path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool parse_fixture(const char *name, uint32_t expected_pages) {
	const std::string path = std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/fnt/" + name;
	std::vector<uint8_t> bytes = read_file(path);
	if (!expect(!bytes.empty(), "fixture should be readable")) return false;

	uint32_t page_count = 0;
	int32_t glyph_spacing = 123;
	fnt_error_t err = fnt_parse_header(bytes.data(), bytes.size(), &page_count, &glyph_spacing);
	if (!expect(err == FNT_OK, "header should parse")) return false;
	if (!expect(page_count == expected_pages, "page count should match fixture size")) return false;
	if (!expect(bytes.size() == fnt_calculate_file_size(page_count), "fixture size should match declared page count")) return false;

	fnt_font_t font;
	err = fnt_parse(bytes.data(), bytes.size(), &font);
	if (!expect(err == FNT_OK, "font should parse")) return false;
	if (!expect(font.num_pages == expected_pages, "parsed font should keep page count")) return false;
	if (!expect(fnt_get_page_data(&font, 0) != nullptr, "parsed font should expose owned page data")) return false;

	const fnt_glyph_t *space = fnt_get_glyph(&font, 32);
	const fnt_glyph_t *last = fnt_get_glyph(&font, 255);
	if (!expect(space != nullptr && last != nullptr, "glyph lookup should cover codes 32 through 255")) return false;
	if (!expect(fnt_get_glyph(&font, 31) == nullptr, "glyph lookup should reject codes before 32")) return false;

	// The builder spreads slot i over page (i % pages): 'A' (slot 33), 'B'
	// (34), 'C' (35) land on pages 0, 1 and 2 of the three-page fixture and all
	// on page 0 of the one-page fixture.
	if (!expect(fnt_get_glyph(&font, 'A')->page == 33 % expected_pages &&
	                    fnt_get_glyph(&font, 'B')->page == 34 % expected_pages &&
	                    fnt_get_glyph(&font, 'C')->page == 35 % expected_pages,
	            "glyph pages follow the builder's slot % pages layout"))
		return false;

	// Every glyph cell is 12 px wide by 16 px tall in the builder's layout.
	int width = 0;
	int height = 0;
	fnt_get_glyph_size(fnt_get_glyph(&font, 'A'), &width, &height);
	if (!expect(width == 12 && height == 16, "glyph rect matches the builder's 12x16 cell")) return false;

	fnt_free(&font);
	return true;
}

} // namespace

int main() {
	if (!parse_fixture("synth_1page.fnt", 1)) return 1;
	if (!parse_fixture("synth_3page.fnt", 3)) return 1;

	// D-FNT-1/2: the +4 word is the DESIGN WIDTH, not a validated version. The
	// engine reads it as the 800/dw glyph scale and never rejects a non-800 font
	// [orig: GameFont_LoadFromBlob @ 0x674740]. Our reader must match: accept + carry it.
	{
		std::vector<uint8_t> bytes =
		    read_file(std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/fnt/synth_1page.fnt");
		if (!expect(!bytes.empty() && bytes.size() >= 8, "fixture readable for D-FNT-1")) return 1;
		fnt_font_t f0;
		if (!expect(fnt_parse(bytes.data(), bytes.size(), &f0) == FNT_OK, "800-design font parses")) return 1;
		if (!expect(fnt_design_scale(f0.design_width) == 1.0f, "the writer's 800 design width -> scale 1.0")) return 1;
		fnt_free(&f0);

		// Rewrite +4 as a non-800 design width; retail would scale it, so must we.
		bytes[4] = 0x00; bytes[5] = 0x04; bytes[6] = 0x00; bytes[7] = 0x00; // 1024 LE
		fnt_font_t f1;
		fnt_error_t err1024 = fnt_parse(bytes.data(), bytes.size(), &f1);
		if (!expect(err1024 == FNT_OK, "non-800 design width must NOT be rejected (D-FNT-1)")) return 1;
		if (!expect(f1.design_width == 1024u, "reader carries the file's design width (D-FNT-2)")) return 1;
		if (!expect(fnt_design_scale(f1.design_width) == 800.0f / 1024.0f, "design scale = 800/dw")) return 1;
		fnt_free(&f1);
	}

	fnt_font_t font;
	fnt_error_t err = fnt_init_blank(&font, 1, -3);
	if (!expect(err == FNT_OK, "blank font should initialize")) return 1;

	font.glyphs[0].page = 0;
	font.glyphs[0].uv.u0 = 0.0f;
	font.glyphs[0].uv.v0 = 0.0f;
	font.glyphs[0].uv.u1 = 4.0f / FNT_TEXTURE_WIDTH;
	font.glyphs[0].uv.v1 = 3.0f / FNT_TEXTURE_HEIGHT;

	uint8_t *page = fnt_get_page_data(&font, 0);
	if (!expect(page != nullptr, "blank font page should be mutable")) return 1;
	page[0] = 12;
	page[1] = 34;
	page[2] = 56;
	page[3] = 200;

	std::vector<uint8_t> written(fnt_calculate_file_size(font.num_pages));
	size_t written_size = 0;
	err = fnt_write(&font, written.data(), written.size(), &written_size);
	if (!expect(err == FNT_OK, "blank font should write")) return 1;
	if (!expect(written_size == written.size(), "writer should report exact output size")) return 1;
	if (!expect(written[16] == 0 && written[31] == 0, "reserved header bytes should be canonical zero")) return 1;

	const size_t first_pixel = FNT_TOTAL_HEADER;
	if (!expect(written[first_pixel + 0] == 255 && written[first_pixel + 1] == 255 && written[first_pixel + 2] == 255,
	            "writer should normalize page RGB to white")) return 1;
	if (!expect(written[first_pixel + 3] == 200, "writer should preserve alpha mask values")) return 1;

	fnt_font_t reparsed;
	err = fnt_parse(written.data(), written.size(), &reparsed);
	if (!expect(err == FNT_OK, "written font should reparse")) return 1;
	if (!expect(reparsed.glyph_spacing == -3, "glyph spacing should round-trip")) return 1;
	int width = 0;
	int height = 0;
	fnt_get_glyph_size(&reparsed.glyphs[0], &width, &height);
	if (!expect(width == 4 && height == 3, "glyph rect should round-trip")) return 1;

	fnt_free(&reparsed);
	fnt_free(&font);
	std::printf("OK: FNT fixtures parse and deterministic write/reload works\n");
	return 0;
}
