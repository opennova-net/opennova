// Generator + guard for the synthetic terrain map fixtures/terrain/tmap
// (fixtures/README.md): Tmap.trn, Tmap.cpt, Tmap_f.pcx and Tmap_m.pcx, minted by our own
// writers (save_trn, CptFile::write, encode_pcx_indexed) from integer data,
// so every platform mints the same bytes. It is the second committed map
// with the same sector layout and synthetic terrain art used by
// godot/tests/support/runtime_fixture.gd (TestFs stages the combined root), but with
// water at 21, its own single-surface charmap (index 1, the value retail's
// Dvxi5 carries everywhere; the sim records no bounce or impact on surface
// 0), two foliage definitions, a painted foliage map and the relief the
// terrain tests need, shaped after what the retail Dvxi5 bake has around
// its own origin: ground at 36 rising gently east of the world origin (a
// thrown round meets climbing ground: the grenade-bounce pins) and along z
// (every cell sloped: the game_world_test surface-normal witness at world
// (6, -4)); near the tile-cache camera at world (64, 27, 64) the ground
// stays within the 42-unit detail-cell reach while a steep rise just east
// of it lifts the surrounding 128-cell quadtree node's height center far
// enough above the camera that quality 0.3 selects 128-unit pages over the
// near cells and 1.0 still reaches the 64-unit leaves; a half-unit checker
// under the static-shadow witness at world (64.125, 64.125) separates the
// point and bilinear samples; the low corner drops under the water plane.
//
// Default: rebuild all three in memory and byte-compare the committed
// files; `--write` (re)writes them. Every run prints the pins the GUT
// consumers carry (the point/bilinear heights at the shadow witness).
#include <formats/cpt/cpt.h>
#include <formats/cpt/cpt_io.h>
#include <formats/pcx/pcx_io.h>
#include <formats/trn/trn_io.h>
#include <runtime/terrain_query/height_field.h>

#include "common/file_io.h"
#include "common/test_paths.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace opennova;

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

constexpr int kDim = 1024;               // the heightmap atlas side
constexpr int kOrigin = 512;             // world (0, 0) in atlas cells (sector origin -4)
constexpr int kOriginHeight = 36;        // ground at the world origin, world units
constexpr int kNearRiseRaw = 32;         // 0.125 units per cell along x over the first 64 cells east
constexpr int kNearRiseCells = 64;
constexpr int kFarRiseStart = 592;       // the steep rise east of the tile-cache camera spot
constexpr int kFarRiseRaw = 192;         // 0.75 units per cell over the next 64 cells
constexpr int kFarRiseCells = 64;
constexpr int kSlopeZRaw = 16;           // 0.0625 units per cell along z (raw16)
constexpr int kMinHeight = 12;
constexpr int kMaxHeight = 240;
constexpr int kWaterHeight = 21;
constexpr int kMapSize = 256;            // foliage map texels (one per 4 atlas cells)
constexpr int kHeightScale = 256;        // raw16 = height * 256

// A parabolic bump of `amplitude` world units over radius r (integer only).
int bump_raw16(int x, int z, int cx, int cz, int radius, int amplitude) {
	const long long dx = x - cx;
	const long long dz = z - cz;
	const long long d2 = dx * dx + dz * dz;
	const long long r2 = static_cast<long long>(radius) * radius;
	if (d2 >= r2) return 0;
	return static_cast<int>((static_cast<long long>(amplitude) * kHeightScale * (r2 - d2)) / r2);
}

bool inside_radius(int x, int z, int cx, int cz, int radius) {
	const long long dx = x - cx;
	const long long dz = z - cz;
	return dx * dx + dz * dz < static_cast<long long>(radius) * radius;
}

uint16_t depth_at(int x, int z) {
	// Two bounded x rises (a slope along a whole 256-sample CDEP block would
	// cost wide deltas in every block): a gentle one from the origin so a
	// thrown round meets climbing ground, and a steep one east of the
	// tile-cache camera spot at atlas (576, 576) so the 128-cell quadtree node
	// around it spans 36..100 while the spot itself stays near 48.
	const int near_rise = std::clamp(x - kOrigin, 0, kNearRiseCells) * kNearRiseRaw;
	const int far_rise = std::clamp(x - kFarRiseStart, 0, kFarRiseCells) * kFarRiseRaw;
	int raw = kOriginHeight * kHeightScale + near_rise + far_rise + (z - kOrigin) * kSlopeZRaw;
	raw += bump_raw16(x, z, 620, 620, 20, 12);   // a hill on the steep rise
	raw += bump_raw16(x, z, 704, 576, 40, -12);  // a basin beside it
	if (inside_radius(x, z, 576, 576, 24))          // the half-unit checker under the shadow witness
		raw += ((x + z) & 1) ? kHeightScale / 2 : -kHeightScale / 2;
	if (raw < kMinHeight * kHeightScale) raw = kMinHeight * kHeightScale;
	if (raw > kMaxHeight * kHeightScale) raw = kMaxHeight * kHeightScale;
	return static_cast<uint16_t>(raw);
}

std::vector<uint16_t> make_depth() {
	std::vector<uint16_t> depth(static_cast<size_t>(kDim) * kDim);
	for (int z = 0; z < kDim; ++z)
		for (int x = 0; x < kDim; ++x)
			depth[static_cast<size_t>(z) * kDim + x] = depth_at(x, z);
	return depth;
}

// The full 1024 -> 64 quadtree, every tile a 5x5 vertex grid drawn as 32
// triangles on all eight LODs (the retail bake simplifies per LOD; this map
// only needs a valid, fully covering mesh at every level).
CptTile make_tile(int tx, int tz, int size) {
	CptTile tile;
	tile.tile_x = static_cast<uint16_t>(tx);
	tile.tile_y = static_cast<uint16_t>(tz);
	tile.tile_size = static_cast<uint16_t>(size);
	tile.vertex_count = 25;
	tile.is_single = false;
	const int step = size / 4;
	for (int j = 0; j < 5; ++j) {
		for (int i = 0; i < 5; ++i) {
			tile.vertex_indices.push_back(static_cast<uint16_t>(i * step));
			tile.vertex_indices.push_back(static_cast<uint16_t>(j * step));
		}
	}
	std::vector<uint16_t> indices;
	for (int j = 0; j < 4; ++j) {
		for (int i = 0; i < 4; ++i) {
			const uint16_t a = static_cast<uint16_t>(j * 5 + i);
			const uint16_t b = static_cast<uint16_t>(a + 1);
			const uint16_t c = static_cast<uint16_t>(a + 5);
			const uint16_t d = static_cast<uint16_t>(c + 1);
			indices.insert(indices.end(), {a, c, b, b, c, d});
		}
	}
	for (CptTileLOD &lod : tile.lods) {
		lod.indices = indices;
		lod.max_index = 24;
		lod.is_strip = false;
	}
	return tile;
}

CptFile make_cpt() {
	CptFile cpt;
	cpt.header.magic = CptFile::MAGIC;
	std::strncpy(cpt.header.terrain_name, "Tmap", sizeof(cpt.header.terrain_name) - 1);
	std::strncpy(cpt.header.creator, "OpenNova", sizeof(cpt.header.creator) - 1);
	std::strncpy(cpt.header.version, "1", sizeof(cpt.header.version) - 1);
	cpt.depth_format = DepthFormat::CDEP;
	cpt.depth_buffer = make_depth();
	for (int size = kDim; size >= 64; size /= 2)
		for (int tz = 0; tz < kDim; tz += size)
			for (int tx = 0; tx < kDim; tx += size)
				cpt.tiles.push_back(make_tile(tx, tz, size));
	return cpt;
}

// An eight-sector layout modeled on the retail Dvxi5 grid
// with this map's own polydata, foliage map, water and foliage definitions.
TrnConfig make_trn() {
	TrnConfig c;
	c.name = "Tmap";
	c.polydata = "Tmap.cpt";
	c.colormap = "mnml_c.tga";
	c.detailmap = "mnml_dm.tga";
	c.detailmap_c1 = "mnml_dc1.tga";
	c.detailmap_c2 = "mnml_dc2.tga";
	c.detailmap_c3 = "mnml_dc3.tga";
	c.detailmapdist = "mnml_dmd.tga";
	c.detailblendmap = "mnml_d1.tga";
	c.charmap = "Tmap_m.pcx";
	c.foliagemap = "Tmap_f.pcx";
	c.tilestrip = "mnml_t.tga";
	c.detail_density = 128;
	c.detail_density2 = 8;
	c.sector_count = 8;
	c.sector_rows = 8;
	c.origin_x = -4;
	c.origin_y = -4;
	c.water_height = kWaterHeight;
	c.sector_grid[3][3] = 1;
	c.sector_grid[3][4] = 3;
	c.sector_grid[4][3] = 2;
	c.sector_grid[4][4] = 4;
	FoliageDef bush1;
	bush1.graphic = "bush1.3di";
	bush1.color_lower = 0;
	bush1.color_upper = 2;
	bush1.match[0] = 254;
	FoliageDef bush2 = bush1;
	bush2.graphic = "bush2.3di";
	bush2.match[0] = 253;
	c.foliage_defs = {bush1, bush2};
	return c;
}

// The surface map (charmap): one surface everywhere, index 1, the value the
// retail Dvxi5 charmap carries on every texel (the minimal set's own is all 0).
IndexedImage8 make_surface_map() {
	IndexedImage8 image;
	image.width = kMapSize;
	image.height = kMapSize;
	image.indices.assign(static_cast<size_t>(kMapSize) * kMapSize, 1);
	image.palette[1][0] = 96; image.palette[1][1] = 96; image.palette[1][2] = 96;
	return image;
}

// The foliage map: unpainted 255 everywhere, match 254 in 9x9 blocks around
// the two witnesses foliage_game_world_integration_test samples (the
// sector-routed model witness at map (98, 122) and the flat detail witness at
// (30, 107); their mirrored/flat counterparts stay 255), plus a small 253
// patch so both definitions have coverage.
IndexedImage8 make_foliage_map() {
	IndexedImage8 image;
	image.width = kMapSize;
	image.height = kMapSize;
	image.indices.assign(static_cast<size_t>(kMapSize) * kMapSize, 255);
	auto paint = [&](int x, int y, uint8_t index) {
		image.indices[static_cast<size_t>(y) * kMapSize + x] = index;
	};
	for (int dy = -4; dy <= 4; ++dy) {
		for (int dx = -4; dx <= 4; ++dx) {
			paint(98 + dx, 122 + dy, 254);
			paint(30 + dx, 107 + dy, 254);
		}
	}
	for (int y = 40; y < 44; ++y)
		for (int x = 200; x < 204; ++x)
			paint(x, y, 253);
	image.palette[253][0] = 0; image.palette[253][1] = 160; image.palette[253][2] = 0;
	image.palette[254][0] = 0; image.palette[254][1] = 255; image.palette[254][2] = 0;
	return image;
}

int guard(const std::string &path, const std::vector<uint8_t> &bytes, bool write_mode) {
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(test_io::read_file(path, committed), (path + " missing; run with --write").c_str())) return 1;
	if (test_io::is_lfs_pointer(committed)) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return 0;
	}
	return expect(committed == bytes, (path + " differs from the generator output; regenerate with --write").c_str()) ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/terrain/tmap";
	std::filesystem::create_directories(dir);
	int failures = 0;

	// --- Tmap.trn ---
	const TrnConfig trn = make_trn();
	std::ostringstream trn_text;
	std::string err;
	if (!expect(save_trn(trn_text, trn, err), ("save_trn: " + err).c_str())) return 1;
	const std::string trn_string = trn_text.str();
	TrnConfig reloaded;
	std::istringstream trn_in(trn_string);
	if (!expect(load_trn(trn_in, reloaded, err), ("load_trn: " + err).c_str())) return 1;
	failures += !expect(reloaded.name == "Tmap" && reloaded.polydata == "Tmap.cpt" &&
	                            reloaded.foliagemap == "Tmap_f.pcx" && reloaded.charmap == "Tmap_m.pcx" &&
	                            reloaded.water_height == kWaterHeight &&
	                            reloaded.foliage_defs.size() == 2 && reloaded.foliage_defs[0].match[0] == 254 &&
	                            reloaded.foliage_defs[1].match[0] == 253 && reloaded.sector_count == 8 &&
	                            reloaded.sector_grid[3][3] == 1 && reloaded.sector_grid[4][4] == 4,
	                    "Tmap.trn round-trips its identity, water, foliage and sector block");
	failures += guard(dir + "/Tmap.trn", std::vector<uint8_t>(trn_string.begin(), trn_string.end()), write_mode);

	// --- Tmap.cpt (written through the CDEP/POLY writer, then compared as bytes) ---
	const CptFile cpt = make_cpt();
	const std::string cpt_tmp = std::string(test_paths_temp_dir()) + "/" + test_paths_unique("minimal_terrain_gen_tmap", ".cpt");
	cpt.write(cpt_tmp);
	std::vector<uint8_t> cpt_bytes;
	failures += !expect(test_io::read_file(cpt_tmp, cpt_bytes), "read the written Tmap.cpt");
	std::filesystem::remove(cpt_tmp);
	CptFile parsed;
	std::string cpt_err;
	failures += !expect(load_cpt(cpt_bytes.data(), cpt_bytes.size(), parsed, cpt_err) &&
	                            parsed.depth_format == DepthFormat::CDEP && parsed.depth_buffer == cpt.depth_buffer &&
	                            parsed.tiles.size() == 341,
	                    "Tmap.cpt parses back to the minted depth atlas and its 341 tiles");
	failures += guard(dir + "/Tmap.cpt", cpt_bytes, write_mode);

	// --- Tmap_f.pcx ---
	std::vector<uint8_t> pcx_bytes;
	failures += !expect(encode_pcx_indexed(make_foliage_map(), pcx_bytes, err), ("encode_pcx_indexed: " + err).c_str());
	IndexedImage8 pcx_back;
	failures += !expect(decode_pcx_indexed(pcx_bytes.data(), pcx_bytes.size(), pcx_back, err) &&
	                            pcx_back.width == kMapSize && pcx_back.indices[122 * kMapSize + 98] == 254 &&
	                            pcx_back.indices[107 * kMapSize + 30] == 254 && pcx_back.indices[250 * kMapSize + 226] == 255,
	                    "Tmap_f.pcx decodes with the two 254 witnesses on a 255 field");
	failures += guard(dir + "/Tmap_f.pcx", pcx_bytes, write_mode);

	// --- Tmap_m.pcx ---
	std::vector<uint8_t> charmap_bytes;
	failures += !expect(encode_pcx_indexed(make_surface_map(), charmap_bytes, err), ("encode_pcx_indexed (charmap): " + err).c_str());
	failures += guard(dir + "/Tmap_m.pcx", charmap_bytes, write_mode);

	// --- the pins the GUT consumers carry (printed every run) ---
	terrain::TerrainHeightField field;
	field.heightmap = cpt.depth_buffer.data();
	field.dim = kDim;
	field.layout.sector_grid = &trn.sector_grid[0][0];
	field.layout.origin_x = trn.origin_x;
	field.layout.origin_y = trn.origin_y;
	const float point = terrain::height_field_height_world(field, 64.125f, 64.125f);
	const float bilinear = terrain::height_field_height_world_bilinear(field, 64.125f, 64.125f);
	const terrain::TerrainSurfaceNormal normal = terrain::height_field_surface_normal_world(field, 6.0f, -4.0f);
	std::printf("pins: shadow witness (64.125, 64.125): point=%.5f bilinear=%.5f; normal at (6, -4): up=%.5f\n",
	            point, bilinear, normal.up);
	failures += !expect(std::fabs(point - bilinear) > 0.05f, "the checker separates the point and bilinear samples");
	failures += !expect(normal.up < 0.9999, "the sloped cell at (6, -4) is not flat");
	// The water plane must have ground on both sides (the minimap water mask
	// test picks its wet and dry cells from the atlas).
	failures += !expect(cpt.depth_buffer.front() < kWaterHeight * kHeightScale &&
	                            cpt.depth_buffer.back() > kWaterHeight * kHeightScale,
	                    "the low corner sits under the water plane and the high corner above it");

	if (failures == 0 && !write_mode) std::printf("OK: fixtures/terrain/tmap byte-reproducible\n");
	return failures == 0 ? 0 : 1;
}
