// A terrain's char map and foliage map made from a picture (runtime/terrain terrain_map_source.h). The char map:
// the legend itself (twenty colours, none twice, 15 and up unlike the shipped legend's white); read from an indexed
// picture's indices (an 8-bit PCX, a palette PNG) or a colour picture's legend colours (a PNG, a 24-bit TGA);
// refused at a side the game does not sample whole and for a texel of no class (named by its place); written as an
// 8-bit PCX the game's reader reads back class for class. The foliage map: an indexed picture by its indices (its
// palette kept), a grey one by its levels; refused holding colour (the texel named) or at a side the game does not
// sample whole. With --retail, JO:CA's shipped Dvxi5_m.pcx and Dvxi5_f.pcx read the same way: their indices kept.
#include <runtime/terrain/terrain_map_source.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <base/resource_index/resource_index.h>
#include <formats/pcx/pcx_io.h>
#include <formats/png/png_decode.h>
#include <formats/png/png_encode.h>
#include <formats/tga/tga.h>
#include <formats/trn/charmap_legend.h>

#include "common/png_test_support.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"

namespace {

using opennova::IndexedImage8;
using opennova::RgbaImage;
using opennova::kCharmapLegend;
using opennova::kCharmapLegendCount;
using opennova::png::decode_png;
using opennova::png::encode_png_rgba;
using opennova::terrain::decode_charmap_source;
using opennova::terrain::decode_foliage_map_source;
using test_png::PngSpec;
using test_png::make_png;

// A surface map's classes, `side` a side: grass (2) everywhere, then a row of 8 x 8-texel patches in the middle,
// one of each class 0 to 19.
std::vector<uint8_t> surface_classes(int side) {
	std::vector<uint8_t> classes(size_t(side) * side, 2);
	for (int k = 0; k < kCharmapLegendCount; ++k)
		for (int row = side / 2 - 16; row < side / 2 - 8; ++row)
			for (int col = side / 2 - 80 + 8 * k; col < side / 2 - 72 + 8 * k; ++col) classes[size_t(row) * side + col] = uint8_t(k);
	return classes;
}

// The classes painted as an RGBA PNG in the legend's colours.
std::vector<uint8_t> surface_colour_png(int side, const std::vector<uint8_t> &classes) {
	std::vector<uint8_t> rgba(size_t(side) * side * 4);
	for (size_t i = 0; i < classes.size(); ++i) {
		const opennova::CharmapLegendColour &c = kCharmapLegend[classes[i]];
		rgba[i * 4] = c.r;
		rgba[i * 4 + 1] = c.g;
		rgba[i * 4 + 2] = c.b;
		rgba[i * 4 + 3] = 255;
	}
	return encode_png_rgba(rgba.data(), uint32_t(side), uint32_t(side));
}

// Indices as a palette PNG, its palette `ramp` (a grey ramp, or a colour ramp the foliage map keeps).
std::vector<uint8_t> palette_png(int side, const std::vector<uint8_t> &indices, bool colour_ramp) {
	PngSpec spec;
	spec.width = uint32_t(side);
	spec.height = uint32_t(side);
	spec.depth = 8;
	spec.color_type = 3;
	for (int i = 0; i < 256; ++i) {
		spec.palette.push_back(uint8_t(i));
		spec.palette.push_back(colour_ramp ? uint8_t(255 - i) : uint8_t(i));
		spec.palette.push_back(colour_ramp ? uint8_t(i / 2) : uint8_t(i));
	}
	for (int y = 0; y < side; ++y) {
		spec.rows.push_back(0);
		spec.rows.insert(spec.rows.end(), indices.begin() + long(y) * side, indices.begin() + long(y + 1) * side);
	}
	return make_png(spec);
}

// Levels as an 8-bit grey PNG of `w` x `h`.
std::vector<uint8_t> grey_png(int w, int h, const std::vector<uint8_t> &levels) {
	PngSpec spec;
	spec.width = uint32_t(w);
	spec.height = uint32_t(h);
	spec.depth = 8;
	spec.color_type = 0;
	for (int y = 0; y < h; ++y) {
		spec.rows.push_back(0);
		spec.rows.insert(spec.rows.end(), levels.begin() + long(y) * w, levels.begin() + long(y + 1) * w);
	}
	return make_png(spec);
}

// A foliage map's codes, `side` a side: 0 everywhere, a block of 253 and one of 254 side by side in the north-west,
// and a block of 77 under the 253s; each block a quarter side.
std::vector<uint8_t> foliage_codes(int side) {
	std::vector<uint8_t> codes(size_t(side) * side, 0);
	const int q = side / 4;
	for (int row = q; row < 2 * q; ++row)
		for (int col = q; col < 2 * q; ++col) {
			codes[size_t(row) * side + col] = 253;
			codes[size_t(row) * side + col + q] = 254;
			codes[size_t(row + q) * side + col] = 77;
		}
	return codes;
}

bool legend_palette(const IndexedImage8 &image) {
	for (int i = 0; i < 256; ++i) {
		const opennova::CharmapLegendColour &c = i < kCharmapLegendCount ? kCharmapLegend[i] : opennova::kCharmapLegendRest;
		if (image.palette[i][0] != c.r || image.palette[i][1] != c.g || image.palette[i][2] != c.b) return false;
	}
	return true;
}

int test_charmap_reads() {
	for (int i = 0; i < kCharmapLegendCount; ++i) {
		TEST_EXPECT(opennova::charmap_legend_class(kCharmapLegend[i].r, kCharmapLegend[i].g, kCharmapLegend[i].b) == i);
		const opennova::CharmapLegendColour &white = opennova::kCharmapLegendRest;
		TEST_EXPECT(kCharmapLegend[i].r != white.r || kCharmapLegend[i].g != white.g || kCharmapLegend[i].b != white.b);
	}
	TEST_EXPECT(opennova::charmap_legend_class(255, 255, 255) == -1 && opennova::charmap_legend_class(153, 118, 62) == -1);

	IndexedImage8 out;
	std::string why;
	// A colour PNG in the legend's colours, its alpha ignored; a palette PNG by its indices alone.
	const std::vector<uint8_t> c256 = surface_classes(256);
	TEST_EXPECT(decode_charmap_source("m.png", surface_colour_png(256, c256), out, why) && out.width == 256 &&
	            out.height == 256 && out.indices == c256 && legend_palette(out));
	TEST_EXPECT(decode_charmap_source("m.png", palette_png(256, c256, false), out, why) && out.indices == c256 &&
	            legend_palette(out));
	// An 8-bit PCX by its indices (its palette any), a 24-bit TGA by its colours.
	const std::vector<uint8_t> c512 = surface_classes(512);
	{
		IndexedImage8 pcx;
		pcx.width = pcx.height = 512;
		pcx.indices = c512;
		std::vector<uint8_t> bytes;
		TEST_EXPECT(opennova::encode_pcx_indexed(pcx, bytes, why));
		TEST_EXPECT(decode_charmap_source("m.pcx", bytes, out, why) && out.indices == c512 && legend_palette(out));
		std::vector<uint8_t> rgb(size_t(512) * 512 * 4), tga;
		for (size_t i = 0; i < c512.size(); ++i) {
			rgb[i * 4] = kCharmapLegend[c512[i]].r;
			rgb[i * 4 + 1] = kCharmapLegend[c512[i]].g;
			rgb[i * 4 + 2] = kCharmapLegend[c512[i]].b;
			rgb[i * 4 + 3] = 255;
		}
		TEST_EXPECT(opennova::tga::tga_write_rgb24(rgb.data(), 512, 512, tga, why));
		TEST_EXPECT(decode_charmap_source("m.tga", tga, out, why) && out.indices == c512);
	}
	// The sides: square, 256 to 1024, a power of two.
	TEST_EXPECT(decode_charmap_source("m.png", surface_colour_png(1024, surface_classes(1024)), out, why) &&
	            out.width == 1024);
	for (const auto &[w, h] : std::vector<std::pair<int, int>>{{128, 128}, {384, 384}, {2048, 2048}, {512, 256}}) {
		std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
		TEST_EXPECT(!decode_charmap_source("m.png", encode_png_rgba(rgba.data(), uint32_t(w), uint32_t(h)), out, why) &&
		            why.find("256, 512 or 1024") != std::string::npos && out.empty());
	}
	// A colour of no class: refused, the first named by its column and row, and how many there are.
	{
		RgbaImage image;
		TEST_EXPECT(decode_png(surface_colour_png(256, c256), image, why));
		uint8_t *p = &image.pixels[(size_t(5) * 256 + 37) * 4];
		p[0] = 0x12, p[1] = 0x34, p[2] = 0x56;
		TEST_EXPECT(!decode_charmap_source("m.png", encode_png_rgba(image.pixels.data(), 256, 256), out, why) && out.empty());
		TEST_EXPECT(why.find("m.png's texel (37, 5) is #123456") != std::string::npos &&
		            why.find("texels in all") == std::string::npos);
		// The legend's colour one step off is no class either: no nearest colour.
		p = &image.pixels[(size_t(200) * 256 + 3) * 4];
		p[0] = 154, p[1] = 118, p[2] = 61;
		TEST_EXPECT(!decode_charmap_source("m.png", encode_png_rgba(image.pixels.data(), 256, 256), out, why) &&
		            why.find("(37, 5) is #123456 (2 texels in all)") != std::string::npos);
	}
	// An index past the classes: refused, by its place.
	{
		std::vector<uint8_t> classes = c256;
		classes[size_t(2) * 256 + 3] = 20;
		TEST_EXPECT(!decode_charmap_source("m.png", palette_png(256, classes, false), out, why) &&
		            why.find("texel (3, 2) holds index 20") != std::string::npos && why.find("0 to 19") != std::string::npos);
	}
	// Written as an 8-bit PCX of the map's side, the legend its palette, the classes its indices, as the game's
	// reader reads it.
	{
		TEST_EXPECT(decode_charmap_source("m.png", surface_colour_png(512, c512), out, why));
		std::vector<uint8_t> bytes;
		TEST_EXPECT(opennova::encode_pcx_indexed(out, bytes, why));
		TEST_EXPECT(bytes[3] == 8 && bytes[65] == 1 && (bytes[66] | (bytes[67] << 8)) == 512 && bytes[bytes.size() - 769] == 0x0C);
		IndexedImage8 back;
		TEST_EXPECT(opennova::decode_pcx_indexed(bytes.data(), bytes.size(), back, why) && back.width == 512 &&
		            back.height == 512 && back.indices == c512 && legend_palette(back));
		// The game's own 8-bit reader (the port of Texture_LoadPCXFromPFF8Bit, the char map's) reads each texel as
		// its class's index: here its legend colour, every one distinct.
		RgbaImage game;
		TEST_EXPECT(opennova::decode_pcx_luminance_alpha(bytes.data(), bytes.size(), game, why) && game.width == 512 &&
		            game.height == 512);
		bool classes = game.pixels.size() == c512.size() * 4;
		for (size_t i = 0; classes && i < c512.size(); ++i) {
			const opennova::CharmapLegendColour &c = kCharmapLegend[c512[i]];
			classes = game.pixels[i * 4] == c.r && game.pixels[i * 4 + 1] == c.g && game.pixels[i * 4 + 2] == c.b;
		}
		TEST_EXPECT(classes);
	}
	std::printf("char map: the legend; each form read; the sides and the texels refused; the PCX written\n");
	return 0;
}

int test_foliage_reads() {
	const std::vector<uint8_t> c256 = foliage_codes(256);
	IndexedImage8 out;
	std::string why;
	TEST_EXPECT(decode_foliage_map_source("f.png", palette_png(256, c256, true), out, why) && out.width == 256 &&
	            out.height == 256 && out.indices == c256);
	TEST_EXPECT(out.palette[253][0] == 253 && out.palette[253][1] == 2 && out.palette[253][2] == 126);
	TEST_EXPECT(decode_foliage_map_source("f.png", grey_png(256, 256, c256), out, why) && out.indices == c256 &&
	            out.palette[77][0] == 77 && out.palette[77][2] == 77);
	{
		IndexedImage8 pcx;
		pcx.width = pcx.height = 256;
		pcx.indices = c256;
		pcx.palette[254][0] = 150, pcx.palette[254][1] = 120, pcx.palette[254][2] = 60;
		std::vector<uint8_t> bytes;
		TEST_EXPECT(opennova::encode_pcx_indexed(pcx, bytes, why));
		TEST_EXPECT(decode_foliage_map_source("f.pcx", bytes, out, why) && out.indices == c256 && out.palette[254][0] == 150);
	}
	// The sides the game samples whole: square, a power of two, at most 1024.
	for (const int side : {64, 1024}) {
		const std::vector<uint8_t> codes = foliage_codes(side);
		TEST_EXPECT(decode_foliage_map_source("f.png", grey_png(side, side, codes), out, why) && out.width == side);
	}
	for (const auto &[w, h] : std::vector<std::pair<int, int>>{{384, 384}, {2048, 2048}, {512, 256}}) {
		const std::vector<uint8_t> codes(size_t(w) * h, 1);
		TEST_EXPECT(!decode_foliage_map_source("f.png", grey_png(w, h, codes), out, why) &&
		            why.find("power of two at most 1024") != std::string::npos && out.empty());
	}
	// Colour: refused, by its place.
	{
		std::vector<uint8_t> rgba(size_t(64) * 64 * 4, 200);
		rgba[(size_t(3) * 64 + 9) * 4] = 10;
		TEST_EXPECT(!decode_foliage_map_source("f.png", encode_png_rgba(rgba.data(), 64, 64), out, why) &&
		            why.find("texel (9, 3) is #0AC8C8") != std::string::npos);
	}
	std::printf("foliage map: indexed and grey read, the palette kept; the sides and colour refused\n");
	return 0;
}

// With --retail: JO:CA's shipped maps read as the import reads a picture. The char map is 512 x 512, 8-bit, its
// palette the legend's first fifteen colours and white past them; the foliage map 256 x 256, 8-bit, its codes 252 to
// 255; their indices (and the foliage map's palette) kept as they are.
int test_retail_maps() {
	const std::string install = retail::install();
	if (install.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (shipped maps, Dvxi5_m.pcx and Dvxi5_f.pcx)");
	opennova::ResourceIndex index;
	std::vector<uint8_t> charmap, foliage;
	TEST_EXPECT(index.scan(install) && index.read_file("Dvxi5_m.pcx", charmap) && index.read_file("Dvxi5_f.pcx", foliage));
	std::string why;
	IndexedImage8 shipped, out;
	TEST_EXPECT(opennova::decode_pcx_indexed(charmap.data(), charmap.size(), shipped, why) && shipped.width == 512 &&
	            shipped.height == 512);
	for (int i = 0; i < 256; ++i) {
		const opennova::CharmapLegendColour &c = i < 15 ? kCharmapLegend[i] : opennova::kCharmapLegendRest;
		TEST_EXPECT(shipped.palette[i][0] == c.r && shipped.palette[i][1] == c.g && shipped.palette[i][2] == c.b);
	}
	TEST_EXPECT(decode_charmap_source("Dvxi5_m.pcx", charmap, out, why) && out.indices == shipped.indices);
	TEST_EXPECT(opennova::decode_pcx_indexed(foliage.data(), foliage.size(), shipped, why) && shipped.width == 256 &&
	            shipped.height == 256);
	TEST_EXPECT(decode_foliage_map_source("Dvxi5_f.pcx", foliage, out, why) && out.indices == shipped.indices &&
	            std::equal(&out.palette[0][0], &out.palette[0][0] + 768, &shipped.palette[0][0]));
	std::printf("retail: Dvxi5's char map and foliage map read, their indices kept\n");
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_charmap_reads();
	failures += test_foliage_reads();
	failures += test_retail_maps();
	if (failures == 0) std::printf("terrain_map_source: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
