// Generator + guard for fixtures/terrain/variants (fixtures/README.md): one file per form the shipped JO:CA
// terrain files take, minted by our own writers (encode_pcx_indexed, encode_pcx_rgb, tga_write, save_til,
// save_cpt) from integer data, so every platform mints the same bytes and no retail byte is carried. Each form
// is the census of the extracted JO:CA tree (2026-10-19, the retail-free program), cited where it is built:
//
//   charmap_m.pcx   the 23 char maps (_m.pcx): 8-bit, version 5, 72 dpi, the EGA palette all 0xFF, palette info 1,
//                   every row ending in a run (the shipped coder and MED's tie), the palette the legend's first
//                   fifteen colours and white past them (Dvxi5_m.pcx, 512 x 512)
//   foliage_f.pcx   the 23 foliage maps (_f.pcx): the same header but the EGA palette's leftover entries (eleven
//                   0xFF entries, four others, a zero one), its codes 252 to 255 (Dvxi5_f.pcx, 256 x 256)
//   picture.pcx     the 135 other paintable PCX (the loading screens, the menu art): rows ending in a lone byte
//                   (the shipped coder's alone), the EGA palette mixed
//   grey_info2.pcx  the 23 sky maps: palette info 2, the EGA palette a descending grey ramp
//   rgb24.pcx       the two three-plane 24-bit PCX the paint sweep holds whole
//   trntile_a.tga   the 3 tile atlases (trntile*.tga), like the 578 object skins: 32 bits, descriptor 8, the TGA
//                   2.0 footer with both offsets 0
//   map_d1.tga      the 13 detail blend maps (_d1.tga) at 32 bits, in the same form
//   map_c.tga       the 23 colour maps (_c.tga), like the 7 other detail blend maps and 129 skins: 24 bits,
//                   descriptor 0, the footer
//   bare24.tga      the one 24-bit TGA without the footer (MH53_OFa.tga)
//   med.til         the 116 tile overlays: MED's header, records grouped by ascending layer (0 to 30), every one
//                   of the eight flag combinations, indices 0 to 63 of a 64-cell atlas, whole-unit positions
//                   either side of the origin
//   shipped.cpt     the 23 polydata files: the CDEP depth section (every block width 1 to 15 its blocks take, and
//                   a block steeper than a block holds, clamped), the shipped header (reserved bytes 00 04 05 00,
//                   Dvxi5.cpt's terrain name, creator and version strings), 341 tiles with strip and list LODs,
//                   one a leaf of every texel whose chunks' least indices pass the 12-bit field
//
// The format sweeps (pcx_shipped_forms, tga_shipped_forms, til_shipped_form, cpt_shipped_form) read them and pin
// the bytes they write again; the editor's document tests (texture paint, tile overlay, terrain heights) open them.
//
// Default: rebuild every file in memory and byte-compare the committed ones. `--write` (re)writes them.
#include <formats/cpt/cpt.h>
#include <formats/cpt/cpt_io.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>
#include <formats/til/til.h>
#include <formats/til/til_io.h>
#include <formats/trn/charmap_legend.h>

#include "common/file_io.h"
#include "common/test_paths.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova;

namespace {

bool expect(bool cond, const std::string &msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
	return cond;
}

// --- PCX ---------------------------------------------------------------------------------------------------

// The char map: grass (class 2) everywhere, a patch of each class 0 to 14 a row band, a lone texel of class 7
// mid-row on every eighth row, each row ending in a run (so both coders write it alike).
IndexedImage8 make_charmap() {
	constexpr int kSide = 512;
	IndexedImage8 image;
	image.width = image.height = kSide;
	image.indices.assign(size_t(kSide) * kSide, 2);
	for (int k = 0; k < 15; ++k)
		for (int y = k * 32 + 8; y < k * 32 + 24; ++y)
			for (int x = 16 + 24 * k; x < 16 + 24 * k + 5 + k; ++x) image.indices[size_t(y) * kSide + x] = uint8_t(k);
	for (int y = 0; y < kSide; y += 8) image.indices[size_t(y) * kSide + 450] = 7;
	// Every row's last two texels a road (class 4): the row ends in a run whatever the grass before it leaves.
	for (int y = 0; y < kSide; ++y) image.indices[size_t(y) * kSide + kSide - 2] = image.indices[size_t(y) * kSide + kSide - 1] = 4;
	// A run of 63 and one past it on row 1 (the coders split a run at 63).
	for (int x = 100; x < 100 + 63; ++x) image.indices[size_t(kSide) + x] = 9;
	for (int x = 200; x < 200 + 64; ++x) image.indices[size_t(kSide) + x] = 11;
	for (int i = 0; i < 256; ++i) {
		const CharmapLegendColour &c = i < 15 ? kCharmapLegend[i] : kCharmapLegendRest;
		image.palette[i][0] = c.r, image.palette[i][1] = c.g, image.palette[i][2] = c.b;
	}
	return image;
}

// The foliage map: 255 (no foliage) everywhere, blocks of 252, 253 and 254 (codes at 0xC0 and up, written as a
// count pair even alone), a lone 254 on every sixteenth row; the palette a ramp of our own.
IndexedImage8 make_foliage() {
	constexpr int kSide = 256;
	IndexedImage8 image;
	image.width = image.height = kSide;
	image.indices.assign(size_t(kSide) * kSide, 255);
	for (int y = 32; y < 96; ++y)
		for (int x = 0; x < kSide; ++x) {
			if (x >= 40 && x < 80) image.indices[size_t(y) * kSide + x] = 252;
			if (x >= 120 && x < 121 + (y & 7)) image.indices[size_t(y) * kSide + x] = 253;
		}
	for (int y = 128; y < 200; ++y)
		for (int x = 180; x < 250; ++x) image.indices[size_t(y) * kSide + x] = 254;
	for (int y = 0; y < kSide; y += 16) image.indices[size_t(y) * kSide + 10] = 254;
	for (int i = 0; i < 256; ++i) image.palette[i][0] = uint8_t(i), image.palette[i][1] = uint8_t(255 - i), image.palette[i][2] = uint8_t(i / 3);
	return image;
}

PcxIndexedForm foliage_form() {
	PcxIndexedForm form;
	// The leftover EGA entries the shipped foliage maps carry: eleven 0xFF entries, four others, a zero one
	// (entries of our own here).
	for (int e = 11; e < 15; ++e) form.ega[size_t(e)] = {uint8_t(0x30 + e), uint8_t(0x60 + e), uint8_t(0x90 + e)};
	form.ega[15] = {0, 0, 0};
	return form;
}

// A picture: rows of mixed runs, each ending in a lone byte below 0xC0 (the shipped coder writes it alone, MED's
// as a pair), lone bytes at 0xC0 and up among them.
IndexedImage8 make_picture() {
	constexpr int kW = 80, kH = 60;
	IndexedImage8 image;
	image.width = kW;
	image.height = kH;
	image.indices.resize(size_t(kW) * kH);
	for (int y = 0; y < kH; ++y)
		for (int x = 0; x < kW; ++x) {
			uint8_t v = uint8_t((x / 6 + y / 4) * 17);
			if (x % 13 == 5) v = uint8_t(0xC0 + (y & 31));
			if (x == kW - 1) v = uint8_t((y * 7) & 0x7F);
			image.indices[size_t(y) * kW + x] = v;
		}
	for (int i = 0; i < 256; ++i) image.palette[i][0] = uint8_t(i * 3), image.palette[i][1] = uint8_t(i), image.palette[i][2] = uint8_t(255 - i);
	return image;
}

PcxIndexedForm picture_form() {
	PcxIndexedForm form;
	for (size_t e = 0; e < form.ega.size(); ++e) form.ega[e] = {uint8_t(e * 11), uint8_t(e * 7 + 3), uint8_t(200 - e * 5)};
	form.ega[14] = {0xFF, 0xFF, 0xFF};
	form.ega[15] = {0, 0, 0};
	return form;
}

// A sky map: a grey picture, palette info 2 (greyscale), the EGA palette a descending grey ramp; rows ending in a
// lone byte or a run.
IndexedImage8 make_grey() {
	constexpr int kSide = 64;
	IndexedImage8 image;
	image.width = image.height = kSide;
	image.indices.resize(size_t(kSide) * kSide);
	for (int y = 0; y < kSide; ++y)
		for (int x = 0; x < kSide; ++x) image.indices[size_t(y) * kSide + x] = uint8_t(((x * x + y * 3) / 9) & 0xFF);
	for (int y = 0; y < kSide; y += 2) image.indices[size_t(y) * kSide + kSide - 1] = image.indices[size_t(y) * kSide + kSide - 2];
	for (int i = 0; i < 256; ++i) image.palette[i][0] = image.palette[i][1] = image.palette[i][2] = uint8_t(i);
	return image;
}

PcxIndexedForm grey_form() {
	PcxIndexedForm form;
	for (size_t e = 0; e < form.ega.size(); ++e) form.ega[e].fill(uint8_t(0x2D - 3 * e));
	form.palette_info = 2;
	return form;
}

RgbImage make_rgb() {
	RgbImage image;
	image.width = 32;
	image.height = 16;
	for (int y = 0; y < image.height; ++y)
		for (int x = 0; x < image.width; ++x)
			image.pixels.insert(image.pixels.end(), {uint8_t(x * 8), uint8_t(y * 16), uint8_t((x + y) & 1 ? 0xE0 : 0x20)});
	return image;
}

// --- TGA ---------------------------------------------------------------------------------------------------

std::vector<uint8_t> tga_pixels(uint32_t w, uint32_t h, bool opaque) {
	std::vector<uint8_t> rgba;
	for (uint32_t y = 0; y < h; ++y)
		for (uint32_t x = 0; x < w; ++x)
			rgba.insert(rgba.end(), {uint8_t(x * 16 + y), uint8_t(y * 32), uint8_t(255 - x * 9), uint8_t(opaque ? 255 : (x * 17) ^ (y * 29))});
	return rgba;
}

// --- TIL ---------------------------------------------------------------------------------------------------

TilOverlayEntry til_entry(int x, int z, uint8_t index, uint8_t flags, uint16_t layer) {
	TilOverlayEntry e;
	e.x_fixed = til_x_fixed_from_world(x);
	e.z_fixed = til_z_fixed_from_world(z);
	e.tile_index = index;
	e.flags = flags;
	e.layer = layer;
	return e;
}

TilFile make_til() {
	TilFile til;
	// Layer 0: each flag combination, indices across the atlas, positions west and south of the origin.
	for (uint8_t flags = 0; flags < 8; ++flags)
		til.entries.push_back(til_entry(-2880 + 16 * flags, -5392 + 32 * flags, uint8_t(flags * 9), flags, 0));
	til.entries.push_back(til_entry(0, 0, 63, 7, 0));
	// Later layers, ascending, the last the highest the shipped files use.
	til.entries.push_back(til_entry(4096, 2592, 41, 5, 1));
	til.entries.push_back(til_entry(4080, 2592, 42, 6, 1));
	til.entries.push_back(til_entry(-16, 16, 1, 3, 2));
	til.entries.push_back(til_entry(-32, 2576, 62, 4, 30));
	return til;
}

// --- CPT ---------------------------------------------------------------------------------------------------

constexpr int kDim = 1024;
constexpr int kBase = 64 * 256; // 64 world units

// The atlas: flat at 64 units but the first block of rows 0 to 15, whose ranges take each width the 4-bit field
// holds (a range of r a width of r + 1's bits: row k's first block a range of 2^k - 1, width k + 1, row 0 flat at
// width 1), and row 15's first block steeper than a block holds (clamped by the writer, read back clamped).
std::vector<uint16_t> make_cpt_depth() {
	std::vector<uint16_t> depth(size_t(kDim) * kDim, uint16_t(kBase));
	for (int k = 0; k < 16; ++k) {
		const int range = k < 15 ? (1 << k) - 1 : 40000;
		for (int i = 0; i < 256; ++i) {
			const int low = k < 15 ? kBase - range / 2 : 8000;
			depth[size_t(k) * kDim + i] = uint16_t(low + (i == 255 ? range : (range * i) / 255));
		}
	}
	return depth;
}

// A tile of a 5 x 5 vertex grid (32 triangles), its LODs alternately a list of all of them and a strip of the rows
// joined by degenerate triangles; the coarsest four LODs a list of the two corner triangles.
CptTile make_cpt_tile(int tx, int ty, int size, int serial) {
	CptTile tile;
	tile.tile_x = uint16_t(tx);
	tile.tile_y = uint16_t(ty);
	tile.tile_size = uint16_t(size);
	tile.vertex_count = 25;
	const int step = size / 4;
	for (int j = 0; j < 5; ++j)
		for (int i = 0; i < 5; ++i) tile.vertex_indices.insert(tile.vertex_indices.end(), {uint16_t(i * step), uint16_t(j * step)});
	std::vector<uint16_t> list, strip;
	for (int j = 0; j < 4; ++j)
		for (int i = 0; i < 4; ++i) {
			const uint16_t a = uint16_t(j * 5 + i), b = uint16_t(a + 1), c = uint16_t(a + 5), d = uint16_t(c + 1);
			list.insert(list.end(), {a, c, b, b, c, d});
		}
	for (int j = 0; j < 4; ++j) {
		if (j > 0) strip.insert(strip.end(), {strip.back(), uint16_t(j * 5)});
		for (int i = 0; i < 5; ++i) strip.insert(strip.end(), {uint16_t(j * 5 + i), uint16_t(j * 5 + 5 + i)});
	}
	for (int l = 0; l < 8; ++l) {
		CptTileLOD &lod = tile.lods[l];
		if (l >= 4) {
			lod.indices = {0, 20, 4, 4, 20, 24};
			lod.max_index = 24;
		} else if ((l + serial) & 1) {
			lod.indices = strip;
			lod.max_index = 24;
			lod.is_strip = true;
		} else {
			lod.indices = list;
			lod.max_index = 24;
		}
	}
	return tile;
}

// A leaf of every texel (65 x 65 = 4,225 vertices: the shipped tiles hold up to 5,815), as TrnGen writes its dense
// tiles: every LOD a strip, LOD 0 the whole grid's rows joined by degenerate triangles, the others the last row's
// strip from its second vertex on, every index past 4,095, so each 48-index chunk's least index is past what its
// 12-bit field holds and the writer stores 4,095 (the shipped .cpt files hold such chunks: Dvxi5 2, G13 27, I6 35).
CptTile make_dense_tile(int tx, int ty) {
	constexpr int kSide = 65;
	CptTile tile;
	tile.tile_x = uint16_t(tx);
	tile.tile_y = uint16_t(ty);
	tile.tile_size = 64;
	tile.vertex_count = kSide * kSide;
	for (int j = 0; j < kSide; ++j)
		for (int i = 0; i < kSide; ++i) tile.vertex_indices.insert(tile.vertex_indices.end(), {uint16_t(i), uint16_t(j)});
	std::vector<uint16_t> whole, last_row;
	for (int j = 0; j + 1 < kSide; ++j) {
		if (j > 0) whole.insert(whole.end(), {whole.back(), uint16_t(j * kSide)});
		for (int i = 0; i < kSide; ++i) whole.insert(whole.end(), {uint16_t(j * kSide + i), uint16_t((j + 1) * kSide + i)});
	}
	for (int i = 1; i < kSide; ++i)
		last_row.insert(last_row.end(), {uint16_t((kSide - 2) * kSide + i), uint16_t((kSide - 1) * kSide + i)});
	for (int l = 0; l < 8; ++l) {
		tile.lods[l].indices = l == 0 ? whole : last_row;
		tile.lods[l].max_index = uint16_t(kSide * kSide - 1);
		tile.lods[l].is_strip = true;
	}
	return tile;
}

CptFile make_cpt() {
	CptFile cpt;
	// The shipped header: bytes 4..7 00 04 05 00, the rest of the reserved bytes zero; Dvxi5.cpt's strings.
	cpt.header.reserved[1] = 0x04;
	cpt.header.reserved[2] = 0x05;
	std::strncpy(cpt.header.terrain_name, "Dvxi5", sizeof(cpt.header.terrain_name) - 1);
	std::strncpy(cpt.header.creator, "Brophy", sizeof(cpt.header.creator) - 1);
	std::strncpy(cpt.header.version, "jbrophy", sizeof(cpt.header.version) - 1);
	cpt.depth_format = DepthFormat::CDEP;
	cpt.depth_buffer = make_cpt_depth();
	int serial = 0;
	for (int size = kDim; size >= 64; size /= 2)
		for (int ty = 0; ty < kDim; ty += size)
			for (int tx = 0; tx < kDim; tx += size) cpt.tiles.push_back(make_cpt_tile(tx, ty, size, serial++));
	cpt.tiles.back() = make_dense_tile(960, 960); // the last leaf
	return cpt;
}

int guard(const std::string &path, const std::vector<uint8_t> &bytes, bool write_mode) {
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(test_io::read_file(path, committed), path + " missing; run with --write")) return 1;
	if (test_io::is_lfs_pointer(committed)) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return 0;
	}
	return expect(committed == bytes, path + " differs from the generator output; regenerate with --write") ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/terrain/variants";
	std::filesystem::create_directories(dir);
	int failures = 0;
	std::string error;

	// The PCX forms.
	struct Indexed {
		const char *name;
		IndexedImage8 image;
		PcxIndexedForm form;
	};
	const Indexed indexed[] = {{"charmap_m.pcx", make_charmap(), PcxIndexedForm{}},
	                           {"foliage_f.pcx", make_foliage(), foliage_form()},
	                           {"picture.pcx", make_picture(), picture_form()},
	                           {"grey_info2.pcx", make_grey(), grey_form()}};
	for (const Indexed &v : indexed) {
		std::vector<uint8_t> bytes;
		failures += !expect(encode_pcx_indexed(v.image, bytes, error, &v.form), std::string(v.name) + ": " + error);
		failures += guard(dir + "/" + v.name, bytes, write_mode);
	}
	{
		std::vector<uint8_t> bytes;
		failures += !expect(encode_pcx_rgb(make_rgb(), bytes, error), "rgb24.pcx: " + error);
		failures += guard(dir + "/rgb24.pcx", bytes, write_mode);
	}

	// The TGA forms.
	struct Tga {
		const char *name;
		tga::TgaForm form;
	};
	for (const Tga &v : {Tga{"trntile_a.tga", {32, tga::TGA_DESCRIPTOR_ALPHA_8, true}},
	                     Tga{"map_d1.tga", {32, tga::TGA_DESCRIPTOR_ALPHA_8, true}}, Tga{"map_c.tga", {24, 0, true}},
	                     Tga{"bare24.tga", {24, 0, false}}}) {
		const bool blend = std::strcmp(v.name, "map_d1.tga") == 0;
		const std::vector<uint8_t> rgba = tga_pixels(blend ? 8 : 16, 8, v.form.bits == 24);
		std::vector<uint8_t> bytes;
		failures += !expect(tga::tga_write(rgba.data(), blend ? 8 : 16, 8, v.form, bytes, error), std::string(v.name) + ": " + error);
		failures += guard(dir + "/" + v.name, bytes, write_mode);
	}

	// MED's .til.
	{
		std::vector<uint8_t> bytes;
		failures += !expect(save_til(make_til(), bytes, error), "med.til: " + error);
		failures += guard(dir + "/med.til", bytes, write_mode);
	}

	// The shipped polydata form.
	{
		const CptFile cpt = make_cpt();
		std::vector<uint8_t> bytes;
		failures += !expect(save_cpt(cpt, bytes, error), "shipped.cpt: " + error);
		std::vector<uint16_t> clamped = cpt.depth_buffer;
		cpt_clamp_steep_blocks(clamped.data(), clamped.size());
		CptFile back;
		failures += !expect(load_cpt(bytes.data(), bytes.size(), back, error) && back.depth_buffer == clamped &&
		                            back.tiles.size() == 341u && cpt_steep_blocks(cpt.depth_buffer) == 1,
		                    "shipped.cpt reads back its clamped atlas and 341 tiles");
		failures += guard(dir + "/shipped.cpt", bytes, write_mode);
	}

	if (failures == 0 && !write_mode) std::printf("OK: fixtures/terrain/variants byte-reproducible\n");
	return failures == 0 ? 0 : 1;
}
