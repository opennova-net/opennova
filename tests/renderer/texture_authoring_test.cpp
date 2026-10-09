// The image import's pieces (runtime/renderer/texture_authoring.h; split from the editor's texture import
// test): the settings an import's options make, each fallback; the option forms (threshold:, key:, <W>x<H>,
// fit:); the sizes asked, a halving by 2 x 2 boxes, an area average, each alpha, a green flipped; every
// format written and read back by the game's reader of its kind (a 24-bit and a 32-bit TGA, an MDT, a
// 24-bit and an 8-bit PCX, a DXT5 DDS with its whole chain each level the authoring encoder's, a DXT1, an
// A8R8G8B8, a PNG); a TGA and a PCX as sources (read as an image program reads them, never through the
// game's readers' faults: a TGA's origin, a 16-bit TGA, an odd PCX's pad bytes, a PCX's indices kept);
// a height map's brightness into the alpha; a source's alpha told by its header.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/png/png_decode.h>
#include <formats/png/png_encode.h>
#include <formats/tga/tga.h>
#include <formats/tga/tga_read.h>
#include <runtime/renderer/dxt_encode.h>
#include <runtime/renderer/texture_authoring.h>
#include <runtime/renderer/texture_dxt.h>

#include "common/test_expect.h"

using namespace opennova;
using namespace opennova::renderer;

namespace {

using Options = std::map<std::string, std::string>;

// A `w` x `h` image of graded colours, its alpha falling left to right.
RgbaImage graded(int w, int h) {
	RgbaImage image;
	image.width = w;
	image.height = h;
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			image.pixels.push_back(uint8_t(x * 255 / std::max(1, w - 1)));
			image.pixels.push_back(uint8_t(y * 255 / std::max(1, h - 1)));
			image.pixels.push_back(uint8_t((x + y) * 16));
			image.pixels.push_back(uint8_t(255 - x * 255 / std::max(1, w - 1)));
		}
	return image;
}

ImageImportSettings settings_of(const Options &options) { return image_import_settings(options); }

bool opaque(const std::vector<uint8_t> &rgba) {
	for (size_t i = 3; i < rgba.size(); i += 4)
		if (rgba[i] != 255) return false;
	return true;
}

int test_settings() {
	// Every option left out its fallback; a token read in lower case, a file name as it is.
	ImageImportSettings settings = settings_of({});
	TEST_EXPECT(settings.format == "tga" && settings.name.empty() && settings.alpha == "source" && settings.size == "source" &&
	            settings.palette == "median_cut" && settings.dds == "dxt5" && settings.mips == "full" &&
	            settings.green == "game" && settings.normal == "normal");
	settings = settings_of({{"format", "DDS"}, {"name", "Body.dds"}, {"alpha", ""}, {"dds", "DXT1"}});
	TEST_EXPECT(settings.format == "dds" && settings.name == "Body.dds" && settings.alpha == "source" && settings.dds == "dxt1");
	TEST_EXPECT(image_format_extension("tga24") == ".tga" && image_format_extension("pcx24") == ".pcx" &&
	            image_format_extension("mdt") == ".mdt" && image_format_extension("dds") == ".dds");
	// The written forms beside the named values.
	TEST_EXPECT(image_alpha_form("threshold:128") && image_alpha_form("key:#FF00ff") && !image_alpha_form("threshold:300") &&
	            !image_alpha_form("key:red") && !image_alpha_form("opaque"));
	TEST_EXPECT(image_size_form("512x256") && image_size_form("fit:1024x1024") && !image_size_form("512") &&
	            !image_size_form("0x4") && !image_size_form("pow2_down") && !image_size_form("20000x1"));
	std::printf("settings: the fallbacks, the extensions, the written forms\n");
	return 0;
}

int test_pieces() {
	uint32_t w = 0, h = 0;
	std::string why;
	TEST_EXPECT(image_target_size("pow2_down", 300, 129, w, h, why) && w == 256 && h == 128);
	TEST_EXPECT(image_target_size("pow2_up", 300, 129, w, h, why) && w == 512 && h == 256);
	TEST_EXPECT(image_target_size("fit:512x512", 1024, 256, w, h, why) && w == 512 && h == 128);
	TEST_EXPECT(image_target_size("fit:2048x2048", 100, 50, w, h, why) && w == 100 && h == 50);
	TEST_EXPECT(image_target_size("64x32", 7, 7, w, h, why) && w == 64 && h == 32);
	TEST_EXPECT(!image_target_size("huge", 7, 7, w, h, why) && !why.empty());
	// Halved: each texel the 2 x 2 box's sum shifted by two.
	const RgbaImage source = graded(4, 4);
	const RgbaImage half = resize_image(source, 2, 2);
	TEST_EXPECT(half.width == 2 && half.height == 2);
	for (int c = 0; c < 4; ++c) {
		const uint32_t sum = uint32_t(source.pixels[size_t(c)]) + source.pixels[size_t(4 + c)] + source.pixels[size_t(16 + c)] +
		                     source.pixels[size_t(20 + c)];
		TEST_EXPECT(half.pixels[size_t(c)] == uint8_t(sum >> 2));
	}
	// A third: each texel the average of what it covers; a size up repeats texels.
	RgbaImage flat;
	flat.width = 3;
	flat.height = 1;
	flat.pixels = {0, 0, 0, 255, 90, 90, 90, 255, 180, 180, 180, 255};
	const RgbaImage one = resize_image(flat, 1, 1);
	TEST_EXPECT(one.pixels == std::vector<uint8_t>({90, 90, 90, 255}));
	const RgbaImage up = resize_image(flat, 6, 1);
	TEST_EXPECT(up.pixels[0] == 0 && up.pixels[4] == 0 && up.pixels[8] == 90 && up.pixels[20] == 180);
	// The alphas.
	RgbaImage image = graded(4, 1);
	TEST_EXPECT(apply_image_alpha(image, "opaque", why) && image.pixels[3] == 255 && image.pixels[15] == 255);
	image = graded(4, 1);
	TEST_EXPECT(apply_image_alpha(image, "threshold:128", why) && image.pixels[3] == 255 && image.pixels[15] == 0);
	image = graded(4, 1);
	TEST_EXPECT(apply_image_alpha(image, "luminance", why) &&
	            image.pixels[7] == uint8_t((85u * (uint32_t(image.pixels[4]) + image.pixels[5] + image.pixels[6])) >> 8));
	image = graded(4, 1);
	const uint8_t key[3] = {image.pixels[4], image.pixels[5], image.pixels[6]};
	char hex[16];
	std::snprintf(hex, sizeof(hex), "key:#%02X%02X%02X", key[0], key[1], key[2]);
	TEST_EXPECT(apply_image_alpha(image, hex, why) && image.pixels[7] == 0 && image.pixels[3] == 255);
	TEST_EXPECT(!apply_image_alpha(image, "glow", why) && !why.empty());
	image = graded(2, 1);
	const uint8_t green = image.pixels[1];
	flip_image_green(image);
	TEST_EXPECT(image.pixels[1] == uint8_t(255 - green));
	// The texels an import encodes: resized, then its alpha made; a value no option takes names its option.
	image = graded(4, 4);
	std::string field;
	TEST_EXPECT(image_import_texels(image, settings_of({{"size", "2x2"}, {"alpha", "opaque"}}), why, field) &&
	            image.width == 2 && opaque(image.pixels));
	image = graded(4, 4);
	TEST_EXPECT(!image_import_texels(image, settings_of({{"alpha", "glow"}}), why, field) && field == "alpha");
	TEST_EXPECT(!image_import_texels(image, settings_of({{"size", "huge"}}), why, field) && field == "size");
	std::printf("pieces: the sizes, a halving, an average, the alphas, the green, the texels an import encodes\n");
	return 0;
}

int test_formats() {
	const RgbaImage image = graded(8, 8);
	std::vector<uint8_t> bytes;
	std::string why, note;
	// A 24-bit TGA: three bytes a texel, bottom row first, read opaque by the game's reader.
	TEST_EXPECT(encode_image(image, settings_of({{"format", "tga24"}}), bytes, why, note));
	tga::TgaHeader header;
	TEST_EXPECT(tga::tga_read_header(bytes.data(), bytes.size(), header) && header.image_type == 2 && header.bits == 24 &&
	            header.descriptor == 0 && bytes.size() == 18 + 8 * 8 * 3);
	tga::TgaImage tga;
	TEST_EXPECT(tga::tga_decode_retail(bytes.data(), bytes.size(), tga, why) && tga.width == 8 && opaque(tga.rgba) &&
	            tga.rgba[0] == image.pixels[0] && tga.rgba[1] == image.pixels[1]);
	// Its alpha dropped is said (the image is translucent).
	TEST_EXPECT(note.find("24-bit TGA carries no alpha") != std::string::npos);
	// A 24-bit PCX: three planes of the colours as they are, read so; the alpha dropped and said.
	note.clear();
	TEST_EXPECT(encode_image(image, settings_of({{"format", "pcx24"}}), bytes, why, note) && bytes.size() > 128 && bytes[65] == 3 &&
	            note.find("carries no alpha") != std::string::npos);
	RgbaImage pcx;
	TEST_EXPECT(decode_pcx_menu_rgba(bytes.data(), bytes.size(), pcx, why) && pcx.width == 8);
	for (size_t i = 0; i < image.pixels.size() && i < pcx.pixels.size(); i += 4)
		TEST_EXPECT(pcx.pixels[i] == image.pixels[i] && pcx.pixels[i + 1] == image.pixels[i + 1] &&
		            pcx.pixels[i + 2] == image.pixels[i + 2]);
	// A 32-bit TGA keeps the alpha; an MDT is its bytes.
	TEST_EXPECT(encode_image(image, settings_of({}), bytes, why, note));
	TEST_EXPECT(tga::tga_decode_retail(bytes.data(), bytes.size(), tga, why) && tga.rgba == image.pixels);
	std::vector<uint8_t> mdt;
	TEST_EXPECT(encode_image(image, settings_of({{"format", "mdt"}}), mdt, why, note) && mdt == bytes);
	// A PCX: the colours kept (64 here), the alpha dropped and said; exact refused past 256 colours.
	note.clear();
	TEST_EXPECT(encode_image(image, settings_of({{"format", "pcx"}}), bytes, why, note) && !note.empty());
	TEST_EXPECT(decode_pcx_menu_rgba(bytes.data(), bytes.size(), pcx, why) && pcx.pixels[0] == image.pixels[0] && pcx.pixels[3] == 255);
	TEST_EXPECT(!encode_image(graded(32, 32), settings_of({{"format", "pcx"}, {"palette", "exact"}}), bytes, why, note) &&
	            why.find("256") != std::string::npos);
	// A DXT5 DDS of every level to 1 x 1, each the authoring encoder's (dxt_encode.h: the D3DX box filter of
	// the level before over the source's texels, rgbcx's blocks): what D3DX reads of it, and the game's
	// decode of the first level.
	TEST_EXPECT(encode_image(image, settings_of({{"format", "dds"}}), bytes, why, note));
	dds::DdsImage dds;
	TEST_EXPECT(dds::dds_read(bytes.data(), bytes.size(), dds, why) && dds.loads && std::string(dds.format.name) == "DXT5" &&
	            dds.levels.size() == 4);
	const std::vector<std::vector<uint8_t>> chain = encode_dxt_levels(image.pixels.data(), 8, 8, true, true);
	TEST_EXPECT(chain.size() == 4 && dxt_full_chain_levels(8, 8) == 4 && dxt_full_chain_levels(256, 32) == 9);
	for (size_t i = 0; i < chain.size() && i < dds.levels.size(); ++i)
		TEST_EXPECT(std::vector<uint8_t>(bytes.begin() + long(dds.levels[i].offset),
		                                 bytes.begin() + long(dds.levels[i].offset + dds.levels[i].bytes)) == chain[i]);
	if (!chain.empty()) {
		DxtSurface surface;
		surface.format = TextureDxtFormat::Dxt5;
		surface.width = 8;
		surface.height = 8;
		surface.blocks = chain[0];
		TEST_EXPECT(decode_dxt_surface(surface).size() == 64);
	}
	// DXT1, one level; A8R8G8B8.
	TEST_EXPECT(encode_image(image, settings_of({{"format", "dds"}, {"dds", "dxt1"}, {"mips", "none"}}), bytes, why, note) &&
	            dds::dds_read(bytes.data(), bytes.size(), dds, why) && std::string(dds.format.name) == "DXT1" &&
	            dds.levels.size() == 1);
	TEST_EXPECT(encode_image(image, settings_of({{"format", "dds"}, {"dds", "argb"}}), bytes, why, note) &&
	            dds::dds_read(bytes.data(), bytes.size(), dds, why) && std::string(dds.format.name) == "A8R8G8B8" &&
	            dds.levels.size() == 1 && dds.levels[0].rgba == image.pixels);
	// Values no option takes.
	TEST_EXPECT(!encode_image(image, settings_of({{"format", "dds"}, {"dds", "dxt3"}}), bytes, why, note) && !why.empty());
	TEST_EXPECT(!encode_image(image, settings_of({{"format", "bmp"}}), bytes, why, note) && !why.empty());
	// A PNG.
	TEST_EXPECT(encode_image(image, settings_of({{"format", "png"}}), bytes, why, note));
	RgbaImage png;
	TEST_EXPECT(png::decode_png(bytes, png, why) && png.pixels == image.pixels);
	std::printf("formats: a 24-bit and a 32-bit TGA, an MDT, a 24-bit and an 8-bit PCX, DXT5 with its chain, DXT1, "
	            "A8R8G8B8, a PNG\n");
	return 0;
}

// A TGA and a PCX as sources: read as an image program reads them, never through the game's readers'
// faults; an 8-bit PCX's indices kept; a height map's brightness into the alpha; a source's alpha by its
// header.
int test_sources() {
	ImageSource source;
	std::string why;
	// A TGA read by its format: bottom first as its header says, and top first where its origin bit says so
	// (the game's reader would draw that one upside down).
	const RgbaImage image = graded(4, 2);
	std::vector<uint8_t> tga;
	TEST_EXPECT(tga::tga_write_rgba32(image.pixels.data(), 4, 2, tga, why));
	TEST_EXPECT(decode_image_source("art/a.tga", tga, source, why) && !source.indexed && source.image.pixels == image.pixels);
	tga[17] |= 0x20;
	std::vector<uint8_t> flipped(image.pixels.begin() + 16, image.pixels.end());
	flipped.insert(flipped.end(), image.pixels.begin(), image.pixels.begin() + 16);
	TEST_EXPECT(decode_image_source("a.TGA", tga, source, why) && source.image.pixels == flipped);
	// A 16-bit TGA's colours (the game's reader zeroes that form).
	{
		std::vector<uint8_t> sixteen = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 16, 0x20, 0x00, 0x7C};
		TEST_EXPECT(decode_image_source("red.tga", sixteen, source, why) && source.image.pixels == std::vector<uint8_t>({255, 0, 0, 255}));
	}
	// An odd-width 8-bit PCX whose rows carry a pad byte (a paint program's): each row's first `width` bytes,
	// unsheared (the game's reader would carry each pad into the next row).
	{
		std::vector<uint8_t> padded(128, 0);
		padded[0] = 0x0A;
		padded[1] = 5;
		padded[2] = 1;
		padded[3] = 8;
		padded[8] = 2; // 3 wide
		padded[10] = 1; // 2 tall
		padded[65] = 1;
		padded[66] = 4; // a pad byte a row
		padded.insert(padded.end(), {1, 2, 3, 0, 4, 5, 6, 0});
		padded.push_back(0x0C);
		for (int i = 0; i < 256; ++i) padded.insert(padded.end(), {uint8_t(i), uint8_t(i), uint8_t(i)});
		TEST_EXPECT(decode_image_source("odd.pcx", padded, source, why) && source.indexed &&
		            source.indices.indices == std::vector<uint8_t>({1, 2, 3, 4, 5, 6}));
	}
	// An 8-bit PCX: its indices and palette kept, each texel its entry's colour, opaque.
	IndexedImage8 indexed;
	indexed.width = 3;
	indexed.height = 2;
	indexed.indices = {0, 5, 9, 9, 5, 0};
	for (int i = 0; i < 256; ++i) {
		indexed.palette[i][0] = uint8_t(i);
		indexed.palette[i][1] = uint8_t(255 - i);
		indexed.palette[i][2] = 7;
	}
	std::vector<uint8_t> pcx;
	TEST_EXPECT(encode_pcx_indexed(indexed, pcx, why));
	TEST_EXPECT(decode_image_source("map.pcx", pcx, source, why) && source.indexed && source.indices.indices == indexed.indices &&
	            source.image.pixels[4] == 5 && source.image.pixels[5] == 250 && source.image.pixels[6] == 7 &&
	            source.image.pixels[7] == 255);
	TEST_EXPECT(!decode_image_source("map.bmp", pcx, source, why) && !why.empty());
	// palette indices: the PCX written from the source's indices and palette as they are, never
	// quantized; refused from a source of colours and at another size, and by encode_image.
	{
		const ImageImportSettings keep = settings_of({{"format", "pcx"}, {"palette", "indices"}});
		TEST_EXPECT(image_keeps_source_indices(keep) && !image_keeps_source_indices(settings_of({{"format", "pcx"}})) &&
		            !image_keeps_source_indices(settings_of({{"format", "tga"}, {"palette", "indices"}})));
		ImageSource indexed_source;
		TEST_EXPECT(decode_image_source("map.pcx", pcx, indexed_source, why));
		std::vector<uint8_t> written;
		std::string field = "x";
		TEST_EXPECT(encode_image_indices(indexed_source, keep, written, why, field) && field.empty());
		ImageSource back;
		TEST_EXPECT(decode_image_source("map.pcx", written, back, why) && back.indexed &&
		            back.indices.indices == indexed.indices && back.indices.palette[9][1] == 246);
		TEST_EXPECT(encode_image_indices(indexed_source, settings_of({{"format", "pcx"}, {"palette", "indices"}, {"size", "3x2"}}),
		                                 written, why, field));
		TEST_EXPECT(!encode_image_indices(indexed_source, settings_of({{"format", "pcx"}, {"palette", "indices"}, {"size", "6x4"}}),
		                                  written, why, field) &&
		            field == "size");
		ImageSource colours;
		colours.image = image;
		TEST_EXPECT(!encode_image_indices(colours, keep, written, why, field) && field == "palette");
		std::string note;
		TEST_EXPECT(!encode_image(indexed_source.image, keep, written, why, note) && why.find("indices") != std::string::npos);
	}
	// A PNG through the PNG reader.
	const std::vector<uint8_t> png = png::encode_png_rgba(image.pixels.data(), 4, 2);
	TEST_EXPECT(decode_image_source("art/a.png", png, source, why) && !source.indexed && source.image.pixels == image.pixels);
	// normal height: each texel's brightness into its alpha, its alpha into its blue.
	{
		RgbaImage bump = image;
		height_into_alpha(bump);
		const uint8_t *was = &image.pixels[4];
		const uint8_t *now = &bump.pixels[4];
		TEST_EXPECT(now[3] == uint8_t((85u * (uint32_t(was[0]) + was[1] + was[2])) >> 8) && now[2] == was[3] &&
		            now[0] == now[3] && now[1] == now[3]);
		// And as an import of a TGA under `normal height` prepares it.
		RgbaImage made = image;
		std::string field;
		TEST_EXPECT(image_import_texels(made, settings_of({{"normal", "height"}}), why, field) && made.pixels == bump.pixels);
	}
	// A source's alpha by its header: a PNG with one, a PNG without, a 32-bit TGA, a 24-bit TGA, a PCX.
	TEST_EXPECT(image_source_has_alpha("a.png", png));
	std::vector<uint8_t> rgb_png = png;
	rgb_png[25] = 2; // its header saying a colour PNG (type 2): the alpha-less form
	TEST_EXPECT(!image_source_has_alpha("a.png", rgb_png));
	TEST_EXPECT(tga::tga_write_rgba32(image.pixels.data(), 4, 2, tga, why) && image_source_has_alpha("a.tga", tga));
	TEST_EXPECT(tga::tga_write_rgb24(image.pixels.data(), 4, 2, tga, why) && !image_source_has_alpha("a.tga", tga));
	TEST_EXPECT(!image_source_has_alpha("a.pcx", tga));
	std::printf("sources: a TGA by its format (its origin, 16 bits), an odd PCX unsheared, a PCX's indices kept, a PNG, a "
	            "height map, a source's alpha by its header\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_settings();
	failures += test_pieces();
	failures += test_formats();
	failures += test_sources();
	if (failures == 0) std::printf("texture_authoring: all passed\n");
	return failures == 0 ? 0 : 1;
}
