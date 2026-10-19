// The region bake over the shipped .cpt files (OPENNOVA_JO_ASSETS; Skipped without it). A shipped .cpt
// keeps no depth input (its tool's map is lost; the CDEP atlas is the leaves drawn back over it), so the
// editor sculpts it with input := atlas and a cold in-memory bake's state (docs/terrain/terrain-re.md, the
// region bake, D-TERRAIN-23):
//
// - every file: each leaf in place, and each leaf drawn back from its own LOD 0 (the strips read as D3D
//   draws them, odd triangles reversed [orig: Terrain_RenderSectorBatch @ 0x609b7e..0x609bd4]) at the atlas
//   heights, under its .trn's locks, against the atlas: printed (W3), the leaves' triangles all facing the
//   draw-back's way but where a tile holds a flipped face;
// - three files (Dvxi5 an island, Dvxg3 a tiled map, g14 locked): a cold bake's state from the atlas; a region
//   bake of nothing leaves the file's bytes as they were; a one-leaf edit remakes that leaf and its three
//   ancestors (and the root's tile at leaf (0, 0)), every other tile and every texel outside the leaf's footprint
//   as shipped, and the file still writes and reads back;
// - the other two whose tiles hold flipped faces (Dvxi4 an island, dvxg9 locked; Dvxg3 the third): the cold bake
//   they are sculpted from (input := atlas, its .trn's locks) bakes.
//
// By hand, `cpt_region_bake_retail_test <dir>` cold-bakes every .cpt under <dir> as well (JO:CA's 23 bake,
// about ten minutes on eight threads).
#include <formats/cpt/cpt_io.h>
#include <formats/cpt/trngen/terrain_bake.h>
#include <formats/cpt/trngen/terrain_bake_state.h>
#include <formats/cpt/trngen/terrain_mesh.h>
#include <formats/cpt/trngen/terrain_region_bake.h>
#include <formats/trn/trn_io.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_parts.h"

using namespace opennova;
using namespace opennova::trngen;

namespace {

int failures = 0;

void expect(bool ok, const std::string &what) {
	if (!ok) {
		++failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

using Locks = std::array<CornerLockFlags, 4>;

std::string lower(std::string s) {
	for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

// The locks of the first .trn (by name) naming each .cpt.
std::map<std::string, Locks> trn_locks(const std::filesystem::path &dir) {
	std::vector<std::filesystem::path> trns;
	for (const auto &entry : std::filesystem::directory_iterator(dir))
		if (entry.is_regular_file() && lower(entry.path().extension().string()) == ".trn") trns.push_back(entry.path());
	std::sort(trns.begin(), trns.end());
	std::map<std::string, Locks> out;
	for (const auto &path : trns) {
		std::ifstream in(path, std::ios::binary);
		TrnConfig config;
		std::string error;
		load_trn(in, config, error);
		if (config.polydata.empty()) continue;
		const TerrainQuadrantLocks q = config.get_quadrant_locks();
		Locks locks{};
		for (size_t i = 0; i < 4; ++i) locks[i] = {q[i].x, q[i].y};
		out.emplace(lower(config.polydata), locks);
	}
	return out;
}

uint16_t tap(const std::vector<uint16_t> &depth, const Locks &locks, int corner_x, int corner_y, int x, int y) {
	const CornerLockFlags &lock = locks[static_cast<size_t>((corner_x >= 0x200 ? 1 : 0) + (corner_y >= 0x200 ? 2 : 0))];
	const int bx = ((lock.x ? (corner_x & 0x200) : 0) + (x & (lock.x ? 511 : 1023))) & 0x3FF;
	const int by = ((lock.y ? (corner_y & 0x200) : 0) + (y & (lock.y ? 511 : 1023))) & 0x3FF;
	return depth[static_cast<size_t>(by) * kDepthSide + static_cast<size_t>(bx)];
}

// W3: every leaf drawn back from its own LOD 0 at the atlas heights; the changed texels.
void check_draw_back(const std::string &name, const CptFile &cpt, const Locks &locks) {
	std::vector<uint16_t> drawn = cpt.depth_buffer;
	int misplaced = 0;
	long facing = 0, flipped = 0;
	// The walk's leaf order: nw, ne, sw, se depth first.
	std::vector<std::pair<int, int>> walk;
	std::vector<std::array<int, 3>> stack{{0, 0, 1024}};
	while (!stack.empty()) {
		const auto [x, y, size] = stack.back();
		stack.pop_back();
		if (size == 64) {
			walk.push_back({x, y});
			continue;
		}
		const int h = size / 2;
		stack.push_back({x + h, y + h, h});
		stack.push_back({x, y + h, h});
		stack.push_back({x + h, y, h});
		stack.push_back({x, y, h});
	}
	for (const auto &[x, y] : walk) {
		const CptTile &tile = cpt.tiles[static_cast<size_t>(cpt_tile_index(4, x, y))];
		if (tile.tile_x != x || tile.tile_y != y || tile.tile_size != 64) ++misplaced;
		std::vector<float> positions(static_cast<size_t>(tile.vertex_count) * 3u);
		for (int i = 0; i < tile.vertex_count; ++i) {
			const int ax = tile.tile_x + tile.vertex_indices[static_cast<size_t>(i) * 2u];
			const int ay = tile.tile_y + tile.vertex_indices[static_cast<size_t>(i) * 2u + 1u];
			positions[static_cast<size_t>(i) * 3u] = static_cast<float>(ax);
			positions[static_cast<size_t>(i) * 3u + 1u] =
				static_cast<float>(tap(cpt.depth_buffer, locks, tile.tile_x, tile.tile_y, ax, ay)) * 0.00390625f;
			positions[static_cast<size_t>(i) * 3u + 2u] = static_cast<float>(ay);
		}
		std::vector<uint16_t> faces;
		const CptTileLOD &lod = tile.lods[0];
		for (size_t i = 0; lod.is_strip && i + 2 < lod.indices.size(); ++i) {
			uint16_t a = lod.indices[i], b = lod.indices[i + 1];
			const uint16_t c = lod.indices[i + 2];
			if (a == b || b == c || a == c) continue;
			if (i & 1u) std::swap(a, b);
			faces.insert(faces.end(), {a, b, c});
			const float *p = positions.data();
			const float o = (p[c * 3] - p[a * 3]) * (p[b * 3 + 2] - p[a * 3 + 2]) -
			                (p[c * 3 + 2] - p[a * 3 + 2]) * (p[b * 3] - p[a * 3]);
			facing += o > 0;
			flipped += o < 0;
		}
		rasterize_terrain_faces(positions.data(), 3, faces.data(), static_cast<int>(faces.size() / 3u), drawn);
	}
	long changed = 0, near = 0;
	for (size_t i = 0; i < drawn.size(); ++i)
		if (drawn[i] != cpt.depth_buffer[i]) {
			++changed;
			near += std::abs(static_cast<int>(drawn[i]) - static_cast<int>(cpt.depth_buffer[i])) <= 1;
		}
	std::printf("  %-10s W3: %ld texels drawn back differently (%ld within 1 raw); %ld triangles facing, %ld flipped\n",
	            name.c_str(), changed, near, facing, flipped);
	expect(misplaced == 0, name + ": every leaf stands where the walk puts it");
	expect(facing > 0 && flipped * 1000 < facing, name + ": the strips' triangles face the draw-back's way");
}

// The cold bake every shipped file is sculpted from (input := atlas, its .trn's locks) bakes: the 341 nodes' state.
void check_cold_bake(const std::string &name, const CptFile &shipped, const Locks &locks) {
	TerrainBakeInput input;
	input.depth16 = shipped.depth_buffer;
	input.locks = locks;
	input.threads = TerrainBakeInput::for_hardware();
	CptFile cold;
	TerrainBakeState state;
	std::string error;
	expect(bake_terrain_state(input, cold, state, error) && !state.empty(), name + ": the cold bake: " + error);
}

void check_region(const std::string &name, const CptFile &shipped, const std::vector<uint8_t> &bytes,
                  const Locks &locks) {
	TerrainBakeInput input;
	input.depth16 = shipped.depth_buffer; // input := atlas
	input.locks = locks;
	input.threads = TerrainBakeInput::for_hardware();
	CptFile cold;
	TerrainBakeState state;
	std::string error;
	expect(bake_terrain_state(input, cold, state, error), name + ": the cold bake: " + error);

	RegionBakeInput none;
	none.depth16 = &shipped.depth_buffer;
	RegionBakeResult nothing;
	expect(bake_terrain_region(state, none, nothing, error) && nothing.rebuilt.empty(),
	       name + ": a region bake of nothing remakes nothing");
	CptFile same = shipped;
	TerrainBakeState same_state = state;
	apply_region_bake_state(nothing, same_state);
	expect(apply_region_bake_cpt(nothing, same, error), "the no-op landing: " + error);
	std::vector<uint8_t> again;
	expect(save_cpt(same, again, error) && again == bytes, name + ": an unedited file writes its bytes again");

	// A one-leaf edit: a 2-unit bump inside leaf (320, 448).
	std::vector<uint16_t> map = shipped.depth_buffer;
	const DepthRect edit{330, 460, 30, 30};
	for (int y = edit.y; y < edit.y + edit.h; ++y)
		for (int x = edit.x; x < edit.x + edit.w; ++x) {
			uint16_t &h = map[static_cast<size_t>(y) * kDepthSide + static_cast<size_t>(x)];
			h = static_cast<uint16_t>(std::min(65535, h + 512));
		}
	RegionBakeInput region;
	region.depth16 = &map;
	region.dirty = {edit};
	region.threads = input.threads;
	RegionBakeResult result;
	expect(bake_terrain_region(state, region, result, error), name + ": the region bake: " + error);
	expect(result.footprints.size() == 1 && result.rebuilt.size() == 4,
	       name + ": one leaf and its three ancestors are remade (" + std::to_string(result.rebuilt.size()) + ")");
	CptFile edited = shipped;
	apply_region_bake_state(result, state);
	expect(apply_region_bake_cpt(result, edited, error), "the landing: " + error);
	int other_tiles = 0;
	for (size_t i = 0; i < edited.tiles.size(); ++i) {
		if (std::find(result.rebuilt.begin(), result.rebuilt.end(), static_cast<int>(i)) != result.rebuilt.end()) continue;
		const CptTile &a = edited.tiles[i], &b = shipped.tiles[i];
		bool equal = a.tile_x == b.tile_x && a.tile_y == b.tile_y && a.vertex_indices == b.vertex_indices;
		for (int l = 0; l < 8; ++l) equal = equal && a.lods[l].indices == b.lods[l].indices;
		other_tiles += !equal;
	}
	expect(other_tiles == 0, name + ": every tile not remade is as shipped");
	const DepthRect footprint = result.footprints.empty() ? DepthRect{} : result.footprints[0];
	long outside = 0, inside_raised = 0;
	for (int y = 0; y < kDepthSide; ++y)
		for (int x = 0; x < kDepthSide; ++x) {
			const size_t i = static_cast<size_t>(y) * kDepthSide + static_cast<size_t>(x);
			const bool in = x >= footprint.x && x < footprint.x + footprint.w && y >= footprint.y &&
			                y < footprint.y + footprint.h;
			if (!in) outside += edited.depth_buffer[i] != shipped.depth_buffer[i];
			else if (x >= edit.x + 10 && x < edit.x + 20 && y >= edit.y + 10 && y < edit.y + 20)
				inside_raised += edited.depth_buffer[i] > shipped.depth_buffer[i];
		}
	expect(outside == 0, name + ": no texel outside the remade leaf's footprint changes");
	expect(inside_raised > 50, name + ": the bump's middle stands higher (" + std::to_string(inside_raised) + " texels)");
	std::vector<uint8_t> written;
	CptFile back;
	expect(save_cpt(edited, written, error) && load_cpt(written.data(), written.size(), back, error) &&
	           back.tiles.size() == 341u && back.depth_buffer == edited.depth_buffer,
	       name + ": the edited file writes and reads back: " + error);
}

// The shipped files and their .trn locks, read once (at the first part a run makes).
struct Corpus {
	std::map<std::string, Locks> locks;
	std::vector<std::filesystem::path> cpts;
};
std::string corpus_dir;

const Corpus &corpus() {
	static const Corpus read = [] {
		Corpus c;
		c.locks = trn_locks(corpus_dir);
		for (const auto &entry : std::filesystem::directory_iterator(corpus_dir))
			if (entry.is_regular_file() && lower(entry.path().extension().string()) == ".cpt") c.cpts.push_back(entry.path());
		std::sort(c.cpts.begin(), c.cpts.end());
		return c;
	}();
	return read;
}

// A shipped file (`path`) read with its 341 tiles and its .trn's locks; false, a failure, when it does not read.
bool read_shipped(const std::filesystem::path &path, std::vector<uint8_t> &bytes, CptFile &cpt, Locks &lock) {
	const std::string name = path.filename().string();
	bytes = test_io::read_file(path.string());
	std::string error;
	if (!load_cpt(bytes.data(), bytes.size(), cpt, error) || cpt.tiles.size() != 341u) {
		expect(false, name + ": reads with 341 tiles: " + error);
		return false;
	}
	const auto at = corpus().locks.find(lower(name));
	lock = at == corpus().locks.end() ? Locks{} : at->second;
	return true;
}

// The part `draw_back`: every shipped file there, each read and its leaves drawn back (W3).
int part_draw_back() {
	const std::vector<std::filesystem::path> &cpts = corpus().cpts;
	expect(cpts.size() >= 23u, "the shipped .cpt files are there (" + std::to_string(cpts.size()) + ")");
	for (const auto &path : cpts) {
		std::vector<uint8_t> bytes;
		CptFile cpt;
		Locks lock;
		if (read_shipped(path, bytes, cpt, lock)) check_draw_back(path.filename().string(), cpt, lock);
	}
	return 0;
}

const std::vector<std::string> kRegionLegs = {"dvxi5.cpt", "dvxg3.cpt", "g14.cpt"};
const std::vector<std::string> kFlipped = {"dvxi4.cpt", "dvxg9.cpt"};

bool among(const std::vector<std::string> &names, const std::string &key) {
	return std::find(names.begin(), names.end(), key) != names.end();
}

// One file's bake (the region leg on Dvxi5, Dvxg3 and g14, the cold bake on the others), timed.
void bake_shipped(const std::filesystem::path &path) {
	const std::string name = path.filename().string();
	std::vector<uint8_t> bytes;
	CptFile cpt;
	Locks lock;
	if (!read_shipped(path, bytes, cpt, lock)) return;
	const auto start = std::chrono::steady_clock::now();
	if (among(kRegionLegs, lower(name))) check_region(name, cpt, bytes, lock);
	else check_cold_bake(name, cpt, lock);
	std::printf("  %-10s baked cold in %.1f s\n", name.c_str(),
	            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
}

// The part of one file's leg (`key`, its lower-case name): the file there and baked.
int part_bake(const char *key) {
	const std::vector<std::filesystem::path> &cpts = corpus().cpts;
	const auto at = std::find_if(cpts.begin(), cpts.end(),
	                             [&](const std::filesystem::path &path) { return lower(path.filename().string()) == key; });
	expect(at != cpts.end(), std::string("the shipped ") + key + " is there");
	if (at != cpts.end()) bake_shipped(*at);
	return 0;
}

} // namespace

// A row per part (tests/common/test_parts.h): `draw_back` W3 over every file, and a part per baked file (the
// region legs on Dvxi5, Dvxg3 and g14, the cold bakes of Dvxi4 and dvxg9), side by side. By hand, a directory runs
// every part over it and then bakes its other files cold.
int main(int argc, char **argv) {
	const bool every = argc > 1 && std::string(argv[1]) != "--part";
	corpus_dir = every ? std::string(argv[1]) : retail::assets();
	if (corpus_dir.empty()) return retail::skip("OPENNOVA_JO_ASSETS (the extracted retail tree with its loose .cpt polydata)");
	failures += test_parts::run(argc, argv,
	                            {{"draw_back", {part_draw_back}},
	                             {"dvxi5", {[] { return part_bake("dvxi5.cpt"); }}},
	                             {"dvxg3", {[] { return part_bake("dvxg3.cpt"); }}},
	                             {"g14", {[] { return part_bake("g14.cpt"); }}},
	                             {"dvxi4", {[] { return part_bake("dvxi4.cpt"); }}},
	                             {"dvxg9", {[] { return part_bake("dvxg9.cpt"); }}}});
	if (every)
		for (const auto &path : corpus().cpts) {
			const std::string key = lower(path.filename().string());
			if (!among(kRegionLegs, key) && !among(kFlipped, key)) bake_shipped(path);
		}
	if (failures) return 1;
	std::printf("OK: region bakes over the shipped .cpt files%s\n", every ? ", every file baked cold" : "");
	return 0;
}
