// A true-colour TGA's carried form (TgaForm): its depth, its descriptor byte and the TGA 2.0 footer every
// shipped terrain colour map, detail blend map and tile atlas ends with. A file the writer makes reads back
// its form and, decoded the way the game's reader decodes it and written again in that form, is the same
// bytes; a file the writer does not make (an image ID, a colour map, run-length rows, a footer with an
// extension area, bytes past the pixels) reads no form.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <formats/tga/tga.h>
#include <formats/tga/tga_read.h>

#include "common/test_expect.h"

using namespace opennova::tga;

namespace {

std::vector<uint8_t> pixels(uint32_t w, uint32_t h, bool opaque) {
	std::vector<uint8_t> rgba;
	for (uint32_t i = 0; i < w * h; ++i)
		rgba.insert(rgba.end(), {uint8_t(i * 7), uint8_t(i * 13 + 1), uint8_t(i * 29 + 2), uint8_t(opaque ? 255 : i * 3)});
	return rgba;
}

// The file decoded as the game decodes it, written again in the form it read.
bool written_again(const std::vector<uint8_t> &bytes, std::vector<uint8_t> &again) {
	TgaForm form;
	TgaImage image;
	std::string error;
	return tga_read_form(bytes.data(), bytes.size(), form) &&
	       tga_decode_retail(bytes.data(), bytes.size(), image, error) &&
	       tga_write(image.rgba.data(), uint32_t(image.width), uint32_t(image.height), form, again, error);
}

} // namespace

int main() {
	std::string error;
	const std::vector<uint8_t> rgba = pixels(5, 3, false);
	const std::vector<uint8_t> opaque = pixels(5, 3, true);

	// The footer: 26 bytes after the pixels, both offsets 0, the signature.
	std::vector<uint8_t> footed;
	TEST_EXPECT(tga_write(rgba.data(), 5, 3, TgaForm{32, TGA_DESCRIPTOR_ALPHA_8, true}, footed, error));
	TEST_EXPECT(footed.size() == TGA_HEADER_SIZE + 5 * 3 * 4 + TGA_FOOTER_SIZE);
	const size_t at = TGA_HEADER_SIZE + 5 * 3 * 4;
	TEST_EXPECT(footed[at] == 0 && footed[at + 7] == 0 && std::string(footed.begin() + at + 8, footed.end()) ==
	                                                          std::string("TRUEVISION-XFILE.\0", 18));
	// Without the footer the writer is tga_write_rgba32.
	std::vector<uint8_t> plain, legacy;
	TEST_EXPECT(tga_write(rgba.data(), 5, 3, TgaForm{}, plain, error) && tga_write_rgba32(rgba.data(), 5, 3, legacy, error) &&
	            plain == legacy);
	TEST_EXPECT(tga_write(opaque.data(), 5, 3, TgaForm{24, 0, false}, plain, error) &&
	            tga_write_rgb24(opaque.data(), 5, 3, legacy, error) && plain == legacy);

	// Each form reads back and writes again the same bytes: 32 and 24 bits, with and without the footer, a
	// descriptor carried as it was (the origin bit no game reader honours included).
	for (const TgaForm form : {TgaForm{32, 8, true}, TgaForm{32, 8, false}, TgaForm{24, 0, true}, TgaForm{24, 0, false},
	                           TgaForm{32, 0x28, true}, TgaForm{24, 0x03, true}}) {
		std::vector<uint8_t> bytes, again;
		TEST_EXPECT(tga_write(form.bits == 32 ? rgba.data() : opaque.data(), 5, 3, form, bytes, error));
		TgaForm read;
		TEST_EXPECT(tga_read_form(bytes.data(), bytes.size(), read) && read.bits == form.bits &&
		            read.descriptor == form.descriptor && read.footer == form.footer);
		TEST_EXPECT(written_again(bytes, again) && again == bytes);
	}

	// Files the writer does not make read no form.
	TgaForm none;
	std::vector<uint8_t> odd = footed;
	odd[0] = 1; // an image ID
	TEST_EXPECT(!tga_read_form(odd.data(), odd.size(), none));
	odd = footed;
	odd[1] = 1; // a colour map
	TEST_EXPECT(!tga_read_form(odd.data(), odd.size(), none));
	odd = footed;
	odd[2] = 10; // run-length rows
	TEST_EXPECT(!tga_read_form(odd.data(), odd.size(), none));
	odd = footed;
	odd[at] = 0x40; // an extension area named
	TEST_EXPECT(!tga_read_form(odd.data(), odd.size(), none));
	odd = footed;
	odd.push_back(0); // a byte past the footer
	TEST_EXPECT(!tga_read_form(odd.data(), odd.size(), none));
	odd = footed;
	odd.resize(at + 3); // a footer cut short
	TEST_EXPECT(!tga_read_form(odd.data(), odd.size(), none));
	odd = footed;
	odd[16] = 16; // a 16-bit depth
	TEST_EXPECT(!tga_read_form(odd.data(), odd.size(), none));
	TEST_EXPECT(!tga_write(rgba.data(), 5, 3, TgaForm{16, 0, false}, odd, error) && odd.empty());

	std::printf("tga_roundtrip: the depth, the descriptor and the TGA 2.0 footer carried; other files read no form\n");
	return 0;
}
