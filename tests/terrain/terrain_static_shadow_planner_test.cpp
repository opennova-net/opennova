// Static terrain-shadow planner: caster snapshot admission, plan memoization
// under the state epoch, suppression canonicalization/config stamping, team
// participation in page identity, the sub-quantum light reuse rule, and the
// per-job material tick (time never moves the state revision or page
// identity; a raster samples the tick it was given).
#include <runtime/terrain/terrain_static_shadow_planner.h>

#include <algorithm>
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
	geometry->materials.push_back(material);
	TerrainStaticShadowResolvedSurface surface;
	surface.render_object = 0;
	surface.material_index = 0;
	surface.vertices = {{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f},
			{0.0f, 2.0f, 0.0f}};
	surface.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
	// Front-facing under retail's ordinary CULLMODE CCW after projection.
	surface.indices = {0, 2, 1};
	geometry->surfaces[0].push_back(std::move(surface));
	opennova::terrain::TerrainStaticShadowRenderObjectCoverage coverage;
	coverage.authored_surface_count = 1;
	coverage.valid_surface_count = 1;
	geometry->coverage[0].push_back(coverage);
	// The model sphere the tile collector reads (model+0x14).
	geometry->model_radius_fixed = 2 << 16;
	return geometry;
}

std::shared_ptr<opennova::terrain::TerrainStaticShadowAlphaPyramid>
constant_alpha(uint8_t value) {
	auto pyramid = std::make_shared<
			opennova::terrain::TerrainStaticShadowAlphaPyramid>();
	pyramid->storage.push_back({value});
	pyramid->mips.push_back({1, 1, 1, pyramid->storage[0].data()});
	return pyramid;
}

std::shared_ptr<TerrainStaticShadowResolvedGeometry>
animated_alpha_geometry() {
	auto geometry = box_geometry();
	geometry->key = 0x9abc;
	TerrainStaticShadowResolvedMaterial &material = geometry->materials[0];
	material.alpha_test_enabled = true;
	material.alpha_ref = 127;
	material.samples_diffuse_alpha = true;
	material.uses_material_alpha = true;
	material.runtime_material.animation.num_frames = 2;
	material.runtime_material.animation.animation_type = 0;
	material.runtime_material.animation.cycle_frame_time = 100;
	material.diffuse_alpha_frames = {constant_alpha(0), constant_alpha(255)};
	return geometry;
}

// A 2x1 cutout: the left texel transparent, the right opaque.
std::shared_ptr<opennova::terrain::TerrainStaticShadowAlphaPyramid>
half_cutout_alpha() {
	auto pyramid = std::make_shared<
			opennova::terrain::TerrainStaticShadowAlphaPyramid>();
	pyramid->storage.push_back({0, 255});
	pyramid->mips.push_back({2, 1, 2, pyramid->storage[0].data()});
	return pyramid;
}

// The retail time-scroll UV mode: u' = u + phase/65536 with the phase
// advancing by speed (8.8 units per 1/256 s) — a continuous translation the
// alpha-tested silhouette genuinely depends on.
std::shared_ptr<TerrainStaticShadowResolvedGeometry>
scrolling_cutout_geometry() {
	auto geometry = box_geometry();
	geometry->key = 0xdef0;
	TerrainStaticShadowResolvedMaterial &material = geometry->materials[0];
	material.alpha_test_enabled = true;
	material.alpha_ref = 127;
	material.samples_diffuse_alpha = true;
	// Only the #UV twins evaluate MatTexCoord1.
	std::snprintf(material.runtime_material.shader_name,
			sizeof(material.runtime_material.shader_name), "%s", "FF_ST_OP#UV");
	material.runtime_material.u_params.style = 16;
	material.runtime_material.u_params.gen_rate = 1.0f;
	material.diffuse_alpha_frames = {half_cutout_alpha()};
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

	TerrainTilePageKey page;
	page.sector_origin_x = 0;
	page.sector_origin_z = 0;
	page.page_local_x = 0;
	page.page_local_z = 0;
	page.page_lod_level = 4; // 64-unit span

	TerrainStaticShadowPlanner planner;
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

	// A material whose retail effect has no PROJSHAD declaration is an exact
	// no-op, not a guessed NORMAL fallback and not an unsupported caster.
	TerrainStaticShadowPlanner no_pass_planner;
	no_pass_planner.set_light({0.3f, 0.9f, 0.3f}, {127, 200, 200});
	TerrainStaticShadowPlannerCaster no_pass_caster = caster(0);
	auto no_pass_geometry = box_geometry();
	no_pass_geometry->key = 0x5678;
	no_pass_geometry->materials[0].casts_projected_shadow = false;
	no_pass_caster.geometry = std::move(no_pass_geometry);
	no_pass_planner.replace_casters({std::move(no_pass_caster)}, false);
	no_pass_planner.reset_frame_diagnostics();
	const TerrainStaticShadowPagePlanResult no_pass_plan =
			no_pass_planner.plan(page);
	if (!expect(no_pass_plan.valid && no_pass_plan.raster_required,
			"a no-pass material still produces an exact current empty page")) {
		return 1;
	}
	TerrainStaticShadowAlphaPage no_pass_page;
	no_pass_page.page = page;
	no_pass_page.content = no_pass_plan.content;
	no_pass_page.width = 64;
	no_pass_page.height = 64;
	no_pass_page.alpha.assign(static_cast<size_t>(64) * 64, 200);
	if (!expect(no_pass_planner.rasterize(page, no_pass_page),
			"a no-pass material rasterizes as an exact no-op")) {
		return 1;
	}
	if (!expect(std::all_of(no_pass_page.alpha.begin(), no_pass_page.alpha.end(),
			[](uint8_t value) { return value == 200; }) &&
			no_pass_planner.diagnostics().frame_triangles == 0,
			"no-pass tracer/flag/glass surfaces cannot darken terrain")) {
		return 1;
	}

	// Animation time is sampled when a page is actually recomposed, but does
	// not manufacture a cache miss: retail's terrain-page hit key is spatial.
	// The tick is a per-job input the adapter sets on its worker planner copy:
	// crossing a selected-frame boundary neither publishes a new snapshot
	// (state revision) nor recompiles the cached plan; the next raster simply
	// samples the frame current at its tick.
	TerrainStaticShadowPlanner animated_planner;
	animated_planner.set_light({0.3f, 0.9f, 0.3f}, {127, 200, 200});
	TerrainStaticShadowPlannerCaster animated_caster = caster(0);
	animated_caster.geometry = animated_alpha_geometry();
	animated_planner.replace_casters({std::move(animated_caster)}, false);
	animated_planner.reset_frame_diagnostics();
	const TerrainStaticShadowPagePlanResult animated_plan =
			animated_planner.plan(page);
	TerrainStaticShadowAlphaPage rejected_page;
	rejected_page.page = page;
	rejected_page.content = animated_plan.content;
	rejected_page.width = 64;
	rejected_page.height = 64;
	rejected_page.alpha.assign(static_cast<size_t>(64) * 64, 200);
	if (!expect(animated_planner.rasterize(page, rejected_page),
			"time-flipbook frame zero rasterizes exactly") ||
			!expect(std::all_of(rejected_page.alpha.begin(),
					rejected_page.alpha.end(),
					[](uint8_t value) { return value == 200; }),
					"transparent frame zero must fail the projected alpha test")) {
		return 1;
	}
	const uint64_t frame_zero_revision = animated_planner.state_revision();
	animated_planner.set_material_time(99);
	if (!expect(animated_planner.state_revision() == frame_zero_revision,
			"time inside one selected frame keeps the worker snapshot stable")) {
		return 1;
	}
	animated_planner.set_material_time(100);
	if (!expect(animated_planner.state_revision() == frame_zero_revision,
			"a selected-frame transition is a per-job tick, not a new snapshot") ||
			!expect(animated_planner.plan(page).content.value ==
					animated_plan.content.value,
					"animation time must not force a resident spatial page miss") ||
			!expect(animated_planner.diagnostics().frame_plan_compiles == 1,
					"animation time must not recompile the cached page plan")) {
		return 1;
	}
	TerrainStaticShadowAlphaPage admitted_page = rejected_page;
	if (!expect(animated_planner.rasterize(page, admitted_page),
			"time-flipbook frame one rasterizes under the cached plan") ||
			!expect(std::any_of(admitted_page.alpha.begin(),
					admitted_page.alpha.end(),
					[](uint8_t value) { return value != 200; }),
					"opaque frame one must admit and darken projected coverage")) {
		return 1;
	}

	// A continuously scrolling alpha cutout: the silhouette consumes the full
	// evaluated UV transform, so no stamp rule can make the phase a snapshot
	// property without changing output. The revision and page identity hold
	// across every tick; rasters agree inside one 1/256 s evaluator unit
	// ((3 << 8) / 1000 == 0) and differ across a half-period scroll.
	TerrainStaticShadowPlanner scroll_planner;
	scroll_planner.set_light({0.3f, 0.9f, 0.3f}, {127, 200, 200});
	TerrainStaticShadowPlannerCaster scroll_caster = caster(0);
	scroll_caster.geometry = scrolling_cutout_geometry();
	// Scaled up so the cutout halves each cover many page samples.
	scroll_caster.world_transform = {8.0f, 0.0f, 0.0f, 0.0f, 8.0f, 0.0f, 0.0f,
			0.0f, 8.0f, 24.0f, 4.0f, 24.0f};
	scroll_planner.replace_casters({std::move(scroll_caster)}, false);
	scroll_planner.reset_frame_diagnostics();
	const uint64_t scroll_revision = scroll_planner.state_revision();
	const TerrainStaticShadowPagePlanResult scroll_plan =
			scroll_planner.plan(page);
	if (!expect(scroll_plan.valid && scroll_plan.raster_required,
			"the scrolling cutout caster plans its page")) {
		return 1;
	}
	const auto raster_at = [&](uint32_t time_ms,
			std::vector<uint8_t> &r_alpha) -> bool {
		scroll_planner.set_material_time(time_ms);
		TerrainStaticShadowAlphaPage scroll_page;
		scroll_page.page = page;
		scroll_page.content = scroll_plan.content;
		scroll_page.width = 64;
		scroll_page.height = 64;
		scroll_page.alpha.assign(static_cast<size_t>(64) * 64, 200);
		if (!scroll_planner.rasterize(page, scroll_page)) return false;
		r_alpha = std::move(scroll_page.alpha);
		return true;
	};
	std::vector<uint8_t> phase_zero;
	std::vector<uint8_t> same_unit;
	std::vector<uint8_t> half_period;
	if (!expect(raster_at(0, phase_zero) && raster_at(3, same_unit) &&
					raster_at(500, half_period),
			"the scrolling cutout rasterizes at every tick")) {
		return 1;
	}
	if (!expect(scroll_planner.state_revision() == scroll_revision,
			"no revision bump across frames: material time is per job") ||
			!expect(scroll_planner.plan(page).content.value ==
					scroll_plan.content.value,
					"a scrolling material never changes spatial page identity") ||
			!expect(scroll_planner.diagnostics().frame_plan_compiles == 1,
					"ticks reuse the cached page plan outright")) {
		return 1;
	}
	if (!expect(std::any_of(phase_zero.begin(), phase_zero.end(),
					[](uint8_t value) { return value != 200; }),
			"the cutout's opaque half must darken projected coverage") ||
			!expect(phase_zero == same_unit,
					"ticks inside one evaluator unit raster identically") ||
			!expect(phase_zero != half_period,
					"a raster samples the UV scroll phase of its own tick")) {
		return 1;
	}

	std::puts("terrain_static_shadow_planner_test: OK");
	return 0;
}
