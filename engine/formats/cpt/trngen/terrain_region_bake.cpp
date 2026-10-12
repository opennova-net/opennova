#include "terrain_region_bake.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <initializer_list>
#include <memory>
#include <thread>
#include <utility>

#include "bake_tiles.h"
#include "mesh_simp.h"
#include "terrain_mesh.h"

namespace opennova::trngen {

namespace {

constexpr int kRootSize = 1024;
constexpr int kLeafSize = 64;
constexpr int kLeafDepth = 4;
constexpr int kLeafTile = 85; // leaf (0, 0)

// The texels one axis of a leaf's vertices reads: corner + 0 .. corner + 64, each (offset + (a & mask))
// & 0x3FF, the whole map's 1023 with no lock, or a locked quadrant's 511 offset by its 512 [orig: TrnGen.exe
// build_mesh_lod_vertices @ 0x4042B0]. One run, or two where the last vertex wraps.
int axis_taps(int corner, bool locked, std::pair<int, int> runs[2]) {
	const int mask = locked ? 511 : 1023;
	const int offset = locked ? (corner & 0x200) : 0;
	const int first = corner & mask;
	if (first + kLeafSize <= mask) {
		runs[0] = {offset + first, offset + first + kLeafSize};
		return 1;
	}
	runs[0] = {offset + first, offset + mask};
	runs[1] = {offset, offset + first + kLeafSize - mask - 1};
	return 2;
}

const CornerLockFlags &quadrant_lock(const std::array<CornerLockFlags, 4> &locks, int x, int y) {
	return locks[static_cast<size_t>((x >= 0x200 ? 1 : 0) + (y >= 0x200 ? 2 : 0))];
}

bool runs_meet(const std::pair<int, int> *runs, int count, int lo, int hi_exclusive) {
	for (int i = 0; i < count; ++i)
		if (runs[i].first < hi_exclusive && lo <= runs[i].second) return true;
	return false;
}

// The walk's order of a level (children nw, ne, sw, se before their parent), as process_quadtree makes it.
void collect(QuadtreeNode &node, std::vector<std::vector<QuadtreeNode *>> &by_depth) {
	if (!node.is_leaf) {
		collect(*node.child_nw, by_depth);
		collect(*node.child_ne, by_depth);
		collect(*node.child_sw, by_depth);
		collect(*node.child_se, by_depth);
	}
	if (by_depth.size() <= static_cast<size_t>(node.depth)) by_depth.resize(static_cast<size_t>(node.depth) + 1u);
	by_depth[static_cast<size_t>(node.depth)].push_back(&node);
}

int tile_of(const QuadtreeNode &node) { return cpt_tile_index(node.depth, node.x, node.y); }

} // namespace

std::vector<int> leaves_tapping(const std::vector<DepthRect> &rects, const std::array<CornerLockFlags, 4> &locks) {
	std::vector<int> out;
	for (int y = 0; y < kRootSize; y += kLeafSize)
		for (int x = 0; x < kRootSize; x += kLeafSize) {
			const CornerLockFlags &lock = quadrant_lock(locks, x, y);
			std::pair<int, int> xs[2], ys[2];
			const int nx = axis_taps(x, lock.x != 0, xs);
			const int ny = axis_taps(y, lock.y != 0, ys);
			for (const DepthRect &rect : rects) {
				if (rect.empty()) continue;
				if (runs_meet(xs, nx, rect.x, rect.x + rect.w) && runs_meet(ys, ny, rect.y, rect.y + rect.h)) {
					out.push_back(cpt_tile_index(kLeafDepth, x, y));
					break;
				}
			}
		}
	return out;
}

DepthRect leaf_footprint(int tile_index) noexcept {
	const int at = tile_index - kCptLevelOffset[kLeafDepth];
	const int x = (at % 16) * kLeafSize;
	const int y = (at / 16) * kLeafSize;
	return {x, y, std::min(kLeafSize + 1, kRootSize - x), std::min(kLeafSize + 1, kRootSize - y)};
}

bool bake_terrain_region(const TerrainBakeState &state, const RegionBakeInput &input, RegionBakeResult &out,
                         std::string &error) {
	out = RegionBakeResult{};
	const auto began = std::chrono::steady_clock::now();
	const auto seconds_since = [](std::chrono::steady_clock::time_point from) {
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
	};
	if (state.empty()) {
		error = "the terrain's bake state is not made";
		return false;
	}
	if (!state.rasterize_depth) {
		error = "the terrain's bake state was made without the draw-back, which a region bake makes";
		return false;
	}
	const size_t texels = static_cast<size_t>(kDepthSide) * kDepthSide;
	if (!input.depth16 || input.depth16->size() != texels) {
		error = "the depth map is not 1024 x 1024";
		return false;
	}
	for (const DepthRect &rect : input.dirty)
		if (!rect.empty() && (rect.x < 0 || rect.y < 0 || rect.x > kDepthSide - rect.w || rect.y > kDepthSide - rect.h)) {
			error = "a changed rect is not inside the depth map (an edit across its wrap is several rects)";
			return false;
		}
	const auto cancelled = [&] {
		if (!input.cancel || !input.cancel->load()) return false;
		error = "cancelled";
		return true;
	};
	const std::vector<int> leaves = leaves_tapping(input.dirty, state.base->locks);
	if (leaves.empty()) return true;

	// The nodes to make: the leaves tapping the edit and their ancestors below the root.
	std::vector<char> remake(static_cast<size_t>(kCptTileCount), 0);
	for (int leaf : leaves) {
		const DepthRect corner = leaf_footprint(leaf);
		for (int depth = kLeafDepth; depth >= 1; --depth) {
			const int size = kRootSize >> depth;
			remake[static_cast<size_t>(cpt_tile_index(depth, corner.x / size * size, corner.y / size * size))] = 1;
		}
	}

	std::unique_ptr<QuadtreeNode> root = build_quadtree(kRootSize, kLeafSize);
	std::vector<std::vector<QuadtreeNode *>> by_depth;
	collect(*root, by_depth);

	BaseMeshSet base = *state.base;
	QuadtreeContext ctx;
	ctx.tile_size = kRootSize;
	ctx.min_tile_size = kLeafSize;
	ctx.depth_buffer = input.depth16->data();
	ctx.base = &base;

	std::vector<std::shared_ptr<NodeBakeState>> made(static_cast<size_t>(kCptTileCount));
	// The remade nodes' .tml bytes (index, size), made into tiles once every level is made.
	struct Made {
		int index = 0;
		int size = 0;
		std::vector<uint8_t> tml;
	};
	std::vector<Made> tmls;
	std::string why;
	for (int depth = kLeafDepth; depth >= 1; --depth) {
		std::vector<QuadtreeNode *> level;
		for (QuadtreeNode *node : by_depth[static_cast<size_t>(depth)])
			if (remake[static_cast<size_t>(tile_of(*node))]) level.push_back(node);
		if (depth < kLeafDepth) {
			// An untouched child merges as its kept section.
			for (QuadtreeNode *node : level)
				for (QuadtreeNode *child : {node->child_nw.get(), node->child_ne.get(), node->child_sw.get(),
				                            node->child_se.get()}) {
					if (child->mesh_data) continue;
					child->mesh_data = std::make_unique<LODMeshData>();
					load_section_mesh(state.nodes[static_cast<size_t>(tile_of(*child))]->section, *child->mesh_data);
				}
		}
		const auto start = std::chrono::steady_clock::now();
		if (!make_nodes(level, ctx, input.threads, why, input.cancel)) {
			MeshSimp_FreeAll();
			error = why == "cancelled" ? why : "the terrain bake failed: " + why;
			return false;
		}
		out.cost[static_cast<size_t>(depth)] = {static_cast<int>(level.size()), seconds_since(start)};
		for (QuadtreeNode *node : level) {
			const int index = tile_of(*node);
			auto kept = std::make_shared<NodeBakeState>();
			capture_node_state(*node, *kept);
			made[static_cast<size_t>(index)] = std::move(kept);
			tmls.push_back({index, node->size, std::move(node->tile_bytes)});
			// The root's tile is leaf (0, 0)'s, sized as the root (the export reads the leaves' level for it).
			if (index == kLeafTile) tmls.push_back({0, kRootSize, tmls.back().tml});
		}
	}
	// The root is not remade (its tile is leaf (0, 0)'s), but its merge of the four quadrants is checked as the
	// whole bake checks every merge (D-TERRAIN-22), so a map the whole bake refuses is refused here; the
	// quadrants' meshes freed with it.
	{
		LODMeshData merged;
		LODMeshData_Init(&merged);
		for (QuadtreeNode *child : {root->child_nw.get(), root->child_ne.get(), root->child_sw.get(), root->child_se.get()}) {
			if (!child->mesh_data) {
				child->mesh_data = std::make_unique<LODMeshData>();
				load_section_mesh(state.nodes[static_cast<size_t>(tile_of(*child))]->section, *child->mesh_data);
			}
			LODMeshData_LoadSectionDedup(&merged, child->mesh_data.get(), 14);
			free_node_mesh(child->mesh_data);
		}
		const bool sound = merged.failure.empty() ? merged_faces_sound(merged, why) : (why = merged.failure, false);
		LODMeshData_Free(&merged);
		if (!sound) {
			MeshSimp_FreeAll();
			error = "the terrain bake failed: " + why;
			return false;
		}
	}
	MeshSimp_FreeAll();
	if (cancelled()) return false;

	// The tiles, each independent of the others, on the bake's threads.
	const auto tiles_from = std::chrono::steady_clock::now();
	std::sort(tmls.begin(), tmls.end(), [](const Made &a, const Made &b) { return a.index < b.index; });
	std::vector<CptTile> tiles(tmls.size());
	std::vector<std::string> failures(tmls.size());
	std::atomic<size_t> next{0};
	const auto convert = [&] {
		for (size_t i = next++; i < tmls.size(); i = next++)
			cpt_tile_from_tml(tmls[i].tml, tmls[i].size, tiles[i], failures[i]);
	};
	const unsigned workers = static_cast<unsigned>(std::min<size_t>(std::max(input.threads, 1u), tmls.size()));
	std::vector<std::thread> pool;
	for (unsigned t = 1; t < workers; ++t) pool.emplace_back(convert);
	convert();
	for (std::thread &each : pool) each.join();
	for (const std::string &failure : failures)
		if (!failure.empty()) {
			error = "the terrain bake failed: " + failure;
			return false;
		}
	out.tile_seconds = seconds_since(tiles_from);
	if (cancelled()) return false;

	// The draw-back over R: the new map there, then every leaf meeting R in the walk's order.
	const auto draw_from = std::chrono::steady_clock::now();
	for (int leaf : leaves) out.footprints.push_back(leaf_footprint(leaf));
	std::vector<uint16_t> drawn = *input.depth16;
	for (QuadtreeNode *node : by_depth[kLeafDepth]) {
		const int index = tile_of(*node);
		const DepthRect footprint = leaf_footprint(index);
		bool meets = false;
		for (const DepthRect &r : out.footprints) meets = meets || footprint.meets(r);
		if (!meets) continue;
		const LeafDrawBack &draw = remake[static_cast<size_t>(index)] ? made[static_cast<size_t>(index)]->draw_back
		                                                              : state.nodes[static_cast<size_t>(index)]->draw_back;
		rasterize_terrain_faces(draw.positions.data(), 3, draw.faces.data(), static_cast<int>(draw.faces.size() / 3u),
		                        drawn);
	}
	for (const DepthRect &r : out.footprints) {
		std::vector<uint16_t> texels_out(static_cast<size_t>(r.w) * static_cast<size_t>(r.h));
		for (int row = 0; row < r.h; ++row)
			std::copy_n(drawn.begin() + static_cast<std::ptrdiff_t>((r.y + row) * kDepthSide + r.x), r.w,
			            texels_out.begin() + static_cast<std::ptrdiff_t>(row * r.w));
		out.atlas.push_back(std::move(texels_out));
	}
	out.draw_back_seconds = seconds_since(draw_from);

	for (size_t i = 0; i < tmls.size(); ++i) {
		const int index = tmls[i].index;
		out.rebuilt.push_back(index);
		out.tiles.push_back(std::move(tiles[i]));
		out.nodes.push_back(index == 0 ? nullptr : std::move(made[static_cast<size_t>(index)]));
	}
	out.seconds = seconds_since(began);
	return true;
}

void apply_region_bake_state(const RegionBakeResult &result, TerrainBakeState &state) {
	if (state.nodes.size() != static_cast<size_t>(kCptTileCount)) return;
	for (size_t i = 0; i < result.rebuilt.size() && i < result.nodes.size(); ++i)
		if (result.rebuilt[i] != 0) state.nodes[static_cast<size_t>(result.rebuilt[i])] = result.nodes[i];
}

bool apply_region_bake_cpt(const RegionBakeResult &result, CptFile &cpt, std::string &error) {
	const size_t texels = static_cast<size_t>(kDepthSide) * kDepthSide;
	if (cpt.tiles.size() != static_cast<size_t>(kCptTileCount) || cpt.depth_buffer.size() != texels) {
		error = "the .cpt is not the bake's: " + std::to_string(cpt.tiles.size()) + " tiles and " +
		        std::to_string(cpt.depth_buffer.size()) + " heights, not 341 and 1024 x 1024";
		return false;
	}
	for (size_t i = 0; i < result.rebuilt.size() && i < result.tiles.size(); ++i)
		cpt.tiles[static_cast<size_t>(result.rebuilt[i])] = result.tiles[i];
	for (size_t i = 0; i < result.footprints.size() && i < result.atlas.size(); ++i) {
		const DepthRect &r = result.footprints[i];
		for (int row = 0; row < r.h; ++row)
			std::copy_n(result.atlas[i].begin() + static_cast<std::ptrdiff_t>(row * r.w), r.w,
			            cpt.depth_buffer.begin() + static_cast<std::ptrdiff_t>((r.y + row) * kDepthSide + r.x));
	}
	return true;
}

} // namespace opennova::trngen
