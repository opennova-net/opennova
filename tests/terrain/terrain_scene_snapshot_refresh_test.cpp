// The terrain's load-time derivations made again over a region (an editor's
// sculpt stroke and the bake that lands it), each equal to a fresh build:
//   1. refresh_terrain_scene_snapshot over the synthetic map
//      fixtures/terrain/tmap/Tmap.cpt: heights changed inside a leaf, across a
//      quadrant edge under the .trn locks and at the atlas's wrapping edge;
//      tiles replaced (their vertices and LOD lists) with the heights; a tile
//      moved (built whole); a copied snapshot (whose mip chain pointed into its
//      source's) refreshed without touching the source.
//   2. refresh_mipchain: every rect equals build_mipchain.
//   3. refresh_heightfield_normal_map: the region form of
//      build_heightfield_normal_map [orig: Terrain_GenerateNormalMap @ 0x603210]
//      under no locks, every lock and mixed locks, rects inside, on a quadrant
//      edge, on the atlas's edges, and the whole atlas.

#include <formats/cpt/cpt.h>
#include <formats/trn/trn.h>
#include <runtime/terrain/quadtree.h>
#include <runtime/terrain/terrain_frame.h>
#include <runtime/terrain/texture_preprocess.h>
#include <runtime/terrain_query/coords.h>
#include <runtime/terrain_query/terrain_field_store.h>
#include <runtime/terrain_query/terrain_field_build.h>
#include "common/test_paths.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

using namespace opennova;

static int g_fail = 0;

static void check(bool cond, const std::string &msg) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
		g_fail = 1;
	}
}

namespace {

struct Lcg {
	uint32_t state;
	uint32_t next() {
		state = state * 1664525u + 1013904223u;
		return state >> 8;
	}
};

// The heights of `rect` (inside the atlas) moved by a pseudo-random step.
void sculpt(std::vector<uint16_t> &depth, int dim, const terrain::CoordsRect &rect, uint32_t seed) {
	Lcg lcg{seed};
	for (int z = rect.z; z < rect.z + rect.h; ++z) {
		for (int x = rect.x; x < rect.x + rect.w; ++x) {
			uint16_t &h = depth[static_cast<size_t>(z) * dim + x];
			const int step = static_cast<int>(lcg.next() % 2049) - 1024;
			const int v = static_cast<int>(h) + step * 8;
			h = static_cast<uint16_t>(v < 0 ? 0 : (v > 65535 ? 65535 : v));
		}
	}
}

bool same_floats(const float *a, const float *b, int n) {
	return std::memcmp(a, b, sizeof(float) * static_cast<size_t>(n)) == 0;
}

size_t level_offset(const Mipchain &mc, int level) {
	return static_cast<size_t>(mc.levels[level] - mc.data.data());
}

bool same_mipchain(const Mipchain &a, const Mipchain &b) {
	if (a.level_count != b.level_count || a.data != b.data) return false;
	for (int i = 0; i < a.level_count; ++i)
		if (level_offset(a, i) != level_offset(b, i)) return false;
	return true;
}

// Every field the frame compiler reads, compared exactly.
std::string snapshot_difference(const TerrainSceneSnapshot &a, const TerrainSceneSnapshot &b) {
	if (a.root_node != b.root_node) return "root node";
	if (std::memcmp(a.l1_children, b.l1_children, sizeof(a.l1_children)) != 0) return "level-1 children";
	if (std::memcmp(a.sector_grid, b.sector_grid, sizeof(a.sector_grid)) != 0) return "sector grid";
	if (a.origin_x != b.origin_x || a.origin_y != b.origin_y || a.wrap_x != b.wrap_x || a.wrap_y != b.wrap_y)
		return "routing";
	if (a.tile_meshes.size() != b.tile_meshes.size() || a.tile_attributes.size() != b.tile_attributes.size())
		return "tile count";
	for (size_t i = 0; i < a.tile_meshes.size(); ++i) {
		const TileMesh &x = a.tile_meshes[i];
		const TileMesh &y = b.tile_meshes[i];
		bool same = x.vertex_count == y.vertex_count && x.tile_size == y.tile_size &&
				same_floats(x.aabb_min, y.aabb_min, 3) && same_floats(x.aabb_max, y.aabb_max, 3) &&
				same_floats(x.center, y.center, 3) && std::memcmp(&x.radius, &y.radius, sizeof(float)) == 0;
		for (int lod = 0; lod < 8; ++lod) same = same && x.lods[lod].index_count == y.lods[lod].index_count;
		if (!same) return "tile mesh " + std::to_string(i);
		const TerrainTileDrawAttributes &p = a.tile_attributes[i];
		const TerrainTileDrawAttributes &q = b.tile_attributes[i];
		if (p.quadrant_x != q.quadrant_x || p.quadrant_z != q.quadrant_z || p.local_page_x != q.local_page_x ||
				p.local_page_z != q.local_page_z || p.source_page_x != q.source_page_x ||
				p.source_page_z != q.source_page_z)
			return "tile attributes " + std::to_string(i);
	}
	if (!same_mipchain(a.mipchain, b.mipchain)) return "mip chain";
	if (a.quad_nodes.size() != b.quad_nodes.size()) return "node count";
	for (size_t i = 0; i < a.quad_nodes.size(); ++i) {
		const QuadNode &x = a.quad_nodes[i];
		const QuadNode &y = b.quad_nodes[i];
		const bool same = x.is_leaf == y.is_leaf && x.lod_level == y.lod_level && x.size == y.size &&
				x.tile_index == y.tile_index && std::memcmp(x.children, y.children, sizeof(x.children)) == 0 &&
				same_floats(x.aabb_min, y.aabb_min, 3) && same_floats(x.aabb_max, y.aabb_max, 3) &&
				same_floats(x.center, y.center, 3) && std::memcmp(&x.radius, &y.radius, sizeof(float)) == 0;
		if (!same) return "node " + std::to_string(i);
	}
	return std::string();
}

TrnConfig make_trn(bool locked) {
	TrnConfig trn;
	trn.origin_x = -1;
	trn.origin_y = -1;
	trn.sector_count = 2;
	trn.sector_rows = 2;
	for (int r = 0; r < 16; ++r)
		for (int c = 0; c < 16; ++c) trn.sector_grid[r][c] = 1 + ((r & 1) + 2 * (c & 1));
	if (locked) {
		trn.lock_topleft = {1, 1};
		trn.lock_topright = {1, 0};
		trn.lock_bottomleft = {0, 1};
		trn.lock_bottomright = {1, 1};
	}
	return trn;
}

int tile_at(const CptFile &cpt, int x, int z, int size) {
	for (size_t i = 0; i < cpt.tiles.size(); ++i)
		if (cpt.tiles[i].tile_x == x && cpt.tiles[i].tile_y == z && cpt.tiles[i].tile_size == size)
			return static_cast<int>(i);
	return -1;
}

// A tile made over: one triangle fewer at every LOD that has one, and its last
// vertex moved one unit inward (a bake's new simplification of the same cell).
void remake_tile(CptTile &tile) {
	for (CptTileLOD &lod : tile.lods) {
		if (!lod.is_strip && lod.indices.size() >= 6) lod.indices.resize(lod.indices.size() - 3);
		else if (lod.is_strip && lod.indices.size() >= 4) lod.indices.pop_back();
	}
	if (tile.vertex_count > 0) {
		uint16_t &rel_x = tile.vertex_indices[static_cast<size_t>(tile.vertex_count - 1) * 2];
		if (rel_x > 0) --rel_x;
	}
}

void test_snapshot(const CptFile &source) {
	struct Case {
		const char *name;
		bool locked;
		terrain::CoordsRect rect;
		bool replace;
	};
	const Case cases[] = {
			{"inside a leaf", false, {300, 300, 20, 20}, false},
			{"across a quadrant edge, locked", true, {500, 505, 30, 12}, false},
			{"across a quadrant edge, open", false, {505, 500, 12, 30}, false},
			{"at the atlas's wrapping edge, locked", true, {1016, 0, 8, 6}, false},
			{"at the atlas's far corner", false, {1000, 1000, 24, 24}, false},
			{"tiles replaced with their heights", false, {256, 320, 64, 64}, true},
			{"tiles replaced, locked", true, {448, 448, 64, 64}, true},
			{"nothing changed", false, {0, 0, 0, 0}, false},
	};
	for (const Case &c : cases) {
		const TrnConfig trn = make_trn(c.locked);
		CptFile cpt = source;
		TerrainSceneSnapshot scene = build_terrain_scene_snapshot(cpt, trn);
		check(scene.valid(), std::string(c.name) + ": the fixture builds a snapshot");
		sculpt(cpt.depth_buffer, 1024, c.rect, 77u + static_cast<uint32_t>(c.rect.x));
		std::vector<int> replaced;
		if (c.replace) {
			// The leaf at the rect's corner and every ancestor up to the root.
			for (int size = 64; size <= 1024; size *= 2) {
				const int index = tile_at(cpt, c.rect.x & ~(size - 1), c.rect.z & ~(size - 1), size);
				check(index >= 0, std::string(c.name) + ": the fixture holds the tile");
				if (index < 0) continue;
				remake_tile(cpt.tiles[index]);
				replaced.push_back(index);
			}
		}
		refresh_terrain_scene_snapshot(scene, cpt, trn, replaced, c.rect);
		const std::string difference = snapshot_difference(scene, build_terrain_scene_snapshot(cpt, trn));
		check(difference.empty(), std::string(c.name) + ": the refreshed snapshot equals a fresh build (" +
				difference + ")");
	}

	// A tile moved: the layout the snapshot was built from no longer holds, so it is built whole.
	{
		const TrnConfig trn = make_trn(false);
		CptFile cpt = source;
		TerrainSceneSnapshot scene = build_terrain_scene_snapshot(cpt, trn);
		const int leaf = tile_at(cpt, 128, 192, 64);
		check(leaf >= 0, "the fixture holds the leaf to move");
		if (leaf >= 0) cpt.tiles[leaf].tile_x = 0;
		refresh_terrain_scene_snapshot(scene, cpt, trn, {}, terrain::CoordsRect{});
		check(snapshot_difference(scene, build_terrain_scene_snapshot(cpt, trn)).empty(),
				"a moved tile builds the snapshot whole");
	}

	// A copy (its mip chain's levels still pointing into its source's bytes) refreshed: equal to a fresh
	// build, its source untouched.
	{
		const TrnConfig trn = make_trn(true);
		CptFile cpt = source;
		const TerrainSceneSnapshot original = build_terrain_scene_snapshot(cpt, trn);
		TerrainSceneSnapshot copy = original;
		const terrain::CoordsRect rect{600, 100, 40, 40};
		sculpt(cpt.depth_buffer, 1024, rect, 5u);
		refresh_terrain_scene_snapshot(copy, cpt, trn, {}, rect);
		check(snapshot_difference(copy, build_terrain_scene_snapshot(cpt, trn)).empty(),
				"a copied snapshot refreshes to a fresh build");
		check(snapshot_difference(original, build_terrain_scene_snapshot(source, trn)).empty(),
				"refreshing a copy leaves its source as built");
	}
}

void test_mipchain(const CptFile &source) {
	const terrain::CoordsRect rects[] = {
			{0, 0, 1, 1}, {1, 1, 1, 1}, {1023, 1023, 1, 1}, {0, 1023, 1024, 1}, {511, 0, 2, 1024},
			{100, 900, 300, 124}, {0, 0, 1024, 1024},
	};
	for (const terrain::CoordsRect &rect : rects) {
		std::vector<uint16_t> depth = source.depth_buffer;
		Mipchain mc = build_mipchain(depth);
		sculpt(depth, 1024, rect, 11u + static_cast<uint32_t>(rect.w));
		refresh_mipchain(mc, depth, rect.x, rect.z, rect.w, rect.h);
		check(same_mipchain(mc, build_mipchain(depth)),
				"the refreshed mip chain equals a fresh build (rect " + std::to_string(rect.x) + "," +
						std::to_string(rect.z) + " " + std::to_string(rect.w) + "x" + std::to_string(rect.h) + ")");
	}
	// A chain of another shape is built whole.
	Mipchain empty;
	refresh_mipchain(empty, source.depth_buffer, 0, 0, 1, 1);
	check(same_mipchain(empty, build_mipchain(source.depth_buffer)), "an empty chain is built whole");
}

void test_normals() {
	const TerrainQuadrantLocks lock_sets[] = {
			TerrainQuadrantLocks{},
			TerrainQuadrantLocks{{{1, 1}, {1, 1}, {1, 1}, {1, 1}}},
			TerrainQuadrantLocks{{{1, 0}, {0, 1}, {0, 0}, {1, 1}}},
	};
	for (const uint32_t dim : {64u, 1024u}) {
		const int half = static_cast<int>(dim / 2);
		const int last = static_cast<int>(dim) - 1;
		const terrain::CoordsRect rects[] = {
				{5, 7, 3, 4}, {half - 2, half - 3, 4, 6}, {0, 0, 1, 1}, {last, 0, 1, 3}, {0, last, 4, 1},
				{half - 1, 0, 1, static_cast<int>(dim)}, {0, 0, static_cast<int>(dim), static_cast<int>(dim)},
				{last - 7, last - 7, 8, 8},
		};
		for (size_t l = 0; l < 3; ++l) {
			for (const terrain::CoordsRect &rect : rects) {
				std::vector<uint16_t> depth(static_cast<size_t>(dim) * dim);
				Lcg lcg{static_cast<uint32_t>(dim + l)};
				for (uint16_t &h : depth) h = static_cast<uint16_t>(lcg.next() & 0xFFFF);
				terrain::Rgba8Image normals = terrain::build_heightfield_normal_map(depth, dim, dim, lock_sets[l]);
				const terrain::Rgba8Image before = normals;
				sculpt(depth, static_cast<int>(dim), rect, 3u + static_cast<uint32_t>(rect.z));
				std::array<bool, 4> written{};
				const bool ok =
						terrain::refresh_heightfield_normal_map(normals, depth, dim, dim, lock_sets[l], rect, &written);
				const terrain::Rgba8Image fresh = terrain::build_heightfield_normal_map(depth, dim, dim, lock_sets[l]);
				const std::string where = "(atlas " + std::to_string(dim) + ", locks " + std::to_string(l) + ", rect " +
						std::to_string(rect.x) + "," + std::to_string(rect.z) + " " + std::to_string(rect.w) + "x" +
						std::to_string(rect.h) + ")";
				check(ok && normals.pixels == fresh.pixels, "the refreshed normal map equals a fresh build " + where);
				// A quadrant not reported written kept every byte.
				for (uint32_t y = 0; y < dim; ++y) {
					for (uint32_t x = 0; x < dim; ++x) {
						const size_t q = (x >= dim / 2 ? 1u : 0u) + (y >= dim / 2 ? 2u : 0u);
						const size_t at = (static_cast<size_t>(y) * dim + x) * 4;
						if (!written[q] && std::memcmp(&before.pixels[at], &fresh.pixels[at], 4) != 0) {
							check(false, "a changed texel lies in a quadrant not reported written " + where);
							y = dim;
							break;
						}
					}
				}
			}
		}
	}
	// A map of another shape is refused, nothing written.
	std::vector<uint16_t> depth(64 * 64, 100);
	terrain::Rgba8Image small = terrain::build_heightfield_normal_map(std::vector<uint16_t>(32 * 32, 5), 32, 32);
	const std::vector<uint8_t> before = small.pixels;
	check(!terrain::refresh_heightfield_normal_map(small, depth, 64, 64, TerrainQuadrantLocks{},
			terrain::CoordsRect{0, 0, 4, 4}),
			"a map of another shape is refused");
	check(small.pixels == before, "a refused refresh writes nothing");
}

// The region contract the four share: a change across the atlas's wrap arrives as two rects, each refreshed in turn,
// and the result is a fresh build's; a rect past the edge is refused by each with nothing changed.
void test_contract(const CptFile &source) {
	const TrnConfig trn = make_trn(false);
	CptFile cpt = source;
	TerrainSceneSnapshot scene = build_terrain_scene_snapshot(cpt, trn);
	terrain::Rgba8Image normals = terrain::build_heightfield_normal_map(cpt.depth_buffer, 1024, 1024, trn.get_quadrant_locks());
	const terrain::CoordsRect east{1016, 300, 8, 20}, west{0, 300, 8, 20};
	// Refused: one rect across the edge, nothing changed.
	const terrain::CoordsRect across{1016, 300, 16, 20};
	const TerrainSceneSnapshot kept = scene;
	const terrain::Rgba8Image kept_normals = normals;
	check(!refresh_terrain_scene_snapshot(scene, cpt, trn, {}, across), "the snapshot refuses a rect past the edge");
	check(snapshot_difference(scene, kept).empty(), "and keeps itself");
	check(!refresh_mipchain(scene.mipchain, cpt.depth_buffer, across.x, across.z, across.w, across.h),
	      "the mip chain refuses it");
	check(!terrain::refresh_heightfield_normal_map(normals, cpt.depth_buffer, 1024, 1024, trn.get_quadrant_locks(), across),
	      "the normal map refuses it");
	check(normals.pixels == kept_normals.pixels, "and keeps itself");
	terrain::TerrainFieldStore store;
	terrain::terrain_field_store_build(store, cpt, trn);
	const std::vector<uint16_t> src(size_t(16) * 20, 7);
	check(!store.patch_heights(across, src.data(), 16), "the field store refuses it");
	// The wrapped brush as its two rects.
	sculpt(cpt.depth_buffer, 1024, east, 31u);
	sculpt(cpt.depth_buffer, 1024, west, 37u);
	for (const terrain::CoordsRect &rect : {east, west}) {
		check(refresh_terrain_scene_snapshot(scene, cpt, trn, {}, rect), "the snapshot takes each rect");
		check(terrain::refresh_heightfield_normal_map(normals, cpt.depth_buffer, 1024, 1024, trn.get_quadrant_locks(), rect),
		      "the normal map takes each rect");
	}
	check(snapshot_difference(scene, build_terrain_scene_snapshot(cpt, trn)).empty(),
	      "a wrapped brush's two rects refresh the snapshot to a fresh build");
	check(normals.pixels == terrain::build_heightfield_normal_map(cpt.depth_buffer, 1024, 1024, trn.get_quadrant_locks()).pixels,
	      "and the normal map");
	// A rect of no size: nothing to make again.
	check(refresh_terrain_scene_snapshot(scene, cpt, trn, {}, terrain::CoordsRect{}), "a rect of no size changes nothing");
}

// The .trn's locks moved since the snapshot was built: built whole, a fresh build's.
void test_lock_change(const CptFile &source) {
	TerrainSceneSnapshot scene = build_terrain_scene_snapshot(source, make_trn(false));
	const TrnConfig locked = make_trn(true);
	check(refresh_terrain_scene_snapshot(scene, source, locked, {}, terrain::CoordsRect{}), "a lock change refreshes");
	check(snapshot_difference(scene, build_terrain_scene_snapshot(source, locked)).empty(),
	      "a lock change builds the snapshot whole");
}

// A copied mip chain reads its own bytes once its source is gone.
void test_mipchain_copy(const CptFile &source) {
	Mipchain copy;
	{
		const Mipchain built = build_mipchain(source.depth_buffer);
		copy = built;
		check(copy.levels[0] != built.levels[0], "the copy's levels are its own");
	}
	const Mipchain fresh = build_mipchain(source.depth_buffer);
	bool same = copy.level_count == fresh.level_count;
	for (int level = 0; same && level < fresh.level_count; ++level) {
		const size_t side = size_t(1) << level;
		same = std::memcmp(copy.levels[level], fresh.levels[level], side * side * 2) == 0;
	}
	check(same, "the copy's levels read what a fresh build holds");
}

} // namespace

int main() {
	const std::filesystem::path path =
			std::filesystem::path(test_paths_repo_root(__FILE__)) / "fixtures" / "terrain" / "tmap" / "Tmap.cpt";
	CptFile source;
	try {
		source = CptFile::read(path.string());
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: %s does not read: %s\n", path.string().c_str(), e.what());
		return 1;
	}
	if (source.tiles.size() != 341 || source.depth_buffer.size() != 1024u * 1024u) {
		std::fprintf(stderr, "FAIL: the fixture is not a whole baked terrain\n");
		return 1;
	}
	test_snapshot(source);
	test_mipchain(source);
	test_normals();
	test_contract(source);
	test_lock_change(source);
	test_mipchain_copy(source);
	if (g_fail == 0) std::printf("terrain_scene_snapshot_refresh: all tests passed\n");
	return g_fail;
}
