// Terrain frame compile (ADR 0033 R2): the per-frame walk and the load-time
// scene snapshot, moved down from the shell adapter's self-driven loop as a
// structural translation of the same decisions.

#include "terrain/terrain_frame.h"

#include <cpt/cpt.h>
#include <terrain/coords.h>
#include <trn/trn.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace opennova {

namespace {

terrain::CoordsQuadrantLocks locks_from_trn(const TrnConfig &trn) {
	const TerrainQuadrantLocks source = trn.get_quadrant_locks();
	terrain::CoordsQuadrantLocks locks{};
	for (int quadrant = 0; quadrant < static_cast<int>(source.size()); ++quadrant) {
		locks.set(quadrant, source[quadrant].x != 0, source[quadrant].y != 0);
	}
	return locks;
}

// The per-LOD index count after conversion — strips through the shared
// degenerate-skipping expansion, lists floored to whole triangles. This is
// the count the embedder's uploaded mesh carries, so mesh availability
// (index_count >= 3) is decided from the same number on both sides.
int converted_index_count(const CptTileLOD &lod, std::vector<uint32_t> &scratch) {
	if (lod.is_strip) {
		scratch.clear();
		strip_to_list(lod.indices, scratch, 0);
		return static_cast<int>(scratch.size());
	}
	return static_cast<int>(lod.indices.size() / 3) * 3;
}

} // namespace

TerrainSceneSnapshot build_terrain_scene_snapshot(const CptFile &cpt,
                                                  const TrnConfig &trn) {
	TerrainSceneSnapshot scene;

	constexpr int hm_size = 1024;
	constexpr size_t expected_depth_samples =
			static_cast<size_t>(hm_size) * static_cast<size_t>(hm_size);
	if (cpt.tiles.empty() || cpt.depth_buffer.size() != expected_depth_samples) {
		return scene;
	}

	// Sector routing straight from the .trn document.
	for (int r = 0; r < 16; ++r) {
		for (int c = 0; c < 16; ++c) {
			scene.sector_grid[r][c] = trn.sector_grid[r][c];
		}
	}
	scene.origin_x = trn.origin_x;
	scene.origin_y = trn.origin_y;
	scene.wrap_x = trn.wrap_x != 0;
	scene.wrap_y = trn.wrap_y != 0;

	const float height_scale = 1.0f / 256.0f;
	const terrain::CoordsQuadrantLocks quadrant_locks = locks_from_trn(trn);

	scene.tile_meshes.resize(cpt.tiles.size());
	scene.tile_attributes.resize(cpt.tiles.size());

	std::vector<uint32_t> index_scratch;
	for (size_t ti = 0; ti < cpt.tiles.size(); ++ti) {
		const CptTile &tile = cpt.tiles[ti];
		TileMesh &meta = scene.tile_meshes[ti];

		meta.vertex_count = tile.vertex_count;
		meta.tile_size = tile.tile_size;
		meta.aabb_min[0] = meta.aabb_min[1] = meta.aabb_min[2] = 1e9f;
		meta.aabb_max[0] = meta.aabb_max[1] = meta.aabb_max[2] = -1e9f;

		const int local_base_x = tile.tile_x & 0x1FF;
		const int local_base_z = tile.tile_y & 0x1FF;

		// The tile's own quadrant decides the lock policy for every one of its
		// vertices; a tile whose last row/column lands on the quadrant boundary
		// is exactly the case the .trn locks exist for.
		// [orig: sub_402D20 — quadrant = (tile_x >= 0x200) + 2 * (tile_y >= 0x200).]
		const terrain::CoordsTaps taps = terrain::coords_taps_for_quadrant(
				quadrant_locks, tile.tile_x & 0x200, tile.tile_y & 0x200, hm_size);

		for (int vi = 0; vi < tile.vertex_count; ++vi) {
			const uint16_t rel_x = tile.vertex_indices[vi * 2 + 0];
			const uint16_t rel_y = tile.vertex_indices[vi * 2 + 1];

			const int wx = tile.tile_x + rel_x;
			const int wz = tile.tile_y + rel_y;
			const int hx = taps.x(wx);
			const int hz = taps.z(wz);
			const float hy = cpt.depth_buffer[hz * hm_size + hx] * height_scale;

			const float lx = static_cast<float>(local_base_x + rel_x);
			const float lz = static_cast<float>(local_base_z + rel_y);

			if (lx < meta.aabb_min[0]) meta.aabb_min[0] = lx;
			if (hy < meta.aabb_min[1]) meta.aabb_min[1] = hy;
			if (lz < meta.aabb_min[2]) meta.aabb_min[2] = lz;
			if (lx > meta.aabb_max[0]) meta.aabb_max[0] = lx;
			if (hy > meta.aabb_max[1]) meta.aabb_max[1] = hy;
			if (lz > meta.aabb_max[2]) meta.aabb_max[2] = lz;
		}

		for (int i = 0; i < 3; ++i) {
			meta.center[i] = (meta.aabb_min[i] + meta.aabb_max[i]) * 0.5f;
		}
		const float dx = meta.aabb_max[0] - meta.center[0];
		const float dy = meta.aabb_max[1] - meta.center[1];
		const float dz = meta.aabb_max[2] - meta.center[2];
		meta.radius = std::sqrt(dx * dx + dy * dy + dz * dz);

		for (int lod = 0; lod < 8; ++lod) {
			meta.lods[lod].index_count = converted_index_count(tile.lods[lod], index_scratch);
		}

		scene.tile_attributes[ti].quadrant_x =
				static_cast<uint8_t>((tile.tile_x >> 9) & 1);
		scene.tile_attributes[ti].quadrant_z =
				static_cast<uint8_t>((tile.tile_y >> 9) & 1);
	}

	// Tile lookup by (x, z, size) for quadtree node -> tile resolution.
	struct TileKey {
		uint16_t x, z, size;
		bool operator<(const TileKey &o) const {
			if (size != o.size) return size < o.size;
			if (x != o.x) return x < o.x;
			return z < o.z;
		}
	};
	std::map<TileKey, int> tile_lookup;
	for (size_t i = 0; i < cpt.tiles.size(); ++i) {
		const CptTile &t = cpt.tiles[i];
		tile_lookup[{t.tile_x, t.tile_y, t.tile_size}] = static_cast<int>(i);
	}

	uint16_t leaf_size = 1024;
	for (const CptTile &t : cpt.tiles) {
		if (t.tile_size < leaf_size) leaf_size = t.tile_size;
	}

	scene.mipchain = build_mipchain(cpt.depth_buffer, hm_size);

	// The 1024 -> leaf subdivision the traversal walks
	// [orig: Terrain_TraverseQuadTreeNode @ 0x5C89C0].
	struct Builder {
		std::vector<QuadNode> &nodes;
		const std::map<TileKey, int> &lut;
		Mipchain &mc;
		uint16_t leaf_sz;
		int atlas_dim;

		int build(int size, int lx, int lz, int lod_level) {
			QuadNode node;
			node.lod_level = lod_level;
			node.size = size;

			node.aabb_min[0] = static_cast<float>(lx & 0x1FF);
			node.aabb_min[2] = static_cast<float>(lz & 0x1FF);
			node.aabb_max[0] = static_cast<float>((lx & 0x1FF) + size);
			node.aabb_max[2] = static_cast<float>((lz & 0x1FF) + size);

			if (lod_level < mc.level_count) {
				const int cells_per_side = atlas_dim / size;
				const int mip_idx = (lx / size) + (lz / size) * cells_per_side;
				const uint8_t *entry = mc.levels[lod_level] + 2 * mip_idx;
				node.aabb_min[1] = static_cast<float>(entry[0]) * 0.5f;
				node.aabb_max[1] = static_cast<float>(entry[1]) * 0.5f;
			} else {
				node.aabb_min[1] = 0.0f;
				node.aabb_max[1] = 128.0f;
			}

			for (int i = 0; i < 3; ++i) {
				node.center[i] = (node.aabb_min[i] + node.aabb_max[i]) * 0.5f;
			}
			const float dx = node.aabb_max[0] - node.center[0];
			const float dy = node.aabb_max[1] - node.center[1];
			const float dz = node.aabb_max[2] - node.center[2];
			node.radius = std::sqrt(dx * dx + dy * dy + dz * dz);

			const auto it = lut.find({static_cast<uint16_t>(lx),
					static_cast<uint16_t>(lz), static_cast<uint16_t>(size)});
			node.tile_index = (it != lut.end()) ? it->second : -1;
			node.is_leaf = (size <= static_cast<int>(leaf_sz));

			const int this_idx = static_cast<int>(nodes.size());
			nodes.push_back(node);

			if (size > static_cast<int>(leaf_sz)) {
				const int half = size / 2;
				nodes[this_idx].children[0] = build(half, lx, lz, lod_level + 1);
				nodes[this_idx].children[1] = build(half, lx + half, lz, lod_level + 1);
				nodes[this_idx].children[2] = build(half, lx, lz + half, lod_level + 1);
				nodes[this_idx].children[3] = build(half, lx + half, lz + half, lod_level + 1);
			}
			return this_idx;
		}
	};

	scene.quad_nodes.reserve(512);
	Builder builder{scene.quad_nodes, tile_lookup, scene.mipchain, leaf_size, hm_size};
	scene.root_node = builder.build(1024, 0, 0, 0);

	if (scene.root_node >= 0) {
		for (int i = 0; i < 4; ++i) {
			scene.l1_children[i] = scene.quad_nodes[scene.root_node].children[i];
		}
	}
	return scene;
}

const TerrainDrawList &TerrainFrameCompiler::compile(
		const TerrainSceneSnapshot &scene, const TerrainViewInput &view) {
	++compile_index_;
	draw_list_.frame_id = compile_index_;
	draw_list_.patches.clear();
	draw_list_.detail_cells.clear();
	draw_list_.debug = TerrainFrameDebugCounters{};
	draw_list_.debug.compile_index = compile_index_;
	visible_.clear();

	if (!scene.valid()) {
		return draw_list_;
	}

	// MVP = proj * view, column-major, then Gribb/Hartmann plane extraction.
	float mvp[16] = {};
	for (int row = 0; row < 4; ++row) {
		for (int col = 0; col < 4; ++col) {
			for (int k = 0; k < 4; ++k) {
				mvp[col * 4 + row] += view.proj[k * 4 + row] * view.view[col * 4 + k];
			}
		}
	}
	const Frustum frustum = extract_frustum(mvp);

	TraversalStats stats;
	if (visible_.capacity() < static_cast<size_t>(kPatchBudget)) {
		visible_.reserve(kPatchBudget);
	}

	// The 512-world-unit sector window around the camera, routed through the
	// .trn sector grid before the shared quadtree walk
	// [orig: Terrain_CollectVisibleSectors @ 0x5C9120].
	const int cam_sx = static_cast<int>(view.cam_x) >> 9;
	const int cam_sz = static_cast<int>(view.cam_z) >> 9;
	const int mask_x = scene.wrap_x ? 0 : -16;
	const int mask_z = scene.wrap_y ? 0 : -16;

	for (int dz = -5; dz <= 5; ++dz) {
		for (int dx = 0; dx < 11; ++dx) {
			const int sx = (cam_sx - 5) + dx;
			const int sz = dz + cam_sz;
			int gx = sx - scene.origin_x;
			int gz = sz - scene.origin_y;

			if ((gx & mask_x) != 0) gx = (gx < 0) ? 0 : 0xFF;
			if ((gz & mask_z) != 0) gz = (gz < 0) ? 0 : 0xFF;

			const int sector_id = scene.sector_grid[gz & 0xF][gx & 0xF];
			if (sector_id <= 0) {
				continue;
			}

			int child = -1;
			if (sector_id == 1) child = 0;
			else if (sector_id == 3) child = 1;
			else if (sector_id == 2) child = 2;
			else if (sector_id == 4) child = 3;
			if (child < 0 || child >= 4 || scene.l1_children[child] < 0) {
				continue;
			}

			const float sector_ox = static_cast<float>(sx * 512);
			const float sector_oz = static_cast<float>(sz * 512);

			const size_t sector_patch_begin = visible_.size();
			traverse_quadtree(scene.quad_nodes, scene.tile_meshes,
					scene.l1_children[child], frustum,
					view.cam_x, view.cam_y, view.cam_z,
					sector_ox, sector_oz, view.config, visible_, stats);
			++draw_list_.debug.sectors_walked;

			// Detail foliage collection is NOT a radial walk: retail's
			// frustum-culled traversal hands each frustum-surviving emitted
			// node of LOD level >= 3 to the 16u cell collector, so cells
			// behind the camera never enter the visible-key list and the
			// far-slot pool's working set stays below its 16-bit-index
			// capacity (over-collection thrashed the witnessed LRU into
			// per-frame cell blink; D-FOLIAGE-13)
			// [orig: Terrain_TraverseQuadtreeNode handoff
			// @ 0x60905c..0x60907c -> Terrain_CollectNearFoliagePatches
			// @ 0x603e60]. The collector re-tests every cell's clamped
			// AABB against the 42u limit, so the handoff is a broad phase.
			for (size_t pi = sector_patch_begin; pi < visible_.size(); ++pi) {
				const VisiblePatch &vp = visible_[pi];
				if (vp.lod_level < 3 || vp.tile_index < 0 ||
						vp.tile_index >= static_cast<int>(scene.tile_meshes.size())) {
					continue;
				}
				const int node_size = 1024 >> vp.lod_level;
				const TileMesh &tm = scene.tile_meshes[vp.tile_index];
				const int local_x = static_cast<int>(std::lround(
						tm.center[0] - node_size * 0.5f));
				const int local_z = static_cast<int>(std::lround(
						tm.center[2] - node_size * 0.5f));
				collect_foliage_detail_patches(scene.mipchain, sector_id,
						sx * 512, sz * 512, local_x, local_z, node_size,
						view.cam_x, view.cam_y, view.cam_z,
						draw_list_.detail_cells);
			}
		}
	}

	std::sort(visible_.begin(), visible_.end(),
			[](const VisiblePatch &a, const VisiblePatch &b) {
				return a.distance < b.distance;
			});

	draw_list_.debug.visible_patches = static_cast<int>(visible_.size());
	const int count = std::min(static_cast<int>(visible_.size()), kPatchBudget);
	if (draw_list_.patches.capacity() < static_cast<size_t>(count)) {
		draw_list_.patches.reserve(kPatchBudget);
	}

	for (int i = 0; i < count; ++i) {
		const VisiblePatch &vp = visible_[i];
		if (vp.tile_index < 0 ||
				vp.tile_index >= static_cast<int>(scene.tile_meshes.size())) {
			++draw_list_.debug.empty_mesh_drops;
			continue;
		}
		const TileMesh &tm = scene.tile_meshes[vp.tile_index];

		// The family select over the recovered 0..15 sublevel, with the
		// empty-family fallback the embedder's mesh set used to impose.
		int lod = terrain_lod_family(vp.lod_sub);
		if (tm.lods[lod].index_count < 3 && lod != 0) {
			lod = 0;
			++stats.lod_fallbacks;
		}
		if (tm.lods[lod].index_count < 3) {
			++draw_list_.debug.empty_mesh_drops;
			continue;
		}

		TerrainPatchDraw draw;
		draw.tile_index = vp.tile_index;
		draw.lod_family = lod;
		draw.sector_ox = vp.sector_ox;
		draw.sector_oz = vp.sector_oz;
		draw.distance = vp.distance;
		draw.quadrant_x = scene.tile_attributes[vp.tile_index].quadrant_x;
		draw.quadrant_z = scene.tile_attributes[vp.tile_index].quadrant_z;
		draw_list_.patches.push_back(draw);
		++draw_list_.debug.lod_distribution[lod];
	}

	draw_list_.debug.emitted_patches = static_cast<int>(draw_list_.patches.size());
	draw_list_.debug.traversal = stats;
	return draw_list_;
}

} // namespace opennova
