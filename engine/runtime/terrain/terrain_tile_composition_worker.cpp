// The terrain tile-composition worker — see terrain_tile_composition_worker.h.

#include <runtime/terrain/terrain_tile_composition_worker.h>

#include <chrono>

namespace opennova::terrain {

TerrainTileCompositionWorker::TerrainTileCompositionWorker() {
	for (std::size_t index = 0; index < kWorkerCount; ++index)
		workers_[index] = std::thread([this]() { worker_loop(); });
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
	std::lock_guard<std::mutex> lock(mutex_);
	++epoch_;
	work_.clear();
	demand_queue_.clear();
	completions_.clear();
	completion_queue_.clear();
	if (clear_sources) current_sources_.reset();
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
			completion.pixels = compose_terrain_tile_page(item.job, view);
			completion.success = completion.pixels.is_valid();
			if (completion.success && item.shadow != nullptr) {
				completion.shadow_attempted = true;
				if (active_shadow != item.shadow) {
					// Cheap: the planner shares its immutable caster set by
					// pointer and copies only the per-page memo caches.
					shadow_planner = item.shadow->planner;
					active_shadow = item.shadow;
				}
				// Material animation samples the requesting frame's tick, as
				// retail's tile render does for each model it submits.
				shadow_planner.set_material_time(item.shadow_material_time_ms);
				shadow_planner.reset_frame_diagnostics();
				const TerrainStaticShadowPagePlanResult shadow_plan =
						shadow_planner.plan(item.job.target.page);
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
	}
}

} // namespace opennova::terrain
