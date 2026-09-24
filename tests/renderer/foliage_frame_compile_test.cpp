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

// The terrain frame generates a missing detail key at its tail before the
// scene core draws the detail passes, so the first compile of a new cell
// already builds its mesh and commands its HIGH + secondary LOW draws.
// [orig: Render_ProcessMainSceneFrame @ 0x5ca654 then @ 0x5ca8ec;
// PolyTrn_RenderFrame @ 0x60f0ea..0x60f10f]
void test_new_detail_cell_draws_in_its_generation_frame() {
	r::FoliageFrameCompiler compiler = one_triangle_compiler();
	const r::FoliageDrawList &list =
			compiler.compile(detail_view(), flat_world(), r::FoliageExpansionSamplers{});
	CHECK(first_detail_build(list) != nullptr);
	CHECK(list.commands.size() == 2);
	if (list.commands.size() == 2) {
		CHECK(list.commands[0].pass == f::DetailPass::HighAlphaTest);
		CHECK(list.commands[1].pass == f::DetailPass::LowAlphaTest);
		CHECK(list.commands[1].near_secondary);
	}
}

// The thermal view reaches the runtime through the view input: one primary
// LOW command per patch at one tenth of the distance fade.
// [orig: Foliage_RenderFarPatches @ 0x60a193..0x60a19c, 0x60a497..0x60a4ae]
void test_thermal_view_commands_one_faint_low_pass() {
	r::FoliageFrameCompiler compiler = one_triangle_compiler();
	r::FoliageViewInput view = detail_view();
	view.thermal_view = true;
	const r::FoliageDrawList &list =
			compiler.compile(view, flat_world(), r::FoliageExpansionSamplers{});
	CHECK(list.commands.size() == 1);
	if (list.commands.size() == 1) {
		const r::FoliageDrawCommand &command = list.commands[0];
		CHECK(command.pass == f::DetailPass::LowAlphaTest);
		CHECK(!command.near_secondary);
		CHECK(near(command.fade, 0.1f, 1e-6f));
		CHECK(near(command.alpha_reference, 8.0f / 255.0f, 1e-6f));
		CHECK(command.high_pass_cutoff == 0.0f);
	}
}

// Every detail command carries the sway clock (the ring term at 1/655360)
// and its patch's sector origin (FB20 << 9 on the Godot-Z axis).
// [orig: Foliage_SetupVertexShaderConstants @ 0x60075e..0x60076d;
// Terrain_CollectNearFoliagePatches @ 0x603fc1]
void test_detail_commands_carry_the_sway_inputs() {
	r::FoliageFrameCompiler compiler = one_triangle_compiler();
	r::FoliageViewInput view = detail_view();
	view.detail_cells[0].key = 0x02107e30u; // Z-min -464: sector origin -512
	view.wind_osc_ring0 = 655360;
	const r::FoliageDrawList &list =
			compiler.compile(view, flat_world(), r::FoliageExpansionSamplers{});
	CHECK(!list.commands.empty());
	for (const r::FoliageDrawCommand &command : list.commands) {
		CHECK(near(command.wind_phase, 1.0f, 1e-6f));
		CHECK(command.wind_sector_origin_z == -512.0f);
	}
}

} // namespace

int main() {
	test_detail_commands_carry_the_sway_inputs();
	test_detail_vertex_placement_matches_retail();
	test_new_detail_cell_draws_in_its_generation_frame();
	test_thermal_view_commands_one_faint_low_pass();
	if (failures == 0) std::printf("foliage_frame_compile_test: all passed\n");
	return failures == 0 ? 0 : 1;
}
