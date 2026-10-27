// The game's PCX decode keeping an 8-bit image's indices and palette beside its colours (engine/formats/pcx
// decode_pcx_menu_rgba's PcxIndexed, the 8-bit path of Texture_LoadPCXFromPFF32 @ 0x56EA30 and
// PCX_LoadToARGB @ 0x664cc0) over images the writer mints (encode_pcx_indexed): every texel's index and
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

// A header of `width` x `height` texels, `planes` planes, `line` bytes a line, then the bytes given.
std::vector<uint8_t> pcx_file(int width, int height, int planes, int line, const std::vector<uint8_t> &body) {
	std::vector<uint8_t> file(128, 0);
	file[0] = 0x0A;
	file[1] = 5;
	file[2] = 1;
	file[3] = 8;
	file[8] = uint8_t((width - 1) & 0xFF);
	file[9] = uint8_t(((width - 1) >> 8) & 0xFF);
	file[10] = uint8_t((height - 1) & 0xFF);
	file[11] = uint8_t(((height - 1) >> 8) & 0xFF);
	file[65] = uint8_t(planes);
	file[66] = uint8_t(line & 0xFF);
	file[67] = uint8_t((line >> 8) & 0xFF);
	file.insert(file.end(), body.begin(), body.end());
	return file;
}

// Past the data every read is a 0 literal, a column each: the decoders write the rest of the rows at once from
// where the stream is spent, as the read-by-read walk would (each case below is that walk worked by hand), and a
// header's BytesPerLine of 65535 over a 1,152-byte file decodes at once instead of walking billions of reads.
int spent_streams() {
	std::vector<uint8_t> palette(768);
	for (int i = 0; i < 768; ++i) palette[size_t(i)] = uint8_t(i / 3);
	std::string error;
	PcxIndexed game;
	RgbaImage menu;
	// A line of 3 for a width of 2: row 0 reads 1, 2, 9 (the 9 its pad, on row 1's start), row 1 reads the
	// palette's 0x0C marker (12, over the 9), then the stream is spent: row 1's last two texels and row 2's
	// three take index 0, the last one past the image.
	{
		std::vector<uint8_t> body = {1, 2, 9, 0x0C};
		body.insert(body.end(), palette.begin(), palette.end());
		const std::vector<uint8_t> file = pcx_file(2, 3, 1, 3, body);
		TEST_EXPECT(decode_pcx_menu_rgba(file.data(), file.size(), menu, error, &game) &&
		            game.indices == std::vector<uint8_t>({1, 2, 12, 0, 0, 0}));
		for (int i = 3; i < 6; ++i) TEST_EXPECT(menu.pixels[size_t(i) * 4 + 3] == 0xFF);
	}
	// A line of 2 for a width of 4: each row writes its first two texels alone; row 1's take index 0 once the
	// stream is spent, and the texels no line reaches stay unwritten (alpha 0).
	{
		std::vector<uint8_t> body = {5, 0x0C};
		body.insert(body.end(), palette.begin(), palette.end());
		const std::vector<uint8_t> file = pcx_file(4, 2, 1, 2, body);
		TEST_EXPECT(decode_pcx_menu_rgba(file.data(), file.size(), menu, error, &game) &&
		            game.indices == std::vector<uint8_t>({5, 12, 0, 0, 0, 0, 0, 0}));
		const uint8_t alpha[8] = {0xFF, 0xFF, 0, 0, 0xFF, 0xFF, 0, 0};
		for (int i = 0; i < 8; ++i) TEST_EXPECT(menu.pixels[size_t(i) * 4 + 3] == alpha[i]);
	}
	// The malformed header: 1 x 32767 texels at 65535 bytes a line over 1,024 bytes of literal 5s.
	const std::vector<uint8_t> big = pcx_file(1, 32767, 1, 65535, std::vector<uint8_t>(1024, 5));
	{
		// The menu decoder reads the bytes before the palette (the last 768): row 0's first 256 texels
		// take 5, and from there every texel takes index 0; the palette (the file's last 768 bytes) is 5s.
		TEST_EXPECT(decode_pcx_menu_rgba(big.data(), big.size(), menu, error, &game) && menu.height == 32767);
		TEST_EXPECT(game.indices.size() == 32767 && game.indices[0] == 5 && game.indices[255] == 5 &&
		            game.indices[256] == 0 && game.indices[32766] == 0);
		TEST_EXPECT(menu.pixels[0] == 5 && menu.pixels[32766 * 4] == 5 && menu.pixels[32766 * 4 + 3] == 0xFF);
	}
	{
		// The luminance decoder reads the whole file: row 0's 1,024 texels take 5, then the spent stream's zeros
		// run over the rest of the image and the whole palette behind it, so every texel decodes to 0.
		TEST_EXPECT(decode_pcx_luminance_alpha(big.data(), big.size(), menu, error) && menu.height == 32767);
		bool all_zero = true;
		for (uint8_t byte : menu.pixels) all_zero = all_zero && byte == 0;
		TEST_EXPECT(all_zero);
	}
	{
		// Three planes: row 0's line holds the 5s, every row after it reads zeros.
		std::vector<uint8_t> rgb = big;
		rgb[65] = 3;
		TEST_EXPECT(decode_pcx_menu_rgba(rgb.data(), rgb.size(), menu, error, &game) && !game.indexed);
		TEST_EXPECT(menu.pixels[0] == 5 && menu.pixels[1] == 5 && menu.pixels[2] == 5 && menu.pixels[3] == 0xFF);
		TEST_EXPECT(menu.pixels[4] == 0 && menu.pixels[32766 * 4] == 0 && menu.pixels[32766 * 4 + 3] == 0xFF);
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
	// The header as the file states it, no field range-checked; how its rows fit the reader's buffer.
	PcxHeader header;
	TEST_EXPECT(pcx_read_header(file.data(), file.size(), header) && header.bits == 8 && header.width == 2 &&
	            header.height == 2 && header.planes == 1 && header.bytes_per_line == 2);
	TEST_EXPECT(pcx_row_fit(header) == PcxRowFit::Exact);
	header.bytes_per_line = 3;
	TEST_EXPECT(pcx_row_fit(header) == PcxRowFit::Overrun);
	header.bytes_per_line = 1;
	TEST_EXPECT(pcx_row_fit(header) == PcxRowFit::Short);
	header.planes = 3;
	TEST_EXPECT(pcx_row_fit(header) == PcxRowFit::Exact);
	TEST_EXPECT(pcx_read_header(four.data(), four.size(), header) && header.bits == 4);
	TEST_EXPECT(pcx_read_header(huge.data(), huge.size(), header) && header.width == 0x7FFF);
	TEST_EXPECT(!pcx_read_header(file.data(), 127, header));
	std::vector<uint8_t> marked = file;
	marked[0] = 0x0B;
	TEST_EXPECT(!pcx_read_header(marked.data(), marked.size(), header));
	// Where the reader stops writing: each row's runs whole from width x row, to its bytes a line at least
	// [orig: Texture_LoadPCXFromPFF32 @ 0x56EDC0..0x56EDD0, the row's test @ 0x56EDE9]. The writer's rows end at
	// the width; a 64 x 64 image whose last row ends with a run of 63 at texel 63 writes 62 texels past its buffer.
	TEST_EXPECT(pcx_indexed_write_end(file.data(), file.size()) == 4);
	{
		std::vector<uint8_t> runs(file.begin(), file.begin() + 128);
		runs[8] = runs[10] = 63;
		runs[9] = runs[11] = 0;
		runs[66] = 64;
		runs[67] = 0;
		for (int row = 0; row < 63; ++row) runs.insert(runs.end(), {0xFF, 5, 5}); // 63 texels, then one
		runs.insert(runs.end(), {0xFF, 5, 0xFF, 5});                              // 63, then 63 more
		runs.insert(runs.end(), file.end() - 769, file.end());
		TEST_EXPECT(pcx_indexed_write_end(runs.data(), runs.size()) == 64 * 64 + 62);
		// The stream ends before the rows do: the rest of a row a texel a byte, its bytes a line.
		std::vector<uint8_t> cut(runs.begin(), runs.begin() + 128 + 3 * 10);
		cut.insert(cut.end(), file.end() - 769, file.end());
		TEST_EXPECT(pcx_indexed_write_end(cut.data(), cut.size()) == 64 * 64);
		runs[65] = 3;
		TEST_EXPECT(pcx_indexed_write_end(runs.data(), runs.size()) == 0);
		// A width of 0 (xmin past xmax): every row from the buffer's start, its bytes a line long.
		std::vector<uint8_t> narrow(file.begin(), file.begin() + 128);
		narrow[4] = 1;
		narrow[8] = 0;
		narrow[66] = 30;
		narrow.insert(narrow.end(), size_t(30) * 2, 0); // two rows of 30 single texels
		narrow.insert(narrow.end(), file.end() - 769, file.end());
		TEST_EXPECT(pcx_indexed_write_end(narrow.data(), narrow.size()) == 30);
	}
	TEST_EXPECT(spent_streams() == 0);
	std::printf("pcx_game_decode: the writer's indices and palette as the game reads them beside its colours, a row's "
	            "pad on the next, a 24-bit image's none, an odd width's rows of its width, the refusals, a spent "
	            "stream's rows written at once\n");
	return 0;
}
