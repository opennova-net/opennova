// The TrnGen.exe terrain bake (engine/formats/cpt/trngen/terrain_bake): a depth map to the .cpt the
// game reads. Always: a bake of a procedural map parses back through the engine's own .cpt reader
// as the game reads it (CDEP, the header's level count 5, the 341 tiles of the 1024 -> 64 quadtree,
// eight LOD lists each, every vertex inside its tile, every index inside its vertices), its depth
// atlas the leaves rasterized over the map, and a second bake of the same map is the same bytes.
//
// By hand, the byte parity with TrnGen.exe itself: `cpt_trngen_bake_test <dir>` bakes each
// historical TrnGen project under <dir> (Sample, Gradient, Checker64 and Perlin: `<case>/<Name>.tpj`
// with its 8-bit `.raw` depth map and TrnGen's own `<case>.cpt`, the corpus retired from fixtures/
// with the old builder; `git show d57608b3d^:fixtures/terrain/...` through `git lfs smudge`
// restores it) and compares every byte. ctest runs the first part alone; the retail-corpus byte diff of
// the CPT writer the bake writes with is the re-encode leg of the gated cpt_jo_assets_sweep
// (tests/terrain/cpt_jo_assets_sweep_test.cpp).
#include <formats/cpt/trngen/lod_mesh_data.h>
#include <formats/cpt/trngen/terrain_bake.h>
#include <formats/cpt/trngen/tristrip.h>
#include <formats/cpt/cpt_io.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::trngen;

namespace {

int failures = 0;

void expect(bool ok, const std::string &what) {
	if (!ok) {
		++failures;
		std::fprintf(stderr, "FAIL: %s\n", what.c_str());
	}
}

std::vector<uint8_t> read_file(const std::filesystem::path &path) {
	std::ifstream in(path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// An island: a raised disc with two hills on it, the sea floor at 10, all in TrnGen's 8-bit range.
std::vector<uint8_t> island_depth8() {
	std::vector<uint8_t> depth(static_cast<size_t>(kDepthSide) * kDepthSide);
	for (int y = 0; y < kDepthSide; ++y)
		for (int x = 0; x < kDepthSide; ++x) {
			const double dx = (x - 512) / 400.0, dy = (y - 512) / 400.0;
			const double r = std::sqrt(dx * dx + dy * dy);
			double h = 10.0 + 80.0 * std::max(0.0, 1.0 - r * r);
			h += 60.0 * std::exp(-((x - 430) * (x - 430) + (y - 470) * (y - 470)) / 4000.0);
			h += 40.0 * std::exp(-((x - 620) * (x - 620) + (y - 600) * (y - 600)) / 2500.0);
			depth[static_cast<size_t>(y) * kDepthSide + x] = static_cast<uint8_t>(std::min(255.0, h));
		}
	return depth;
}

void test_bake_reads_back() {
	TerrainBakeInput input;
	input.depth8 = island_depth8();
	input.terrain_name = "Island";
	input.creator = "OpenNova";
	input.version = "opennova";
	input.threads = TerrainBakeInput::for_hardware(); // the embedder's count: the desktop sizing
	std::vector<uint8_t> bytes;
	std::string error;
	expect(bake_terrain_bytes(input, bytes, error), "the island bakes: " + error);

	CptFile cpt;
	expect(load_cpt(bytes.data(), bytes.size(), cpt, error), "the .cpt reads back: " + error);
	expect(cpt.depth_format == DepthFormat::CDEP, "the depth section is CDEP");
	expect(std::memcmp(bytes.data(), "CPT1", 4) == 0, "the magic is CPT1");
	expect(bytes[6] == 5 && bytes[7] == 0, "the header's +6 level count is 5");
	expect(bytes[4] == 0x00 && bytes[5] == 0x04, "the header's +4 word is the root's size, 1024");
	expect(std::string(cpt.header.terrain_name) == "Island" && std::string(cpt.header.creator) == "OpenNova" &&
	               std::string(cpt.header.version) == "opennova",
	       "the header's strings are the input's");
	expect(cpt.tiles.size() == 341, "341 tiles: 1 + 4 + 16 + 64 + 256, got " + std::to_string(cpt.tiles.size()));

	// The depth atlas: the smoothed map where no leaf triangle covers a texel, the simplified mesh
	// rasterized elsewhere (TrnGen's Output.dep), which on this smooth island stays within two
	// world units of the map; most texels are within a quarter unit.
	const std::vector<uint16_t> smoothed = smooth_depthmap(input.depth8);
	expect(cpt.depth_buffer.size() == smoothed.size(), "the atlas is 1024 x 1024");
	int far = 0, near = 0;
	for (size_t i = 0; i < smoothed.size() && i < cpt.depth_buffer.size(); ++i) {
		const int off = std::abs(static_cast<int>(cpt.depth_buffer[i]) - static_cast<int>(smoothed[i]));
		if (off > 512) ++far;
		if (off <= 64) ++near;
	}
	expect(far == 0, "the rasterized atlas stays within two world units of the map (" + std::to_string(far) + " far)");
	expect(near > static_cast<int>(smoothed.size() * 9 / 10), "nine in ten texels are within a quarter unit");

	// Every tile, level by level, rows outer: its size, eight LOD lists, its vertices inside it and
	// its indices inside its vertices.
	size_t at = 0;
	int bad = 0;
	for (int size = 1024; size >= 64; size /= 2)
		for (int y = 0; y < 1024; y += size)
			for (int x = 0; x < 1024; x += size, ++at) {
				if (at >= cpt.tiles.size()) break;
				const CptTile &tile = cpt.tiles[at];
				if (tile.tile_size != size || tile.vertex_count == 0) ++bad;
				// The root's one tile is the first leaf's mesh (TrnGen's export); every other tile
				// stands where the walk puts it.
				if (size != 1024 && (tile.tile_x != x || tile.tile_y != y)) ++bad;
				for (int lod = 0; lod < 8; ++lod) {
					const CptTileLOD &list = tile.lods[lod];
					if (list.indices.empty() || !list.is_strip) ++bad;
					for (uint16_t index : list.indices)
						if (index >= tile.vertex_count) ++bad;
				}
				for (size_t v = 0; v < tile.vertex_indices.size(); ++v)
					if (tile.vertex_indices[v] > 64 * (size / 64) + 1) ++bad;
			}
	expect(bad == 0, "every tile is in place, eight strips each, indices and vertices in range (" +
	                         std::to_string(bad) + " bad)");

	// Again on four threads: the same bytes whatever the thread count and whatever ran before.
	std::vector<uint8_t> again;
	input.threads = 4;
	expect(bake_terrain_bytes(input, again, error) && again == bytes,
	       "a second bake, on four threads, is byte-identical");

	TerrainBakeInput wrong;
	wrong.depth8.assign(16, 0);
	expect(!bake_terrain_bytes(wrong, bytes, error) && error.find("1024") != std::string::npos,
	       "a map of another size is refused in words: " + error);
}

void test_bake_16bit() {
	// A 16-bit map is the raw16 heights as they are: a flat plane at 40 units stays 40 units.
	TerrainBakeInput input;
	input.depth16.assign(static_cast<size_t>(kDepthSide) * kDepthSide, static_cast<uint16_t>(40 * 256));
	input.terrain_name = "Flat";
	input.threads = TerrainBakeInput::for_hardware();
	std::vector<uint8_t> bytes;
	std::string error;
	expect(bake_terrain_bytes(input, bytes, error), "the flat map bakes: " + error);
	CptFile cpt;
	expect(load_cpt(bytes.data(), bytes.size(), cpt, error) && cpt.tiles.size() == 341, "the flat .cpt reads back");
	bool flat = !cpt.depth_buffer.empty();
	for (uint16_t raw : cpt.depth_buffer) flat = flat && raw == 40 * 256;
	expect(flat, "the flat map's atlas is 40 units everywhere");
}

// A TrnGen project file: `key "value"` lines, `lock_<corner> x y`.
struct Tpj {
	std::string name, creator, depthmap;
	std::array<CornerLockFlags, 4> locks{};
};

Tpj read_tpj(const std::filesystem::path &path) {
	Tpj tpj;
	std::ifstream in(path);
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream words(line);
		std::string key;
		words >> key;
		const auto quoted = [&] {
			const size_t a = line.find('"'), b = line.find('"', a + 1);
			return a == std::string::npos || b == std::string::npos ? std::string() : line.substr(a + 1, b - a - 1);
		};
		const char *corners[] = {"lock_topleft", "lock_topright", "lock_bottomleft", "lock_bottomright"};
		if (key == "terrainname") tpj.name = quoted();
		else if (key == "creator") tpj.creator = quoted();
		else if (key == "depthmap") tpj.depthmap = quoted();
		for (int i = 0; i < 4; ++i)
			if (key == corners[i]) words >> tpj.locks[i].x >> tpj.locks[i].y;
	}
	return tpj;
}

void test_trngen_parity(const std::filesystem::path &root) {
	const char *cases[][2] = {{"sample", "Sample"}, {"gradient", "Gradient"}, {"checker64", "Checker64"},
	                          {"perlin", "Perlin"}};
	for (const auto &each : cases) {
		const std::filesystem::path dir = root / each[0];
		const Tpj tpj = read_tpj(dir / (std::string(each[1]) + ".tpj"));
		TerrainBakeInput input;
		// TrnGen opens the project's depth map case-insensitively (Windows): find it by name.
		for (const auto &entry : std::filesystem::directory_iterator(dir)) {
			std::string a = entry.path().filename().string(), b = tpj.depthmap;
			for (char &c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			for (char &c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			if (a == b) input.depth8 = read_file(entry.path());
		}
		input.terrain_name = tpj.name;
		input.creator = tpj.creator;
		input.version = "taylor"; // the Windows user TrnGen ran as when it made the goldens
		input.locks = tpj.locks;
		input.depth_format = DepthFormat::DPTH; // TrnGen's own depth section
		input.threads = TerrainBakeInput::for_hardware();
		std::vector<uint8_t> bytes;
		std::string error;
		const bool baked = bake_terrain_bytes(input, bytes, error);
		const std::vector<uint8_t> golden = read_file(dir / (std::string(each[0]) + ".cpt"));
		size_t differ = 0, first = 0;
		for (size_t i = 0; i < std::min(bytes.size(), golden.size()); ++i)
			if (bytes[i] != golden[i] && differ++ == 0) first = i;
		const bool same = baked && !golden.empty() && bytes.size() == golden.size() && differ == 0;
		std::printf("%-10s %s (%zu bytes, golden %zu, %zu differ from 0x%zx)\n", each[0],
		            same ? "byte-identical" : "DIFFERS", bytes.size(), golden.size(), differ, first);
		expect(same, std::string(each[0]) + " is byte-identical with TrnGen's .cpt " + error);
	}
}

// The port's buffer guards refuse in words, nothing written (no exception: ADR 0049 d5), and the
// thread count is the embedder's, the desktop sizing at least one.
void test_refusals() {
	LODMeshData mesh;
	LODMeshData_Init(&mesh);
	const float vertex[10] = {};
	expect(LODMeshData_SetVertex(&mesh, 1 << 27, vertex) == -1 && mesh.vertex_data == nullptr &&
	               mesh.failure.find("SetVertex") != std::string::npos,
	       "a vertex past 256 MiB is refused, the mesh's failure in words: " + mesh.failure);
	expect(LODMeshData_SetVertex(&mesh, 0, vertex) == 0 && mesh.failure.find("SetVertex") != std::string::npos,
	       "a sound vertex is set, the first failure kept");
	LODMeshData_Free(&mesh);

	TriStripResult strips;
	std::string error;
	const uint16_t face[3] = {0, 1, 2};
	expect(!build_triangle_strips(face, -1, 3, strips, error) && error.find("negative") != std::string::npos,
	       "a negative face count is refused in words: " + error);
	expect(build_triangle_strips(face, 1, 3, strips, error) && strips.total_indices == 3,
	       "one face is one strip of three");

	expect(TerrainBakeInput{}.threads == 1 && TerrainBakeInput::for_hardware() >= 1,
	       "a bake runs on the caller's thread unless the embedder asks for more");
}

} // namespace

int main(int argc, char **argv) {
	test_bake_reads_back();
	test_bake_16bit();
	test_refusals();
	if (argc > 1) test_trngen_parity(argv[1]);
	if (failures == 0) std::printf("cpt_trngen_bake: all passed\n");
	return failures == 0 ? 0 : 1;
}
