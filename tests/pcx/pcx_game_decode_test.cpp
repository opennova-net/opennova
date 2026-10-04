// The game's PCX decode keeping an 8-bit image's indices and palette beside its colours (engine/formats/pcx
// decode_pcx_menu_rgba's PcxIndexed, the 8-bit path of Texture_LoadPCXFromPFF32 @ 0x56EA30 and
// load_pcx_to_argb @ 0x664cc0) over images the writer mints (encode_pcx_indexed): every texel's index and
// the palette as written, an odd width included (the writer's rows hold the width exactly, as every PCX
// the game ships does, so the game's decode lays no padding onto the next row); each texel's colour its
// index's palette entry, opaque; a row's pad bytes landing on the next row's start, the indices as the
// colours; a 24-bit image not indexed; and what it refuses (not 8 bits a plane, a short header, a header
// naming more texels than the file can describe, which nothing the header sizes is allocated for).
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <formats/pcx/pcx_io.h>

#include "common/test_expect.h"

using namespace opennova;

namespace {

int check(int width, int height) {
	IndexedImage8 source;
	source.width = width;
	source.height = height;
	for (int i = 0; i < width * height; ++i) source.indices.push_back(uint8_t((i * 37 + 11) & 0xFF));
	for (int i = 0; i < 256; ++i) {
		source.palette[i][0] = uint8_t(i);
		source.palette[i][1] = uint8_t(255 - i);
		source.palette[i][2] = uint8_t(i * 3);
	}
	std::vector<uint8_t> file;
	std::string error;
	TEST_EXPECT(encode_pcx_indexed(source, file, error));
	PcxIndexed game;
	RgbaImage menu;
	TEST_EXPECT(decode_pcx_menu_rgba(file.data(), file.size(), menu, error, &game) && menu.width == width);
	TEST_EXPECT(game.indexed && game.indices == source.indices);
	for (int i = 0; i < 256; ++i)
		TEST_EXPECT(game.palette[i][0] == source.palette[i][0] && game.palette[i][1] == source.palette[i][1] &&
		            game.palette[i][2] == source.palette[i][2]);
	for (size_t i = 0; i < game.indices.size(); ++i) {
		const uint8_t *colour = game.palette[game.indices[i]];
		TEST_EXPECT(menu.pixels[i * 4] == colour[0] && menu.pixels[i * 4 + 1] == colour[1] &&
		            menu.pixels[i * 4 + 2] == colour[2] && menu.pixels[i * 4 + 3] == 0xFF);
	}
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(check(8, 4) == 0);
	TEST_EXPECT(check(7, 3) == 0);
	std::vector<uint8_t> file;
	std::string error;
	IndexedImage8 tiny;
	tiny.width = tiny.height = 2;
	tiny.indices.assign(4, 1);
	TEST_EXPECT(encode_pcx_indexed(tiny, file, error));
	PcxIndexed game;
	RgbaImage menu;
	// A row of three bytes for a width of two: its pad lands on the next row's start, which the next row then
	// writes over, as the colours do.
	{
		std::vector<uint8_t> padded(file.begin(), file.begin() + 128);
		padded[66] = 3;
		padded.insert(padded.end(), {1, 2, 9, 3, 4, 8});
		padded.insert(padded.end(), file.end() - 769, file.end());
		TEST_EXPECT(decode_pcx_menu_rgba(padded.data(), padded.size(), menu, error, &game) &&
		            game.indices == std::vector<uint8_t>({1, 2, 3, 4}) && menu.pixels[8] == game.palette[3][0]);
	}
	// A 24-bit image: colours, no indices.
	{
		std::vector<uint8_t> rgb = file;
		rgb[65] = 3;
		TEST_EXPECT(decode_pcx_menu_rgba(rgb.data(), rgb.size(), menu, error, &game) && !game.indexed);
	}
	// Refused: not 8 bits a plane; a short header; more texels than the file can describe.
	std::vector<uint8_t> four = file;
	four[3] = 4;
	TEST_EXPECT(!decode_pcx_menu_rgba(four.data(), four.size(), menu, error, &game) && !error.empty());
	TEST_EXPECT(!decode_pcx_menu_rgba(file.data(), 0x45, menu, error, &game));
	std::vector<uint8_t> huge = file;
	huge[8] = huge[10] = 0xFE;
	huge[9] = huge[11] = 0x7F;
	TEST_EXPECT(!decode_pcx_menu_rgba(huge.data(), huge.size(), menu, error, &game) &&
	            error.find("more pixels") != std::string::npos && game.indices.empty());
	std::printf("pcx_game_decode: the writer's indices and palette as the game reads them beside its colours, a row's "
	            "pad on the next, a 24-bit image's none, an odd width's rows of its width, the refusals\n");
	return 0;
}
