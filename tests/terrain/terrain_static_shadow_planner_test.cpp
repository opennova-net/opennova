// Static terrain-shadow planner: caster snapshot admission, plan memoization
// under the state epoch, suppression canonicalization/config stamping, team
// participation in page identity, and the sub-quantum light reuse rule.
#include <terrain/terrain_static_shadow_planner.h>

#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

using opennova::terrain::TerrainStaticShadowResolvedGeometry;
using opennova::terrain::TerrainStaticShadowResolvedMaterial;
using opennova::terrain::TerrainStaticShadowResolvedSurface;

// One exact opaque box caster: one material, one ROBJ, one triangle.
std::shared_ptr<TerrainStaticShadowResolvedGeometry> box_geometry() {
	auto geometry = std::make_shared<TerrainStaticShadowResolvedGeometry>();
	geometry->key = 0x1234;
	geometry->render_object_counts = {1, 0};
	TerrainStaticShadowResolvedMaterial material;
	material.exact = true;
	geometry->materials.push_back(material);
	TerrainStaticShadowResolvedSurface surface;
	surface.render_object = 0;
	surface.material_index = 0;
	surface.vertices = {{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f},
			{0.0f, 2.0f, 0.0f}};
	surface.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
	surface.indices = {0, 1, 2};
	geometry->surfaces[0].push_back(std::move(surface));
	opennova::terrain::TerrainStaticShadowRenderObjectCoverage coverage;
	coverage.authored_surface_count = 1;
	coverage.valid_surface_count = 1;
	geometry->coverage[0].push_back(coverage);
	geometry->local_min = {0.0f, 0.0f, 0.0f};
	geometry->local_max = {2.0f, 2.0f, 2.0f};
	geometry->has_bounds = true;
	geometry->bounds_exact = true;
	return geometry;
}

opennova::terrain::TerrainStaticShadowPlannerCaster caster(int team) {
	opennova::terrain::TerrainStaticShadowPlannerCaster record;
	record.bms_id = 42;
	record.entity_kind = 2; // building
	record.entity_index = 0;
	record.team = team;
	record.active = true;
	record.graphic = "Box";
	// Identity transform rows + origin inside the page below.
	record.world_transform = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
			1.0f, 8.0f, 4.0f, 8.0f};
	record.geometry = box_geometry();
	record.ground_y = 0.0f;
	record.caster_identity = 7;
	return record;
}

// A second caster placed at origin_x, distinct in every identity field.
opennova::terrain::TerrainStaticShadowPlannerCaster caster_at(float origin_x,
		int32_t entity_index, int32_t bms_id) {
	opennova::terrain::TerrainStaticShadowPlannerCaster record = caster(0);
	record.bms_id = bms_id;
	record.entity_index = entity_index;
	record.caster_identity = 7 + static_cast<uint64_t>(entity_index);
	record.world_transform[9] = origin_x;
	return record;
}

} // namespace

int main() {
	using namespace opennova;
	using namespace opennova::terrain;

	// Flat 64x64 height field at 0.
	const int dim = 64;
	std::vector<uint16_t> heightmap(static_cast<size_t>(dim) * dim, 0);
	std::vector<int> sector_grid(256, 1);
	TerrainHeightField field;
	field.heightmap = heightmap.data();
	field.dim = dim;
	field.layout.sector_grid = sector_grid.data();
	if (!expect(field.valid(), "the synthetic height field is valid")) {
		return 1;
	}

	TerrainTilePageKey page;
	page.sector_origin_x = 0;
	page.sector_origin_z = 0;
	page.page_local_x = 0;
	page.page_local_z = 0;
	page.page_lod_level = 4; // 64-unit span

	TerrainStaticShadowPlanner planner;
	planner.set_receiver_terrain(field, 1);
	planner.set_light({0.3f, 0.9f, 0.3f}, {127, 200, 200});
	planner.replace_casters({caster(0)}, false);
	if (!expect(planner.snapshot_exact(),
			"an exact caster snapshot plans exactly")) {
		return 1;
	}

	planner.reset_frame_diagnostics();
	const TerrainStaticShadowPagePlanResult first = planner.plan(page);
	if (!expect(first.valid && first.raster_required &&
					first.content.value != 0,
			"the caster's page plans with a raster requirement and stamp")) {
		return 1;
	}
	if (!expect(planner.diagnostics().frame_plan_compiles == 1,
			"the first plan compiles the page job")) {
		return 1;
	}
	const TerrainStaticShadowPagePlanResult again = planner.plan(page);
	if (!expect(again.valid && again.content.value == first.content.value &&
					planner.diagnostics().frame_plan_compiles == 1,
			"an unchanged epoch replans from the cache without compiling")) {
		return 1;
	}

	// Sub-quantum raw light movement keeps the cached plans; an epoch byte
	// change rebuilds them.
	planner.reset_frame_diagnostics();
	planner.set_light({0.30001f, 0.9f, 0.3f}, {127, 200, 200});
	(void)planner.plan(page);
	if (!expect(planner.diagnostics().frame_plan_compiles == 0,
			"sub-quantum light movement reuses cached plans outright")) {
		return 1;
	}
	planner.set_light({0.3f, 0.5f, 0.8f}, {127, 201, 200});
	(void)planner.plan(page);
	if (!expect(planner.diagnostics().frame_plan_compiles == 1,
			"a quantized epoch change rebuilds the page plan")) {
		return 1;
	}

	// Suppression canonicalizes and participates in page identity.
	const TerrainTileContentStamp unsuppressed = planner.plan(page).content;
	planner.set_suppressed_bms_ids({9, 3, 9, 1});
	if (!expect(planner.suppressed_bms_ids() ==
					std::vector<int32_t>({1, 3, 9}),
			"suppressed ids canonicalize to a sorted unique list")) {
		return 1;
	}
	const TerrainTileContentStamp suppressed = planner.plan(page).content;
	if (!expect(suppressed.value != unsuppressed.value,
			"the suppression config participates in the page content stamp")) {
		return 1;
	}
	planner.set_suppressed_bms_ids({3, 1, 9});
	if (!expect(planner.plan(page).content.value == suppressed.value,
			"an equivalent suppression list is a stamp no-op")) {
		return 1;
	}

	// Team selects TEX_TEAM alpha frames, so it participates in identity.
	planner.replace_casters({caster(0)}, false);
	const TerrainTileContentStamp team_zero = planner.plan(page).content;
	planner.replace_casters({caster(1)}, false);
	const TerrainTileContentStamp team_one = planner.plan(page).content;
	if (!expect(team_zero.value != team_one.value,
			"a caster team change invalidates its resident pages")) {
		return 1;
	}

	// An identical caster snapshot is a no-op: the state revision holds and
	// cached plans survive.
	planner.reset_frame_diagnostics();
	const uint64_t revision_before_identical = planner.state_revision();
	planner.replace_casters({caster(1)}, false);
	if (!expect(planner.state_revision() == revision_before_identical,
			"an identical caster snapshot keeps the state revision")) {
		return 1;
	}
	(void)planner.plan(page);
	if (!expect(planner.diagnostics().frame_plan_compiles == 0,
			"an identical caster snapshot replans from the cache")) {
		return 1;
	}

	// Per-page stamps localize caster changes: moving an east-page caster
	// leaves the west page's content stamp untouched.
	TerrainTilePageKey east_page = page;
	east_page.page_local_x = 64;
	planner.replace_casters({caster(1), caster_at(72.0f, 1, 43)}, false);
	const TerrainTileContentStamp west_before = planner.plan(page).content;
	const TerrainTileContentStamp east_before =
			planner.plan(east_page).content;
	planner.replace_casters({caster(1), caster_at(74.0f, 1, 43)}, false);
	if (!expect(planner.plan(page).content.value == west_before.value,
			"moving an east-page caster keeps the west page's stamp")) {
		return 1;
	}
	if (!expect(planner.plan(east_page).content.value != east_before.value,
			"moving an east-page caster changes the east page's stamp")) {
		return 1;
	}

	// A repeated receiver clear is a no-op; the first clear still bumps.
	const uint64_t revision_before_clear = planner.state_revision();
	planner.clear_receiver_terrain();
	const uint64_t revision_after_clear = planner.state_revision();
	if (!expect(revision_after_clear != revision_before_clear,
			"clearing a live receiver bumps the state revision")) {
		return 1;
	}
	planner.clear_receiver_terrain();
	if (!expect(planner.state_revision() == revision_after_clear,
			"clearing an absent receiver keeps the state revision")) {
		return 1;
	}
	planner.set_receiver_terrain(field, 1);

	// A missing-geometry admitted caster fails planning closed.
	planner.replace_casters({caster(0)}, true);
	if (!expect(!planner.snapshot_exact(),
			"an admitted caster without geometry poisons snapshot exactness")) {
		return 1;
	}
	const TerrainStaticShadowPagePlanResult failed = planner.plan(page);
	if (!expect(!failed.valid,
			"an inexact snapshot cannot publish page plans")) {
		return 1;
	}

	// Rasterizing the planned page writes the opaque silhouette into alpha.
	planner.replace_casters({caster(0)}, false);
	const TerrainStaticShadowPagePlanResult plan = planner.plan(page);
	if (!expect(plan.valid && plan.raster_required, "the box page replans")) {
		return 1;
	}
	TerrainTileCompositionJob job;
	job.target.page = page;
	job.layout.texture_dimension = 64;
	job.layout.world_span = 64;
	TerrainStaticShadowAlphaPage alpha_page;
	alpha_page.page = page;
	alpha_page.content = plan.content;
	alpha_page.width = 64;
	alpha_page.height = 64;
	alpha_page.alpha.assign(static_cast<size_t>(64) * 64, 200);
	if (!expect(planner.rasterize(page, alpha_page),
			"the cached plan rasterizes its page")) {
		return 1;
	}
	bool darkened = false;
	for (const uint8_t value : alpha_page.alpha) {
		if (value != 200) {
			darkened = true;
			break;
		}
	}
	if (!expect(darkened,
			"the opaque box silhouette must darken page light alpha")) {
		return 1;
	}
	if (!expect(planner.diagnostics().frame_triangles > 0,
			"raster diagnostics count the submitted triangles")) {
		return 1;
	}

	std::puts("terrain_static_shadow_planner_test: OK");
	return 0;
}
