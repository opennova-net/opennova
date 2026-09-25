// The terrain tile-composition worker — see terrain_tile_composition_worker.h.

#include <runtime/terrain/terrain_tile_composition_worker.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace opennova::terrain {

// --- the shared row-stripe lane pool (runtime/terrain/row_stripes.h) --------

namespace {

// One run_lanes_on_pool call: every participant claims lane indices until
// none remain. Lives on the caller's stack; the caller returns only after
// every pool thread that picked it up has let go.
struct LaneBatch {
	void (*invoke)(const void *, std::size_t) = nullptr;
	const void *context = nullptr;
	std::size_t lanes = 0;
	std::atomic<std::size_t> next{ 0 };
	// Pool threads inside this batch (guarded by the pool mutex).
	std::size_t helpers = 0;
	std::condition_variable released;
};

void claim_lanes(LaneBatch &batch) noexcept {
	for (;;) {
		const std::size_t index = batch.next.fetch_add(1, std::memory_order_relaxed);
		if (index >= batch.lanes) return;
		batch.invoke(batch.context, index);
	}
}

class LanePool {
public:
	LanePool() {
		const std::size_t hardware = std::max<unsigned>(std::thread::hardware_concurrency(), 2u);
		try {
			threads_.reserve(hardware - 1);
			for (std::size_t index = 0; index + 1 < hardware; ++index)
				threads_.emplace_back([this]() { worker_loop(); });
		} catch (...) {
		}
	}

	~LanePool() {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stopping_ = true;
		}
		wake_.notify_all();
		for (std::thread &thread : threads_) thread.join();
	}

	LanePool(const LanePool &) = delete;
	LanePool &operator=(const LanePool &) = delete;

	void run(std::size_t lanes, void (*invoke)(const void *, std::size_t),
			const void *context) noexcept {
		LaneBatch batch;
		batch.invoke = invoke;
		batch.context = context;
		batch.lanes = lanes;
		const std::size_t wanted = std::min(lanes - 1, threads_.size());
		std::size_t queued = 0;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			try {
				for (; queued < wanted; ++queued) queue_.push_back(&batch);
			} catch (...) {
			}
		}
		for (std::size_t index = 0; index < queued; ++index) wake_.notify_one();
		claim_lanes(batch);
		std::unique_lock<std::mutex> lock(mutex_);
		// Entries no thread reached are withdrawn; every lane is claimed.
		queue_.erase(std::remove(queue_.begin(), queue_.end(), &batch), queue_.end());
		batch.released.wait(lock, [&batch]() { return batch.helpers == 0; });
	}

private:
	void worker_loop() noexcept {
		std::unique_lock<std::mutex> lock(mutex_);
		for (;;) {
			wake_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
			if (stopping_) return;
			LaneBatch *batch = queue_.front();
			queue_.pop_front();
			++batch->helpers;
			lock.unlock();
			claim_lanes(*batch);
			lock.lock();
			if (--batch->helpers == 0) batch->released.notify_all();
		}
	}

	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<LaneBatch *> queue_;
	bool stopping_ = false;
	std::vector<std::thread> threads_;
};

// The pool lives while anything holds a lease (the page workers hold one for
// their lifetime); a call with no holder builds and retires a pool around
// itself. No static owns threads, so unloading the library never has to stop
// one.
std::mutex g_pool_mutex;
std::weak_ptr<LanePool> g_pool;

std::shared_ptr<LanePool> acquire_pool() {
	std::lock_guard<std::mutex> lock(g_pool_mutex);
	std::shared_ptr<LanePool> pool = g_pool.lock();
	if (pool == nullptr) {
		pool = std::make_shared<LanePool>();
		g_pool = pool;
	}
	return pool;
}

} // namespace

RowStripePoolLease retain_row_stripe_pool() {
	try {
		return acquire_pool();
	} catch (...) {
		return nullptr;
	}
}

namespace detail {

void run_lanes_on_pool(std::size_t lanes, void (*invoke)(const void *, std::size_t),
		const void *context) noexcept {
	std::shared_ptr<LanePool> pool;
	try {
		pool = acquire_pool();
	} catch (...) {
	}
	if (pool == nullptr) {
		for (std::size_t index = 0; index < lanes; ++index) invoke(context, index);
		return;
	}
	pool->run(lanes, invoke, context);
}

} // namespace detail

// --- the page-composition worker --------------------------------------------

std::size_t TerrainTileCompositionWorker::worker_count() noexcept {
	const std::size_t hardware = std::thread::hardware_concurrency();
	return std::clamp<std::size_t>(hardware / 2, 2, 8);
}

TerrainTileCompositionWorker::TerrainTileCompositionWorker()
		: lane_pool_(retain_row_stripe_pool()) {
	const std::size_t count = worker_count();
	workers_.reserve(count);
	for (std::size_t index = 0; index < count; ++index)
		workers_.emplace_back([this]() { worker_loop(); });
}

TerrainTileCompositionWorker::~TerrainTileCompositionWorker() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
		work_.clear();
		demand_queue_.clear();
		completions_.clear();
		completion_queue_.clear();
		current_sources_.reset();
	}
	wake_.notify_all();
	idle_.notify_all();
	for (std::thread &worker : workers_)
		if (worker.joinable()) worker.join();
}

void TerrainTileCompositionWorker::install_sources(std::shared_ptr<const SourceSnapshot> sources) {
	std::lock_guard<std::mutex> lock(mutex_);
	current_sources_ = std::move(sources);
}

std::shared_ptr<const TerrainTileCompositionWorker::SourceSnapshot>
TerrainTileCompositionWorker::sources() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return current_sources_;
}

void TerrainTileCompositionWorker::cancel(bool clear_sources) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		++epoch_;
		work_.clear();
		demand_queue_.clear();
		completions_.clear();
		completion_queue_.clear();
		if (clear_sources) current_sources_.reset();
	}
	idle_.notify_all();
}

bool TerrainTileCompositionWorker::enqueue(const TerrainTileCompositionJob &job,
		const std::shared_ptr<const SourceSnapshot> &sources, const std::array<float, 3> &tint,
		const TerrainTileLightEpoch &light, TerrainScorchPagePlan scorch, uint64_t demand_frame,
		const std::shared_ptr<const TerrainStaticShadowCompilationSnapshot> &shadow,
		uint32_t shadow_material_time_ms, bool capture_diagnostics) {
	if (sources == nullptr) return false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_) return false;
		const uint64_t sequence = next_demand_sequence_++;
		const std::vector<uint64_t> removed_completions =
				completion_queue_.remove_older_generations(job.target.layer, job.target.generation);
		for (const uint64_t removed : removed_completions) {
			const auto payload = std::find_if(completions_.begin(), completions_.end(),
					[&](const Completion &queued) { return queued.demand_sequence == removed; });
			if (payload != completions_.end()) completions_.erase(payload);
		}
		const std::size_t nonqueued = completions_.size() + active_jobs_;
		const std::size_t maximum_work =
				nonqueued < kMaximumQueuedJobs ? kMaximumQueuedJobs - nonqueued : 0;
		const auto scheduled = demand_queue_.enqueue(
				{ job.target.layer, job.target.generation, demand_frame, sequence }, maximum_work);
		for (const uint64_t removed : scheduled.removed_sequences) {
			const auto payload = std::find_if(work_.begin(), work_.end(),
					[&](const WorkItem &queued) { return queued.demand_sequence == removed; });
			if (payload != work_.end()) work_.erase(payload);
		}
		if (!scheduled.accepted) return false;
		work_.push_back(WorkItem{ epoch_, demand_frame, sequence, job, sources, tint, light,
				std::move(scorch), shadow, shadow_material_time_ms, capture_diagnostics });
	}
	wake_.notify_one();
	return true;
}

std::optional<TerrainTileCompositionWorker::Completion> TerrainTileCompositionWorker::take_completion() {
	return take_completion_if([](const Completion &) noexcept { return true; });
}

void TerrainTileCompositionWorker::wait_idle() {
	std::unique_lock<std::mutex> lock(mutex_);
	idle_.wait(lock, [this]() {
		const auto active = active_jobs_by_epoch_.find(epoch_);
		return stopping_ || (demand_queue_.empty() &&
				(active == active_jobs_by_epoch_.end() || active->second == 0));
	});
}

std::size_t TerrainTileCompositionWorker::pending_jobs() const {
	std::lock_guard<std::mutex> lock(mutex_);
	const auto active = active_jobs_by_epoch_.find(epoch_);
	return work_.size() + completions_.size() +
			(active == active_jobs_by_epoch_.end() ? 0 : active->second);
}

std::size_t TerrainTileCompositionWorker::current_epoch_active_jobs() const {
	std::lock_guard<std::mutex> lock(mutex_);
	const auto active = active_jobs_by_epoch_.find(epoch_);
	return active == active_jobs_by_epoch_.end() ? 0 : active->second;
}

std::size_t TerrainTileCompositionWorker::completed_jobs() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return completions_.size();
}

void TerrainTileCompositionWorker::worker_loop() {
	std::shared_ptr<const TerrainStaticShadowCompilationSnapshot> active_shadow;
	TerrainStaticShadowPlanner shadow_planner;
	for (;;) {
		WorkItem item;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this]() { return stopping_ || !demand_queue_.empty(); });
			if (stopping_ && demand_queue_.empty()) return;
			const auto demand = demand_queue_.take_next();
			if (!demand.has_value()) continue;
			const auto payload = std::find_if(work_.begin(), work_.end(),
					[&](const WorkItem &queued) { return queued.demand_sequence == demand->sequence; });
			if (payload == work_.end()) continue;
			item = std::move(*payload);
			work_.erase(payload);
			++active_jobs_;
			++active_jobs_by_epoch_[item.epoch];
		}

		Completion completion;
		completion.epoch = item.epoch;
		completion.demand_frame = item.demand_frame;
		completion.demand_sequence = item.demand_sequence;
		completion.job = item.job;
		const auto started = std::chrono::steady_clock::now();
		try {
			// A page the registry cannot route carries no overlay: the base
			// page still composes (the composer fails closed only on a
			// DECLARED plan it cannot draw).
			const TerrainTilePageSourceView view =
					item.sources->view(item.tint, item.light, item.scorch.valid ? &item.scorch : nullptr);
			completion.pixels = compose_terrain_tile_page(item.job, view, worker_count());
			completion.success = completion.pixels.is_valid();
			const auto page_done = std::chrono::steady_clock::now();
			completion.page_us = static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::microseconds>(page_done - started).count());
			if (completion.success && item.shadow != nullptr) {
				completion.shadow_attempted = true;
				if (active_shadow != item.shadow) {
					// Cheap: the planner shares its immutable caster set by
					// pointer and copies only the per-page memo caches.
					shadow_planner = item.shadow->planner;
					active_shadow = item.shadow;
					// The frame waits on the slowest page, so one page's
					// shadow pixel loop may spread over the pool's width.
					shadow_planner.set_raster_threads(worker_count());
				}
				// Material animation samples the requesting frame's tick, as
				// retail's tile render does for each model it submits.
				shadow_planner.set_material_time(item.shadow_material_time_ms);
				shadow_planner.reset_frame_diagnostics();
				const TerrainStaticShadowPagePlanResult shadow_plan =
						shadow_planner.plan(item.job.target.page);
				const auto plan_done = std::chrono::steady_clock::now();
				completion.shadow_plan_us = static_cast<uint64_t>(
						std::chrono::duration_cast<std::chrono::microseconds>(plan_done - page_done).count());
				std::vector<uint8_t> composed_before_shadow;
				if (item.capture_diagnostics) composed_before_shadow = completion.pixels.pixels;
				if (!shadow_plan.valid || !shadow_plan.raster_required) {
					completion.success = false;
				} else {
					TerrainStaticShadowAlphaPage shadow_page = begin_terrain_static_shadow_alpha_page(
							item.job, completion.pixels, shadow_plan.content);
					completion.success = shadow_page.is_valid() &&
							shadow_planner.rasterize(item.job.target.page, shadow_page) &&
							apply_terrain_static_shadow_alpha_page(item.job, shadow_page, completion.pixels);
				}
				completion.shadow_diagnostics = shadow_planner.diagnostics();
				if (completion.success && item.capture_diagnostics) {
					for (std::size_t byte = 0; byte < completion.pixels.pixels.size(); ++byte) {
						if ((byte & 3u) == 3u) {
							if (composed_before_shadow[byte] != 0) ++completion.shadow_base_nonzero_alpha_bytes;
							if (completion.pixels.pixels[byte] != composed_before_shadow[byte])
								++completion.shadow_alpha_changed_bytes;
						} else if (completion.pixels.pixels[byte] != composed_before_shadow[byte]) {
							++completion.shadow_rgb_changed_bytes;
						}
					}
				}
			}
		} catch (...) {
			completion.success = false;
		}
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - started)
									 .count();
		completion.compose_us = static_cast<uint64_t>(std::max<int64_t>(elapsed, 1));

		{
			std::lock_guard<std::mutex> lock(mutex_);
			--active_jobs_;
			auto active = active_jobs_by_epoch_.find(item.epoch);
			if (active != active_jobs_by_epoch_.end() && --active->second == 0)
				active_jobs_by_epoch_.erase(active);
			if (!stopping_ && item.epoch == epoch_) {
				const auto scheduled = completion_queue_.enqueue(
						{ item.job.target.layer, item.job.target.generation, item.demand_frame,
								item.demand_sequence },
						kMaximumQueuedJobs);
				for (const uint64_t removed : scheduled.removed_sequences) {
					const auto payload = std::find_if(completions_.begin(), completions_.end(),
							[&](const Completion &queued) { return queued.demand_sequence == removed; });
					if (payload != completions_.end()) completions_.erase(payload);
				}
				if (scheduled.accepted) completions_.push_back(std::move(completion));
			}
		}
		idle_.notify_all();
	}
}

} // namespace opennova::terrain
