// Pins the portable foliage frame compile against the recovered retail
// instructions: the detail tier's per-vertex placement of the source model,
// its commands, the MODEL tier's normalized mesh and GridPlacementVS
// instance blocks, and the water-side split. Expected values are derived by
// hand (or by an independent emulation of the instruction sequence) from the
// witnessed formulas, not from the compiler.

#include <runtime/renderer/foliage_frame.h>

#include <cmath>
#include <cstdio>
#include <vector>

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
r::FoliageFrameCompiler one_triangle_compiler(float p_model_radius = 1.0f) {
	std::array<f::RuntimeSlot, opennova::FOLIAGE_MAX_DEFS> slots{};
	std::array<r::FoliageSlotGeometry, opennova::FOLIAGE_MAX_DEFS> geometry{};
	slots[0].enabled = true;
	slots[0].model_radius = p_model_radius;
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
		// The secondary follows the HIGH draw of the same geometry: a
		// depth-sorting embedder draws it a hair nearer, the primary unbiased.
		// [orig: Foliage_RenderDetailPatches @ 0x60a653, @ 0x60a659..0x60a694]
		CHECK(list.commands[0].sorting_offset == 0.0f);
		CHECK(list.commands[1].sorting_offset ==
				r::kFoliageSecondaryLowSortingOffset);
		CHECK(r::kFoliageSecondaryLowSortingOffset > 0.0f);
	}
}

// The thermal view reaches the runtime through the view input: one primary
// LOW command per patch at one tenth of the distance fade.
// [orig: Foliage_RenderDetailPatches @ 0x60a193..0x60a19c, 0x60a497..0x60a4ae]
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

// A camera at (32, cam_y, 98) looking down -Z: the anchor at (32, 0, 48)
// sits at view depth 50, inside the MODEL walk's >= 38 floor.
r::FoliageViewInput silhouette_view(float cam_y, float water_height) {
	r::FoliageViewInput view;
	view.cam_x = 32.0f;
	view.cam_y = cam_y;
	view.cam_z = 98.0f;
	view.view[0] = view.view[5] = view.view[10] = view.view[15] = 1.0f;
	view.view[12] = -32.0f;
	view.view[13] = -cam_y;
	view.view[14] = -98.0f;
	view.water_height = water_height;
	view.silhouette_anchors.push_back({32.0f, 0.0f, 48.0f});
	return view;
}

// The instanced VB normalizes the source over its bound square with the
// imported x mirrored back to retail's, and halves y.
// [orig: Foliage_FillInstancedModelBuffers @ 0x5ffa2a..0x5ffac6]
void test_model_mesh_is_the_normalized_instanced_vb() {
	r::FoliageFrameCompiler compiler = one_triangle_compiler();
	const r::FoliageSlotModelMesh &mesh = compiler.model_mesh(0);
	CHECK(mesh.valid && mesh.vertices.size() == 3 && mesh.indices.size() == 3);
	if (mesh.vertices.size() != 3) return;
	// Imported (1, 0, 0) is retail x = -1: (-1 - 0) * 0.5 / 1 + 0.5 = 0.
	CHECK(near(mesh.vertices[0].x, 0.0f) && near(mesh.vertices[0].z, 0.5f));
	CHECK(near(mesh.vertices[1].x, 0.5f) && near(mesh.vertices[1].z, 1.0f));
	CHECK(near(mesh.vertices[2].y, 1.0f) && near(mesh.max_half_height, 1.0f));
	CHECK(!compiler.model_mesh(1).valid);
}

// Each MODEL submission carries its GridPlacementVS blocks: render x (the
// Godot Z) of the corners, the heights, render z (Godot X) and the fold;
// the anchor at Godot (32, 48) first draws key 0x00207FE0 candidate 30,
// whose corners the independent emulation places at (35.453812, 44.654190),
// (35.551880, 47.652588), (32.455414, 44.752258), (32.553482, 47.750656).
// The c9.x wind term is sin(++counter * 0.001) * 0.08 per draw.
// [orig: Foliage_UploadModelTileVSConstants @ 0x60108e..0x601209;
// Foliage_GenerateModelTileInstances @ 0x600bd0..0x600c6a]
void test_model_commands_carry_retail_instance_blocks() {
	r::FoliageFrameCompiler compiler = one_triangle_compiler(2.0f);
	const r::FoliageDrawList &list = compiler.compile(
			silhouette_view(10.0f, -1000.0f), flat_world(),
			r::FoliageExpansionSamplers{});
	std::vector<const r::FoliageDrawCommand *> models;
	for (const r::FoliageDrawCommand &command : list.commands) {
		if (command.tier == r::FoliageTier::Silhouette) models.push_back(&command);
	}
	CHECK(models.size() == 4);
	CHECK(list.model_instances.size() == 9);
	if (models.size() != 4 || list.model_instances.empty()) return;
	const r::FoliageDrawCommand &first = *models[0];
	CHECK(first.cell_key == 0x00207FE0u && first.instance_count == 1 &&
			first.first_instance == 0);
	const float *rows = list.model_instances[0].rows;
	const float corner_x[4] = {35.453812f, 35.551880f, 32.455414f, 32.553482f};
	const float corner_z[4] = {44.654190f, 47.652588f, 44.752258f, 47.750656f};
	for (int k = 0; k < 4; ++k) {
		CHECK(near(rows[k], corner_z[k], 2e-4f));
		CHECK(rows[4 + k] == 0.0f);
		CHECK(near(rows[8 + k], corner_x[k], 2e-4f));
		CHECK(rows[12 + k] == 0.0f);
	}
	CHECK(near(first.wind_offset,
			static_cast<float>(std::sin(0.001) * 0.079999998), 1e-7f));
	CHECK(near(models[1]->wind_offset,
			static_cast<float>(std::sin(0.002) * 0.079999998), 1e-7f));
	CHECK(first.aabb_min[0] <= 32.4556f && first.aabb_max[0] >= 35.5517f);
	CHECK(first.aabb_min[2] <= 44.6543f && first.aabb_max[2] >= 47.7505f);
	CHECK(first.aabb_max[1] >= 1.0f);
	// Camera distance sqrt(10^2 + 50^2) = 50.99: int(4096 / (50 + 1)) = 80.
	CHECK(near(first.alpha_reference, 80.0f / 255.0f, 1e-6f));
}

// The BySide waves and the detail passes split by the water: an anchor's
// entity z - 1.0 below the water is the below-water wave, far while the
// camera is at or above it; a detail patch whose maximum height is at or
// below the water draws in the far pass while the camera is above.
// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dd2, @ 0x5c7dfd..0x5c7e18;
// Foliage_RenderDetailPatches @ 0x609df4..0x609e1b, @ 0x60a1a0..0x60a1c6]
void test_water_sides() {
	{
		r::FoliageFrameCompiler compiler = one_triangle_compiler(2.0f);
		// Camera above water 0.5, the anchor's feet at 0: z - 1 < 0.5.
		const r::FoliageDrawList &list = compiler.compile(
				silhouette_view(10.0f, 0.5f), flat_world(),
				r::FoliageExpansionSamplers{});
		CHECK(!list.commands.empty());
		for (const r::FoliageDrawCommand &command : list.commands) {
			CHECK(command.far_side);
			// The far wave's masks lead the far-side alpha rung.
			// [orig: Terrain_RenderWorldScene @ 0x5c955f, @ 0x5c9596]
			CHECK(command.render_rung == opennova::renderer::kRungAlphaFarSide);
			CHECK(command.sorting_offset == r::kFoliageMaskSortingOffset);
		}
	}
	{
		r::FoliageFrameCompiler compiler = one_triangle_compiler(2.0f);
		// The same anchor with the camera below the water: one side.
		const r::FoliageDrawList &list = compiler.compile(
				silhouette_view(0.25f, 0.5f), flat_world(),
				r::FoliageExpansionSamplers{});
		CHECK(!list.commands.empty());
		for (const r::FoliageDrawCommand &command : list.commands) {
			CHECK(!command.far_side);
			// The camera wave's masks follow the water pass and lead the
			// scars. [orig: Terrain_RenderWorldScene @ 0x5c95dc,
			// @ 0x5c9638, @ 0x5c9658]
			CHECK(command.render_rung == opennova::renderer::kRungScars);
			CHECK(command.sorting_offset == r::kFoliageMaskSortingOffset);
		}
	}
	{
		r::FoliageFrameCompiler compiler = one_triangle_compiler(2.0f);
		// Feet at 2: z - 1 = 1 is not below 0.5; the camera above: one side.
		r::FoliageViewInput view = silhouette_view(10.0f, 0.5f);
		view.silhouette_anchors[0][1] = 2.0f;
		const r::FoliageDrawList &list =
				compiler.compile(view, flat_world(), r::FoliageExpansionSamplers{});
		CHECK(!list.commands.empty());
		for (const r::FoliageDrawCommand &command : list.commands) {
			CHECK(!command.far_side);
		}
	}
	const struct {
		float max_height;
		float cam_y;
		bool far;
	} detail_cases[] = {
		{0.5f, 10.0f, true},  // wholly at the water, camera above: far pass
		{0.6f, 10.0f, false}, // reaches above it: camera pass
		{0.5f, 0.0f, false},  // camera below: the at-water patch is its side
		{0.6f, 0.0f, true},   // and the patch above the water is the far one
	};
	for (const auto &detail_case : detail_cases) {
		r::FoliageFrameCompiler compiler = one_triangle_compiler();
		r::FoliageViewInput view = detail_view();
		view.water_height = 0.5f;
		view.cam_y = detail_case.cam_y;
		view.detail_cells[0].max_height = detail_case.max_height;
		const r::FoliageDrawList &list =
				compiler.compile(view, flat_world(), r::FoliageExpansionSamplers{});
		CHECK(!list.commands.empty());
		for (const r::FoliageDrawCommand &command : list.commands) {
			CHECK(command.far_side == detail_case.far);
			// Detail pass 0 before the water, pass 1 before the camera-side
			// alpha. [orig: Terrain_RenderWorldScene @ 0x5c95c5,
			// @ 0x5c9665]
			CHECK(command.render_rung ==
					(detail_case.far ? opennova::renderer::kRungFoliageFarSide
					                 : opennova::renderer::kRungFoliageCameraSide));
		}
	}
}

// The BySide wave split every mask consumer shares: the camera is above at
// or over the water (setnl), and an entity whose z - 1.0 is below the water
// rides the far wave exactly while the camera is above.
// [orig: Terrain_RenderWorldScene @ 0x5c93a1..0x5c93b0;
// Terrain_RenderSectorEntitiesBySide @ 0x5c7dd2, @ 0x5c7dfd..0x5c7e18]
void test_entity_water_side() {
	CHECK(r::foliage_camera_above_water(0.5f, 0.5f));
	CHECK(!r::foliage_camera_above_water(0.49f, 0.5f));
	// Camera above: the feet at 1.49 are below (0.49 < 0.5), 1.5 are not.
	CHECK(r::foliage_entity_far_side(1.49f, 10.0f, 0.5f));
	CHECK(!r::foliage_entity_far_side(1.5f, 10.0f, 0.5f));
	// Camera below: the above-water entity is the far one.
	CHECK(r::foliage_entity_far_side(1.5f, 0.0f, 0.5f));
	CHECK(!r::foliage_entity_far_side(1.49f, 0.0f, 0.5f));
	// Env_WaterHeightFixed 0 (no water) still splits at z - 1 < 0.
	CHECK(!r::foliage_entity_far_side(1.0f, 20.0f, 0.0f));
	CHECK(r::foliage_entity_far_side(0.99f, 20.0f, 0.0f));
}

} // namespace

int main() {
	test_entity_water_side();
	test_model_mesh_is_the_normalized_instanced_vb();
	test_model_commands_carry_retail_instance_blocks();
	test_water_sides();
	test_detail_commands_carry_the_sway_inputs();
	test_detail_vertex_placement_matches_retail();
	test_new_detail_cell_draws_in_its_generation_frame();
	test_thermal_view_commands_one_faint_low_pass();
	if (failures == 0) std::printf("foliage_frame_compile_test: all passed\n");
	return failures == 0 ? 0 : 1;
}
