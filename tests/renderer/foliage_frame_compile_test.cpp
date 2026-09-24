// Pins the portable foliage frame compile against the recovered retail
// instructions: the detail tier's per-vertex placement of the source model.
// Expected values are derived by hand from the witnessed formulas, not from
// the compiler.

#include <runtime/renderer/foliage_frame.h>

#include <cmath>
#include <cstdio>

namespace {
namespace r = opennova::renderer;
namespace f = opennova::foliage;

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                          \
		if (!(condition)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);           \
			++failures;                                                                \
		}                                                                           \
	} while (0)

bool near(float actual, float expected, float epsilon = 1e-4f) {
	return std::fabs(actual - expected) <= epsilon;
}

f::WorldSamplers flat_world() {
	f::WorldSamplers world;
	world.detail_foliage_mask_at = [](int32_t, int32_t) { return 1u; };
	world.model_foliage_mask_at = [](int32_t, int32_t) { return 1u; };
	world.height_at = [](float, float) { return 0.0f; };
	world.path_blocked = [](float, float, float) { return false; };
	return world;
}

// A one-triangle source model: vertex 0 on +X, vertex 1 on +Z, vertex 2 up.
r::FoliageFrameCompiler one_triangle_compiler() {
	std::array<f::RuntimeSlot, opennova::FOLIAGE_MAX_DEFS> slots{};
	std::array<r::FoliageSlotGeometry, opennova::FOLIAGE_MAX_DEFS> geometry{};
	slots[0].enabled = true;
	slots[0].model_radius = 1.0f;
	slots[0].source_vertex_count = 3;
	geometry[0].vertices = {
		{1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
		{0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
		{0.0f, 2.0f, 0.0f, 0.0f, 1.0f},
	};
	geometry[0].indices = {0, 1, 2};
	geometry[0].radius = 1.0f;
	geometry[0].valid = true;
	r::FoliageFrameCompiler compiler;
	compiler.configure_slots(slots, geometry);
	return compiler;
}

r::FoliageViewInput detail_view() {
	r::FoliageViewInput view;
	view.no_frustum = true;
	view.detail_cells.push_back({0x00100030u, 10.0f});
	return view;
}

const r::FoliageMeshBuild *first_detail_build(const r::FoliageDrawList &list) {
	for (const r::FoliageMeshBuild &build : list.mesh_builds) {
		if (build.tier == r::FoliageTier::Detail && build.vertex_count > 0) {
			return &build;
		}
	}
	return nullptr;
}

// Retail places each source vertex (sx, sy, sz) of candidate 0 at render
// x = keyLo + B + sx*sin + sz*cos (the Godot-Z axis) and render z = keyHi +
// A + sx*cos - sz*sin (Godot X), y = ground + sy*0.5; the 3DI import stores
// vertex.x = -sx. Candidate 0 of key 0x00100030 has centre (keyHi + A,
// keyLo + B) = (18.63215637, 49.69863892) and yaw 4.83126879.
// [orig: generate_foliage_instances_0 @ 0x600112..0x60014d, 0x600121 (y)]
void test_detail_vertex_placement_matches_retail() {
	r::FoliageFrameCompiler compiler = one_triangle_compiler();
	const f::WorldSamplers world = flat_world();
	const r::FoliageExpansionSamplers expansion;
	const r::FoliageViewInput view = detail_view();
	const r::FoliageMeshBuild *build = nullptr;
	for (int frame = 0; frame < 2 && build == nullptr; ++frame) {
		build = first_detail_build(compiler.compile(view, world, expansion));
	}
	CHECK(build != nullptr);
	if (build == nullptr) return;
	const r::FoliageDrawList &list = compiler.last_draw_list();
	const float cx = 18.63215637f;
	const float cz = 49.69863892f;
	const float yaw = 4.83126879f;
	const float c = std::cos(yaw);
	const float s = std::sin(yaw);
	// Source (sx, sy, sz) for each imported vertex (x negated).
	const float source[3][3] = {
		{-1.0f, 0.0f, 0.0f},
		{0.0f, 0.0f, 1.0f},
		{0.0f, 2.0f, 0.0f},
	};
	for (int v = 0; v < 3; ++v) {
		const r::FoliageVertex &got = list.vertices[build->first_vertex + v];
		const float sx = source[v][0];
		const float sy = source[v][1];
		const float sz = source[v][2];
		const float expect_x = cx + sx * c - sz * s;
		const float expect_z = cz + sx * s + sz * c;
		CHECK(near(got.x, expect_x));
		CHECK(near(got.z, expect_z));
		CHECK(near(got.y, sy * 0.5f));
	}
}

} // namespace

int main() {
	test_detail_vertex_placement_matches_retail();
	if (failures == 0) std::printf("foliage_frame_compile_test: all passed\n");
	return failures == 0 ? 0 : 1;
}
