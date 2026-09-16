// The retail terrain view/cull contract: the clip cone built from the
// horizontal FOV alone (vertical = 5/6 of it), the mutable far slab, the
// far-straddle subdivide arms, the sphere-then-AABB plane refinement, and the
// settings-derived LOD multiplier.
// [orig: sub_603DA0 @0x603DA0; Terrain_TraverseQuadtreeNode @0x608A00;
//  Terrain_TestAABBOutsideFrustumPlane @0x6086C0; sub_605D70 @0x605D70;
//  Terrain_Init @0x60fc33; PolyTrn_RenderFrame @0x60EAC0]
#include <runtime/terrain/quadtree.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool near_eq(float a, float b, float tol = 1e-5f) { return std::fabs(a - b) <= tol; }

void identity(float m[16]) {
	for (int i = 0; i < 16; ++i) m[i] = 0.0f;
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// One leaf node of the given half-extent centred at (cx, cy, cz).
opennova::QuadNode leaf_at(float cx, float cy, float cz, float half, int lod_level = 4) {
	opennova::QuadNode n;
	n.is_leaf = true;
	n.lod_level = lod_level;
	n.size = static_cast<int>(half * 2.0f);
	n.aabb_min[0] = cx - half; n.aabb_min[1] = cy - half; n.aabb_min[2] = cz - half;
	n.aabb_max[0] = cx + half; n.aabb_max[1] = cy + half; n.aabb_max[2] = cz + half;
	n.center[0] = cx; n.center[1] = cy; n.center[2] = cz;
	n.radius = std::sqrt(3.0f) * half;
	n.tile_index = 0;
	return n;
}

int emitted(const opennova::QuadNode &node, const opennova::TerrainViewCull &cull,
            opennova::TraversalStats *stats_out = nullptr) {
	std::vector<opennova::QuadNode> nodes{node};
	std::vector<opennova::TileMesh> meshes(1);
	std::vector<opennova::VisiblePatch> out;
	opennova::TraversalStats stats;
	opennova::TraversalConfig config;
	opennova::traverse_quadtree(nodes, meshes, 0, cull, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
			config, out, stats);
	if (stats_out != nullptr) *stats_out = stats;
	return static_cast<int>(out.size());
}

int test_cone_planes_from_horizontal_fov() {
	// fov 90: halfH = 45 deg, halfV = 0.5 * (90 * 0.83333331) = 37.5 deg.
	float view[16];
	identity(view);
	const auto cull = opennova::make_terrain_view_cull(view, 90.0f, 2000.0f);
	const float cos45 = std::cos(45.0f * 3.14159265358979323846f / 180.0f);
	const float cos375 = std::cos(37.5f * 3.14159265358979323846f / 180.0f);
	const float sin375 = std::sin(37.5f * 3.14159265358979323846f / 180.0f);
	if (!expect(near_eq(cull.planes[0][0], cos45) && near_eq(cull.planes[0][2], cos45) &&
			near_eq(cull.planes[1][0], -cos45) && near_eq(cull.planes[1][2], cos45),
			"horizontal planes are (+-cos 45, 0, sin 45)")) return 1;
	if (!expect(near_eq(cull.planes[2][1], -cos375) && near_eq(cull.planes[2][2], sin375) &&
			near_eq(cull.planes[3][1], cos375) && near_eq(cull.planes[3][2], sin375),
			"vertical planes use 5/6 of the horizontal FOV, not the projection aspect")) return 1;
	if (!expect(near_eq(cull.planes[0][1], 0.0f) && near_eq(cull.planes[2][0], 0.0f),
			"planes pass through the eye with zero cross terms")) return 1;
	if (!expect(cull.far_distance == 2000.0f, "the far slab is carried through")) return 1;
	return 0;
}

int test_far_slab_and_near_reject() {
	float view[16];
	identity(view); // camera at the origin looking down -Z (depth = -z)
	auto cull = opennova::make_terrain_view_cull(view, 80.0f, opennova::kTerrainDefaultFarDistance);
	const float half = 32.0f;
	const float r = std::sqrt(3.0f) * half;
	// depth - r > far rejects; depth - r == far survives (a strict compare).
	if (!expect(emitted(leaf_at(0.0f, 0.0f, -(2000.0f + r + 1.0f), half), cull) == 0,
			"a node wholly past the 2000-unit default far slab is rejected")) return 1;
	if (!expect(emitted(leaf_at(0.0f, 0.0f, -(2000.0f + r - 1.0f), half), cull) == 1,
			"a node touching the far slab survives")) return 1;
	cull = opennova::make_terrain_view_cull(view, 80.0f, 300.0f);
	if (!expect(emitted(leaf_at(0.0f, 0.0f, -400.0f, half), cull) == 0,
			"a positive frame view distance overrides the far slab")) return 1;
	if (!expect(emitted(leaf_at(0.0f, 0.0f, r + 1.0f, half), cull) == 0,
			"a node wholly behind the eye (depth + r < 0) is rejected")) return 1;
	opennova::TraversalStats stats;
	emitted(leaf_at(0.0f, 0.0f, r + 1.0f, half), cull, &stats);
	if (!expect(stats.rej_nearfar == 1, "the depth rejections count as near/far")) return 1;
	return 0;
}

int test_side_planes_sphere_then_aabb() {
	float view[16];
	identity(view);
	const auto cull = opennova::make_terrain_view_cull(view, 90.0f, 2000.0f);
	const float half = 32.0f;
	const float r = std::sqrt(3.0f) * half;
	// Left plane (cos45, 0, sin45): dist = (x + depth) * cos45. Wholly past
	// it by more than the radius is a sphere reject.
	const float depth = 200.0f;
	opennova::TraversalStats stats;
	if (!expect(emitted(leaf_at(-(depth + 2.0f * r), 0.0f, -depth, half), cull, &stats) == 0 &&
			stats.rej_left == 1,
			"a sphere wholly outside the left cone plane is rejected by the sphere test")) return 1;
	// Straddling by more than 0.33 r but with a corner inside: the AABB
	// refinement keeps it (a leaf cannot subdivide, so it is emitted).
	const float straddle_x = -(depth + 0.6f * r);
	if (!expect(emitted(leaf_at(straddle_x, 0.0f, -depth, half), cull) == 1,
			"a sphere straddling a plane with an inside corner survives the AABB refinement")) return 1;
	// The vertical cone: at fov 90 the top plane is 37.5 deg; a node at
	// elevation 45 deg (inside a projection's 45-deg vertical half-angle)
	// is outside retail's 5/6 cone.
	if (!expect(emitted(leaf_at(0.0f, depth + 2.0f * r, -depth, half), cull, &stats) == 0 &&
			stats.rej_top == 1,
			"the top plane is the 5/6 cone, tighter than the projection")) return 1;
	return 0;
}

int test_far_straddle_arms_force_subdivision() {
	float view[16];
	identity(view);
	const auto cull = opennova::make_terrain_view_cull(view, 80.0f, 100.0f);
	// A two-level tree: a level-1 parent straddling the far slab by more than
	// 0.25 r must subdivide even though its LOD distance would emit it.
	std::vector<opennova::QuadNode> nodes;
	opennova::QuadNode parent = leaf_at(0.0f, 0.0f, -130.0f, 64.0f, 1);
	parent.is_leaf = false;
	parent.tile_index = 0;
	nodes.push_back(parent);
	for (int i = 0; i < 4; ++i) {
		const float ox = (i & 1) ? 32.0f : -32.0f;
		const float oz = (i & 2) ? 32.0f : -32.0f;
		nodes.push_back(leaf_at(ox, 0.0f, -130.0f + oz, 32.0f, 4));
		nodes[0].children[i] = i + 1;
	}
	std::vector<opennova::TileMesh> meshes(1);
	std::vector<opennova::VisiblePatch> out;
	opennova::TraversalStats stats;
	opennova::TraversalConfig config;
	opennova::traverse_quadtree(nodes, meshes, 0, cull, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
			config, out, stats);
	bool any_parent = false;
	for (const auto &p : out) any_parent |= p.lod_level == 1;
	if (!expect(!any_parent && stats.partial_subdiv_count >= 1,
			"a level-1 node past the far slab by > 0.25 r subdivides instead of emitting")) return 1;
	return 0;
}

int test_lod_quality_scale() {
	// (detail + 1) * 0.25, clamped, then * 0.8 + 0.2: detail 0 -> 0.4, 3 -> 1.0.
	if (!expect(near_eq(opennova::terrain_lod_quality_scale(1.0f, 3), 1.0f),
			"max polygon detail leaves the context scale untouched")) return 1;
	if (!expect(near_eq(opennova::terrain_lod_quality_scale(1.0f, 0), 0.4f),
			"polygon detail 0 remaps to 0.4")) return 1;
	if (!expect(near_eq(opennova::terrain_lod_quality_scale(1.0f, 1), 0.6f) &&
			near_eq(opennova::terrain_lod_quality_scale(1.0f, 2), 0.8f),
			"the remap is linear between")) return 1;
	if (!expect(near_eq(opennova::terrain_lod_quality_scale(0.5f, 7), 0.5f),
			"detail above 3 clamps at 1.0 and the context scale multiplies")) return 1;
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_cone_planes_from_horizontal_fov();
	failures += test_far_slab_and_near_reject();
	failures += test_side_planes_sphere_then_aabb();
	failures += test_far_straddle_arms_force_subdivision();
	failures += test_lod_quality_scale();
	if (failures == 0) std::printf("terrain_view_cull_test: all passed\n");
	return failures == 0 ? 0 : 1;
}
