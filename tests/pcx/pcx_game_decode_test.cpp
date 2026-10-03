// The game's PCX decode keeping an 8-bit image's indices and palette (engine/formats/pcx
// decode_pcx_game, the 8-bit path of Texture_LoadPCXFromPFF32 @ 0x56EA30 and load_pcx_to_argb @
// 0x664cc0) over images the writer mints (encode_pcx_indexed): every texel's index and the palette as
// written, an odd width included (the writer's rows hold the width exactly, as every PCX the game ships
// does, so the game's decode lays no padding onto the next row); each texel's palette colour exactly the
// colour decode_pcx_menu_rgba gives it, opaque; the header's facts; and what it refuses (not 8 bits a
// plane, a short header).
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
	PcxGameImage game;
	TEST_EXPECT(decode_pcx_game(file.data(), file.size(), game, error));
	TEST_EXPECT(game.indexed && game.width == width && game.height == height && game.planes == 1 && game.bits == 8 &&
	            game.rle && game.palette_marker && game.bytes_per_line == width);
	TEST_EXPECT(game.indices == source.indices);
	for (int i = 0; i < 256; ++i)
		TEST_EXPECT(game.palette[i][0] == source.palette[i][0] && game.palette[i][1] == source.palette[i][1] &&
		            game.palette[i][2] == source.palette[i][2]);
	RgbaImage menu;
	TEST_EXPECT(decode_pcx_menu_rgba(file.data(), file.size(), menu, error) && menu.width == width);
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
	// Refused: not 8 bits a plane; a short header.
	std::vector<uint8_t> file;
	std::string error;
	IndexedImage8 tiny;
	tiny.width = tiny.height = 2;
	tiny.indices.assign(4, 1);
	TEST_EXPECT(encode_pcx_indexed(tiny, file, error));
	PcxGameImage game;
	std::vector<uint8_t> four = file;
	four[3] = 4;
	TEST_EXPECT(!decode_pcx_game(four.data(), four.size(), game, error) && !error.empty());
	TEST_EXPECT(!decode_pcx_game(file.data(), 0x45, game, error));
	std::printf("pcx_game_decode: the writer's indices and palette as the game reads them, the colours "
	            "decode_pcx_menu_rgba gives, an odd width's rows of its width\n");
	return 0;
}
