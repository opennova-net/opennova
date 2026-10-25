// The terrain tile-composition worker (runtime/terrain/terrain_tile_composition_worker.h):
// no sources = nothing enqueues; a queued job composes one complete 256x256
// page off the installed snapshot; wait_idle() returns once every queued job
// has completed; a completion a predicate rejects stays queued while an
// allowed one behind it drains; an epoch bump drops everything queued; a
// one-worker, no-lane-thread budget composes the same bytes as the hardware
// sizing, whose page rasters take every lane of the pool; a page enqueued for the
// device pass leaves composed with its casters' device job, which drawn and applied
// gives the CPU raster's bytes.
#include <runtime/terrain/terrain_tile_composition_worker.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <thread>

using opennova::terrain::Rgba8Image;
using opennova::terrain::TerrainTileCompositionWorker;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

Rgba8Image solid_image(uint32_t width, uint32_t height, std::array<uint8_t, 4> rgba) {
	Rgba8Image image;
	image.width = width;
	image.height = height;
	image.pixels.resize(static_cast<size_t>(width) * height * 4u);
	for (size_t i = 0; i < image.pixels.size(); i += 4)
		std::copy(rgba.begin(), rgba.end(), image.pixels.begin() + i);
	return image;
}

// One cache hands out the jobs so two pages land in two layers.
opennova::TerrainTileCompositionJob job_for(opennova::TerrainTileCompositionCache &cache,
		int sector_x, uint8_t page_lod) {
	opennova::TerrainTilePageKey page{};
	page.sector_origin_x = sector_x;
	page.page_lod_level = page_lod;
	const auto decision = cache.request(opennova::TerrainTileCompositionRequest{
			page, 0, 0, 0 });
	if (!decision.has_value() || !decision->job.has_value()) {
		std::printf("FAIL the cache issued no job for sector %d\n", sector_x);
		++failures;
		return opennova::TerrainTileCompositionJob{};
	}
	return *decision->job;
}

// Waits (bounded) until the worker holds exactly `expected_completed`
// untaken completions with nothing queued or in flight for its epoch.
bool settle(const TerrainTileCompositionWorker &worker, std::size_t expected_completed) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < deadline) {
		if (worker.current_epoch_active_jobs() == 0 &&
				worker.completed_jobs() == expected_completed &&
				worker.pending_jobs() == expected_completed)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

std::shared_ptr<TerrainTileCompositionWorker::SourceSnapshot> solid_sources() {
	auto snapshot = std::make_shared<TerrainTileCompositionWorker::SourceSnapshot>();
	snapshot->colormap = std::make_shared<const opennova::terrain::TerrainTileQuadrantSource>(
			opennova::terrain::build_terrain_tile_quadrant_source(solid_image(2, 2, { 0, 0, 0, 255 })));
	snapshot->heightfield_normal = std::make_shared<const opennova::terrain::TerrainTileQuadrantSource>(
			opennova::terrain::build_terrain_tile_quadrant_source(solid_image(2, 2, { 128, 128, 255, 128 })));
	return snapshot;
}

// A one-triangle opaque caster standing at world (x, 4, z): front-facing under the ordinary
// CULLMODE CCW once projected (the planner test's box).
opennova::terrain::TerrainStaticShadowPlannerCaster box_caster(float x, float z) {
	auto geometry = std::make_shared<opennova::terrain::TerrainStaticShadowResolvedGeometry>();
	geometry->key = 0x1234;
	geometry->render_object_counts = { 1, 0 };
	geometry->materials.push_back(opennova::terrain::TerrainStaticShadowResolvedMaterial{});
	opennova::terrain::TerrainStaticShadowResolvedSurface surface;
	surface.render_object = 0;
	surface.material_index = 0;
	surface.vertices = { { 0.0f, 0.0f, 0.0f }, { 6.0f, 0.0f, 0.0f }, { 0.0f, 6.0f, 0.0f } };
	surface.uvs = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f } };
	surface.indices = { 0, 2, 1 };
	geometry->surfaces[0].push_back(std::move(surface));
	opennova::terrain::TerrainStaticShadowRenderObjectCoverage coverage;
	coverage.authored_surface_count = 1;
	coverage.valid_surface_count = 1;
	geometry->coverage[0].push_back(coverage);
	geometry->model_radius_fixed = 6 << 16;
	opennova::terrain::TerrainStaticShadowPlannerCaster record;
	record.bms_id = 42;
	record.entity_kind = 2; // building
	record.active = true;
	record.graphic = "Box";
	record.world_transform = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, x, 4.0f, z };
	record.origin_q16 = { static_cast<int32_t>(x * 65536.0f), static_cast<int32_t>(-z * 65536.0f) };
	record.geometry = geometry;
	record.caster_identity = 7;
	return record;
}

// One page composed on a worker of its own with the given budget.
Rgba8Image compose_one(TerrainTileCompositionWorker::Threads threads) {
	TerrainTileCompositionWorker worker(threads);
	opennova::TerrainTileCompositionCache cache;
	cache.begin_frame(0);
	cache.begin_frame(0);
	worker.install_sources(solid_sources());
	CHECK(worker.worker_count() == std::max<std::size_t>(threads.page_workers, 1));
	// One page's rasters take the whole lane pool plus their page worker, not
	// the page-worker count: the TOD refresh composes a single page a frame,
	// and the frame waits on it (LF-s1).
	CHECK(worker.raster_lanes() == threads.lane_threads + 1);
	const opennova::terrain::TerrainTileLightEpoch light{ 128, 128, 255 };
	CHECK(worker.enqueue(job_for(cache, 10, 1), worker.sources(), {}, light, {}, 1, nullptr, 0,
			false));
	worker.wait_idle();
	std::optional<TerrainTileCompositionWorker::Completion> done = worker.take_completion();
	CHECK(done.has_value() && done->success);
	return done.has_value() ? done->pixels : Rgba8Image{};
}

} // namespace

int main() {
	// Before any other worker, so the lane pool this one starts has no threads.
	const Rgba8Image small_budget = compose_one(TerrainTileCompositionWorker::Threads{ 1, 0 });
	CHECK(small_budget.is_valid());
	CHECK(compose_one(TerrainTileCompositionWorker::Threads::for_hardware()).pixels ==
			small_budget.pixels);

	TerrainTileCompositionWorker worker;
	opennova::TerrainTileCompositionCache cache;
	// A record must be unused for more than one frame before it is claimed.
	cache.begin_frame(0);
	cache.begin_frame(0);
	const std::array<float, 3> tint{};
	const opennova::terrain::TerrainTileLightEpoch light{ 128, 128, 255 };

	// No sources installed: nothing enqueues.
	CHECK(worker.sources() == nullptr);
	CHECK(!worker.enqueue(job_for(cache, 0, 1), worker.sources(), tint, light, {}, 1, nullptr, 0, false));
	CHECK(worker.pending_jobs() == 0);

	auto snapshot = solid_sources();
	worker.install_sources(snapshot);
	CHECK(worker.sources() == snapshot);

	// One job composes one complete page (a page the cache has not issued yet:
	// a second request for sector 0 would be a hit, not a job).
	const opennova::TerrainTileCompositionJob first = job_for(cache, 10, 1);
	CHECK(worker.enqueue(first, worker.sources(), tint, light, {}, 7, nullptr, 0, false));
	CHECK(settle(worker, 1));
	std::optional<TerrainTileCompositionWorker::Completion> done = worker.take_completion();
	CHECK(done.has_value());
	if (done.has_value()) {
		CHECK(done->success && done->pixels.is_valid() && done->pixels.width == 256 &&
				done->pixels.height == 256);
		CHECK(done->job.target.layer == first.target.layer && done->demand_frame == 7 &&
				done->compose_us >= 1 && !done->shadow_attempted);
	}
	CHECK(!worker.take_completion().has_value());

	// A rejected completion stays queued; the allowed one behind it drains.
	const opennova::TerrainTileCompositionJob a = job_for(cache, 1, 1);
	const opennova::TerrainTileCompositionJob b = job_for(cache, 2, 1);
	CHECK(a.target.layer != b.target.layer);
	CHECK(worker.enqueue(a, worker.sources(), tint, light, {}, 8, nullptr, 0, false));
	CHECK(worker.enqueue(b, worker.sources(), tint, light, {}, 8, nullptr, 0, false));
	worker.wait_idle();
	CHECK(worker.current_epoch_active_jobs() == 0 && worker.completed_jobs() == 2);
	done = worker.take_completion_if([&](const TerrainTileCompositionWorker::Completion &c) {
		return c.job.target.layer == b.target.layer;
	});
	CHECK(done.has_value() && done->job.target.layer == b.target.layer);
	CHECK(worker.pending_jobs() == 1);
	done = worker.take_completion();
	CHECK(done.has_value() && done->job.target.layer == a.target.layer);
	CHECK(worker.pending_jobs() == 0);

	// The device form of a shadowed page: the worker composes the page and walks its casters
	// into a device job, leaving the page as composed (its A the DOT3 light alpha the temp
	// target starts from); that job drawn into the page (here on the CPU, its oracle) and
	// applied gives the CPU raster's bytes.
	// [orig: Terrain_CollectAndRenderTileModels @0x60D250 - Render_SubmitEntity (the call
	//  @0x60D971), CRenderBatchQueue_SortAndFlush (the call @0x60D97D); the temp-blue
	//  composite PolyTrn_RenderTile @0x60E0C6..0x60E19D]
	{
		opennova::TerrainTilePageKey shadow_page_key{};
		shadow_page_key.sector_origin_x = 128;
		shadow_page_key.page_lod_level = 4; // a 64-unit span
		auto shadow = std::make_shared<opennova::terrain::TerrainStaticShadowCompilationSnapshot>();
		shadow->revision = 1;
		shadow->planner.set_light({ 0.3f, 0.9f, 0.3f }, { 127, 200, 200 });
		shadow->planner.replace_casters({ box_caster(136.0f, 8.0f) }, false);
		CHECK(shadow->planner.plan(shadow_page_key).raster_required);
		std::array<std::optional<TerrainTileCompositionWorker::Completion>, 2> modes;
		for (std::size_t mode = 0; mode < modes.size(); ++mode) {
			TerrainTileCompositionWorker shadow_worker(TerrainTileCompositionWorker::Threads{ 1, 0 });
			opennova::TerrainTileCompositionCache shadow_cache;
			shadow_cache.begin_frame(0);
			shadow_cache.begin_frame(0);
			shadow_worker.install_sources(solid_sources());
			const opennova::TerrainTileCompositionJob shadow_job = job_for(shadow_cache, 128, 4);
			CHECK(shadow_worker.enqueue(shadow_job, shadow_worker.sources(), tint, light, {}, 11,
					shadow, 0, false, /*device_shadow=*/mode == 1));
			shadow_worker.wait_idle();
			modes[mode] = shadow_worker.take_completion();
		}
		CHECK(modes[0].has_value() && modes[0]->success && modes[0]->shadow_attempted &&
				modes[0]->device_shadow == nullptr);
		CHECK(modes[1].has_value() && modes[1]->success && modes[1]->shadow_attempted &&
				modes[1]->device_shadow != nullptr);
		if (modes[0].has_value() && modes[1].has_value() && modes[1]->device_shadow != nullptr) {
			const opennova::terrain::TerrainStaticShadowDeviceJob &device_job =
					*modes[1]->device_shadow;
			CHECK(device_job.draws.size() == 1 && device_job.triangle_count == 1);
			CHECK(modes[1]->pixels.pixels != modes[0]->pixels.pixels);
			opennova::terrain::Rgba8Image device_pixels = modes[1]->pixels;
			opennova::terrain::TerrainStaticShadowAlphaPage alpha_page =
					opennova::terrain::begin_terrain_static_shadow_alpha_page(
							modes[1]->job, device_pixels, {});
			CHECK(opennova::terrain::rasterize_terrain_static_shadow_device_job(
					device_job, alpha_page, 1));
			CHECK(opennova::terrain::apply_terrain_static_shadow_alpha_page(
					modes[1]->job, alpha_page, device_pixels));
			CHECK(device_pixels.pixels == modes[0]->pixels.pixels);
		}
	}

	// An epoch bump drops the queue (and the completions of in-flight work).
	CHECK(worker.enqueue(job_for(cache, 3, 1), worker.sources(), tint, light, {}, 9, nullptr, 0, false));
	worker.cancel(false);
	CHECK(settle(worker, 0));
	CHECK(!worker.take_completion().has_value());
	CHECK(worker.sources() == snapshot);
	worker.cancel(true);
	CHECK(worker.sources() == nullptr);

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("terrain_tile_composition_worker_test OK\n");
	return 0;
}
