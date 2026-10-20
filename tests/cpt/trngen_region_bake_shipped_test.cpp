// The region bake over a .cpt in the form TrnGen ships (D-TERRAIN-23; docs/terrain/terrain-re.md, the region bake and
// W3). A shipped .cpt keeps no depth input (its tool's map is lost; the CDEP atlas is the leaves drawn back over it),
// so the editor sculpts it with input := atlas and a cold in-memory bake's state, and its tiles are not the ones a bake
// of ours makes. This test mints such a file from integer data: tiles of its own (a 5 x 5 grid each, LOD 0 a strip,
// as TrnGen's strip pass writes it), six of the leaves' strips each carrying a flipped face (TrnGen's own: W3 counted
// 1, 6 and 39 in Dvxg3, Dvxi4 and dvxg9), and the atlas those leaves drawn back over a lost input in the walk's order.
// Then:
//
// - draw_back: each leaf in place, and each leaf drawn back from its own LOD 0 (the strips read as D3D draws them,
//   odd triangles reversed [orig: Terrain_RenderSectorBatch @ 0x609b7e..0x609bd4]) at the atlas heights, under its
//   locks, against the atlas: the leaves' triangles all facing the draw-back's way but the six flipped faces;
// - open (Dvxi5's locks, none) and locked (g14's: lock_topleft 0 1, lock_topright 0 1, lock_bottomleft 1 1,
//   lock_bottomright 1 1): a cold bake's state from the atlas; a region bake of nothing leaves the file's bytes as
//   they were; a one-leaf edit remakes that leaf and its three ancestors, every other tile (the flipped ones among
//   them) and every texel outside the leaf's footprint as shipped, and the file still writes and reads back.
//
// The minted file's bytes are pinned (FNV-1a64, `--dump` prints them): a changed vector is a changed CPT writer.
//
// By hand, `trngen_region_bake_shipped_test <dir>` runs the draw-back over every .cpt under <dir> (each under the locks
// of the first .trn naming it) and bakes each cold (JO:CA's 23: about ten minutes on eight threads).
#include <formats/cpt/cpt_io.h>
#include <formats/cpt/trngen/terrain_bake.h>
#include <formats/cpt/trngen/terrain_bake_state.h>
#include <formats/cpt/trngen/terrain_mesh.h>
#include <formats/cpt/trngen/terrain_region_bake.h>
#include <formats/trn/trn_io.h>

#include <base/io/hash.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "common/file_io.h"
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

Locks locks_of(const TrnConfig &config) {
	const TerrainQuadrantLocks q = config.get_quadrant_locks();
	Locks locks{};
	for (size_t i = 0; i < 4; ++i) locks[i] = {q[i].x, q[i].y};
	return locks;
}

uint16_t tap(const std::vector<uint16_t> &depth, const Locks &locks, int corner_x, int corner_y, int x, int y) {
	const CornerLockFlags &lock = locks[static_cast<size_t>((corner_x >= 0x200 ? 1 : 0) + (corner_y >= 0x200 ? 2 : 0))];
	const int bx = ((lock.x ? (corner_x & 0x200) : 0) + (x & (lock.x ? 511 : 1023))) & 0x3FF;
	const int by = ((lock.y ? (corner_y & 0x200) : 0) + (y & (lock.y ? 511 : 1023))) & 0x3FF;
	return depth[static_cast<size_t>(by) * kDepthSide + static_cast<size_t>(bx)];
}

// The walk's leaf order: nw, ne, sw, se depth first.
std::vector<std::pair<int, int>> leaf_walk() {
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
	return walk;
}

// A leaf's LOD-0 faces as D3D draws its strip (odd triangles reversed, degenerate ones skipped), and its vertex
// positions at `depth`'s heights (x, height, z), the way W3 reads them.
void leaf_faces(const CptTile &tile, const std::vector<uint16_t> &depth, const Locks &locks, std::vector<float> &positions,
                std::vector<uint16_t> &faces, long *facing = nullptr, long *flipped = nullptr) {
	positions.assign(static_cast<size_t>(tile.vertex_count) * 3u, 0.0f);
	for (int i = 0; i < tile.vertex_count; ++i) {
		const int ax = tile.tile_x + tile.vertex_indices[static_cast<size_t>(i) * 2u];
		const int ay = tile.tile_y + tile.vertex_indices[static_cast<size_t>(i) * 2u + 1u];
		positions[static_cast<size_t>(i) * 3u] = static_cast<float>(ax);
		positions[static_cast<size_t>(i) * 3u + 1u] =
			static_cast<float>(tap(depth, locks, tile.tile_x, tile.tile_y, ax, ay)) * 0.00390625f;
		positions[static_cast<size_t>(i) * 3u + 2u] = static_cast<float>(ay);
	}
	faces.clear();
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
		if (facing) *facing += o > 0;
		if (flipped) *flipped += o < 0;
	}
}

// W3: every leaf drawn back from its own LOD 0 at the atlas heights; the changed texels. Returns the flipped faces.
long check_draw_back(const std::string &name, const CptFile &cpt, const Locks &locks) {
	std::vector<uint16_t> drawn = cpt.depth_buffer;
	int misplaced = 0;
	long facing = 0, flipped = 0;
	for (const auto &[x, y] : leaf_walk()) {
		const CptTile &tile = cpt.tiles[static_cast<size_t>(cpt_tile_index(4, x, y))];
		if (tile.tile_x != x || tile.tile_y != y || tile.tile_size != 64) ++misplaced;
		std::vector<float> positions;
		std::vector<uint16_t> faces;
		leaf_faces(tile, cpt.depth_buffer, locks, positions, faces, &facing, &flipped);
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
	return flipped;
}

// The cold bake a shipped file is sculpted from (input := atlas, its locks) bakes: the 341 nodes' state.
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

// --- the minted shipped form ---------------------------------------------------------------------------------

// The lost input the tool baked from: an island (ground at 20 units, a hill of 60 at the map's middle, a ridge and
// ripples), raw16 = units * 256.
std::vector<uint16_t> lost_input() {
	std::vector<uint16_t> depth(static_cast<size_t>(kDepthSide) * kDepthSide);
	for (int y = 0; y < kDepthSide; ++y)
		for (int x = 0; x < kDepthSide; ++x) {
			const int dx = x - 512, dy = y - 512;
			double h = 20.0 + 60.0 * std::exp(-(dx * dx + dy * dy) / 60000.0);
			h += 8.0 * std::exp(-((x - 300) * (x - 300)) / 900.0) + 2.0 * std::sin(x * 0.05) * std::cos(y * 0.07);
			depth[static_cast<size_t>(y) * kDepthSide + x] = static_cast<uint16_t>(h * 256.0);
		}
	return depth;
}

// A tile as the strip pass writes one: a 5 x 5 vertex grid, LOD 0 a strip of its rows joined by degenerate
// triangles (every face facing the draw-back's way), the coarser LODs a list of the two corner triangles.
CptTile shipped_tile(int tx, int ty, int size) {
	CptTile tile;
	tile.tile_x = static_cast<uint16_t>(tx);
	tile.tile_y = static_cast<uint16_t>(ty);
	tile.tile_size = static_cast<uint16_t>(size);
	tile.vertex_count = 25;
	const int step = size / 4;
	for (int j = 0; j < 5; ++j)
		for (int i = 0; i < 5; ++i)
			tile.vertex_indices.insert(tile.vertex_indices.end(), {static_cast<uint16_t>(i * step), static_cast<uint16_t>(j * step)});
	std::vector<uint16_t> strip;
	for (int j = 0; j < 4; ++j) {
		if (j > 0) strip.insert(strip.end(), {strip.back(), static_cast<uint16_t>(j * 5)});
		for (int i = 0; i < 5; ++i)
			strip.insert(strip.end(), {static_cast<uint16_t>(j * 5 + i), static_cast<uint16_t>(j * 5 + 5 + i)});
	}
	tile.lods[0].indices = strip;
	tile.lods[0].is_strip = true;
	tile.lods[0].max_index = 24;
	for (int l = 1; l < 8; ++l) {
		tile.lods[l].indices = {0, 20, 4, 4, 20, 24};
		tile.lods[l].max_index = 24;
	}
	return tile;
}

// One face of the leaf drawn again the other way, past a degenerate bridge at the strip's end: the face (p, q, r)
// (its first grid cell's), its order chosen by the strip's parity so D3D draws it reversed.
void flip_a_face(CptTile &tile) {
	std::vector<uint16_t> &s = tile.lods[0].indices;
	const uint16_t p = 0, q = 5, r = 1; // v(0,0), v(0,1), v(1,0): a face the first row's strip draws facing
	const uint16_t last = s.back();
	// The real triangle lands at the strip's size + 2: even draws (x, y, z) as is, odd swaps the first two.
	if ((s.size() + 2) % 2 == 0) s.insert(s.end(), {last, q, q, p, r});
	else s.insert(s.end(), {last, p, p, q, r});
}

constexpr int kFlippedLeaves[][2] = {{64, 0}, {320, 128}, {576, 576}, {832, 192}, {128, 896}, {960, 960}};

// The shipped form under `locks`: the tiles, the six flipped faces, and the atlas the leaves drawn back over the lost
// input in the walk's order (TrnGen's own draw-back [orig: BuildsManipulatedHeightMap @ 0x402E60..0x402F10
// (TrnGen.exe)]).
CptFile shipped_form(const Locks &locks) {
	CptFile cpt;
	cpt.header.reserved[1] = 0x04;
	cpt.header.reserved[2] = 0x05;
	std::strncpy(cpt.header.terrain_name, "Island", sizeof(cpt.header.terrain_name) - 1);
	std::strncpy(cpt.header.creator, "OpenNova", sizeof(cpt.header.creator) - 1);
	std::strncpy(cpt.header.version, "trngen", sizeof(cpt.header.version) - 1);
	cpt.depth_format = DepthFormat::CDEP;
	for (int size = kDepthSide; size >= 64; size /= 2)
		for (int ty = 0; ty < kDepthSide; ty += size)
			for (int tx = 0; tx < kDepthSide; tx += size) cpt.tiles.push_back(shipped_tile(tx, ty, size));
	for (const auto &leaf : kFlippedLeaves) flip_a_face(cpt.tiles[static_cast<size_t>(cpt_tile_index(4, leaf[0], leaf[1]))]);
	const std::vector<uint16_t> lost = lost_input();
	cpt.depth_buffer = lost;
	for (const auto &[x, y] : leaf_walk()) {
		std::vector<float> positions;
		std::vector<uint16_t> faces;
		leaf_faces(cpt.tiles[static_cast<size_t>(cpt_tile_index(4, x, y))], lost, locks, positions, faces);
		rasterize_terrain_faces(positions.data(), 3, faces.data(), static_cast<int>(faces.size() / 3u), cpt.depth_buffer);
	}
	return cpt;
}

Locks open_locks() { return Locks{}; } // Dvxi5.trn: every lock 0 0

Locks g14_locks() {
	TrnConfig config;
	config.lock_topleft = {0, 1};
	config.lock_topright = {0, 1};
	config.lock_bottomleft = {1, 1};
	config.lock_bottomright = {1, 1};
	return locks_of(config);
}

struct Minted {
	CptFile cpt;
	std::vector<uint8_t> bytes;
};

const Minted &minted(bool locked) {
	static Minted forms[2];
	static bool made[2] = {false, false};
	if (!made[locked]) {
		forms[locked].cpt = shipped_form(locked ? g14_locks() : open_locks());
		std::string error;
		CptFile back;
		expect(save_cpt(forms[locked].cpt, forms[locked].bytes, error) &&
		           load_cpt(forms[locked].bytes.data(), forms[locked].bytes.size(), back, error) && back.tiles.size() == 341u,
		       "the minted shipped form writes and reads back: " + error);
		forms[locked].cpt = back; // as a reader of the file holds it
		made[locked] = true;
	}
	return forms[locked];
}

struct Vector {
	const char *name;
	size_t len;
	uint64_t hash;
};

const Vector kExpected[] = {
	{"open", 1617902u, 0xb7d95169f11738abull},
	{"locked", 1644110u, 0x1ac6ff4763fa08ccull},
};

// The part `draw_back`: both minted files read and their leaves drawn back (W3), the six flipped faces counted, and
// their bytes the pinned vectors.
int part_draw_back() {
	for (const bool locked : {false, true}) {
		const char *name = locked ? "locked" : "open";
		const Minted &form = minted(locked);
		expect(check_draw_back(name, form.cpt, locked ? g14_locks() : open_locks()) == 6,
		       std::string(name) + ": the six flipped faces, and only they, face the other way");
		const Vector &v = kExpected[locked];
		const uint64_t h = io::fnv1a64_bytes(io::kFnv1a64Offset, form.bytes.data(), form.bytes.size());
		if (form.bytes.size() != v.len || h != v.hash)
			std::printf("  %s: len %zu hash 0x%016llx, expected len %zu hash 0x%016llx\n", name, form.bytes.size(),
			            (unsigned long long)h, v.len, (unsigned long long)v.hash);
		expect(form.bytes.size() == v.len && h == v.hash, std::string(name) + ": the pinned vector");
	}
	return 0;
}

int part_region(bool locked) {
	const char *name = locked ? "locked" : "open";
	const Minted &form = minted(locked);
	const auto start = std::chrono::steady_clock::now();
	check_region(name, form.cpt, form.bytes, locked ? g14_locks() : open_locks());
	std::printf("  %-10s region legs in %.1f s\n", name,
	            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
	return 0;
}

// By hand: every .cpt under `dir`, each under the locks of the first .trn (by name) naming it, drawn back and baked cold.
int run_directory(const std::filesystem::path &dir) {
	std::vector<std::filesystem::path> trns, cpts;
	for (const auto &entry : std::filesystem::directory_iterator(dir)) {
		if (!entry.is_regular_file()) continue;
		const std::string ext = lower(entry.path().extension().string());
		if (ext == ".trn") trns.push_back(entry.path());
		if (ext == ".cpt") cpts.push_back(entry.path());
	}
	std::sort(trns.begin(), trns.end());
	std::sort(cpts.begin(), cpts.end());
	std::map<std::string, Locks> locks;
	for (const auto &path : trns) {
		std::ifstream in(path, std::ios::binary);
		TrnConfig config;
		std::string error;
		load_trn(in, config, error);
		if (!config.polydata.empty()) locks.emplace(lower(config.polydata), locks_of(config));
	}
	for (const auto &path : cpts) {
		const std::string name = path.filename().string();
		const std::vector<uint8_t> bytes = test_io::read_file(path.string());
		CptFile cpt;
		std::string error;
		if (!load_cpt(bytes.data(), bytes.size(), cpt, error) || cpt.tiles.size() != 341u) {
			expect(false, name + ": reads with 341 tiles: " + error);
			continue;
		}
		const auto at = locks.find(lower(name));
		const Locks lock = at == locks.end() ? Locks{} : at->second;
		check_draw_back(name, cpt, lock);
		const auto start = std::chrono::steady_clock::now();
		check_cold_bake(name, cpt, lock);
		std::printf("  %-10s baked cold in %.1f s\n", name.c_str(),
		            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
	}
	return 0;
}

} // namespace

// A row per part (tests/common/test_parts.h): `draw_back` W3 over both minted files, and a part per region leg (the
// open and the locked file), side by side. `--dump` prints the pinned vectors; a directory runs the by-hand sweep.
int main(int argc, char **argv) {
	if (argc > 1 && std::strcmp(argv[1], "--dump") == 0) {
		for (const bool locked : {false, true}) {
			const Minted &form = minted(locked);
			std::printf("\t{\"%s\", %zuu, 0x%016llxull},\n", locked ? "locked" : "open", form.bytes.size(),
			            (unsigned long long)io::fnv1a64_bytes(io::kFnv1a64Offset, form.bytes.data(), form.bytes.size()));
		}
		return 0;
	}
	if (argc > 1 && std::string(argv[1]) != "--part") {
		run_directory(argv[1]);
		if (failures) return 1;
		std::printf("OK: every .cpt under %s drawn back and baked cold\n", argv[1]);
		return 0;
	}
	failures += test_parts::run(argc, argv,
	                            {{"draw_back", {part_draw_back}},
	                             {"open", {[] { return part_region(false); }}},
	                             {"locked", {[] { return part_region(true); }}}});
	if (failures) return 1;
	std::printf("OK: region bakes over the shipped .cpt form\n");
	return 0;
}
