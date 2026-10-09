// A terrain made from images (ADR 0046 S20): the terrain set's text; a heightmap read at every depth
// it takes (an 8-bit PNG TrnGen's way, a 16-bit one scaled by `top`, TrnGen's own .raw and the game's
// raw16) and refused at any other size; the new_terrain request over a project (the images copied
// into art/terrain/ as the set's inputs, the set and its record written, the import making the
// terrain's files under the cache, which the scan types and the build packs while the images are
// never packed); the outputs read back through the engine's readers as the game reads them (the .trn
// past the admission gate, the .cpt's CDEP and 341 tiles, the colour map 1024 x 1024, the blend map,
// the empty .til); the runtime's own terrain load over them (the field store's heights, the frame
// compiler's patches from afar, and near an eye standing on the slope the finest tile under it drawn
// at the ground's height whichever way it looks); the import rerun-stable (forced again, the same bytes); the blank environment
// a mission can be made under, and a blank mission made on the new terrain; and the refusals that
// write nothing (a name taken, a colour map of another size, a value of no key). The surface map: read
// from an indexed image's indices (an 8-bit PCX, a palette PNG) or a colour image's legend colours,
// refused at another size and for a texel of no class (named by its place); written as <stem>_m.pcx,
// 8-bit with the legend its palette, the .trn's polytrn_charmap naming it; and the runtime's surface
// sampler reading the classes painted at known mission positions over the island, at 512 and at 1024.
// With --retail, a shipped char map (JO:CA's Dvxi5_m.pcx) through the same read: its palette the legend,
// its indices kept.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <base/resource_index/resource_index.h>
#include <editor/assets/asset_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/project_validation.h>
#include <editor/import/import_run.h>
#include <editor/import/png_decode.h>
#include <editor/import/png_encode.h>
#include <editor/import/sidecar.h>
#include <editor/import/terrain_import.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_plan.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/texture_import_state.h>
#include <formats/cpt/cpt_io.h>
#include <formats/env/env.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>
#include <formats/til/til_io.h>
#include <formats/til/til_tsd.h>
#include <formats/trn/charmap_legend.h>
#include <formats/trn/trn_io.h>
#include <runtime/terrain/terrain_frame.h>
#include <runtime/terrain_query/foliage_mask_map.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/terrain_query/surface_type_map.h>
#include <runtime/terrain_query/terrain_field_build.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/png_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace fs = std::filesystem;
using opennova::FOLIAGE_ATTRIB_FORCE_ON;
using opennova::FOLIAGE_ATTRIB_SHADOW;
using opennova::FOLIAGE_MATCH_UNSET;
using opennova::FoliageDef;
using opennova::IndexedImage8;
using opennova::RgbaImage;
using editor_test::NoProcess;
using editor_test::PngSpec;
using editor_test::make_png;

namespace {

constexpr int kSide = 1024;

// A 16-bit grey PNG of an island: a cone from the centre, its top `peak` of 65535.
std::vector<uint8_t> island_png16(double peak = 0.5) {
	PngSpec spec;
	spec.width = kSide;
	spec.height = kSide;
	spec.depth = 16;
	spec.color_type = 0;
	for (int y = 0; y < kSide; ++y) {
		spec.rows.push_back(0);
		for (int x = 0; x < kSide; ++x) {
			const double r = std::hypot(x - 512.0, y - 512.0) / 400.0;
			const uint16_t v = static_cast<uint16_t>(65535.0 * peak * std::max(0.0, 1.0 - r));
			spec.rows.push_back(uint8_t(v >> 8));
			spec.rows.push_back(uint8_t(v & 0xFF));
		}
	}
	return make_png(spec);
}

// An 8-bit grey PNG of `side` texels, a ramp.
std::vector<uint8_t> ramp_png8(int side) {
	PngSpec spec;
	spec.width = side;
	spec.height = side;
	spec.depth = 8;
	spec.color_type = 0;
	for (int y = 0; y < side; ++y) {
		spec.rows.push_back(0);
		for (int x = 0; x < side; ++x) spec.rows.push_back(uint8_t(x * 255 / std::max(1, side - 1)));
	}
	return make_png(spec);
}

// An RGBA image of `side` texels, green fading to sand.
std::vector<uint8_t> colour_png(int side) {
	std::vector<uint8_t> rgba(size_t(side) * side * 4);
	for (int y = 0; y < side; ++y)
		for (int x = 0; x < side; ++x) {
			uint8_t *p = &rgba[(size_t(y) * side + x) * 4];
			p[0] = uint8_t(60 + x * 120 / side);
			p[1] = uint8_t(140 - y * 40 / side);
			p[2] = 60;
			p[3] = 255;
		}
	return encode_png_rgba(rgba.data(), uint32_t(side), uint32_t(side));
}

// A surface map's classes, `side` a side: grass (2) everywhere, then a row of 8 x 8-texel patches in the
// middle, one of each class 0 to 19, the first at column side / 2 - 80 and row side / 2 - 16 (the
// patches a little north of the world's origin, around the player's start).
std::vector<uint8_t> surface_classes(int side) {
	std::vector<uint8_t> classes(size_t(side) * side, 2);
	for (int k = 0; k < opennova::kCharmapLegendCount; ++k)
		for (int row = side / 2 - 16; row < side / 2 - 8; ++row)
			for (int col = side / 2 - 80 + 8 * k; col < side / 2 - 72 + 8 * k; ++col) classes[size_t(row) * side + col] = uint8_t(k);
	return classes;
}

// The classes painted as an RGBA PNG in the legend's colours.
std::vector<uint8_t> surface_colour_png(int side, const std::vector<uint8_t> &classes) {
	std::vector<uint8_t> rgba(size_t(side) * side * 4);
	for (size_t i = 0; i < classes.size(); ++i) {
		const opennova::CharmapLegendColour &c = opennova::kCharmapLegend[classes[i]];
		rgba[i * 4] = c.r;
		rgba[i * 4 + 1] = c.g;
		rgba[i * 4 + 2] = c.b;
		rgba[i * 4 + 3] = 255;
	}
	return encode_png_rgba(rgba.data(), uint32_t(side), uint32_t(side));
}

// The classes as a palette PNG, its indices the classes, its palette any colours (a grey ramp: an
// indexed image's palette is never read).
std::vector<uint8_t> surface_palette_png(int side, const std::vector<uint8_t> &classes) {
	PngSpec spec;
	spec.width = uint32_t(side);
	spec.height = uint32_t(side);
	spec.depth = 8;
	spec.color_type = 3;
	for (int i = 0; i < 256; ++i) spec.palette.insert(spec.palette.end(), 3, uint8_t(i));
	for (int y = 0; y < side; ++y) {
		spec.rows.push_back(0);
		spec.rows.insert(spec.rows.end(), classes.begin() + long(y) * side, classes.begin() + long(y + 1) * side);
	}
	return make_png(spec);
}

// The mission position (16.16) at the centre of a surface map's texel (col, row) over the island layout:
// the heightmap's texel (512 + x, 512 - y), the map's side scaling it from the heightmap's 1024.
void mission_at(int side, int col, int row, int32_t &x_fixed, int32_t &y_fixed) {
	const double scale = 1024.0 / side;
	x_fixed = int32_t(std::lround(((col + 0.5) * scale - 512.0) * 65536.0));
	y_fixed = int32_t(std::lround((512.0 - (row + 0.5) * scale) * 65536.0));
}

// The classes the runtime's sampler reads over a terrain the import made, at the centre of each patch and
// of the map's corners, against the classes painted there; off the island, the ocean's 7.
int expect_surface_sampled(const opennova::terrain::SurfaceTypeMap &map, int side, const std::vector<uint8_t> &classes) {
	TEST_EXPECT(map.data != nullptr && map.width == side && map.height == side);
	std::vector<std::pair<int, int>> texels = {{0, 0}, {side - 1, 0}, {0, side - 1}, {side - 1, side - 1}};
	for (int k = 0; k < opennova::kCharmapLegendCount; ++k) texels.emplace_back(side / 2 - 76 + 8 * k, side / 2 - 12);
	for (const auto &[col, row] : texels) {
		int32_t x = 0, y = 0;
		mission_at(side, col, row, x, y);
		const int32_t read = opennova::terrain::surface_type_at_fixed(map, x, y);
		if (read != classes[size_t(row) * side + col])
			std::fprintf(stderr, "  surface at texel (%d, %d) of %d: read %d, painted %d\n", col, row, side, read,
			             classes[size_t(row) * side + col]);
		TEST_EXPECT(read == classes[size_t(row) * side + col]);
	}
	TEST_EXPECT(opennova::terrain::surface_type_at_fixed(map, 700 << 16, 0) == 7);
	return 0;
}

std::vector<uint8_t> read(const std::string &path) {
	std::vector<uint8_t> bytes;
	std::string message;
	opennova::io::read_file_bytes(path, bytes, message);
	return bytes;
}

// A camera at `eye` looking along `fwd` (normalized here): the column-major world-to-view matrix the
// frame compiler culls with, -Z forward, +Y up.
opennova::TerrainViewInput camera_at(const float eye[3], float fwd[3]) {
	opennova::TerrainViewInput camera;
	camera.cam_x = eye[0];
	camera.cam_y = eye[1];
	camera.cam_z = eye[2];
	const float length = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
	for (int i = 0; i < 3; ++i) fwd[i] /= length;
	float right[3] = {-fwd[2], 0.0f, fwd[0]}; // fwd x up
	const float side = std::sqrt(right[0] * right[0] + right[2] * right[2]);
	for (float &r : right) r /= side;
	const float up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
	                     right[0] * fwd[1] - right[1] * fwd[0]};
	const auto dot = [](const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
	float *m = camera.view;
	m[0] = right[0]; m[4] = right[1]; m[8] = right[2]; m[12] = -dot(right, eye);
	m[1] = up[0]; m[5] = up[1]; m[9] = up[2]; m[13] = -dot(up, eye);
	m[2] = -fwd[0]; m[6] = -fwd[1]; m[10] = -fwd[2]; m[14] = dot(fwd, eye);
	m[3] = 0.0f; m[7] = 0.0f; m[11] = 0.0f; m[15] = 1.0f;
	return camera;
}

// The drawn surface's height at sector-local (x, z): the triangle of the tile's LOD list (strip or
// list, as the embedder converts it) that holds the point, interpolated; NAN when none does.
float drawn_height(const std::vector<opennova::TerrainTileVertex> &vertices, const opennova::CptTileLOD &lod, float x,
                   float z) {
	const auto at = [&](uint16_t a, uint16_t b, uint16_t c) {
		if (a == b || b == c || a == c || std::max({a, b, c}) >= vertices.size()) return NAN;
		const auto &p0 = vertices[a].position, &p1 = vertices[b].position, &p2 = vertices[c].position;
		const float det = (p1[0] - p0[0]) * (p2[2] - p0[2]) - (p2[0] - p0[0]) * (p1[2] - p0[2]);
		if (det == 0.0f) return NAN;
		const float l1 = ((x - p0[0]) * (p2[2] - p0[2]) - (p2[0] - p0[0]) * (z - p0[2])) / det;
		const float l2 = ((p1[0] - p0[0]) * (z - p0[2]) - (x - p0[0]) * (p1[2] - p0[2])) / det;
		const float l0 = 1.0f - l1 - l2;
		if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) return NAN;
		return l0 * p0[1] + l1 * p1[1] + l2 * p2[1];
	};
	const std::vector<uint16_t> &idx = lod.indices;
	for (size_t i = 2; i < idx.size(); i += lod.is_strip ? 1 : 3) {
		const float h = at(idx[i - 2], idx[i - 1], idx[i]);
		if (!std::isnan(h)) return h;
	}
	return NAN;
}

int test_set_text() {
	TerrainSet set;
	set.heightmap = "isle_heightmap.png";
	set.colormap = "isle_colormap.png";
	set.tiles = "../shared/tiles.tga";
	TerrainSet back;
	std::string why;
	TEST_EXPECT(parse_terrain_set(write_terrain_set(set), back, why));
	TEST_EXPECT(back.heightmap == set.heightmap && back.colormap == set.colormap && back.detail.empty() &&
	            back.tiles == set.tiles && back.surface.empty());
	set.surface = "isle_surface.png";
	TEST_EXPECT(parse_terrain_set(write_terrain_set(set), back, why) && back.surface == "isle_surface.png" &&
	            back.tiles == set.tiles);
	{
		const std::vector<uint8_t> written = write_terrain_set(set);
		TEST_EXPECT(std::string(written.begin(), written.end()).find("\r\nsurface isle_surface.png\r\n") != std::string::npos);
	}
	const std::string text = "; a comment\r\nheightmap h.png ; the heights\r\ncolormap  c.png\r\nSurface m.pcx\r\n";
	TEST_EXPECT(parse_terrain_set(std::vector<uint8_t>(text.begin(), text.end()), back, why) && back.heightmap == "h.png" &&
	            back.colormap == "c.png" && back.surface == "m.pcx");
	TEST_EXPECT(terrain_output_names("isle", false, false, false) ==
	            (std::vector<std::string>{"isle.cpt", "isle_c.tga", "isle_dt.tga", "isle_dm.tga", "isle_d1.tga", "isle.til",
	                                      "isle.trn"}));
	TEST_EXPECT(terrain_output_names("isle", true, true, false) ==
	            (std::vector<std::string>{"isle.cpt", "isle_c.tga", "isle_dt.tga", "isle_dm.tga", "isle_d1.tga", "isle_t.tga",
	                                      "isle.til", "isle_m.pcx", "isle.trn"}));
	const std::string wrong = "heightmap h.png\r\ncharmap m.pcx\r\n";
	TEST_EXPECT(!parse_terrain_set(std::vector<uint8_t>(wrong.begin(), wrong.end()), back, why) &&
	            why.find("charmap") != std::string::npos && why.find("surface") != std::string::npos &&
	            why.find("foliagemap") != std::string::npos);
	const std::string half = "heightmap h.png\r\n";
	TEST_EXPECT(!parse_terrain_set(std::vector<uint8_t>(half.begin(), half.end()), back, why) &&
	            why.find("colormap") != std::string::npos);
	TEST_EXPECT(terrain_stem_fits("island", why) && !terrain_stem_fits("islandlong", why) && !terrain_stem_fits("is-le", why) &&
	            !terrain_stem_fits("", why));
	// The options: a row each, the fallbacks, a value no row takes refused with its key.
	TerrainImportSettings settings;
	std::string field;
	TEST_EXPECT(terrain_import_settings({}, settings, why, field) && settings.top == 127.5 && settings.water == 0.0 &&
	            settings.layout == "island");
	TEST_EXPECT(terrain_import_settings({{"top", "64"}, {"water", "10.5"}, {"layout", "Tiled"}}, settings, why, field) &&
	            settings.top == 64.0 && settings.water == 10.5 && settings.layout == "tiled");
	TEST_EXPECT(!terrain_import_settings({{"top", "300"}}, settings, why, field) && field == "top");
	TEST_EXPECT(!terrain_import_settings({{"layout", "ring"}}, settings, why, field) && field == "layout");
	TEST_EXPECT(!terrain_import_settings({{"depth", "1"}}, settings, why, field) && field == "depth");
	return 0;
}

int test_heightmap_depths() {
	TerrainHeights heights;
	std::string why;
	// 16 bits: 0..65535 over 0..top, 256 raw a world unit.
	TEST_EXPECT(decode_terrain_heightmap("h.png", island_png16(1.0), 127.5, heights, why));
	TEST_EXPECT(heights.depth8.empty() && heights.depth16.size() == size_t(kSide) * kSide);
	TEST_EXPECT(heights.depth16[512 * kSide + 512] == 32640 && heights.depth16[0] == 0);
	TEST_EXPECT(decode_terrain_heightmap("h.png", island_png16(1.0), 200.0, heights, why) &&
	            heights.depth16[512 * kSide + 512] == 51200);
	// 8 bits at TrnGen's own scale go to the bake as they are; at another, TrnGen's smoothing then the scale.
	TEST_EXPECT(decode_terrain_heightmap("h.png", ramp_png8(kSide), 127.5, heights, why) && heights.depth16.empty() &&
	            heights.depth8.size() == size_t(kSide) * kSide && heights.depth8[kSide - 1] == 255);
	TEST_EXPECT(decode_terrain_heightmap("h.png", ramp_png8(kSide), 63.75, heights, why) && heights.depth8.empty() &&
	            heights.depth16.size() == size_t(kSide) * kSide);
	// TrnGen's .raw (1 MiB, 8 bits) and the game's raw16 (2 MiB), taken as they are.
	TEST_EXPECT(decode_terrain_heightmap("h.raw", std::vector<uint8_t>(size_t(kSide) * kSide, 7), 127.5, heights, why) &&
	            heights.depth8[0] == 7);
	std::vector<uint8_t> raw16(size_t(kSide) * kSide * 2, 0);
	raw16[0] = 0x34;
	raw16[1] = 0x12;
	TEST_EXPECT(decode_terrain_heightmap("h.raw", raw16, 10.0, heights, why) && heights.depth16[0] == 0x1234);
	// Any other size refused in words.
	TEST_EXPECT(!decode_terrain_heightmap("h.png", ramp_png8(512), 127.5, heights, why) &&
	            why.find("1024 x 1024") != std::string::npos);
	TEST_EXPECT(!decode_terrain_heightmap("h.raw", std::vector<uint8_t>(100), 127.5, heights, why));
	// The steep check: a 256-texel stretch spanning more than 128 units.
	std::vector<uint16_t> steep(1024, 0);
	steep[300] = 40000;
	TEST_EXPECT(terrain_steep_blocks(steep) == 1);
	return 0;
}

// The surface map: the legend itself (twenty colours, none twice, 15 and up unlike the shipped legend's
// white); each form it is read from; the sizes and the texels refused; the PCX it is written as.
int test_surface_reads() {
	using opennova::kCharmapLegend;
	using opennova::kCharmapLegendCount;
	for (int i = 0; i < kCharmapLegendCount; ++i) {
		TEST_EXPECT(opennova::charmap_legend_class(kCharmapLegend[i].r, kCharmapLegend[i].g, kCharmapLegend[i].b) == i);
		const opennova::CharmapLegendColour &white = opennova::kCharmapLegendRest;
		TEST_EXPECT(kCharmapLegend[i].r != white.r || kCharmapLegend[i].g != white.g || kCharmapLegend[i].b != white.b);
	}
	TEST_EXPECT(opennova::charmap_legend_class(255, 255, 255) == -1 && opennova::charmap_legend_class(153, 118, 62) == -1);
	// The new_terrain request's words list the legend, each class by its name and colour.
	const std::string doc = request_kind_row(EditorRequestKind::NewTerrain).doc;
	for (int i = 0; i < kCharmapLegendCount; ++i) {
		char entry[48];
		std::snprintf(entry, sizeof(entry), "%d %s #%02X%02X%02X", i, opennova::til_tsd_surface_names[i] + 4, kCharmapLegend[i].r,
		              kCharmapLegend[i].g, kCharmapLegend[i].b);
		TEST_EXPECT(doc.find(entry) != std::string::npos);
	}

	IndexedImage8 out;
	std::string why;
	const auto same = [](const IndexedImage8 &image, const std::vector<uint8_t> &classes) {
		return image.indices == classes;
	};
	const auto legend_palette = [](const IndexedImage8 &image) {
		for (int i = 0; i < 256; ++i) {
			const opennova::CharmapLegendColour &c = i < kCharmapLegendCount ? kCharmapLegend[i] : opennova::kCharmapLegendRest;
			if (image.palette[i][0] != c.r || image.palette[i][1] != c.g || image.palette[i][2] != c.b) return false;
		}
		return true;
	};
	// A colour PNG in the legend's colours, its alpha ignored; a palette PNG by its indices alone.
	const std::vector<uint8_t> c256 = surface_classes(256);
	TEST_EXPECT(decode_terrain_surface("m.png", surface_colour_png(256, c256), out, why) && out.width == 256 &&
	            out.height == 256 && same(out, c256) && legend_palette(out));
	TEST_EXPECT(decode_terrain_surface("m.png", surface_palette_png(256, c256), out, why) && same(out, c256) && legend_palette(out));
	// An 8-bit PCX by its indices (its palette any), a 24-bit TGA by its colours.
	const std::vector<uint8_t> c512 = surface_classes(512);
	{
		IndexedImage8 pcx;
		pcx.width = pcx.height = 512;
		pcx.indices = c512;
		std::vector<uint8_t> bytes;
		TEST_EXPECT(opennova::encode_pcx_indexed(pcx, bytes, why));
		TEST_EXPECT(decode_terrain_surface("m.pcx", bytes, out, why) && same(out, c512) && legend_palette(out));
		std::vector<uint8_t> rgb(size_t(512) * 512 * 4), tga;
		for (size_t i = 0; i < c512.size(); ++i) {
			rgb[i * 4] = kCharmapLegend[c512[i]].r;
			rgb[i * 4 + 1] = kCharmapLegend[c512[i]].g;
			rgb[i * 4 + 2] = kCharmapLegend[c512[i]].b;
			rgb[i * 4 + 3] = 255;
		}
		TEST_EXPECT(opennova::tga::tga_write_rgb24(rgb.data(), 512, 512, tga, why));
		TEST_EXPECT(decode_terrain_surface("m.tga", tga, out, why) && same(out, c512));
	}
	// The sides: square, 256 to 1024, a power of two.
	TEST_EXPECT(decode_terrain_surface("m.png", surface_colour_png(1024, surface_classes(1024)), out, why) && out.width == 1024);
	for (const auto &[w, h] : std::vector<std::pair<int, int>>{{128, 128}, {384, 384}, {2048, 2048}, {512, 256}}) {
		std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
		TEST_EXPECT(!decode_terrain_surface("m.png", encode_png_rgba(rgba.data(), uint32_t(w), uint32_t(h)), out, why) &&
		            why.find("256, 512 or 1024") != std::string::npos && out.empty());
	}
	// A colour of no class: refused, the first named by its column and row, and how many there are.
	{
		std::vector<uint8_t> classes = c256;
		std::vector<uint8_t> png = surface_colour_png(256, classes);
		RgbaImage image;
		TEST_EXPECT(decode_png(png, image, why));
		uint8_t *p = &image.pixels[(size_t(5) * 256 + 37) * 4];
		p[0] = 0x12, p[1] = 0x34, p[2] = 0x56;
		TEST_EXPECT(!decode_terrain_surface("m.png", encode_png_rgba(image.pixels.data(), 256, 256), out, why) && out.empty());
		TEST_EXPECT(why.find("m.png's texel (37, 5) is #123456") != std::string::npos && why.find("texels in all") == std::string::npos);
		// The legend's colour one step off is no class either: no nearest colour.
		p = &image.pixels[(size_t(200) * 256 + 3) * 4];
		p[0] = 154, p[1] = 118, p[2] = 61;
		TEST_EXPECT(!decode_terrain_surface("m.png", encode_png_rgba(image.pixels.data(), 256, 256), out, why) &&
		            why.find("(37, 5) is #123456 (2 texels in all)") != std::string::npos);
	}
	// An index past the classes: refused, by its place.
	{
		std::vector<uint8_t> classes = c256;
		classes[size_t(2) * 256 + 3] = 20;
		TEST_EXPECT(!decode_terrain_surface("m.png", surface_palette_png(256, classes), out, why) &&
		            why.find("texel (3, 2) holds index 20") != std::string::npos && why.find("0 to 19") != std::string::npos);
	}
	// What the import writes of it: an 8-bit PCX of the map's side, the legend its palette, the classes
	// its indices, as the runtime's reader reads it.
	{
		TEST_EXPECT(decode_terrain_surface("m.png", surface_colour_png(512, c512), out, why));
		std::vector<uint8_t> bytes;
		TEST_EXPECT(opennova::encode_pcx_indexed(out, bytes, why));
		TEST_EXPECT(bytes[3] == 8 && bytes[65] == 1 && (bytes[66] | (bytes[67] << 8)) == 512 && bytes[bytes.size() - 769] == 0x0C);
		IndexedImage8 back;
		TEST_EXPECT(opennova::decode_pcx_indexed(bytes.data(), bytes.size(), back, why) && back.width == 512 && back.height == 512 &&
		            same(back, c512) && legend_palette(back));
		// The game's own 8-bit reader (the port of Texture_LoadPCXFromPFF8Bit, the char map's) reads each texel
		// as its class's index: here its legend colour, every one distinct.
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
	return 0;
}

// With --retail: a shipped char map read as the importer reads a surface map. JO:CA's are 512 x 512, 8-bit,
// their palette the legend's first fifteen colours and white past them; their indices kept as they are.
int test_retail_charmap() {
	const std::string install = retail::install();
	if (install.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (a shipped char map, Dvxi5_m.pcx)");
	opennova::ResourceIndex index;
	std::vector<uint8_t> bytes;
	TEST_EXPECT(index.scan(install) && index.read_file("Dvxi5_m.pcx", bytes));
	std::string why;
	IndexedImage8 shipped, out;
	TEST_EXPECT(opennova::decode_pcx_indexed(bytes.data(), bytes.size(), shipped, why) && shipped.width == 512 &&
	            shipped.height == 512);
	for (int i = 0; i < 256; ++i) {
		const opennova::CharmapLegendColour &c = i < 15 ? opennova::kCharmapLegend[i] : opennova::kCharmapLegendRest;
		TEST_EXPECT(shipped.palette[i][0] == c.r && shipped.palette[i][1] == c.g && shipped.palette[i][2] == c.b);
	}
	TEST_EXPECT(decode_terrain_surface("Dvxi5_m.pcx", bytes, out, why) && out.indices == shipped.indices);
	return 0;
}

struct Project {
	editor_test::TempProjectDir dir;
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session;
	std::string root;
	explicit Project(const char *name) : dir(name), session(platform, preferences) {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Terrains"));
		root = session.view().project.root;
	}
	std::string at(const std::string &relative) const { return root + "/" + relative; }
	const AssetEntry *find(const std::string &name) const { return session.view().project.scan->find(name); }
	std::string output(const std::string &name) const {
		const AssetEntry *entry = find(name);
		return entry ? at(entry->relative_path) : std::string();
	}
};

int test_new_terrain() {
	Project project("opennova_editor_terrain_import");
	const std::string images = project.dir.file("images");
	TEST_EXPECT(editor_test::write_bytes(images + "/height.png", island_png16()));
	TEST_EXPECT(editor_test::write_bytes(images + "/colour.png", colour_png(kSide)));
	TEST_EXPECT(editor_test::write_bytes(images + "/small.png", colour_png(256)));
	const ProjectSession &session = project.session;

	// Refused, nothing written: a colour map of another size, a value of no key, a name too long.
	ActionOutcome outcome = editor_test::handle_to_end(project.session,
	        request::new_terrain("isle", {{"heightmap", images + "/height.png"}, {"colormap", images + "/small.png"}}));
	TEST_EXPECT(outcome.refused && !outcome.findings.empty() && outcome.findings.back().code() == "import.terrain");
	TEST_EXPECT(outcome.findings.back().message.find("1024 x 1024") != std::string::npos);
	TEST_EXPECT(!fs::exists(system_path(project.at("art/terrain"))));
	outcome = editor_test::handle_to_end(project.session, request::new_terrain("isle",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}, {"sea", "4"}}));
	TEST_EXPECT(outcome.refused && outcome.findings.back().message.find("'sea'") != std::string::npos);
	outcome = editor_test::handle_to_end(project.session, request::new_terrain("averylongname",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}}));
	TEST_EXPECT(outcome.refused && !fs::exists(system_path(project.at("art/terrain"))));

	// Made: the images copied in as the set's inputs, the set and its record, then imported.
	outcome = editor_test::handle_to_end(project.session, request::new_terrain("isle",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}, {"water", "12"}, {"top", "100"}}));
	for (const Diagnostic &d : outcome.findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(!outcome.refused);
	TerrainSet set;
	std::string why;
	TEST_EXPECT(parse_terrain_set(read(project.at("art/terrain/isle.tset")), set, why));
	TEST_EXPECT(set.heightmap == "isle_heightmap.png" && set.colormap == "isle_colormap.png" && set.detail.empty());
	TEST_EXPECT(read(project.at("art/terrain/isle_heightmap.png")) == island_png16());
	ImportSidecar record;
	Diagnostic error;
	TEST_EXPECT(load_import_sidecar(project.at("art/terrain/isle.tset.import"), record, error));
	TEST_EXPECT(record.importer == "terrain" && record.options.at("water") == "12" && record.options.at("top") == "100");
	TEST_EXPECT((record.inputs == std::vector<std::string>{"isle_heightmap.png", "isle_colormap.png"}));
	TEST_EXPECT(record.outputs == terrain_output_names("isle", false, false, false));

	// The scan: the outputs project files of their kinds, the set an import source, the images inputs.
	const SessionView &view = session.view();
	TEST_EXPECT(view.project.imports && view.project.imports->size() == 1 && (*view.project.imports)[0].ok);
	TEST_EXPECT(project.find("isle.trn") && project.find("isle.trn")->kind == AssetKind::Terrain);
	TEST_EXPECT(project.find("isle.cpt") && project.find("isle.cpt")->kind == AssetKind::TerrainPolyData);
	TEST_EXPECT(project.find("isle.til") && project.find("isle.til")->kind == AssetKind::TileInfo);
	for (const char *texture : {"isle_c.tga", "isle_dt.tga", "isle_dm.tga", "isle_d1.tga"})
		TEST_EXPECT(project.find(texture) && project.find(texture)->kind == AssetKind::Texture);
	TEST_EXPECT(project.find("isle.tset") && project.find("isle.tset")->kind == AssetKind::ImportSource);
	TEST_EXPECT(project.find("isle_heightmap.png") && project.find("isle_heightmap.png")->kind == AssetKind::ImportInput);
	TEST_EXPECT(project.find("isle_colormap.png") && project.find("isle_colormap.png")->kind == AssetKind::ImportInput);

	// The build packs the terrain's files, never its images or its set.
	AssetGraph graph;
	ValidationCache cache;
	const ProjectPaths paths = ProjectPaths::for_root(project.root);
	const BuildPlan plan = plan_build(paths, *view.project.scan, *view.project.requirements,
	                                  validate_project({paths, *view.project.document, *view.project.scan, view.documents.open},
	                                                   graph, cache));
	std::map<std::string, bool> packed;
	for (const BuildArchive &archive : plan.archives)
		for (const BuildEntry &entry : archive.entries) packed[entry.logical_name] = true;
	for (const BuildEntry &entry : plan.loose) packed[entry.logical_name] = true;
	TEST_EXPECT(packed["isle.trn"] && packed["isle.cpt"] && packed["isle_c.tga"] && packed["isle_d1.tga"] && packed["isle.til"]);
	TEST_EXPECT(!packed["isle_heightmap.png"] && !packed["isle_colormap.png"] && !packed["isle.tset"]);

	// The outputs, read back as the game reads them.
	opennova::TrnConfig trn;
	{
		const std::vector<uint8_t> text = read(project.output("isle.trn"));
		std::istringstream in(std::string(text.begin(), text.end()));
		TEST_EXPECT(opennova::load_trn(in, trn, why)); // the admission gate
	}
	TEST_EXPECT(trn.name == "isle" && trn.colormap == "isle_c.tga" && trn.polydata == "isle.cpt" && trn.detailmap == "isle_dm.tga");
	TEST_EXPECT(trn.detailmap_c1 == "isle_dt.tga" && trn.detailblendmap == "isle_d1.tga" && trn.tileinfo == "isle.til");
	TEST_EXPECT(trn.water_height == 24 && trn.sector_count == 8 && trn.origin_x == -4);
	TEST_EXPECT(trn.sector_grid[3][3] == 1 && trn.sector_grid[3][4] == 3 && trn.sector_grid[4][3] == 2 && trn.sector_grid[4][4] == 4);
	TEST_EXPECT(trn.sector_grid[0][0] == 0 && trn.wrap_x == 0 && trn.charmap.empty());
	const std::vector<uint8_t> cpt_bytes = read(project.output("isle.cpt"));
	opennova::CptFile cpt;
	TEST_EXPECT(opennova::load_cpt(cpt_bytes.data(), cpt_bytes.size(), cpt, why));
	TEST_EXPECT(cpt.depth_format == opennova::DepthFormat::CDEP && cpt.tiles.size() == 341 && cpt_bytes[6] == 5);
	// The peak: half of 65535 over 0..100 units is 50 units, within the mesh's rasterized error.
	const double peak = cpt.depth_buffer[512 * kSide + 512] / 256.0;
	TEST_EXPECT(peak > 48.0 && peak < 52.0 && cpt.depth_buffer[0] == 0);
	for (const char *texture : {"isle_c.tga", "isle_d1.tga"}) {
		const std::vector<uint8_t> tga = read(project.output(texture));
		uint32_t w = 0, h = 0;
		TEST_EXPECT(opennova::tga::tga_header_size(tga.data(), tga.size(), w, h) && w == 1024 && h == 1024);
	}
	TEST_EXPECT(read(project.output("isle_c.tga"))[16] == 24 && read(project.output("isle_d1.tga"))[16] == 32);
	opennova::TilFile til;
	const std::vector<uint8_t> til_bytes = read(project.output("isle.til"));
	TEST_EXPECT(til_bytes.size() == 16 && opennova::load_til(til_bytes.data(), til_bytes.size(), til, why) && til.empty());

	// The runtime's own load over the outputs: the heights the field store samples, the patches a
	// frame compiles over the island.
	{
		opennova::ResourceIndex index;
		const std::string dir = utf8_of(path_of(project.output("isle.trn")).parent_path());
		TEST_EXPECT(index.scan(dir));
		opennova::terrain::TerrainFieldStore store;
		TEST_EXPECT(opennova::terrain::terrain_field_store_load(store, index, "isle", "", "", why));
		TEST_EXPECT(store.valid());
		// No surface map: the game's surface 1 everywhere, on the island and off it.
		TEST_EXPECT(store.surface_map().data == nullptr && opennova::terrain::surface_type_at_fixed(store.surface_map(), 0, 0) == 1 &&
		            opennova::terrain::surface_type_at_fixed(store.surface_map(), 700 << 16, 0) == 1);
		const float centre = opennova::terrain::height_field_height_world_bilinear(store.height_field(), 0.0f, 0.0f);
		const float shore = opennova::terrain::height_field_height_world_bilinear(store.height_field(), 450.0f, 0.0f);
		TEST_EXPECT(centre > 48.0f && centre < 52.0f && shore < 2.0f);
		const opennova::TerrainSceneSnapshot scene = opennova::build_terrain_scene_snapshot(cpt, trn);
		TEST_EXPECT(scene.valid());
		// From the island's south, looking at its centre.
		const float far_eye[3] = {0.0f, 200.0f, 600.0f};
		float at_centre[3] = {-far_eye[0], -far_eye[1], -far_eye[2]};
		opennova::TerrainFrameCompiler compiler;
		TEST_EXPECT(!compiler.compile(scene, camera_at(far_eye, at_centre)).patches.empty());

		// Near the eye: the finest tiles all carry vertices and every LOD list a triangle, every leaf
		// node the traversal reaches resolves to its tile, and an eye standing on the slope (1.7 over
		// the ground, the island's sector grid putting world (x, z) at texel (512 + x, 512 + z)) draws
		// the leaf tile under it, at the finest family, its surface there the ground's, whichever
		// way it looks.
		int leaves = 0;
		for (const opennova::CptTile &tile : cpt.tiles) {
			if (tile.tile_size != 64) continue;
			++leaves;
			TEST_EXPECT(tile.vertex_count > 0);
			for (const opennova::CptTileLOD &lod : tile.lods) TEST_EXPECT(lod.indices.size() >= 3);
		}
		TEST_EXPECT(leaves == 256);
		for (const auto &node : scene.quad_nodes)
			if (node.is_leaf) TEST_EXPECT(node.size == 64 && node.tile_index >= 0);
		const int eye_x = 60, eye_z = -40;
		const float ground = cpt.depth_buffer[(512 + eye_z) * kSide + 512 + eye_x] / 256.0f;
		TEST_EXPECT(ground > 35.0f && ground < 45.0f);
		const float eye[3] = {float(eye_x), ground + 1.7f, float(eye_z)};
		for (const float yaw : {0.0f, 90.0f, 180.0f, 270.0f}) {
			for (const float pitch : {0.0f, -40.0f}) {
				const float y = yaw * 3.14159265f / 180.0f, p = pitch * 3.14159265f / 180.0f;
				float fwd[3] = {std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p)};
				const opennova::TerrainDrawList &near = compiler.compile(scene, camera_at(eye, fwd));
				TEST_EXPECT(near.debug.traversal.budget_drops == 0);
				int under = 0;
				for (const opennova::TerrainPatchDraw &draw : near.patches) {
					const opennova::CptTile &tile = cpt.tiles[draw.tile_index];
					const float local_x = eye[0] - draw.sector_ox, local_z = eye[2] - draw.sector_oz;
					const int x0 = tile.tile_x & 0x1ff, z0 = tile.tile_y & 0x1ff;
					if (draw.zero_height || local_x < x0 || local_x > x0 + tile.tile_size || local_z < z0 ||
					    local_z > z0 + tile.tile_size)
						continue;
					++under;
					TEST_EXPECT(draw.page_lod_level == 4 && draw.lod_family == 0 && tile.tile_size == 64);
					const float drawn = drawn_height(
					        opennova::build_terrain_tile_vertices(cpt, trn, draw.tile_index, false),
					        tile.lods[draw.lod_family], local_x, local_z);
					TEST_EXPECT(std::fabs(drawn - ground) < 0.25f);
				}
				TEST_EXPECT(under == 1);
			}
		}
	}

	// Rerun-stable: imported again with nothing changed, the same bytes.
	std::map<std::string, std::vector<uint8_t>> before;
	for (const std::string &name : terrain_output_names("isle", false, false, false)) before[name] = read(project.output(name));
	outcome = editor_test::handle_to_end(project.session, request::reimport("art/terrain/isle.tset", true));
	TEST_EXPECT(!outcome.refused && (*session.view().project.imports)[0].reimported);
	for (const auto &[name, bytes] : before) TEST_EXPECT(!bytes.empty() && read(project.output(name)) == bytes);

	// An input changed imports it again: the colour map's first texel.
	std::vector<uint8_t> rgba(size_t(kSide) * kSide * 4, 200);
	TEST_EXPECT(editor_test::write_bytes(project.at("art/terrain/isle_colormap.png"), encode_png_rgba(rgba.data(), kSide, kSide)));
	outcome = editor_test::handle_to_end(project.session, request::reimport(std::string(), false));
	TEST_EXPECT(read(project.output("isle_c.tga")) != before["isle_c.tga"] && read(project.output("isle.cpt")) == before["isle.cpt"]);

	// A name taken: refused, nothing written over.
	outcome = editor_test::handle_to_end(project.session, request::new_terrain("isle",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}}));
	TEST_EXPECT(outcome.refused && outcome.findings.back().message.find("already") != std::string::npos);

	// An environment, then a mission on the new terrain under it.
	outcome = editor_test::handle_to_end(project.session, request::create_file("isle.env", "environment"));
	for (const Diagnostic &d : outcome.findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(!outcome.refused && fs::is_regular_file(system_path(project.at("terrain/isle.env"))));
	{
		const std::vector<uint8_t> text = read(project.at("terrain/isle.env"));
		std::istringstream in(std::string(text.begin(), text.end()));
		opennova::env::Config env;
		TEST_EXPECT(opennova::env::load_env(in, env, why) && env.keyframes.size() == 3 && env.name == "isle");
	}
	editor_test::set_missions(project.session, true);
	outcome = editor_test::handle_to_end(project.session,
	        request::create_file("isle1.bms", "mission", {{"terrain", "isle.trn"}, {"environment", "isle.env"}}));
	for (const Diagnostic &d : outcome.findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(!outcome.refused && fs::is_regular_file(system_path(project.at("missions/isle1.bms"))));
	return 0;
}

// A terrain with a surface map (the surface value): refused for a texel of no class, nothing written;
// made, the map an input of the import, written as reef_m.pcx (8-bit, the legend its palette) and named by
// the .trn, packed by the build; the runtime's load sampling the painted classes at their mission
// positions; the map changed to a palette PNG of 1024, imported again and sampled at that scale.
int test_new_terrain_surface() {
	Project project("opennova_editor_terrain_surface");
	const std::string images = project.dir.file("images");
	const std::vector<uint8_t> c512 = surface_classes(512);
	std::vector<uint8_t> stray = c512;
	TEST_EXPECT(editor_test::write_bytes(images + "/height.png", island_png16()));
	TEST_EXPECT(editor_test::write_bytes(images + "/colour.png", colour_png(kSide)));
	TEST_EXPECT(editor_test::write_bytes(images + "/surface.png", surface_colour_png(512, c512)));
	TEST_EXPECT(editor_test::write_bytes(images + "/bad.png", surface_palette_png(512, std::vector<uint8_t>(c512.size(), 31))));
	const ProjectSession &session = project.session;

	ActionOutcome outcome = editor_test::handle_to_end(project.session, request::new_terrain("reef",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}, {"surface", images + "/bad.png"}}));
	TEST_EXPECT(outcome.refused && outcome.findings.back().code() == "import.terrain" &&
	            outcome.findings.back().message.find("texel (0, 0) holds index 31") != std::string::npos);
	TEST_EXPECT(!fs::exists(system_path(project.at("art/terrain"))));

	outcome = editor_test::handle_to_end(project.session, request::new_terrain("reef",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}, {"surface", images + "/surface.png"}}));
	for (const Diagnostic &d : outcome.findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(!outcome.refused);
	TerrainSet set;
	std::string why;
	TEST_EXPECT(parse_terrain_set(read(project.at("art/terrain/reef.tset")), set, why) && set.surface == "reef_surface.png");
	ImportSidecar record;
	Diagnostic error;
	TEST_EXPECT(load_import_sidecar(project.at("art/terrain/reef.tset.import"), record, error));
	TEST_EXPECT((record.inputs == std::vector<std::string>{"reef_heightmap.png", "reef_colormap.png", "reef_surface.png"}));
	TEST_EXPECT(record.outputs == terrain_output_names("reef", false, true, false));
	const SessionView &view = session.view();
	TEST_EXPECT(view.project.imports && (*view.project.imports)[0].ok);
	TEST_EXPECT(project.find("reef_m.pcx") && project.find("reef_m.pcx")->kind == AssetKind::Texture);
	TEST_EXPECT(project.find("reef_surface.png") && project.find("reef_surface.png")->kind == AssetKind::ImportInput);

	// The .trn names it; the file is the game's 8-bit PCX, the legend its palette.
	opennova::TrnConfig trn;
	{
		const std::vector<uint8_t> text = read(project.output("reef.trn"));
		const std::string words(text.begin(), text.end());
		TEST_EXPECT(words.find("polytrn_charmap") != std::string::npos && words.find("reef_m.pcx") != std::string::npos);
		std::istringstream in(words);
		TEST_EXPECT(opennova::load_trn(in, trn, why) && trn.charmap == "reef_m.pcx");
	}
	{
		const std::vector<uint8_t> pcx = read(project.output("reef_m.pcx"));
		IndexedImage8 map;
		TEST_EXPECT(pcx.size() > 897 && pcx[3] == 8 && pcx[65] == 1);
		TEST_EXPECT(opennova::decode_pcx_indexed(pcx.data(), pcx.size(), map, why) && map.width == 512 && map.indices == c512);
		for (int i = 0; i < opennova::kCharmapLegendCount; ++i)
			TEST_EXPECT(map.palette[i][0] == opennova::kCharmapLegend[i].r && map.palette[i][1] == opennova::kCharmapLegend[i].g &&
			            map.palette[i][2] == opennova::kCharmapLegend[i].b);
	}

	// The build packs it; the project's checks find nothing wrong with it.
	AssetGraph graph;
	ValidationCache cache;
	const ProjectPaths paths = ProjectPaths::for_root(project.root);
	const std::vector<Diagnostic> findings =
	        validate_project({paths, *view.project.document, *view.project.scan, view.documents.open}, graph, cache);
	for (const Diagnostic &d : findings)
		if (d.message.find("reef_m") != std::string::npos || d.asset.find("reef_m") != std::string::npos) {
			std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
			TEST_EXPECT(d.severity != DiagnosticSeverity::Error);
		}
	const BuildPlan plan = plan_build(paths, *view.project.scan, *view.project.requirements, findings);
	bool packed = false;
	for (const BuildArchive &archive : plan.archives)
		for (const BuildEntry &entry : archive.entries) packed = packed || entry.logical_name == "reef_m.pcx";
	for (const BuildEntry &entry : plan.loose) packed = packed || entry.logical_name == "reef_m.pcx";
	TEST_EXPECT(packed);

	// The texture view asks nothing of a terrain's outputs: the import makes each as the game reads it.
	for (const char *made : {"reef_m.pcx", "reef_c.tga"}) {
		TextureImportState state;
		TEST_EXPECT(texture_import_state(view, made, state, why) && state.sidecar.importer == "terrain");
		TEST_EXPECT(state.needs.uses == 0 && state.needs.conflicts.empty() && state.needs.options.empty());
	}

	// The runtime's load: the store's surface map the .trn's char map, the classes painted where they read.
	const std::string dir = utf8_of(path_of(project.output("reef.trn")).parent_path());
	{
		opennova::ResourceIndex index;
		TEST_EXPECT(index.scan(dir));
		opennova::terrain::TerrainFieldStore store;
		TEST_EXPECT(opennova::terrain::terrain_field_store_load(store, index, "reef", "", "", why) && store.valid());
		TEST_EXPECT(expect_surface_sampled(store.surface_map(), 512, c512) == 0);
	}

	// The map made again as a palette PNG of 1024: imported again, the classes read at that scale.
	const std::vector<uint8_t> c1024 = surface_classes(1024);
	TEST_EXPECT(editor_test::write_bytes(project.at("art/terrain/reef_surface.png"), surface_palette_png(1024, c1024)));
	outcome = editor_test::handle_to_end(project.session, request::reimport(std::string(), false));
	TEST_EXPECT(!outcome.refused && (*session.view().project.imports)[0].ok);
	{
		opennova::ResourceIndex index;
		TEST_EXPECT(index.scan(dir));
		opennova::terrain::TerrainFieldStore store;
		TEST_EXPECT(opennova::terrain::terrain_field_store_load(store, index, "reef", "", "", why) && store.valid());
		TEST_EXPECT(expect_surface_sampled(store.surface_map(), 1024, c1024) == 0);
	}
	return 0;
}

// --- DI-29: the terrain's foliage map ----------------------------------------------------------------------

// A foliage map's codes, `side` a side: 0 (nothing) everywhere, a block of 253 and one of 254 side by side in
// the north-west, and a block of 77 (a code no definition matches) under the 253s; each block a quarter side.
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

// The codes as a palette PNG, its palette a colour ramp the import keeps.
std::vector<uint8_t> foliage_palette_png(int side, const std::vector<uint8_t> &codes) {
	PngSpec spec;
	spec.width = uint32_t(side);
	spec.height = uint32_t(side);
	spec.depth = 8;
	spec.color_type = 3;
	for (int i = 0; i < 256; ++i) {
		spec.palette.push_back(uint8_t(i));
		spec.palette.push_back(uint8_t(255 - i));
		spec.palette.push_back(uint8_t(i / 2));
	}
	for (int y = 0; y < side; ++y) {
		spec.rows.push_back(0);
		spec.rows.insert(spec.rows.end(), codes.begin() + long(y) * side, codes.begin() + long(y + 1) * side);
	}
	return make_png(spec);
}

// The codes as an 8-bit grey PNG, each level a code.
std::vector<uint8_t> foliage_grey_png(int side, const std::vector<uint8_t> &codes) {
	PngSpec spec;
	spec.width = uint32_t(side);
	spec.height = uint32_t(side);
	spec.depth = 8;
	spec.color_type = 0;
	for (int y = 0; y < side; ++y) {
		spec.rows.push_back(0);
		spec.rows.insert(spec.rows.end(), codes.begin() + long(y) * side, codes.begin() + long(y + 1) * side);
	}
	return make_png(spec);
}

// The set's foliage keys: the map and up to four definitions in the .trn's own block form, written and read
// back; the refusals of a block the game would not read as given; the one-line definitions new_terrain takes.
int test_foliage_set_text() {
	TerrainSet set;
	set.heightmap = "h.png";
	set.colormap = "c.png";
	set.foliagemap = "isle_foliage.png";
	FoliageDef grass;
	grass.graphic = "grass.3di";
	grass.match = {253, FOLIAGE_MATCH_UNSET, FOLIAGE_MATCH_UNSET, FOLIAGE_MATCH_UNSET};
	grass.color_upper = 2;
	FoliageDef shrub;
	shrub.graphic = "shrub.3di";
	shrub.match = {254, 253, FOLIAGE_MATCH_UNSET, FOLIAGE_MATCH_UNSET};
	shrub.attrib_flags = FOLIAGE_ATTRIB_FORCE_ON | FOLIAGE_ATTRIB_SHADOW;
	set.foliage = {grass, shrub};
	const std::vector<uint8_t> written = write_terrain_set(set);
	const std::string text(written.begin(), written.end());
	TEST_EXPECT(text.find("\r\nfoliagemap isle_foliage.png\r\n") != std::string::npos);
	TEST_EXPECT(text.find("foliage\r\n  graphic shrub.3di\r\n  match 254 253\r\n  color_lower 0\r\n  color_upper 0\r\n"
	                      "  attrib forceon shadow\r\nend\r\n") != std::string::npos);
	TerrainSet back;
	std::string why;
	TEST_EXPECT(parse_terrain_set(written, back, why) && back.foliagemap == "isle_foliage.png" && back.foliage.size() == 2);
	if (back.foliage.size() != 2) return 1;
	TEST_EXPECT(back.foliage[0].graphic == "grass.3di" && back.foliage[0].match == grass.match &&
	            back.foliage[0].color_upper == 2 && back.foliage[0].attrib_flags == 0);
	TEST_EXPECT(back.foliage[1].match == shrub.match && back.foliage[1].attrib_flags == shrub.attrib_flags);
	TEST_EXPECT(terrain_output_names("isle", false, true, true) ==
	            (std::vector<std::string>{"isle.cpt", "isle_c.tga", "isle_dt.tga", "isle_dm.tga", "isle_d1.tga", "isle.til",
	                                      "isle_m.pcx", "isle_f.pcx", "isle.trn"}));
	// A block as the shipped .trn writes one (tabs, commas, a comment), read the same.
	const std::string shipped = "heightmap h.png\r\ncolormap c.png\r\nfoliagemap f.pcx\r\nfoliage\r\n  graphic\tmveg5b.3di ; <= 20 verts\r\n"
	                            "  color_lower 0\r\n  match 254,252\r\n  ATTRIB\tshadow\r\nEND\r\n";
	TEST_EXPECT(parse_terrain_set(std::vector<uint8_t>(shipped.begin(), shipped.end()), back, why) && back.foliage.size() == 1 &&
	            back.foliage[0].graphic == "mveg5b.3di" && back.foliage[0].match[1] == 252 &&
	            back.foliage[0].attrib_flags == FOLIAGE_ATTRIB_SHADOW);
	// Refused, each naming its line or block.
	const std::string head = "heightmap h.png\r\ncolormap c.png\r\n";
	const std::string block = "foliage\r\n graphic g.3di\r\n match 1\r\nend\r\n";
	const std::vector<std::pair<std::string, const char *>> refused = {
	        {"foliage\r\n graphic g.3di\r\n match 1\r\n", "has no end"},
	        {block + block + block + block + block, "fifth foliage block"},
	        {"foliage\r\n graphic g.3di\r\n match 1\r\n scale 2\r\nend\r\n", "'scale' is no key"},
	        {"end\r\n", "outside a foliage block"},
	        {"match 253\r\n", "outside a foliage block"},
	        {"foliage\r\n graphic g.3di\r\n match 0\r\nend\r\n", "no code from 1 to 255"},
	        {"foliage\r\n graphic g.3di\r\n match 256\r\nend\r\n", "no code from 1 to 255"},
	        {"foliage\r\n graphic g.3di\r\n match 1 2 3 4 5\r\nend\r\n", "one to four codes"},
	        {"foliage\r\n graphic g.3di\r\n match 1\r\n color_upper 3\r\nend\r\n", "0, 1 or 2"},
	        {"foliage\r\n graphic g.3di\r\n match 1\r\n attrib glow\r\nend\r\n", "neither forceon nor shadow"},
	        {"foliage\r\n match 1\r\nend\r\n", "names no graphic"},
	        {"foliage\r\n graphic g.3di\r\nend\r\n", "matches no code"},
	        {"foliage f.pcx\r\n", "foliagemap"},
	        {"foliage\r\n graphic g.3di\r\n foliage\r\n", "inside the one of line 3"},
	};
	for (const auto &[body, words] : refused) {
		const std::string all = head + body;
		const bool parsed = parse_terrain_set(std::vector<uint8_t>(all.begin(), all.end()), back, why);
		if (parsed || why.find(words) == std::string::npos) std::fprintf(stderr, "  refused? '%s': %s\n", words, why.c_str());
		TEST_EXPECT(!parsed && why.find(words) != std::string::npos);
	}
	// The one-line form new_terrain takes, several split by |.
	std::vector<FoliageDef> defs;
	TEST_EXPECT(parse_foliage_definitions("graphic grass.3di match 253 color_upper 2 | Graphic shrub.3di match 254 253 attrib "
	                                      "forceon shadow |",
	                                      defs, why) &&
	            defs.size() == 2);
	if (defs.size() == 2)
		TEST_EXPECT(defs[0].graphic == "grass.3di" && defs[0].match == grass.match && defs[0].color_upper == 2 &&
		            defs[1].match == shrub.match && defs[1].attrib_flags == shrub.attrib_flags);
	TEST_EXPECT(parse_foliage_definitions("", defs, why) && defs.empty());
	TEST_EXPECT(!parse_foliage_definitions("grass.3di match 253", defs, why) && why.find("definition 1") != std::string::npos);
	TEST_EXPECT(!parse_foliage_definitions("graphic a.3di match 1 | graphic b.3di", defs, why) &&
	            why.find("definition 2 matches no code") != std::string::npos);
	TEST_EXPECT(!parse_foliage_definitions("graphic a match 1|graphic b match 1|graphic c match 1|graphic d match 1|graphic e match 1",
	                                       defs, why) &&
	            why.find("four") != std::string::npos);
	return 0;
}

// A foliage map read as the import reads it: an indexed image by its indices (its palette kept), a grey one by
// its levels; refused holding colour (the texel named) or at a side the game does not sample whole; what it
// grows by the definitions (codes no definition matches, definitions whose codes it lacks).
int test_foliage_reads() {
	const std::vector<uint8_t> c256 = foliage_codes(256);
	IndexedImage8 out;
	std::string why;
	TEST_EXPECT(decode_terrain_foliage("f.png", foliage_palette_png(256, c256), out, why) && out.width == 256 &&
	            out.height == 256 && out.indices == c256);
	TEST_EXPECT(out.palette[253][0] == 253 && out.palette[253][1] == 2 && out.palette[253][2] == 126);
	TEST_EXPECT(decode_terrain_foliage("f.png", foliage_grey_png(256, c256), out, why) && out.indices == c256 &&
	            out.palette[77][0] == 77 && out.palette[77][2] == 77);
	{
		IndexedImage8 pcx;
		pcx.width = pcx.height = 256;
		pcx.indices = c256;
		pcx.palette[254][0] = 150, pcx.palette[254][1] = 120, pcx.palette[254][2] = 60;
		std::vector<uint8_t> bytes;
		TEST_EXPECT(opennova::encode_pcx_indexed(pcx, bytes, why));
		TEST_EXPECT(decode_terrain_foliage("f.pcx", bytes, out, why) && out.indices == c256 && out.palette[254][0] == 150);
	}
	// The sides the game samples whole: square, a power of two, at most 1024.
	for (const int side : {64, 1024}) {
		const std::vector<uint8_t> codes = foliage_codes(side);
		TEST_EXPECT(decode_terrain_foliage("f.png", foliage_grey_png(side, codes), out, why) && out.width == side);
	}
	for (const auto &[w, h] : std::vector<std::pair<int, int>>{{384, 384}, {2048, 2048}, {512, 256}}) {
		const std::vector<uint8_t> codes(size_t(w) * h, 1);
		PngSpec spec;
		spec.width = uint32_t(w);
		spec.height = uint32_t(h);
		spec.depth = 8;
		spec.color_type = 0;
		for (int y = 0; y < h; ++y) {
			spec.rows.push_back(0);
			spec.rows.insert(spec.rows.end(), codes.begin() + long(y) * w, codes.begin() + long(y + 1) * w);
		}
		TEST_EXPECT(!decode_terrain_foliage("f.png", make_png(spec), out, why) &&
		            why.find("power of two at most 1024") != std::string::npos && out.empty());
	}
	// Colour: refused, by its place.
	{
		std::vector<uint8_t> rgba(size_t(64) * 64 * 4, 200);
		rgba[(size_t(3) * 64 + 9) * 4] = 10;
		TEST_EXPECT(!decode_terrain_foliage("f.png", encode_png_rgba(rgba.data(), 64, 64), out, why) &&
		            why.find("texel (9, 3) is #0AC8C8") != std::string::npos);
	}
	// What it grows: 77 matched by none; a definition of codes the map lacks; 0 never listed.
	FoliageDef grass, ghost;
	grass.graphic = "grass.3di";
	grass.match[0] = 253;
	ghost.graphic = "ghost.3di";
	ghost.match[0] = 9;
	TEST_EXPECT(decode_terrain_foliage("f.png", foliage_grey_png(256, c256), out, why));
	const std::vector<std::string> notes = terrain_foliage_notes(out, {grass, ghost});
	TEST_EXPECT(notes.size() == 2 && notes[0].find("codes 77, 254 (8192 texels) match no foliage definition") == 0 &&
	            notes[1].find("foliage 2 (ghost.3di) matches no code") == 0);
	return 0;
}

// A terrain with a foliage map and its definitions (the foliagemap and foliage values): refused for a definition
// the game would not read and for a map of colour, nothing written; made, the map an input, written as
// isle_f.pcx (8-bit, its codes the indices) and named by the .trn with the definitions as its foliage blocks, a
// code no definition matches a warning; the game's foliage sampler over the runtime's load growing each
// definition where the map holds its codes.
int test_new_terrain_foliage() {
	Project project("opennova_editor_terrain_foliage");
	const std::string images = project.dir.file("images");
	const std::vector<uint8_t> c256 = foliage_codes(256);
	TEST_EXPECT(editor_test::write_bytes(images + "/height.png", island_png16()));
	TEST_EXPECT(editor_test::write_bytes(images + "/colour.png", colour_png(kSide)));
	TEST_EXPECT(editor_test::write_bytes(images + "/foliage.png", foliage_palette_png(256, c256)));
	TEST_EXPECT(editor_test::write_bytes(images + "/coloured.png", colour_png(256)));
	const std::string defs = "graphic grass.3di match 253 color_upper 2 | graphic shrub.3di match 254 253 attrib forceon";

	ActionOutcome outcome = editor_test::handle_to_end(project.session, request::new_terrain("isle",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}, {"foliagemap", images + "/foliage.png"},
	         {"foliage", "graphic grass.3di match 0"}}));
	TEST_EXPECT(outcome.refused && outcome.findings.back().code() == "import.terrain" &&
	            outcome.findings.back().message.find("no code from 1 to 255") != std::string::npos);
	outcome = editor_test::handle_to_end(project.session, request::new_terrain("isle",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}, {"foliagemap", images + "/coloured.png"},
	         {"foliage", defs}}));
	TEST_EXPECT(outcome.refused && outcome.findings.back().message.find("texels are codes") != std::string::npos);
	TEST_EXPECT(!fs::exists(system_path(project.at("art/terrain"))));

	outcome = editor_test::handle_to_end(project.session, request::new_terrain("isle",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}, {"foliagemap", images + "/foliage.png"},
	         {"foliage", defs}}));
	for (const Diagnostic &d : outcome.findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(!outcome.refused);
	TerrainSet set;
	std::string why;
	TEST_EXPECT(parse_terrain_set(read(project.at("art/terrain/isle.tset")), set, why) && set.foliagemap == "isle_foliagemap.png" &&
	            set.foliage.size() == 2);
	ImportSidecar record;
	Diagnostic error;
	TEST_EXPECT(load_import_sidecar(project.at("art/terrain/isle.tset.import"), record, error));
	TEST_EXPECT((record.inputs == std::vector<std::string>{"isle_heightmap.png", "isle_colormap.png", "isle_foliagemap.png"}));
	TEST_EXPECT(record.outputs == terrain_output_names("isle", false, false, true));
	const SessionView &view = project.session.view();
	TEST_EXPECT(view.project.imports && (*view.project.imports)[0].ok);
	TEST_EXPECT(project.find("isle_f.pcx") && project.find("isle_f.pcx")->kind == AssetKind::Texture);
	TEST_EXPECT(project.find("isle_foliagemap.png") && project.find("isle_foliagemap.png")->kind == AssetKind::ImportInput);
	// The code no definition matches: a warning of the import, on the set.
	bool loose = false;
	for (const Diagnostic &d : view.findings.diagnostics)
		loose = loose || (d.code() == "import.terrain" && d.severity == DiagnosticSeverity::Warning &&
		                  d.message.find("codes 77 (4096 texels) match no foliage definition") != std::string::npos);
	TEST_EXPECT(loose);

	// The .trn names the map and holds the definitions as its blocks; the map is the game's 8-bit PCX.
	opennova::TrnConfig trn;
	{
		const std::vector<uint8_t> text = read(project.output("isle.trn"));
		std::istringstream in(std::string(text.begin(), text.end()));
		TEST_EXPECT(opennova::load_trn(in, trn, why) && trn.foliagemap == "isle_f.pcx" && trn.foliage_defs.size() == 2);
	}
	if (trn.foliage_defs.size() != 2) return 1;
	TEST_EXPECT(trn.foliage_defs[0].graphic == "grass.3di" && trn.foliage_defs[0].match[0] == 253 &&
	            trn.foliage_defs[0].color_upper == 2 && trn.foliage_defs[1].match[1] == 253 &&
	            trn.foliage_defs[1].attrib_flags == FOLIAGE_ATTRIB_FORCE_ON);
	IndexedImage8 map;
	{
		const std::vector<uint8_t> pcx = read(project.output("isle_f.pcx"));
		TEST_EXPECT(pcx.size() > 897 && pcx[3] == 8 && pcx[65] == 1);
		TEST_EXPECT(opennova::decode_pcx_indexed(pcx.data(), pcx.size(), map, why) && map.width == 256 && map.indices == c256);
	}

	// The game's sampler over the runtime's load, the map remapped by the definitions as the game does at load:
	// the 253s grow both, the 254s the shrub, the 77s and the 0s nothing, off the island nothing.
	const std::string dir = utf8_of(path_of(project.output("isle.trn")).parent_path());
	opennova::ResourceIndex index;
	TEST_EXPECT(index.scan(dir));
	opennova::terrain::TerrainFieldStore store;
	TEST_EXPECT(opennova::terrain::terrain_field_store_load(store, index, "isle", "", "", why) && store.valid());
	std::vector<uint8_t> masks(map.indices.size());
	for (size_t i = 0; i < masks.size(); ++i)
		masks[i] = uint8_t(opennova::foliage_remap_pixel_to_def_mask(trn.foliage_defs, map.indices[i]));
	opennova::terrain::FoliageMaskMap sampler;
	sampler.data = masks.data();
	sampler.width = sampler.height = 256;
	sampler.sector_grid = store.height_field().layout.sector_grid;
	sampler.origin_x = store.height_field().layout.origin_x;
	sampler.origin_y = store.height_field().layout.origin_y;
	const auto grows = [&](int col, int row) {
		int32_t x = 0, y = 0;
		mission_at(256, col, row, x, y);
		return opennova::terrain::foliage_mask_at_fixed(sampler, x, y);
	};
	TEST_EXPECT(grows(100, 100) == 3 && grows(160, 100) == 2 && grows(100, 160) == 0 && grows(10, 10) == 0);
	TEST_EXPECT(opennova::terrain::foliage_mask_at_fixed(sampler, 700 << 16, 0) == 0);
	return 0;
}

// With --retail: a shipped foliage map read as the importer reads one. JO:CA's are 256 x 256, 8-bit, their codes
// 252 to 255 kept as they are, their palette kept; Dvxi5's definitions match 254 and 253, so its 255s grow
// nothing.
int test_retail_foliage_map() {
	const std::string install = retail::install();
	if (install.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (a shipped foliage map, Dvxi5_f.pcx)");
	opennova::ResourceIndex index;
	std::vector<uint8_t> bytes, text;
	TEST_EXPECT(index.scan(install) && index.read_file("Dvxi5_f.pcx", bytes) && index.read_file("Dvxi5.trn", text));
	std::string why;
	IndexedImage8 shipped, out;
	TEST_EXPECT(opennova::decode_pcx_indexed(bytes.data(), bytes.size(), shipped, why) && shipped.width == 256 &&
	            shipped.height == 256);
	TEST_EXPECT(decode_terrain_foliage("Dvxi5_f.pcx", bytes, out, why) && out.indices == shipped.indices &&
	            std::equal(&out.palette[0][0], &out.palette[0][0] + 768, &shipped.palette[0][0]));
	opennova::TrnConfig trn;
	std::istringstream in(std::string(text.begin(), text.end()));
	TEST_EXPECT(opennova::load_trn(in, trn, why) && trn.foliagemap == "Dvxi5_f.pcx" && trn.foliage_defs.size() == 2);
	const std::vector<std::string> notes = terrain_foliage_notes(out, trn.foliage_defs);
	TEST_EXPECT(notes.size() == 1 && notes[0].find("codes 255 (") == 0);
	return 0;
}

// The blank environment: the writer's template, named after its file, CRLF lines the game parses.
int test_blank_environment() {
	BlankRequest blank;
	blank.logical_name = "dusk.env";
	std::vector<uint8_t> bytes;
	Diagnostic error;
	TEST_EXPECT(make_blank(blank, AssetKind::Environment, bytes, error));
	const std::string text(bytes.begin(), bytes.end());
	TEST_EXPECT(text.find("enviro_name \"dusk\"\r\n") == 0 && text.find("tod_begin") != std::string::npos);
	TEST_EXPECT(find_blank_factory_for_kind(AssetKind::Environment) != nullptr);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_set_text();
	failures += test_heightmap_depths();
	failures += test_surface_reads();
	failures += test_retail_charmap();
	failures += test_blank_environment();
	failures += test_new_terrain();
	failures += test_new_terrain_surface();
	failures += test_foliage_set_text();
	failures += test_foliage_reads();
	failures += test_new_terrain_foliage();
	failures += test_retail_foliage_map();
	if (failures == 0) std::printf("editor_terrain_import: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
