// The terrain tile-composition worker (runtime/terrain/terrain_tile_composition_worker.h):
// no sources = nothing enqueues; a queued job composes one complete 256x256
// page off the installed snapshot; a completion the frame's budget rejects
// stays queued while an allowed one behind it drains; an epoch bump drops
// everything queued.
#include <runtime/terrain/terrain_tile_composition_worker.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
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
			page, 0, 0, 0, opennova::TerrainTileContentStamp{ 1 } });
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

} // namespace

int main() {
	TerrainTileCompositionWorker worker;
	opennova::TerrainTileCompositionCache cache;
	const std::array<float, 3> tint{};
	const opennova::terrain::TerrainTileLightEpoch light{ 128, 128, 255 };

	// No sources installed: nothing enqueues.
	CHECK(worker.sources() == nullptr);
	CHECK(!worker.enqueue(job_for(cache, 0, 1), worker.sources(), tint, light, {}, 1, nullptr, 0, false));
	CHECK(worker.pending_jobs() == 0);

	auto snapshot = std::make_shared<TerrainTileCompositionWorker::SourceSnapshot>();
	snapshot->colormap = solid_image(2, 2, { 0, 0, 0, 255 });
	snapshot->heightfield_normal = solid_image(2, 2, { 128, 128, 255, 128 });
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
	CHECK(settle(worker, 2));
	done = worker.take_completion_if([&](const TerrainTileCompositionWorker::Completion &c) {
		return c.job.target.layer == b.target.layer;
	});
	CHECK(done.has_value() && done->job.target.layer == b.target.layer);
	CHECK(worker.pending_jobs() == 1);
	done = worker.take_completion();
	CHECK(done.has_value() && done->job.target.layer == a.target.layer);
	CHECK(worker.pending_jobs() == 0);

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
