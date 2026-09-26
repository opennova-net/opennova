// Quadtree LOD traversal, frustum culling, mipchain.

#include <runtime/terrain/quadtree.h>

// [orig: jodemo Terrain_TraverseQuadTreeNode @0x5C89C0, Terrain_CollectVisibleSectors @0x5C9120, Terrain_BuildHeightMipChain @0x5C5310; docs/terrain/terrain-re.md]

#include <runtime/terrain/foliage_detail_collector.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace opennova {

// The traversal's 16-unit near zone: the LOD distance subtracts it scaled by
// the quality factor, while the foliage handoff subtracts the raw literal
// from the raw node distance. [orig: Terrain_TraverseQuadtreeNode @ 0x608A00,
// flt_7C4870 = 16.0 @ 0x608D46, quality scale @ 0x608D4C, handoff subtract
// @ 0x60906B]
constexpr float kTraversalNearZoneUnit = 16.0f;

// ---------------------------------------------------------------------------
// The retail terrain view/cull contract
// ---------------------------------------------------------------------------

// [orig: sub_603DA0 @0x603DA0 — halfH = fov * 0.5 @0x603db0, halfV =
//  0.5 * (fov * 0.83333331) @0x603dc4, deg->rad literal 0.01745327777777778,
//  the four plane stores @0x603de8..0x603e4f]
TerrainViewCull make_terrain_view_cull(const float view[16], float fov_deg,
                                       float far_distance) {
	TerrainViewCull cull;
	std::memcpy(cull.view, view, sizeof(cull.view));
	cull.far_distance = far_distance;
	const double half_h = double(fov_deg) * 0.5;
	const double half_v = 0.5 * (double(fov_deg) * double(kTerrainVerticalFovRatio));
	const float cos_h = static_cast<float>(std::cos(half_h * kTerrainDegToRad));
	const float sin_h = static_cast<float>(std::sin(half_h * kTerrainDegToRad));
	const float cos_v = static_cast<float>(std::cos(half_v * kTerrainDegToRad));
	const float sin_v = static_cast<float>(std::sin(half_v * kTerrainDegToRad));
	cull.planes[0][0] = cos_h;  cull.planes[0][1] = 0.0f;   cull.planes[0][2] = sin_h;
	cull.planes[1][0] = -cos_h; cull.planes[1][1] = 0.0f;   cull.planes[1][2] = sin_h;
	cull.planes[2][0] = 0.0f;   cull.planes[2][1] = -cos_v; cull.planes[2][2] = sin_v;
	cull.planes[3][0] = 0.0f;   cull.planes[3][1] = cos_v;  cull.planes[3][2] = sin_v;
	return cull;
}

float terrain_lod_quality_scale(float context_scale, int polygon_detail) {
	// [orig: Terrain_Init @0x60fc33 `(detail + 1) * 0.25`; sub_605D70 @0x605D70
	//  clamp @0x605d7c..0x605d8c, `* 0.80000001 + 0.2` @0x605d9a]
	float detail = static_cast<float>(polygon_detail + 1) * 0.25f;
	if (detail < 0.0f) detail = 0.0f;
	if (detail > 1.0f) detail = 1.0f;
	return context_scale * (detail * 0.80000001f + 0.2f);
}

namespace {

// World -> view-space (x, y, depth) with retail's forward-positive depth.
void view_point(const float view[16], float x, float y, float z, float out[3]) {
	out[0] = view[0] * x + view[4] * y + view[8] * z + view[12];
	out[1] = view[1] * x + view[5] * y + view[9] * z + view[13];
	out[2] = -(view[2] * x + view[6] * y + view[10] * z + view[14]);
}

// The eight AABB corners against one view-space plane: outside only when no
// corner sits strictly inside. Flat sectors collapse the Y extent to 0.
// [orig: Terrain_TestAABBOutsideFrustumPlane @ 0x6086C0 — flat Y/extent
//  @0x6086DF/0x608772, the `> 0` inside tests @0x608800..0x6089e0]
bool node_aabb_outside_view_plane(const float view[16], const float plane[3],
                                  const float wmin[3], const float wmax[3]) {
	for (int corner = 0; corner < 8; ++corner) {
		const float x = (corner & 1) ? wmax[0] : wmin[0];
		const float y = (corner & 2) ? wmax[1] : wmin[1];
		const float z = (corner & 4) ? wmax[2] : wmin[2];
		float v[3];
		view_point(view, x, y, z, v);
		if (plane[0] * v[0] + plane[1] * v[1] + plane[2] * v[2] > 0.0f) return false;
	}
	return true;
}

struct NodeCull {
	bool reject = false;
	bool force_subdivide = false;
	int rejected_plane = -1; // -1 = the depth slab
};

// The per-node view tests in retail order: the far/near depth slab, the
// far-straddle subdivide arms, the four cone planes against the bound
// sphere, then the AABB refinement for a sphere that straddles a plane.
// [orig: Terrain_TraverseQuadtreeNode @ 0x608A00 — center @0x608a19..0x608a5c,
//  transform @0x608a62..0x608aa2, far @0x608aaa..0x608ab9, near
//  @0x608abf..0x608acb, straddle arms @0x608ad1..0x608b03 (flt_7C333C = 0.25),
//  sphere planes @0x608b08..0x608bd7, AABB refinement @0x608bdd..0x608c97
//  (flt_7C59B4 = 0.33)]
NodeCull cull_node(const QuadNode& node, const TerrainViewCull& cull,
                   const float wmin[3], const float wmax[3],
                   float center_x, float center_y, float center_z,
                   const TraversalConfig& config) {
	NodeCull result;
	if (config.no_frustum) return result;
	float v[3];
	view_point(cull.view, center_x, center_y, center_z, v);
	const float depth = v[2];
	const float r = node.radius;
	if (!config.no_nearfar) {
		if (cull.far_distance < depth - r) { result.reject = true; return result; }
		if (depth + r < 0.0f) { result.reject = true; return result; }
		const float straddle = depth - cull.far_distance;
		if (straddle > 0.0f && node.lod_level == 0) result.force_subdivide = true;
		if (straddle > r * 0.25f && node.lod_level == 1) result.force_subdivide = true;
	}
	if (config.no_sideplanes) return result;
	float dist[4];
	for (int p = 0; p < 4; ++p) {
		const float* pl = cull.planes[p];
		dist[p] = pl[0] * v[0] + pl[1] * v[1] + pl[2] * v[2];
		if (dist[p] < -r) {
			result.reject = true;
			result.rejected_plane = p;
			return result;
		}
	}
	if (config.no_partial_subdiv) return result;
	const float threshold = -(r * 0.33000001f);
	for (int p = 0; p < 4; ++p) {
		if (dist[p] < threshold) {
			if (node_aabb_outside_view_plane(cull.view, cull.planes[p], wmin, wmax)) {
				result.reject = true;
				result.rejected_plane = p;
				return result;
			}
			if (node.lod_level < 3) result.force_subdivide = true;
		}
	}
	return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Mipchain — port of gobj_trn_build_heightmap_mipchain (0x10030C91)
// ---------------------------------------------------------------------------

Mipchain build_mipchain(const std::vector<uint16_t>& heightmap) {
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
                       const TerrainViewCull& cull,
                       float cam_x, float cam_y, float cam_z,
                       float sector_ox, float sector_oz,
                       const TraversalConfig& config,
                       std::vector<VisiblePatch>& out_patches,
                       TraversalStats& stats, bool zero_height,
                       std::vector<VisiblePatch>* out_foliage_handoffs) {
	if (node_idx < 0 || node_idx >= (int)quad_nodes.size()) return;
	const QuadNode& node = quad_nodes[node_idx];

	// World-space AABB
	float wmin[3] = { sector_ox + node.aabb_min[0], node.aabb_min[1], sector_oz + node.aabb_min[2] };
	float wmax[3] = { sector_ox + node.aabb_max[0], node.aabb_max[1], sector_oz + node.aabb_max[2] };

	// Flat-sector culling and LOD use zero Y, retaining the source radius.
	// [orig: Terrain_TraverseQuadtreeNode @ 0x608A00, center @ 0x608A50;
	// Terrain_TestAABBOutsideFrustumPlane @ 0x6086C0, Y/extent @ 0x6086DF/0x608772]
	if (zero_height) wmin[1] = wmax[1] = 0.0f;

	// The cull center is the node's own sphere center (world X/Z offset by the
	// sector origin, the source Y or zero when flat), not the AABB midpoint.
	// [orig: @0x608a19..0x608a5c reads node+0x30/+0x34/+0x38]
	const float center_x = sector_ox + node.center[0];
	const float center_y = zero_height ? 0.0f : node.center[1];
	const float center_z = sector_oz + node.center[2];
	const NodeCull view_cull = cull_node(node, cull, wmin, wmax,
			center_x, center_y, center_z, config);
	if (view_cull.reject) {
		switch (view_cull.rejected_plane) {
			// Plane 0 (cos H, 0, sin H) rejects far-left centers, plane 1
			// far-right, plane 2 (0, -cos V, sin V) far-above, plane 3 far-below.
			case 0: stats.rej_left++; break;
			case 1: stats.rej_right++; break;
			case 2: stats.rej_top++; break;
			case 3: stats.rej_bottom++; break;
			default: stats.rej_nearfar++; break;
		}
		return;
	}
	int force_subdiv_partial = 0;
	if (view_cull.force_subdivide) {
		force_subdiv_partial = 1;
		stats.partial_subdiv_count++;
	}

	// Distance: X/Z clamped to the world box, Y against the cull center.
	// [orig: @0x608c9a..0x608d3c]
	float world_center[3] = {
		(wmin[0] + wmax[0]) * 0.5f,
		center_y,
		(wmin[2] + wmax[2]) * 0.5f
	};
	float dist = node_distance(wmin, wmax, world_center, cam_x, cam_y, cam_z);

	// LOD decision
	float near_zone = config.quality * kTraversalNearZoneUnit;
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
			const VisiblePatch patch{
				node.tile_index, compute_lod_sub(), node.lod_level,
				dist, sector_ox, sector_oz, zero_height
			};
			// Exhausting terrain draws does not end the foliage handoff. Keep
			// the same frustum/LOD decision, then retail's own node gate: the
			// raw traversal distance (flat sectors: the zeroed center Y) less
			// the fixed 16.0, not the quality-scaled near zone, must be within
			// the 42-unit foliage limit (the x87 stack keeps [dist, 16.0]
			// through the emit block). The collector then re-tests each 16u
			// leaf and owns the 128-entry capacity.
			// [orig: Terrain_TraverseQuadtreeNode @ 0x608A00, 16.0 @ 0x608D46,
			// cap bypass @ 0x608FBC -> 0x609012, LOD gate @ 0x609065,
			// handoff gate @ 0x60906B..0x609078, call @ 0x60907C]
			if (out_foliage_handoffs != nullptr && node.lod_level >= 3 &&
					dist - kTraversalNearZoneUnit <= kFoliageDetailDistanceLimit)
				out_foliage_handoffs->push_back(patch);
			if (out_patches.size() < 224) {
				out_patches.push_back(patch);
				if (dist < stats.dist_min) stats.dist_min = dist;
				if (dist > stats.dist_max) stats.dist_max = dist;
			} else {
				stats.budget_drops++;
			}
		}
	} else if (!node.is_leaf) {
		for (int i = 0; i < 4; i++) {
			traverse_quadtree(quad_nodes, tile_meshes, node.children[i],
			                  cull, cam_x, cam_y, cam_z,
			                  sector_ox, sector_oz, config, out_patches, stats,
			                  zero_height, out_foliage_handoffs);
		}
	}
}

// ---------------------------------------------------------------------------
// Visible-bounds tracking
// ---------------------------------------------------------------------------

void VisibleBounds::include(const float wmin[3], const float wmax[3]) {
	if (!valid) {
		for (int i = 0; i < 3; ++i) { min[i] = wmin[i]; max[i] = wmax[i]; }
		valid = true;
		return;
	}
	for (int i = 0; i < 3; ++i) {
		if (wmin[i] < min[i]) min[i] = wmin[i];
		if (wmax[i] > max[i]) max[i] = wmax[i];
	}
}

// The same view rejection as the draw traversal, no distance heuristic:
// a surviving node subdivides until the LOD cap (the leaves) and each
// terminal node's world AABB joins the running bounds.
// [orig: Terrain_TraverseQuadtreeNode @ 0x608a00 trackBounds leg]
void track_visible_bounds(const std::vector<QuadNode>& quad_nodes,
                          int node_idx,
                          const TerrainViewCull& cull,
                          float sector_ox, float sector_oz,
                          const TraversalConfig& config,
                          VisibleBounds& out_bounds, bool zero_height) {
	if (node_idx < 0 || node_idx >= (int)quad_nodes.size()) return;
	const QuadNode& node = quad_nodes[node_idx];
	float wmin[3] = { sector_ox + node.aabb_min[0], node.aabb_min[1], sector_oz + node.aabb_min[2] };
	float wmax[3] = { sector_ox + node.aabb_max[0], node.aabb_max[1], sector_oz + node.aabb_max[2] };
	if (zero_height) wmin[1] = wmax[1] = 0.0f;
	const NodeCull view_cull = cull_node(node, cull, wmin, wmax,
			sector_ox + node.center[0], zero_height ? 0.0f : node.center[1],
			sector_oz + node.center[2], config);
	if (view_cull.reject) return;
	bool has_children = false;
	if (!node.is_leaf) {
		for (int i = 0; i < 4; i++) {
			if (node.children[i] >= 0) {
				has_children = true;
				track_visible_bounds(quad_nodes, node.children[i], cull,
				                     sector_ox, sector_oz, config, out_bounds,
				                     zero_height);
			}
		}
	}
	if (!has_children) {
		// The trackBounds stores still read the source node's raw Y limits,
		// even when its culling center and rendered vertices were flattened.
		// [orig: Terrain_TraverseQuadtreeNode @ 0x608A00, stores @ 0x608E04..0x608E5F]
		wmin[1] = node.aabb_min[1];
		wmax[1] = node.aabb_max[1];
		out_bounds.include(wmin, wmax);
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
