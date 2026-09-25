#pragma once

// THE TERRAIN TILE-COMPOSITION WORKER: the CPU half of the 128-page tile
// cache — the immutable source snapshot pages compose from, the demand queue
// the worker threads drain (the newest generation per layer wins, an epoch
// bump cancels everything queued), the page composition itself
// (compose_terrain_tile_page plus the static-shadow alpha pass the requesting
// frame's material time drives) and the completion queue the embedder drains
// once wait_idle() returns, before the frame's terrain draw (retail composes
// every missing visible page inside PolyTrn_RenderFrame). The embedder owns
// the texture upload and the generation validation (the composition cache);
// nothing here touches a rendering API. Infrastructure, not a port: the
// witnessed cache semantics live in terrain_tile_composition_cache.h and the
// pixel rules in terrain_tile_composer.h.

#include <runtime/terrain/terrain_scorch.h>
#include <runtime/terrain/terrain_static_shadow_alpha.h>
#include <runtime/terrain/terrain_static_shadow_planner.h>
#include <runtime/terrain/terrain_tile_composer.h>
#include <runtime/terrain/terrain_tile_composition_cache.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace opennova::terrain {

// Main-thread-owned provider state published once per semantic shadow epoch
// (caster set, light quantum, config — never material time, which rides each
// work item). Worker threads clone only the planner, whose immutable caster
// set is shared by pointer.
struct TerrainStaticShadowCompilationSnapshot {
	uint64_t revision = 0;
	TerrainStaticShadowPlanner planner;
};

class TerrainTileCompositionWorker {
public:
	// Pages compose on this pool while the terrain frame waits for them
	// (retail composes each missing visible page inside the frame that draws
	// it), so a frame that claims several pages spends its wait on the
	// slowest worker: half the hardware threads, 2..8.
	static std::size_t worker_count() noexcept;
	static constexpr std::size_t kMaximumQueuedJobs = TerrainTileCompositionCache::kCapacity * 2;

	// The immutable page sources one mission's cache composes from, already
	// split into the retail quadrant textures and level sets.
	struct SourceSnapshot {
		TerrainTileQuadrantSource colormap;
		TerrainTileQuadrantSource heightfield_normal;
		std::vector<Rgba8Image> tilestrip;
		TilFile tile_info;
		bool tile_overlay_ready = false;
		std::array<TerrainScorchTexture, kTerrainScorchTextureSlots> scorch_textures;

		TerrainTilePageSourceView view(const std::array<float, 3> &tint,
				const TerrainTileLightEpoch &light, const TerrainScorchPagePlan *scorch) const {
			TerrainTilePageSourceView result;
			result.colormap = &colormap;
			result.heightfield_normal = &heightfield_normal;
			if (tile_overlay_ready) {
				result.tile_info = &tile_info;
				result.tilestrip = &tilestrip;
			}
			result.tile_overlay_tint = tint;
			result.light_bytes = light;
			result.scorch_plan = scorch;
			result.scorch_textures = &scorch_textures;
			return result;
		}
	};

	struct Completion {
		uint64_t epoch = 0;
		uint64_t demand_frame = 0;
		uint64_t demand_sequence = 0;
		TerrainTileCompositionJob job;
		Rgba8Image pixels;
		uint64_t compose_us = 0;
		// The split of compose_us: the page raster, then the static-shadow
		// plan (the shadow raster and its alpha apply are the remainder).
		uint64_t page_us = 0;
		uint64_t shadow_plan_us = 0;
		bool success = false;
		bool shadow_attempted = false;
		uint64_t shadow_alpha_changed_bytes = 0;
		uint64_t shadow_rgb_changed_bytes = 0;
		uint64_t shadow_base_nonzero_alpha_bytes = 0;
		TerrainStaticShadowPlannerDiagnostics shadow_diagnostics;
	};

	TerrainTileCompositionWorker();
	~TerrainTileCompositionWorker();
	TerrainTileCompositionWorker(const TerrainTileCompositionWorker &) = delete;
	TerrainTileCompositionWorker &operator=(const TerrainTileCompositionWorker &) = delete;

	void install_sources(std::shared_ptr<const SourceSnapshot> sources);
	std::shared_ptr<const SourceSnapshot> sources() const;
	// Drops every queued and completed job (an epoch bump: an in-flight job's
	// completion is discarded when it lands); `clear_sources` also forgets the
	// snapshot.
	void cancel(bool clear_sources);
	// False when no sources are installed, the worker is stopping, or the
	// demand policy rejects the job (the queue is full of newer work).
	bool enqueue(const TerrainTileCompositionJob &job,
			const std::shared_ptr<const SourceSnapshot> &sources, const std::array<float, 3> &tint,
			const TerrainTileLightEpoch &light, TerrainScorchPagePlan scorch, uint64_t demand_frame,
			const std::shared_ptr<const TerrainStaticShadowCompilationSnapshot> &shadow,
			uint32_t shadow_material_time_ms, bool capture_diagnostics);

	std::optional<Completion> take_completion();
	// Blocks until every job of the current epoch has completed (or been
	// dropped by a cancel); the completions then wait in the queue.
	void wait_idle();

	// take_completion, but a completion the predicate rejects stays queued
	// (with its policy ordering) for a later frame's budget instead of being
	// consumed. Rejected layers do not block allowed ones behind them.
	template <typename Allow>
	std::optional<Completion> take_completion_if(Allow &&allow) {
		std::lock_guard<std::mutex> lock(mutex_);
		std::vector<TerrainTileCompositionDemand> deferred;
		std::optional<Completion> result;
		while (!completion_queue_.empty()) {
			const auto demand = completion_queue_.take_next();
			if (!demand.has_value()) break;
			const auto payload = std::find_if(completions_.begin(), completions_.end(),
					[&](const Completion &queued) { return queued.demand_sequence == demand->sequence; });
			if (payload == completions_.end()) continue;
			if (!allow(*payload)) {
				deferred.push_back(*demand);
				continue;
			}
			result = std::move(*payload);
			completions_.erase(payload);
			break;
		}
		for (const TerrainTileCompositionDemand &demand : deferred)
			completion_queue_.enqueue(demand, kMaximumQueuedJobs);
		return result;
	}

	// Queued + completed + the current epoch's in-flight jobs.
	std::size_t pending_jobs() const;
	std::size_t current_epoch_active_jobs() const;
	// Completed and not yet taken.
	std::size_t completed_jobs() const;

private:
	struct WorkItem {
		uint64_t epoch = 0;
		uint64_t demand_frame = 0;
		uint64_t demand_sequence = 0;
		TerrainTileCompositionJob job;
		std::shared_ptr<const SourceSnapshot> sources;
		std::array<float, 3> tint{};
		TerrainTileLightEpoch light{};
		TerrainScorchPagePlan scorch;
		std::shared_ptr<const TerrainStaticShadowCompilationSnapshot> shadow;
		// The requesting frame's Render_ShaderTickMs: the shared snapshot
		// never carries time, the job does.
		uint32_t shadow_material_time_ms = 0;
		bool capture_diagnostics = false;
	};

	void worker_loop();

	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::condition_variable idle_;
	std::deque<WorkItem> work_;
	TerrainTileCompositionDemandQueue demand_queue_;
	std::deque<Completion> completions_;
	TerrainTileCompositionDemandQueue completion_queue_;
	std::vector<std::thread> workers_;
	std::shared_ptr<const SourceSnapshot> current_sources_;
	std::size_t active_jobs_ = 0;
	std::unordered_map<uint64_t, std::size_t> active_jobs_by_epoch_;
	uint64_t next_demand_sequence_ = 1;
	uint64_t epoch_ = 1;
	bool stopping_ = false;
};

} // namespace opennova::terrain
