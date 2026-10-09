// The game's TGA reader (formats/tga/tga_read.h): every row flipped whatever the
// descriptor says, the pixels right after the header and image ID, the type and depth
// forms it reads and the ones it reads as zeros, the run-length packets.
// [orig: CTerrainTileData_LoadTGAFromArchive @ 0x56E570]

#include <formats/tga/tga_read.h>

#include <cstdio>
#include <string>
#include <vector>

using opennova::tga::TgaImage;
using opennova::tga::tga_decode_retail;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);       \
			++failures;                                                        \
		}                                                                      \
	} while (0)

// An 18-byte header plus `id` bytes of image ID.
std::vector<uint8_t> header(uint8_t type, int width, int height, uint8_t depth, uint8_t descriptor,
		size_t id = 0, uint16_t map_length = 0, uint8_t map_depth = 0) {
	std::vector<uint8_t> out(18 + id, 0x5A);
	out[0] = static_cast<uint8_t>(id);
	out[1] = map_length ? 1 : 0;
	out[2] = type;
	out[3] = 0;
	out[4] = 0;
	out[5] = static_cast<uint8_t>(map_length & 0xFF);
	out[6] = static_cast<uint8_t>(map_length >> 8);
	out[7] = map_depth;
	for (int i = 8; i < 12; ++i) out[static_cast<size_t>(i)] = 0;
	out[12] = static_cast<uint8_t>(width & 0xFF);
	out[13] = static_cast<uint8_t>(width >> 8);
	out[14] = static_cast<uint8_t>(height & 0xFF);
	out[15] = static_cast<uint8_t>(height >> 8);
	out[16] = depth;
	out[17] = descriptor;
	return out;
}

// RGBA at (x, y), the top row 0.
std::vector<uint8_t> pixel(const TgaImage &image, int x, int y) {
	const size_t at = (static_cast<size_t>(y) * image.width + x) * 4;
	return {image.rgba[at], image.rgba[at + 1], image.rgba[at + 2], image.rgba[at + 3]};
}

using Rgba = std::vector<uint8_t>;

TgaImage decode(const std::vector<uint8_t> &bytes) {
	TgaImage image;
	std::string error;
	if (!tga_decode_retail(bytes.data(), bytes.size(), image, error))
		std::fprintf(stderr, "  decode: %s\n", error.c_str());
	return image;
}

// A 1x2 true-colour file: first stored row red, second green (B, G, R order).
std::vector<uint8_t> two_rows(uint8_t descriptor) {
	std::vector<uint8_t> bytes = header(2, 1, 2, 24, descriptor);
	for (uint8_t b : {0, 0, 255, 0, 255, 0}) bytes.push_back(b);
	return bytes;
}

void test_rows_always_flipped() {
	// The first stored row is the bottom one, origin bit or not
	// [orig: the flip @ 0x56E995..0x56E9EA; byte 17 never read].
	for (uint8_t descriptor : {uint8_t(0x00), uint8_t(0x20), uint8_t(0x28)}) {
		const TgaImage image = decode(two_rows(descriptor));
		CHECK(image.width == 1 && image.height == 2, "a 1x2 image");
		CHECK(pixel(image, 0, 0) == Rgba({0, 255, 0, 255}), "the second stored row draws on top");
		CHECK(pixel(image, 0, 1) == Rgba({255, 0, 0, 255}), "the first stored row draws at the bottom");
	}
}

void test_pixels_follow_the_image_id_not_the_colour_map() {
	// A true-colour file carrying a 2-entry colour map: the reader starts at 18 + ID
	// length, so it reads the map's bytes as pixels [orig: @ 0x56E6BA].
	std::vector<uint8_t> bytes = header(2, 1, 1, 24, 0, 3, 2, 24);
	for (uint8_t b : {10, 20, 30, 40, 50, 60}) bytes.push_back(b); // the map
	for (uint8_t b : {1, 2, 3}) bytes.push_back(b);                 // the pixel
	const TgaImage image = decode(bytes);
	CHECK(pixel(image, 0, 0) == Rgba({30, 20, 10, 255}), "the map's first entry reads as the pixel");
}

void test_forms() {
	// 32-bit keeps alpha.
	std::vector<uint8_t> bytes = header(2, 1, 1, 32, 8);
	for (uint8_t b : {1, 2, 3, 4}) bytes.push_back(b);
	CHECK(pixel(decode(bytes), 0, 0) == Rgba({3, 2, 1, 4}), "32-bit B, G, R, A");
	// 16-bit true colour reads as zeros [orig: @ 0x56E979].
	bytes = header(2, 1, 1, 16, 0);
	bytes.push_back(0xFF);
	bytes.push_back(0x7F);
	CHECK(pixel(decode(bytes), 0, 0) == Rgba({0, 0, 0, 0}), "16-bit true colour is transparent black");
	// Colour-mapped with a 24-bit map: index 1 -> the map's second entry, opaque.
	bytes = header(1, 1, 1, 8, 0, 0, 2, 24);
	for (uint8_t b : {9, 9, 9, 7, 8, 9}) bytes.push_back(b);
	bytes.push_back(1);
	CHECK(pixel(decode(bytes), 0, 0) == Rgba({9, 8, 7, 255}), "a 24-bit map entry, opaque");
	// A 16-bit map reads as zeros [orig: @ 0x56E6D9].
	bytes = header(1, 1, 1, 8, 0, 0, 1, 16);
	for (uint8_t b : {0xFF, 0x7F, 0}) bytes.push_back(b);
	CHECK(pixel(decode(bytes), 0, 0) == Rgba({0, 0, 0, 0}), "a 16-bit map reads as zeros");
	// 8-bit grey, opaque.
	bytes = header(3, 1, 1, 8, 0);
	bytes.push_back(77);
	CHECK(pixel(decode(bytes), 0, 0) == Rgba({77, 77, 77, 255}), "8-bit grey");
	// Types 9 and 11 read as zeros [orig: @ 0x56E73F, @ 0x56E971].
	for (uint8_t type : {uint8_t(9), uint8_t(11)}) {
		bytes = header(type, 1, 1, 8, 0);
		bytes.push_back(0x80);
		bytes.push_back(77);
		CHECK(pixel(decode(bytes), 0, 0) == Rgba({0, 0, 0, 0}), "run-length colour-mapped and grey read as zeros");
	}
	// A short header fails; a zero side fails.
	TgaImage image;
	std::string error;
	CHECK(!tga_decode_retail(bytes.data(), 17, image, error), "a short header fails");
	bytes = header(2, 0, 1, 24, 0);
	CHECK(!tga_decode_retail(bytes.data(), bytes.size(), image, error), "no pixels fails");
}

void test_run_length() {
	// 3x1 RLE 24-bit: a repeat packet of 2, then a raw packet of 1
	// [orig: case 10 @ 0x56E7FB].
	std::vector<uint8_t> bytes = header(10, 3, 1, 24, 0);
	for (uint8_t b : {0x81, 1, 2, 3, 0x00, 4, 5, 6}) bytes.push_back(b);
	const TgaImage image = decode(bytes);
	CHECK(pixel(image, 0, 0) == Rgba({3, 2, 1, 255}) && pixel(image, 1, 0) == Rgba({3, 2, 1, 255}),
			"the repeat packet");
	CHECK(pixel(image, 2, 0) == Rgba({6, 5, 4, 255}), "the raw packet");
	// 32-bit: a repeat packet clipped at the image's end keeps alpha.
	bytes = header(10, 2, 1, 32, 8);
	for (uint8_t b : {0x83, 1, 2, 3, 9}) bytes.push_back(b);
	const TgaImage clipped = decode(bytes);
	CHECK(pixel(clipped, 1, 0) == Rgba({3, 2, 1, 9}), "a 32-bit run clipped at the end");
	// A file shorter than its pixels reads the rest as zeros.
	bytes = header(2, 2, 1, 24, 0);
	for (uint8_t b : {1, 2, 3}) bytes.push_back(b);
	CHECK(pixel(decode(bytes), 1, 0) == Rgba({0, 0, 0, 255}), "bytes past the file read as 0");
}

void test_allocation_bounded_by_the_data() {
	// A header naming 32767 x 32767 pixels over a few bytes fails before any
	// header-sized buffer is allocated (retail's allocation failing, load code 2).
	std::vector<uint8_t> bytes = header(2, 32767, 32767, 32, 8);
	for (int i = 0; i < 12; ++i) bytes.push_back(0x7F);
	TgaImage image;
	std::string error;
	CHECK(!tga_decode_retail(bytes.data(), bytes.size(), image, error), "a bogus header fails");
	CHECK(image.rgba.empty(), "nothing allocated");
	int w = 0, h = 0;
	CHECK(!opennova::tga::tga_retail_size(bytes.data(), bytes.size(), w, h, error), "the size check fails too");
	// 32 pixels a byte stays inside the bound: a 64-pixel run-length image in 4 bytes.
	bytes = header(10, 64, 1, 24, 0);
	for (uint8_t b : {0xFF, 1, 2, 3}) bytes.push_back(b);
	CHECK(tga_decode_retail(bytes.data(), bytes.size(), image, error) && image.width == 64,
			"a short run-length file expanding to its pixels decodes");
}

void test_particle_loose_leg() {
	using opennova::tga::TgaReaderForm;
	// The loose leg's flip steps the bottom rows by the height: a 3 x 2 image's row 0
	// swaps with the words from 2 x 1 = 2 on [orig: CTextureData_LoadTGA @ 0x5F7FCD].
	std::vector<uint8_t> bytes = header(3, 3, 2, 8, 0);
	for (uint8_t b : {10, 11, 12, 13, 14, 15}) bytes.push_back(b);
	TgaImage loose, archive;
	std::string error;
	CHECK(tga_decode_retail(bytes.data(), bytes.size(), loose, error, TgaReaderForm::ParticleLoose),
			"the loose leg decodes");
	CHECK(tga_decode_retail(bytes.data(), bytes.size(), archive, error), "the archive reader decodes");
	const uint8_t want_loose[6] = {12, 13, 14, 11, 10, 15};
	const uint8_t want_archive[6] = {13, 14, 15, 10, 11, 12};
	bool loose_ok = true, archive_ok = true;
	for (int i = 0; i < 6; ++i) {
		loose_ok = loose_ok && loose.rgba[static_cast<size_t>(i) * 4] == want_loose[i];
		archive_ok = archive_ok && archive.rgba[static_cast<size_t>(i) * 4] == want_archive[i];
	}
	CHECK(loose_ok, "the height-stride flip garbles a non-square image");
	CHECK(archive_ok, "the archive reader flips whole rows");
	// The loose leg reads the sides unsigned: 0x8000 wide is 32768 there, negative in
	// the archive reader.
	bytes = header(3, 0x8000, 1, 8, 0);
	for (int i = 0; i < 0x8000; ++i) bytes.push_back(static_cast<uint8_t>(i));
	CHECK(!tga_decode_retail(bytes.data(), bytes.size(), archive, error), "signed: no pixels");
	CHECK(tga_decode_retail(bytes.data(), bytes.size(), loose, error, TgaReaderForm::ParticleLoose) &&
					loose.width == 0x8000,
			"unsigned: 32768 wide");
}

opennova::tga::TgaHeader read_header(const std::vector<uint8_t> &bytes) {
	opennova::tga::TgaHeader h;
	opennova::tga::tga_read_header(bytes.data(), bytes.size(), h);
	return h;
}

void test_retail_form() {
	using opennova::tga::TgaRetailForm;
	using opennova::tga::tga_retail_form;
	// The forms the reader decodes, zeroes, and leaves unset.
	CHECK(tga_retail_form(read_header(header(2, 2, 2, 24, 0))) == TgaRetailForm::Decoded, "2 at 24 decoded");
	CHECK(tga_retail_form(read_header(header(10, 2, 2, 32, 0))) == TgaRetailForm::Decoded, "10 at 32 decoded");
	CHECK(tga_retail_form(read_header(header(3, 2, 2, 8, 0))) == TgaRetailForm::Decoded, "3 at 8 decoded");
	CHECK(tga_retail_form(read_header(header(1, 2, 2, 8, 0, 0, 4, 24))) == TgaRetailForm::Decoded, "1 with a 24-bit map");
	CHECK(tga_retail_form(read_header(header(1, 2, 2, 8, 0, 0, 4, 32))) == TgaRetailForm::Zeroed, "1 with a 32-bit map zeroed");
	CHECK(tga_retail_form(read_header(header(2, 2, 2, 16, 0))) == TgaRetailForm::Zeroed, "2 at 16 zeroed");
	CHECK(tga_retail_form(read_header(header(10, 2, 2, 16, 0))) == TgaRetailForm::Zeroed, "10 at 16 zeroed");
	CHECK(tga_retail_form(read_header(header(9, 2, 2, 8, 0))) == TgaRetailForm::Zeroed &&
			tga_retail_form(read_header(header(11, 2, 2, 8, 0))) == TgaRetailForm::Zeroed, "9 and 11 zeroed");
	CHECK(tga_retail_form(read_header(header(3, 2, 2, 16, 0))) == TgaRetailForm::Unset, "3 at 16 unset");
	CHECK(tga_retail_form(read_header(header(7, 2, 2, 24, 0))) == TgaRetailForm::Unset, "an unknown type unset");
}

void test_reads_past_end() {
	using opennova::tga::tga_reads_past_end;
	// 2 x 2 at 32 bits: 16 texel bytes after the header and its 3-byte ID.
	std::vector<uint8_t> whole = header(2, 2, 2, 32, 0, 3);
	whole.resize(whole.size() + 16, 0);
	CHECK(!tga_reads_past_end(read_header(whole), whole.data(), whole.size()), "every texel held");
	std::vector<uint8_t> cut = whole;
	cut.pop_back();
	CHECK(tga_reads_past_end(read_header(cut), cut.data(), cut.size()), "a byte short");
	// A zeroed form reads nothing.
	const std::vector<uint8_t> zeroed = header(2, 2, 2, 16, 0);
	CHECK(!tga_reads_past_end(read_header(zeroed), zeroed.data(), zeroed.size()), "a zeroed form reads nothing");
	// Run-length: a run of 4 reads one pixel; a raw packet copies each.
	std::vector<uint8_t> run = header(10, 2, 2, 24, 0);
	run.insert(run.end(), {0x83, 1, 2, 3});
	CHECK(!tga_reads_past_end(read_header(run), run.data(), run.size()), "one run holds four pixels");
	std::vector<uint8_t> raw = header(10, 2, 2, 24, 0);
	raw.insert(raw.end(), {0x03, 1, 2, 3, 4, 5, 6, 7, 8, 9});
	CHECK(tga_reads_past_end(read_header(raw), raw.data(), raw.size()), "a raw packet short of its pixels");
	// A colour-mapped file: its 24-bit map and its indices.
	std::vector<uint8_t> mapped = header(1, 2, 2, 8, 0, 0, 2, 24);
	mapped.resize(mapped.size() + 6 + 4, 0);
	CHECK(!tga_reads_past_end(read_header(mapped), mapped.data(), mapped.size()), "a map and its indices held");
	mapped.pop_back();
	CHECK(tga_reads_past_end(read_header(mapped), mapped.data(), mapped.size()), "an index short");
}

void test_colour_map_misread() {
	using opennova::tga::tga_colour_map_misread;
	CHECK(tga_colour_map_misread(read_header(header(2, 2, 2, 24, 0, 0, 4, 24))), "a true-colour file's map not skipped");
	CHECK(!tga_colour_map_misread(read_header(header(2, 2, 2, 24, 0))), "no map");
	CHECK(!tga_colour_map_misread(read_header(header(1, 2, 2, 8, 0, 0, 4, 24))), "a colour-mapped file's map is read");
}

} // namespace

int main() {
	test_retail_form();
	test_reads_past_end();
	test_colour_map_misread();
	test_rows_always_flipped();
	test_pixels_follow_the_image_id_not_the_colour_map();
	test_forms();
	test_run_length();
	test_allocation_bounded_by_the_data();
	test_particle_loose_leg();
	if (failures != 0) {
		std::fprintf(stderr, "tga_read: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("tga_read: ok\n");
	return 0;
}
