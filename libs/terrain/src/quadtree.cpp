// Quadtree LOD traversal, frustum culling, mipchain.

#include "terrain/quadtree.h"

// Engine: jodemo.exe Terrain_TraverseQuadTreeNode@0x5C89C0,
// Terrain_CollectVisibleSectors@0x5C9120, Terrain_BuildHeightMipChain@0x5C5310
// [orig: Terrain_TraverseQuadTreeNode @ 0x5C89C0, Terrain_CollectVisibleSectors @ 0x5C9120, Terrain_BuildHeightMipChain @ 0x5C5310 (jodemo); docs/terrain/terrain-re.md]
// docs/engine_spec_terrain.md 5.3, 7.1

#include <algorithm>
#include <cmath>
#include <cstring>

namespace opennova {

// ---------------------------------------------------------------------------
// Frustum extraction — Gribb/Hartmann method
// ---------------------------------------------------------------------------

Frustum extract_frustum(const float mvp[16]) {
	Frustum wf;
	// Column-major: element (row, col) = mvp[4*col + row]
	for (int i = 0; i < 4; i++) {
		float r0 = mvp[4 * i], r1 = mvp[4 * i + 1], r2 = mvp[4 * i + 2], r3 = mvp[4 * i + 3];
		wf.planes[Frustum::P_LEFT][i]   = r3 + r0;
		wf.planes[Frustum::P_RIGHT][i]  = r3 - r0;
		wf.planes[Frustum::P_BOTTOM][i] = r3 + r1;
		wf.planes[Frustum::P_TOP][i]    = r3 - r1;
		wf.planes[Frustum::P_NEAR][i]   = r3 + r2;
		wf.planes[Frustum::P_FAR][i]    = r3 - r2;
	}
	for (int p = 0; p < 6; p++) {
		float len = std::sqrt(wf.planes[p][0] * wf.planes[p][0] +
		                      wf.planes[p][1] * wf.planes[p][1] +
		                      wf.planes[p][2] * wf.planes[p][2]);
		if (len > 0.0f) {
			float inv = 1.0f / len;
			for (int i = 0; i < 4; i++) wf.planes[p][i] *= inv;
		}
	}
	return wf;
}

bool aabb_outside_plane(const float plane[4],
                        const float aabb_min[3], const float aabb_max[3]) {
	float px = (plane[0] >= 0) ? aabb_max[0] : aabb_min[0];
	float py = (plane[1] >= 0) ? aabb_max[1] : aabb_min[1];
	float pz = (plane[2] >= 0) ? aabb_max[2] : aabb_min[2];
	return (plane[0] * px + plane[1] * py + plane[2] * pz + plane[3]) < 0.0f;
}

// ---------------------------------------------------------------------------
// Mipchain — port of gobj_trn_build_heightmap_mipchain (0x10030C91)
// ---------------------------------------------------------------------------

Mipchain build_mipchain(const std::vector<uint16_t>& heightmap, int atlas_size) {
	Mipchain mc;
	mc.data.resize(699052, 0);
	uint8_t* write_ptr = mc.data.data();

	mc.levels[0] = write_ptr;
	int mip_size = 512;

	for (int row = 2; row < 1026; row += 2) {
		int r0 = ((row - 2) & 0x3FF) << 10;
		int r1 = ((row - 1) & 0x3FF) << 10;
		int r2 = ((row)     & 0x3FF) << 10;

		for (int col = 2; col < 1026; col += 2) {
			int c0 = (col - 2) & 0x3FF;
			int c1 = (col - 1) & 0x3FF;
			int c2 = (col)     & 0x3FF;

			int h[9] = {
				heightmap[c0 + r0], heightmap[c1 + r0], heightmap[c2 + r0],
				heightmap[c0 + r1], heightmap[c1 + r1], heightmap[c2 + r1],
				heightmap[c0 + r2], heightmap[c1 + r2], heightmap[c2 + r2],
			};

			int min_h = h[0], max_h = h[0];
			for (int k = 1; k < 9; k++) {
				if (h[k] < min_h) min_h = h[k];
				if (h[k] > max_h) max_h = h[k];
			}

			*write_ptr++ = (uint8_t)(min_h >> 7);
			*write_ptr++ = (uint8_t)((max_h + 127) >> 7);
		}
	}

	mc.level_count = 1;
	while (mip_size > 1) {
		mc.levels[mc.level_count] = write_ptr;
		int prev_size = mip_size;
		mip_size >>= 1;
		uint8_t* prev = mc.levels[mc.level_count - 1];

		for (int rr = 0; rr < mip_size; rr++) {
			for (int cc = 0; cc < mip_size; cc++) {
				uint8_t* a = prev + (rr * 2 * prev_size + cc * 2) * 2;
				uint8_t* b = a + 2;
				uint8_t* c = a + prev_size * 2;
				uint8_t* d = c + 2;

				uint8_t mn = a[0];
				if (b[0] < mn) mn = b[0];
				if (c[0] < mn) mn = c[0];
				if (d[0] < mn) mn = d[0];

				uint8_t mx = a[1];
				if (b[1] > mx) mx = b[1];
				if (c[1] > mx) mx = c[1];
				if (d[1] > mx) mx = d[1];

				*write_ptr++ = mn;
				*write_ptr++ = mx;
			}
		}
		mc.level_count++;
	}

	// Reverse levels: build order is finest-first, engine wants coarsest-first
	for (int i = 0; i < mc.level_count / 2; i++)
		std::swap(mc.levels[i], mc.levels[mc.level_count - 1 - i]);

	return mc;
}

// ---------------------------------------------------------------------------
// Distance + traversal — port of sub_10032BA6
// ---------------------------------------------------------------------------

int terrain_lod_family(int lod_sub) noexcept {
	return std::clamp(lod_sub, 0, 15) / 2;
}

float node_distance(const float aabb_min[3], const float aabb_max[3],
                    const float center[3], float px, float py, float pz) {
	float dx = 0, dz = 0;
	if (px < aabb_min[0]) dx = aabb_min[0] - px;
	else if (px > aabb_max[0]) dx = px - aabb_max[0];
	if (pz < aabb_min[2]) dz = aabb_min[2] - pz;
	else if (pz > aabb_max[2]) dz = pz - aabb_max[2];
	float dy = std::fabs(center[1] - py);
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void traverse_quadtree(const std::vector<QuadNode>& quad_nodes,
                       const std::vector<TileMesh>& tile_meshes,
                       int node_idx,
                       const Frustum& frustum,
                       float cam_x, float cam_y, float cam_z,
                       float sector_ox, float sector_oz,
                       const TraversalConfig& config,
                       std::vector<VisiblePatch>& out_patches,
                       TraversalStats& stats) {
	if (node_idx < 0 || node_idx >= (int)quad_nodes.size()) return;
	stats.nodes_visited++;
	const QuadNode& node = quad_nodes[node_idx];

	// World-space AABB
	float wmin[3] = { sector_ox + node.aabb_min[0], node.aabb_min[1], sector_oz + node.aabb_min[2] };
	float wmax[3] = { sector_ox + node.aabb_max[0], node.aabb_max[1], sector_oz + node.aabb_max[2] };

	int force_subdiv_partial = 0;

	if (!config.no_frustum) {
		// Near/far planes
		if (!config.no_nearfar) {
			if (aabb_outside_plane(frustum.planes[Frustum::P_NEAR], wmin, wmax)) { stats.rej_nearfar++; return; }
			if (aabb_outside_plane(frustum.planes[Frustum::P_FAR], wmin, wmax))  { stats.rej_nearfar++; return; }
		}

		// Side planes
		if (!config.no_sideplanes) {
			if (aabb_outside_plane(frustum.planes[Frustum::P_LEFT], wmin, wmax))   { stats.rej_left++;   return; }
			if (aabb_outside_plane(frustum.planes[Frustum::P_RIGHT], wmin, wmax))  { stats.rej_right++;  return; }
			if (aabb_outside_plane(frustum.planes[Frustum::P_BOTTOM], wmin, wmax)) { stats.rej_bottom++; return; }
			if (aabb_outside_plane(frustum.planes[Frustum::P_TOP], wmin, wmax))    { stats.rej_top++;    return; }
		}

		// Partial-subdivision: force subdivision when AABB center is deep
		// past a side frustum plane (IDA sub_10032BA6)
		if (!config.no_partial_subdiv) {
			float cx = (wmin[0] + wmax[0]) * 0.5f;
			float cy = (wmin[1] + wmax[1]) * 0.5f;
			float cz = (wmin[2] + wmax[2]) * 0.5f;
			float threshold = -(node.radius * 0.33000001f);
			for (int p = Frustum::P_LEFT; p <= Frustum::P_TOP; p++) {
				const float* pl = frustum.planes[p];
				float center_dist = pl[0] * cx + pl[1] * cy + pl[2] * cz + pl[3];
				if (center_dist < threshold && node.lod_level < 3) {
					force_subdiv_partial = 1;
					stats.partial_subdiv_count++;
					if (node.lod_level >= 0 && node.lod_level < 5)
						stats.partial_subdiv_per_level[node.lod_level]++;
					break;
				}
			}
		}
	}

	// Distance
	float world_center[3] = {
		(wmin[0] + wmax[0]) * 0.5f,
		(wmin[1] + wmax[1]) * 0.5f,
		(wmin[2] + wmax[2]) * 0.5f
	};
	float dist = node_distance(wmin, wmax, world_center, cam_x, cam_y, cam_z);

	// LOD decision
	float near_zone = config.quality * 16.0f;
	float effective_dist = (dist > near_zone) ? dist - near_zone : 0.0f;
	float lod_threshold = (float)node.size * config.quality * 0.7f;

	// LOD sub-level interpolation
	float lod_sub_range = (float)node.size * 0.7f;
	auto compute_lod_sub = [&]() -> int {
		int sub = (int)((effective_dist - lod_sub_range) / lod_sub_range * 16.0f);
		if (sub < 0) sub = 0;
		if (sub > 15) sub = 15;
		return sub;
	};

	bool emit_here = false;
	if (node.is_leaf) {
		emit_here = true;
	} else if (!config.force_leaves && effective_dist >= lod_threshold && !force_subdiv_partial) {
		emit_here = true;
	}

	// Non-leaf wants to emit but has no tile — fall back to children
	if (emit_here && !node.is_leaf && node.tile_index < 0) {
		emit_here = false;
	}

	if (emit_here) {
		if (node.tile_index >= 0) {
			if (out_patches.size() < 224) {
				out_patches.push_back({
					node.tile_index, compute_lod_sub(), node.lod_level,
					dist, sector_ox, sector_oz
				});
				if (node.is_leaf) stats.leaf_emits++; else stats.nonleaf_emits++;
				if (dist < stats.dist_min) stats.dist_min = dist;
				if (dist > stats.dist_max) stats.dist_max = dist;
			} else {
				stats.budget_drops++;
			}
		}
	} else if (!node.is_leaf) {
		for (int i = 0; i < 4; i++) {
			traverse_quadtree(quad_nodes, tile_meshes, node.children[i],
			                  frustum, cam_x, cam_y, cam_z,
			                  sector_ox, sector_oz, config, out_patches, stats);
		}
	}
}

// ---------------------------------------------------------------------------
// Strip to list
// ---------------------------------------------------------------------------

void strip_to_list(const std::vector<uint16_t>& strip,
                   std::vector<uint32_t>& out, uint32_t base) {
	if (strip.size() < 3) return;
	for (size_t i = 2; i < strip.size(); i++) {
		uint16_t a = strip[i - 2], b = strip[i - 1], c = strip[i];
		if (a == b || b == c || a == c) continue;
		if (i & 1) { out.push_back(base + b); out.push_back(base + a); out.push_back(base + c); }
		else       { out.push_back(base + a); out.push_back(base + b); out.push_back(base + c); }
	}
}

} // namespace opennova
