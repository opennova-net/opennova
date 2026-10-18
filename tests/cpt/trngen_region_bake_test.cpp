// The region bake (engine/formats/cpt/trngen/terrain_region_bake.h): a depth map baked whole with its state
// kept (bake_terrain_state, the same bytes as bake_terrain), then edited in places and baked again over the
// edited region alone, lands the bytes and the state a whole bake of the edited map makes: an edit inside one
// leaf, on a 64 edge, at a four-leaf corner, across the map's wrap, along a locked quadrant's seam, at leaf
// (0, 0) (whose tile is the root's too), across the map's vertical wrap, along its bottom row, several at once, and
// two region bakes in a row. A cancel stops a whole bake, making nothing, and one never set changes no byte; one set
// while the walk runs stops it at its next node. One
// thread or many make the same tiles; a cancel stops a region bake with the state as it was; a
// state not made, one made without the draw-back, a map of the wrong size, a changed rect not inside the map and
// ground the whole bake refuses are refused in words; a .cpt not the bake's is refused by the .cpt landing.
#include <formats/cpt/cpt_io.h>
#include <formats/cpt/trngen/mesh_simp.h>
#include <formats/cpt/trngen/quadtree_node.h>
#include <formats/cpt/trngen/terrain_bake.h>
#include <formats/cpt/trngen/terrain_bake_state.h>
#include <formats/cpt/trngen/terrain_mesh.h>
#include <formats/cpt/trngen/terrain_region_bake.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
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

// Rolling ground with a ridge and a hill, raw16 well inside [0, 128] units.
std::vector<uint16_t> ground() {
	std::vector<uint16_t> depth(static_cast<size_t>(kDepthSide) * kDepthSide);
	for (int y = 0; y < kDepthSide; ++y)
		for (int x = 0; x < kDepthSide; ++x) {
			double h = 30.0 + 12.0 * std::sin(x * 0.011) * std::cos(y * 0.017) + 6.0 * std::sin((x + 2 * y) * 0.043);
			h += 25.0 * std::exp(-((x - 600) * (x - 600) + (y - 420) * (y - 420)) / 9000.0);
			depth[static_cast<size_t>(y) * kDepthSide + x] = static_cast<uint16_t>(h * 256.0);
		}
	return depth;
}

// The map with a bump raised over `rect` (wrapping where the rectangle runs past the edge is the caller's).
void raise(std::vector<uint16_t> &depth, const DepthRect &rect, double units) {
	for (int y = rect.y; y < rect.y + rect.h; ++y)
		for (int x = rect.x; x < rect.x + rect.w; ++x) {
			const double cx = rect.x + (rect.w - 1) / 2.0, cy = rect.y + (rect.h - 1) / 2.0;
			const double r = std::hypot((x - cx) / (rect.w / 2.0 + 1), (y - cy) / (rect.h / 2.0 + 1));
			uint16_t &h = depth[static_cast<size_t>(y) * kDepthSide + x];
			h = static_cast<uint16_t>(std::clamp(h + units * 256.0 * std::max(0.0, 1.0 - r), 0.0, 65535.0));
		}
}

std::vector<uint8_t> bytes_of(const CptFile &cpt) {
	std::vector<uint8_t> out;
	std::string error;
	expect(save_cpt(cpt, out, error), "the .cpt writes: " + error);
	return out;
}

bool same_section(const BakeSection &a, const BakeSection &b) {
	return a.vertex_count == b.vertex_count && a.mesh_vertex_count == b.mesh_vertex_count && a.faces == b.faces &&
	       a.records == b.records;
}

// Every node's state but the root's (which nothing reads: the root is never merged).
int state_differences(const TerrainBakeState &a, const TerrainBakeState &b) {
	if (a.nodes.size() != b.nodes.size()) return 1000;
	int differ = 0;
	for (size_t i = 1; i < a.nodes.size(); ++i)
		if (!same_section(a.nodes[i]->section, b.nodes[i]->section) ||
		    a.nodes[i]->draw_back.positions != b.nodes[i]->draw_back.positions ||
		    a.nodes[i]->draw_back.faces != b.nodes[i]->draw_back.faces)
			++differ;
	return differ;
}

TerrainBakeInput input_of(const std::vector<uint16_t> &depth, const std::array<CornerLockFlags, 4> &locks) {
	TerrainBakeInput input;
	input.depth16 = depth;
	input.terrain_name = "Region";
	input.creator = "OpenNova";
	input.version = "opennova";
	input.locks = locks;
	input.threads = TerrainBakeInput::for_hardware();
	return input;
}

struct Baked {
	CptFile cpt;
	TerrainBakeState state;
};

Baked bake_whole(const std::vector<uint16_t> &depth, const std::array<CornerLockFlags, 4> &locks) {
	Baked out;
	std::string error;
	expect(bake_terrain_state(input_of(depth, locks), out.cpt, out.state, error), "the whole bake: " + error);
	return out;
}

// From `from` (the map `before` baked), the edits `rects` raised by `units`: the region bake applied, against
// a whole bake of the edited map. Returns the edited map and leaves `from` landed.
// `compare` false skips the whole bake (a later region's comparison covers this one's landing too).
std::vector<uint16_t> check_region(Baked &from, std::vector<uint16_t> map, const std::vector<DepthRect> &rects,
                                   double units, const std::array<CornerLockFlags, 4> &locks, const std::string &what,
                                   size_t want_leaves = 0, bool compare = true) {
	for (const DepthRect &r : rects) raise(map, r, units);
	RegionBakeInput input;
	input.depth16 = &map;
	input.dirty = rects;
	input.threads = TerrainBakeInput::for_hardware();
	RegionBakeResult result;
	std::string error;
	expect(bake_terrain_region(from.state, input, result, error), what + ": the region bake: " + error);
	if (want_leaves) expect(result.footprints.size() == want_leaves, what + ": " + std::to_string(want_leaves) +
	                                                                   " leaves remade, got " +
	                                                                   std::to_string(result.footprints.size()));
	apply_region_bake_state(result, from.state);
	expect(apply_region_bake_cpt(result, from.cpt, error), what + ": the .cpt landing: " + error);
	if (!compare) return map;
	const Baked whole = bake_whole(map, locks);
	const std::vector<uint8_t> got = bytes_of(from.cpt), want = bytes_of(whole.cpt);
	size_t differ = 0;
	for (size_t i = 0; i < std::min(got.size(), want.size()); ++i) differ += got[i] != want[i];
	expect(got == want, what + ": the .cpt is the whole bake's (" + std::to_string(got.size()) + " against " +
	                        std::to_string(want.size()) + " bytes, " + std::to_string(differ) + " differ)");
	expect(from.cpt.depth_buffer == whole.cpt.depth_buffer, what + ": the atlas is the whole bake's");
	const int states = state_differences(from.state, whole.state);
	expect(states == 0, what + ": the state is the whole bake's (" + std::to_string(states) + " nodes differ)");
	return map;
}

void test_threads_cancel_refusals(const Baked &baked);
void test_rough_ground(const Baked &baked);

// The whole bake's state, and what a region bake over it refuses (test_regions bakes the same map again, its own
// part's, for the chain of edits).
void test_state() {
	const std::array<CornerLockFlags, 4> none{};
	const std::vector<uint16_t> map = ground();
	const Baked baked = bake_whole(map, none);

	// bake_terrain_state is bake_terrain, keeping its state, and a cancel never set (TerrainBakeInput::cancel)
	// changes no byte: the plain bake runs under one, so one comparison holds both.
	std::string error;
	std::atomic<bool> go{false};
	TerrainBakeInput unstopped = input_of(map, none);
	unstopped.cancel = &go;
	std::vector<uint8_t> plain;
	expect(bake_terrain_bytes(unstopped, plain, error), "the plain bake, under a cancel never set: " + error);
	expect(bytes_of(baked.cpt) == plain,
	       "bake_terrain_state makes bake_terrain's bytes, and a cancel never set changes no byte");
	// A whole bake stops at a cancel, between nodes.
	{
		std::atomic<bool> stop{true};
		TerrainBakeInput stopped = input_of(map, none);
		stopped.cancel = &stop;
		CptFile none_cpt;
		TerrainBakeState none_state;
		expect(!bake_terrain_state(stopped, none_cpt, none_state, error) && error == "cancelled" && none_state.empty(),
		       "a cancel stops the whole bake: " + error);
	}
	expect(!baked.state.empty() && baked.state.nodes.size() == 341u, "the state holds the 341 nodes");
	int leaves_drawn = 0;
	for (int i = 85; i < 341; ++i) leaves_drawn += !baked.state.nodes[static_cast<size_t>(i)]->draw_back.faces.empty();
	expect(leaves_drawn == 256, "every leaf keeps its draw-back faces");
	expect(baked.state.nodes[1]->draw_back.faces.empty(), "a branch keeps no draw-back");

	test_threads_cancel_refusals(baked);
	test_rough_ground(baked);
}

void test_regions() {
	const std::array<CornerLockFlags, 4> none{};
	std::vector<uint16_t> map = ground();
	Baked baked = bake_whole(map, none);
	map = check_region(baked, map, {{270, 140, 30, 30}}, 3.0, none, "inside a leaf", 1, false);
	map = check_region(baked, map, {{300, 300, 40, 12}}, -2.0, none, "across a 64 edge", 2, false);
	map = check_region(baked, map, {{630, 630, 20, 20}}, 4.0, none,
	                   "at a four-leaf corner (after a leaf and an edge)", 4);
	map = check_region(baked, map, {{1016, 200, 8, 10}, {0, 200, 6, 10}}, 2.5, none, "across the wrap", 0, false);
	map = check_region(baked, map, {{4, 6, 20, 20}}, 5.0, none, "at leaf (0, 0), the root's tile", 1, false);
	map = check_region(baked, map, {{300, 1018, 10, 6}, {300, 0, 10, 4}}, 2.0, none,
	                   "across the vertical wrap (after leaf (0, 0))");
	map = check_region(baked, map, {{700, 1020, 12, 4}}, -2.0, none, "along the bottom row", 0, false);
	map = check_region(baked, map, {{100, 700, 10, 10}, {800, 100, 12, 12}, {500, 500, 3, 3}}, -1.5, none,
	                   "several regions at once (after the wrap and leaf (0, 0))");
}

void test_locks() {
	std::array<CornerLockFlags, 4> locks{};
	locks[0] = {1, 0};
	locks[1] = {1, 1};
	locks[3] = {0, 1};
	std::vector<uint16_t> map = ground();
	Baked baked = bake_whole(map, locks);
	map = check_region(baked, map, {{506, 300, 12, 12}}, 3.0, locks, "along a locked seam", 0, false);
	map = check_region(baked, map, {{512, 0, 10, 6}}, 2.0, locks,
	                   "at a locked quadrant's first row (after a locked seam)");
}

void test_threads_cancel_refusals(const Baked &baked) {
	std::vector<uint16_t> map = ground();
	raise(map, {200, 600, 70, 70}, 4.0);

	RegionBakeInput input;
	input.depth16 = &map;
	input.dirty = {{200, 600, 70, 70}};
	std::string error;
	RegionBakeResult one, many;
	input.threads = 1;
	expect(bake_terrain_region(baked.state, input, one, error), "one thread: " + error);
	input.threads = 7;
	expect(bake_terrain_region(baked.state, input, many, error), "seven threads: " + error);
	// Every tile whole (all eight LODs, their strip flags and highest indices): the .cpt bytes each lands.
	CptFile landed_one = baked.cpt, landed_many = baked.cpt;
	expect(apply_region_bake_cpt(one, landed_one, error) && apply_region_bake_cpt(many, landed_many, error),
	       "both land: " + error);
	expect(one.rebuilt == many.rebuilt && one.atlas == many.atlas && bytes_of(landed_one) == bytes_of(landed_many),
	       "one thread and seven make the same region, every tile whole");
	expect(one.cost[4].nodes == 4 && one.cost[3].nodes >= 1 && one.cost[1].nodes == 1 && one.cost[0].nodes == 0,
	       "the cost counts the leaves, their ancestors and no root");

	std::atomic<bool> cancel{true};
	input.cancel = &cancel;
	RegionBakeResult cancelled;
	expect(!bake_terrain_region(baked.state, input, cancelled, error) && error == "cancelled",
	       "a cancel stops the bake: " + error);
	input.cancel = nullptr;
	RegionBakeResult again;
	expect(bake_terrain_region(baked.state, input, again, error) && again.atlas == one.atlas,
	       "after a cancel the state bakes the region as before");

	RegionBakeInput nothing;
	nothing.depth16 = &map;
	RegionBakeResult none_out;
	expect(bake_terrain_region(baked.state, nothing, none_out, error) && none_out.rebuilt.empty(),
	       "no edit remakes nothing");

	TerrainBakeState empty;
	expect(!bake_terrain_region(empty, input, none_out, error) && error.find("state") != std::string::npos,
	       "a state not made is refused in words: " + error);
	TerrainBakeState flat = baked.state;
	flat.rasterize_depth = false;
	expect(!bake_terrain_region(flat, input, none_out, error) && error.find("draw-back") != std::string::npos,
	       "a state made without the draw-back is refused in words: " + error);
	RegionBakeInput past = input;
	past.dirty = {{1016, 200, 16, 10}};
	expect(!bake_terrain_region(baked.state, past, none_out, error) && error.find("inside") != std::string::npos,
	       "a changed rect past the map's edge is refused in words: " + error);
	CptFile short_file;
	expect(!apply_region_bake_cpt(one, short_file, error) && error.find("341") != std::string::npos,
	       "a .cpt not the bake's is refused by the landing: " + error);
	std::vector<uint16_t> small(16, 0);
	input.depth16 = &small;
	expect(!bake_terrain_region(baked.state, input, none_out, error) && error.find("1024") != std::string::npos,
	       "a map of another size is refused in words: " + error);
}

// Ground the whole bake refuses (every texel at random over the 16-bit range, D-TERRAIN-22) is refused by a region
// bake too, its state as it was. The whole bake refuses it at a merge two levels above the leaves, so the changed
// rect is the texels only the top-left 256 node's 16 leaves tap (inside its edges, which its neighbours' leaves tap
// too): the leaves and their four parents are remade from the rough ground and the 256 node's merge of those four
// refuses, as a region bake of the whole map refuses, at a sixteenth of the leaves.
void test_rough_ground(const Baked &baked) {
	std::vector<uint16_t> rough(static_cast<size_t>(kDepthSide) * kDepthSide);
	uint32_t seed = 0x2545F491u;
	for (uint16_t &h : rough) {
		seed = seed * 1664525u + 1013904223u;
		h = static_cast<uint16_t>(seed >> 16);
	}
	RegionBakeInput input;
	input.depth16 = &rough;
	input.dirty = {{1, 1, 255, 255}};
	input.threads = TerrainBakeInput::for_hardware();
	RegionBakeResult out;
	std::string error;
	expect(!bake_terrain_region(baked.state, input, out, error) && error.find("too rough") != std::string::npos,
	       "ground the whole bake refuses is refused by the region bake: " + error);
}

// A cancel set while the walk runs (an editor's close as its seed bakes): the walk the whole bake runs
// (process_quadtree) stops at the next node it would start (make_nodes' check between nodes), "cancelled", the
// nodes made so far kept and none after. The cancel is set as the leaves' level lands (a node count on the walk's
// node_made), so the next level's first node is where it stops: its 16 leaves filed, no branch made. The tree is
// 256 across (16 leaves under four 128 branches, over the map's top-left corner), not the bake's 1024: the walk
// takes any tree build_quadtree makes (the size is the bakes' refusal, not the walk's), and a cancel lands only
// once a whole level is made, so the bake's 256 leaves would make the same claim at sixteen times the cost.
void test_walk_cancelled_partway() {
	const std::vector<uint16_t> depth = ground();
	for (const unsigned threads : {1u, 4u}) {
		BaseMeshSet base;
		std::string error;
		expect(generate_base_terrain_meshes(base, error), "the base meshes: " + error);
		std::unique_ptr<QuadtreeNode> root = build_quadtree(256, 64);
		TileFiles files;
		std::atomic<bool> stop{false};
		int made = 0;
		int made_after = 0;
		QuadtreeContext ctx;
		ctx.tile_size = 256;
		ctx.min_tile_size = 64;
		ctx.depth_buffer = depth.data();
		ctx.base = &base;
		ctx.files = &files;
		ctx.cancel = &stop;
		ctx.node_made = [&](const QuadtreeNode &) {
			if (stop.load()) ++made_after;
			if (++made == 16) stop.store(true);
		};
		expect(!process_quadtree(root.get(), ctx, threads, error) && error == "cancelled",
		       "a cancel set partway stops the walk: " + error);
		expect(made == 16 && made_after == 0 && files.size() == 16,
		       "the walk stops at its next node, the leaves made and no branch (threads " + std::to_string(threads) +
		           ", made " + std::to_string(made) + ", files " + std::to_string(files.size()) + ")");
		MeshSimp_FreeAll();
	}
}

} // namespace

// `--part <name>` runs one part (a ctest row each, so their whole bakes run side by side); no argument runs every part.
int main(int argc, char **argv) {
	const bool by_part = argc == 3 && std::string(argv[1]) == "--part";
	const std::string part = by_part ? argv[2] : "";
	if (argc != 1 && !by_part) {
		std::fprintf(stderr, "usage: cpt_trngen_region_bake_test [--part state|regions|locks|walk]\n");
		return 2;
	}
	if (by_part && part != "state" && part != "regions" && part != "locks" && part != "walk") {
		std::fprintf(stderr, "FAIL: no part %s (state, regions, locks, walk)\n", part.c_str());
		return 1;
	}
	const auto runs = [&](const char *name) { return !by_part || part == name; };
	if (runs("state")) test_state();
	if (runs("regions")) test_regions();
	if (runs("locks")) test_locks();
	if (runs("walk")) test_walk_cancelled_partway();
	if (failures) return 1;
	std::printf("OK: region bakes land the whole bake's bytes and state%s%s\n", by_part ? ": " : "", part.c_str());
	return 0;
}
