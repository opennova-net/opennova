// Device leg only: Godot Image/Texture2DArray marshalling, the async worker
// drain, and upload counters. Page identity, LRU, and invalidation — the
// witnessed cache semantics — live in engine/runtime/terrain/
// terrain_tile_composition_cache.{h,cpp}, held here as a member (see the
// class header's witness block and docs/terrain/terrain-re.md; retail's
// device-side twin is the D3D tile-texture pool the record maps).
#include "terrain/terrain_tile_cache_device.h"

#include "terrain/terrain_data.h"
#include "terrain/terrain_surface_inputs.h"
#include "terrain/terrain_tile_info.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace godot {
using opennova::TerrainTileCompositionDemandQueue;

namespace {

bool image_to_rgba8(const Ref<Image> &p_source,
		opennova::terrain::Rgba8Image &r_output) {
	r_output = {};
	if (p_source.is_null() || p_source->is_empty()) {
		return false;
	}
	Ref<Image> image = p_source->duplicate();
	if (image.is_null() ||
			(image->is_compressed() && image->decompress() != OK)) {
		return false;
	}
	if (image->get_format() != Image::FORMAT_RGBA8) {
		image->convert(Image::FORMAT_RGBA8);
	}
	const int width = image->get_width();
	const int height = image->get_height();
	if (width <= 0 || height <= 0) {
		return false;
	}
	const size_t byte_count = static_cast<size_t>(width) * height * 4u;
	const PackedByteArray bytes = image->get_data();
	if (bytes.size() < static_cast<int64_t>(byte_count)) {
		return false;
	}
	r_output.width = static_cast<uint32_t>(width);
	r_output.height = static_cast<uint32_t>(height);
	r_output.pixels.assign(bytes.ptr(), bytes.ptr() + byte_count);
	return true;
}

bool texture_to_rgba8(const Ref<Texture2D> &p_texture,
		opennova::terrain::Rgba8Image &r_output) {
	return p_texture.is_valid() && image_to_rgba8(p_texture->get_image(), r_output);
}

PackedByteArray packed_bytes(const std::vector<uint8_t> &p_bytes) {
	PackedByteArray result;
	result.resize(static_cast<int64_t>(p_bytes.size()));
	if (!p_bytes.empty()) {
		std::memcpy(result.ptrw(), p_bytes.data(), p_bytes.size());
	}
	return result;
}

Ref<Image> image_from_rgba8(const opennova::terrain::Rgba8Image &p_source) {
	if (!p_source.is_valid()) {
		return {};
	}
	return Image::create_from_data(
			static_cast<int32_t>(p_source.width),
			static_cast<int32_t>(p_source.height), false,
			Image::FORMAT_RGBA8, packed_bytes(p_source.pixels));
}

uint8_t quantize_unorm(float p_value) {
	return static_cast<uint8_t>(std::clamp(
			static_cast<int>(std::lround(
					std::clamp(p_value, 0.0f, 1.0f) * 255.0f)), 0, 255));
}

uint64_t mix_byte(uint64_t p_hash, uint8_t p_value) {
	return (p_hash ^ p_value) * UINT64_C(1099511628211);
}

bool same_page_key(const opennova::TerrainTilePageKey &p_left,
		const opennova::TerrainTilePageKey &p_right) {
	return p_left.sector_origin_x == p_right.sector_origin_x &&
			p_left.sector_origin_z == p_right.sector_origin_z &&
			p_left.page_local_x == p_right.page_local_x &&
			p_left.page_local_z == p_right.page_local_z &&
			p_left.page_lod_level == p_right.page_lod_level;
}

template <typename T>
uint64_t mix_value_bytes(uint64_t p_hash, const T &p_value) {
	const auto *bytes = reinterpret_cast<const uint8_t *>(&p_value);
	for (std::size_t index = 0; index < sizeof(T); ++index) {
		p_hash = mix_byte(p_hash, bytes[index]);
	}
	return p_hash;
}

// The page's resident-output identity: the page key plus its pixels. The
// pixel fold consumes eight bytes per step (a 256 KB page per upload, up to
// two uploads a frame, on the main thread) — only relative equality of these
// hashes is ever read (the tile-cache diagnostics).
uint64_t page_output_hash(
		const opennova::TerrainTileCompositionJob &p_job,
		const opennova::terrain::Rgba8Image &p_pixels) {
	uint64_t hash = UINT64_C(1469598103934665603);
	hash = mix_value_bytes(hash, p_job.target.page.sector_origin_x);
	hash = mix_value_bytes(hash, p_job.target.page.sector_origin_z);
	hash = mix_value_bytes(hash, p_job.target.page.page_local_x);
	hash = mix_value_bytes(hash, p_job.target.page.page_local_z);
	hash = mix_value_bytes(hash, p_job.target.page.page_lod_level);
	const uint8_t *bytes = p_pixels.pixels.data();
	const std::size_t size = p_pixels.pixels.size();
	std::size_t index = 0;
	for (; index + 8 <= size; index += 8) {
		uint64_t word = 0;
		std::memcpy(&word, bytes + index, sizeof(word));
		hash = (hash ^ word) * UINT64_C(1099511628211);
		hash ^= hash >> 29;
	}
	for (; index < size; ++index) hash = mix_byte(hash, bytes[index]);
	return hash;
}

} // namespace

struct TerrainTileCacheDevice::AsyncState {
	static constexpr std::size_t kWorkerCount = 2;
	static constexpr std::size_t kMaximumQueuedJobs =
			opennova::TerrainTileCompositionCache::kCapacity * 2;
	static constexpr std::size_t kUploadBudgetPerFrame = 2;

	struct SourceSnapshot {
		opennova::terrain::Rgba8Image colormap;
		opennova::terrain::Rgba8Image heightfield_normal;
		opennova::terrain::Rgba8Image tilestrip;
		opennova::TilFile tile_info;
		bool tile_overlay_ready = false;
		std::array<opennova::terrain::TerrainScorchTexture,
				opennova::terrain::kTerrainScorchTextureSlots>
				scorch_textures;

		opennova::terrain::TerrainTilePageSourceView view(
				const std::array<float, 3> &tint,
				const opennova::terrain::TerrainTileLightEpoch &light,
				const opennova::terrain::TerrainScorchPagePlan *scorch) const {
			opennova::terrain::TerrainTilePageSourceView result;
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

	struct WorkItem {
		uint64_t epoch = 0;
		uint64_t demand_frame = 0;
		uint64_t demand_sequence = 0;
		opennova::TerrainTileCompositionJob job;
		std::shared_ptr<const SourceSnapshot> sources;
		std::array<float, 3> tint{};
		opennova::terrain::TerrainTileLightEpoch light{};
		opennova::terrain::TerrainScorchPagePlan scorch;
		bool capture_diagnostics = false;
	};

	struct Completion {
		uint64_t epoch = 0;
		uint64_t demand_frame = 0;
		uint64_t demand_sequence = 0;
		opennova::TerrainTileCompositionJob job;
		opennova::terrain::Rgba8Image pixels;
		uint64_t compose_us = 0;
		bool success = false;
	};

	AsyncState() {
		for (std::size_t index = 0; index < kWorkerCount; ++index) {
			workers[index] = std::thread([this]() { worker_loop(); });
		}
	}

	~AsyncState() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			work.clear();
			demand_queue.clear();
			completions.clear();
			completion_queue.clear();
			current_sources.reset();
		}
		wake.notify_all();
		for (std::thread &worker : workers) {
			if (worker.joinable()) worker.join();
		}
	}

	void install_sources(std::shared_ptr<const SourceSnapshot> sources) {
		std::lock_guard<std::mutex> lock(mutex);
		current_sources = std::move(sources);
	}

	std::shared_ptr<const SourceSnapshot> sources() const {
		std::lock_guard<std::mutex> lock(mutex);
		return current_sources;
	}

	void cancel(bool clear_sources) {
		std::lock_guard<std::mutex> lock(mutex);
		++epoch;
		work.clear();
		demand_queue.clear();
		completions.clear();
		completion_queue.clear();
		if (clear_sources) current_sources.reset();
	}

	bool enqueue(const opennova::TerrainTileCompositionJob &job,
			const std::shared_ptr<const SourceSnapshot> &sources,
			const std::array<float, 3> &tint,
			const opennova::terrain::TerrainTileLightEpoch &light,
			opennova::terrain::TerrainScorchPagePlan scorch,
			uint64_t demand_frame,
			bool capture_diagnostics) {
		if (sources == nullptr) return false;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stopping) return false;
			const uint64_t sequence = next_demand_sequence++;
			const std::vector<uint64_t> removed_completions =
					completion_queue.remove_older_generations(
							job.target.layer, job.target.generation);
			for (const uint64_t removed : removed_completions) {
				const auto payload = std::find_if(completions.begin(),
						completions.end(), [&](const Completion &queued) {
							return queued.demand_sequence == removed;
						});
				if (payload != completions.end()) completions.erase(payload);
			}
			const std::size_t nonqueued = completions.size() + active_jobs;
			const std::size_t maximum_work =
					nonqueued < kMaximumQueuedJobs
					? kMaximumQueuedJobs - nonqueued
					: 0;
			const auto scheduled = demand_queue.enqueue(
					{job.target.layer, job.target.generation,
							demand_frame, sequence}, maximum_work);
			for (const uint64_t removed : scheduled.removed_sequences) {
				const auto payload = std::find_if(work.begin(), work.end(),
						[&](const WorkItem &queued) {
							return queued.demand_sequence == removed;
						});
				if (payload != work.end()) work.erase(payload);
			}
			if (!scheduled.accepted) return false;
			work.push_back(WorkItem{epoch, demand_frame, sequence, job, sources,
					tint, light, std::move(scorch), capture_diagnostics});
		}
		wake.notify_one();
		return true;
	}

	std::optional<Completion> take_completion() {
		return take_completion_if(
				[](const Completion &) noexcept { return true; });
	}

	// take_completion, but a completion the predicate rejects stays queued
	// (with its policy ordering) for a later frame's budget instead of being
	// consumed. Rejected layers do not block allowed ones behind them.
	template <typename Allow>
	std::optional<Completion> take_completion_if(Allow &&allow) {
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<opennova::TerrainTileCompositionDemand> deferred;
		std::optional<Completion> result;
		while (!completion_queue.empty()) {
			const auto demand = completion_queue.take_next();
			if (!demand.has_value()) break;
			const auto payload = std::find_if(completions.begin(),
					completions.end(), [&](const Completion &queued) {
						return queued.demand_sequence == demand->sequence;
					});
			if (payload == completions.end()) continue;
			if (!allow(*payload)) {
				deferred.push_back(*demand);
				continue;
			}
			result = std::move(*payload);
			completions.erase(payload);
			break;
		}
		for (const opennova::TerrainTileCompositionDemand &demand : deferred) {
			completion_queue.enqueue(demand, kMaximumQueuedJobs);
		}
		return result;
	}

	std::size_t pending_jobs() const {
		std::lock_guard<std::mutex> lock(mutex);
		const auto active = active_jobs_by_epoch.find(epoch);
		return work.size() + completions.size() +
				(active == active_jobs_by_epoch.end() ? 0 : active->second);
	}

	std::size_t current_epoch_active_jobs() const {
		std::lock_guard<std::mutex> lock(mutex);
		const auto active = active_jobs_by_epoch.find(epoch);
		return active == active_jobs_by_epoch.end() ? 0 : active->second;
	}

private:
	void worker_loop() {
		for (;;) {
			WorkItem item;
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock,
						[this]() { return stopping || !demand_queue.empty(); });
				if (stopping && demand_queue.empty()) return;
				const auto demand = demand_queue.take_next();
				if (!demand.has_value()) continue;
				const auto payload = std::find_if(work.begin(), work.end(),
						[&](const WorkItem &queued) {
							return queued.demand_sequence == demand->sequence;
						});
				if (payload == work.end()) continue;
				item = std::move(*payload);
				work.erase(payload);
				++active_jobs;
				++active_jobs_by_epoch[item.epoch];
			}

			Completion completion;
			completion.epoch = item.epoch;
			completion.demand_frame = item.demand_frame;
			completion.demand_sequence = item.demand_sequence;
			completion.job = item.job;
			const auto started = std::chrono::steady_clock::now();
			try {
				// A page the registry cannot route carries no overlay: the
				// base page still composes (the composer fails closed only on
				// a DECLARED plan it cannot draw).
				const opennova::terrain::TerrainTilePageSourceView view =
						item.sources->view(item.tint, item.light,
								item.scorch.valid ? &item.scorch : nullptr);
				completion.pixels = opennova::terrain::compose_terrain_tile_page(
						item.job, view);
				completion.success = completion.pixels.is_valid();
			} catch (...) {
				completion.success = false;
			}
			const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - started).count();
			completion.compose_us = static_cast<uint64_t>(
					std::max<int64_t>(elapsed, 1));

			{
				std::lock_guard<std::mutex> lock(mutex);
				--active_jobs;
				auto active = active_jobs_by_epoch.find(item.epoch);
				if (active != active_jobs_by_epoch.end() && --active->second == 0) {
					active_jobs_by_epoch.erase(active);
				}
				if (!stopping && item.epoch == epoch) {
					const auto scheduled = completion_queue.enqueue(
							{item.job.target.layer,
									item.job.target.generation,
									item.demand_frame,
									item.demand_sequence},
							kMaximumQueuedJobs);
					for (const uint64_t removed : scheduled.removed_sequences) {
						const auto payload = std::find_if(completions.begin(),
								completions.end(),
								[&](const Completion &queued) {
									return queued.demand_sequence == removed;
								});
						if (payload != completions.end()) {
							completions.erase(payload);
						}
					}
					if (scheduled.accepted) {
						completions.push_back(std::move(completion));
					}
				}
			}
		}
	}

	mutable std::mutex mutex;
	std::condition_variable wake;
	std::deque<WorkItem> work;
	TerrainTileCompositionDemandQueue demand_queue;
	std::deque<Completion> completions;
	TerrainTileCompositionDemandQueue completion_queue;
	std::array<std::thread, kWorkerCount> workers;
	std::shared_ptr<const SourceSnapshot> current_sources;
	std::size_t active_jobs = 0;
	std::unordered_map<uint64_t, std::size_t> active_jobs_by_epoch;
	uint64_t next_demand_sequence = 1;
	uint64_t epoch = 1;
	bool stopping = false;
};

TerrainTileCacheDevice::TerrainTileCacheDevice() :
		async_(std::make_unique<AsyncState>()) {}

TerrainTileCacheDevice::~TerrainTileCacheDevice() = default;

bool TerrainTileCacheDevice::rebuild(
		const Ref<TerrainData> &p_data,
		const Ref<TerrainSurfaceInputs> &p_surface_inputs,
		const Ref<TerrainTileInfo> &p_tile_info_override,
		bool p_tile_overlay_enabled) {
	clear();
	if (p_data.is_null() || p_surface_inputs.is_null()) {
		return false;
	}
	auto snapshot = std::make_shared<AsyncState::SourceSnapshot>();

	const Ref<Image> live_colormap = p_data->get_colormap_image();
	const bool have_colormap = live_colormap.is_valid() && !live_colormap->is_empty()
			? image_to_rgba8(live_colormap, snapshot->colormap)
			: texture_to_rgba8(p_data->get_colormap(), snapshot->colormap);
	const bool have_normal = texture_to_rgba8(
			p_surface_inputs->get_heightfield_normal_texture(),
			snapshot->heightfield_normal);
	if (!have_colormap || !have_normal) {
		return false;
	}
	// Scorch decals are an OPTIONAL overlay source, not a base page source.
	// A mission whose resource root does not carry the scorch TGAs (loose
	// authoring roots, fixture terrains, tile-free missions) must still get a
	// complete colormap/normal page cache -- terrain paging and the detail
	// foliage that borrows its page binding both depend on it. Failing the
	// rebuild here inverted that dependency and left foliage permanently on
	// its analytic fallback. append_terrain_scorch() already gates every
	// record on scorch_textures_ready_, so an unresolved set simply means "no
	// scorch overlay this mission".
	bool scorch_ready = true;
	for (const uint8_t texture_index :
			{uint8_t{0}, uint8_t{1}, uint8_t{2}, uint8_t{4}}) {
		const std::string_view name =
				opennova::terrain::terrain_scorch_texture_name(texture_index);
		const Ref<Texture2D> texture = p_data->load_source_texture(
				String::utf8(name.data(), static_cast<int>(name.size())));
		opennova::terrain::Rgba8Image base;
		if (!texture_to_rgba8(texture, base)) {
			scorch_ready = false;
			break;
		}
		snapshot->scorch_textures[texture_index] =
				opennova::terrain::build_terrain_scorch_texture(base);
		if (!snapshot->scorch_textures[texture_index].is_valid()) {
			scorch_ready = false;
			break;
		}
	}
	if (!scorch_ready) {
		for (auto &slot : snapshot->scorch_textures) {
			slot = opennova::terrain::TerrainScorchTexture{};
		}
	}
	Ref<TerrainTileInfo> tile_info = p_tile_info_override;
	const bool tile_info_declared = tile_info.is_valid() ||
			!p_data->get_tileinfo_filename().strip_edges().is_empty();
	if (tile_info.is_null()) {
		tile_info = p_data->get_tileinfo_resource();
	}
	if (p_tile_overlay_enabled && tile_info_declared && tile_info.is_null()) {
		// A TRN-declared .til that could not be resolved or parsed is not the
		// same as a mission authored without tile overlays. Publishing only the
		// base sources here would make the missing overlay invisible to capture.
		tile_overlay_required_ = true;
		return false;
	}
	if (p_tile_overlay_enabled && tile_info.is_valid() &&
			tile_info->get_entry_count() > 0) {
		tile_overlay_required_ = true;
		if (!texture_to_rgba8(p_data->get_tilestrip_tex(),
				snapshot->tilestrip)) {
			return false;
		}
		snapshot->tile_info = tile_info->to_native();
		snapshot->tile_overlay_ready = true;
		tile_overlay_ready_ = true;
	}

	source_revision_ = next_source_revision_++;
	sources_ready_ = _allocate_texture();
	if (sources_ready_) {
		async_->install_sources(std::move(snapshot));
		// Only the scorch overlay is gated on its own sources resolving; the
		// base page cache is ready either way.
		scorch_textures_ready_ = scorch_ready;
	}
	return sources_ready_;
}

void TerrainTileCacheDevice::clear() {
	async_->cancel(true);
	cache_.clear();
	texture_.unref();
	tile_overlay_required_ = false;
	tile_overlay_ready_ = false;
	sources_ready_ = false;
	source_revision_ = 0;
	ready_generations_.fill(0);
	ready_page_keys_.fill(opennova::TerrainTilePageKey{});
	ready_page_output_hashes_.fill(0);
	compose_jobs_ = 0;
	cache_hits_ = 0;
	cache_misses_ = 0;
	upload_failures_ = 0;
	scorch_registry_.clear();
	scorch_textures_ready_ = false;
	scorch_records_rejected_ = 0;
	scorch_page_invalidations_ = 0;
	diagnostic_frame_active_ = false;
	diagnostic_frame_id_ = 0;
	frame_requests_ = 0;
	frame_ready_hits_ = 0;
	frame_stale_hits_ = 0;
	frame_selected_ready_pages_ = 0;
	frame_selected_ready_layers_.fill(false);
	frame_compose_jobs_ = 0;
	frame_compose_us_ = 0;
	frame_uploads_ = 0;
	frame_capacity_fallbacks_ = 0;
	frame_output_pages_ = 0;
	frame_output_hash_ = 0;
}

void TerrainTileCacheDevice::begin_frame(uint64_t p_frame_id) {
	if (!diagnostic_frame_active_ || diagnostic_frame_id_ != p_frame_id) {
		diagnostic_frame_active_ = true;
		diagnostic_frame_id_ = p_frame_id;
		frame_requests_ = 0;
		frame_ready_hits_ = 0;
		frame_stale_hits_ = 0;
		frame_selected_ready_pages_ = 0;
		frame_selected_ready_layers_.fill(false);
		frame_compose_jobs_ = 0;
		frame_compose_us_ = 0;
		frame_uploads_ = 0;
		frame_capacity_fallbacks_ = 0;
		frame_output_pages_ = 0;
		frame_output_hash_ = UINT64_C(1469598103934665603);
		cache_.begin_frame(p_frame_id);
		_drain_completed();
		return;
	}
	cache_.begin_frame(p_frame_id);
}

void TerrainTileCacheDevice::_invalidate_page(
		const opennova::TerrainTilePageKey &p_page) {
	cache_.invalidate(p_page);
	for (std::size_t layer = 0; layer < ready_generations_.size(); ++layer) {
		if (ready_generations_[layer] == 0 ||
				!same_page_key(ready_page_keys_[layer], p_page)) {
			continue;
		}
		ready_generations_[layer] = 0;
		ready_page_output_hashes_[layer] = 0;
		if (frame_selected_ready_layers_[layer]) {
			frame_selected_ready_layers_[layer] = false;
			--frame_selected_ready_pages_;
		}
		return;
	}
}

void TerrainTileCacheDevice::_retire_ready_scorch_overlaps(
		const opennova::terrain::TerrainScorchEntry &p_entry) {
	for (std::size_t layer = 0; layer < ready_generations_.size(); ++layer) {
		if (ready_generations_[layer] == 0 ||
				!opennova::TerrainTileCompositionCache::page_overlaps_q16(
						ready_page_keys_[layer], p_entry.minimum_x_q16,
						p_entry.minimum_z_q16, p_entry.maximum_x_q16,
						p_entry.maximum_z_q16)) {
			continue;
		}
		ready_generations_[layer] = 0;
		ready_page_output_hashes_[layer] = 0;
		if (frame_selected_ready_layers_[layer]) {
			frame_selected_ready_layers_[layer] = false;
			--frame_selected_ready_pages_;
		}
	}
}

bool TerrainTileCacheDevice::append_terrain_scorch(
		const opennova::terrain::TerrainScorchEntry &p_entry) {
	if (!scorch_textures_ready_ || !scorch_registry_.append(p_entry)) {
		++scorch_records_rejected_;
		return false;
	}
	const std::size_t invalidated = cache_.invalidate_overlapping_q16(
			p_entry.minimum_x_q16, p_entry.minimum_z_q16,
			p_entry.maximum_x_q16, p_entry.maximum_z_q16);
	scorch_page_invalidations_ += invalidated;
	_retire_ready_scorch_overlaps(p_entry);
	return true;
}

void TerrainTileCacheDevice::clear_terrain_scorches() {
	if (scorch_registry_.size() == 0) return;
	// A reset removes every record, so every compiled page's content identity
	// changes. Cancel queued plans before invalidating their generations.
	async_->cancel(false);
	cache_.invalidate_all();
	ready_generations_.fill(0);
	ready_page_output_hashes_.fill(0);
	frame_selected_ready_layers_.fill(false);
	frame_selected_ready_pages_ = 0;
	scorch_registry_.clear();
	scorch_records_rejected_ = 0;
}

bool TerrainTileCacheDevice::_allocate_texture() {
	TypedArray<Ref<Image>> layers;
	const Ref<Image> blank = Image::create(
			opennova::TerrainTileCompositionCache::kDimension,
			opennova::TerrainTileCompositionCache::kDimension, false,
			Image::FORMAT_RGBA8);
	if (blank.is_null()) {
		return false;
	}
	for (int layer = 0;
			layer < opennova::TerrainTileCompositionCache::kCapacity; ++layer) {
		layers.append(blank);
	}
	texture_.instantiate();
	if (texture_.is_null() || texture_->create_from_images(layers) != OK) {
		texture_.unref();
		return false;
	}
	return true;
}

void TerrainTileCacheDevice::_drain_completed() {
	if (texture_.is_null()) return;
	// Refresh uploads (layers that already serve a published payload — during
	// the refresh they serve it stale) trickle under the per-frame budget;
	// cold layers have nothing on screen but the fallback shader path, so
	// they drain unbounded and first-fill/teleport completes in a few frames.
	std::size_t refresh_uploads = 0;
	while (true) {
		std::optional<AsyncState::Completion> ready = async_->take_completion_if(
				[&](const AsyncState::Completion &candidate) {
					return ready_generations_[candidate.job.target.layer] == 0 ||
							refresh_uploads < AsyncState::kUploadBudgetPerFrame;
				});
		if (!ready.has_value()) break;
		AsyncState::Completion &completion = *ready;
		const bool refresh_upload =
				ready_generations_[completion.job.target.layer] != 0;
		frame_compose_us_ += completion.compose_us;
		const opennova::TerrainTileCompositionJob &job = completion.job;
		// Validate the lease before touching its Texture2DArray layer. The cache
		// is render-thread-owned, so it cannot become stale between this check
		// and publish() below.
		if (!cache_.can_publish(job)) continue;
		if (!completion.success) {
			++upload_failures_;
			cache_.invalidate(job.target.page);
			continue;
		}
		const Ref<Image> image = image_from_rgba8(completion.pixels);
		if (image.is_null()) {
			++upload_failures_;
			cache_.invalidate(job.target.page);
			continue;
		}
		texture_->update_layer(image, job.target.layer);
		if (!cache_.publish(job)) {
			++upload_failures_;
			cache_.invalidate(job.target.page);
			continue;
		}
		if (refresh_upload) ++refresh_uploads;
		++frame_uploads_;
		++frame_output_pages_;
		ready_generations_[job.target.layer] = job.target.generation;
		ready_page_keys_[job.target.layer] = job.target.page;
		ready_page_output_hashes_[job.target.layer] =
				page_output_hash(job, completion.pixels);
		if (capture_diagnostics_) {
			frame_output_hash_ = mix_value_bytes(frame_output_hash_,
					job.target.page.sector_origin_x);
			frame_output_hash_ = mix_value_bytes(frame_output_hash_,
					job.target.page.sector_origin_z);
			frame_output_hash_ = mix_value_bytes(frame_output_hash_,
					job.target.page.page_local_x);
			frame_output_hash_ = mix_value_bytes(frame_output_hash_,
					job.target.page.page_local_z);
			frame_output_hash_ = mix_value_bytes(frame_output_hash_,
					job.target.page.page_lod_level);
			for (uint8_t value : completion.pixels.pixels) {
				frame_output_hash_ = mix_byte(frame_output_hash_, value);
			}
		}
	}
}

void TerrainTileCacheDevice::_record_frame_selected_ready(
		const opennova::TerrainTilePageBinding &p_binding) {
	if (!p_binding.ready ||
			p_binding.layer >= frame_selected_ready_layers_.size() ||
			frame_selected_ready_layers_[p_binding.layer]) {
		return;
	}
	frame_selected_ready_layers_[p_binding.layer] = true;
	++frame_selected_ready_pages_;
}

uint64_t TerrainTileCacheDevice::_content_stamp(
		const Vector3 &p_tile_tint,
		const Vector3 &p_light_direction,
		opennova::terrain::TerrainTilePageSourceView &r_sources) const {
	const uint8_t tint[3] = {
		quantize_unorm(p_tile_tint.x),
		quantize_unorm(p_tile_tint.y),
		quantize_unorm(p_tile_tint.z),
	};
	// Environment_GetLightDirectionFloat is packed into texture-basis
	// (g2,g0,g1) before the tile-cache DOT3 pass.
	const opennova::terrain::TerrainTileLightEpoch light =
			opennova::terrain::terrain_tile_light_epoch_from_environment_tuple(
					p_light_direction.x, p_light_direction.y,
					p_light_direction.z);
	for (int channel = 0; channel < 3; ++channel) {
		r_sources.tile_overlay_tint[channel] = tint[channel] / 255.0f;
		r_sources.light_bytes[channel] = light[channel];
	}

	uint64_t hash = UINT64_C(1469598103934665603);
	for (int shift = 0; shift < 64; shift += 8) {
		hash = mix_byte(hash,
				static_cast<uint8_t>(source_revision_ >> shift));
	}
	for (uint8_t value : tint) hash = mix_byte(hash, value);
	for (uint8_t value : light) hash = mix_byte(hash, value);
	return hash;
}

opennova::TerrainTilePageBinding TerrainTileCacheDevice::request(
		const opennova::TerrainPatchDraw &p_draw,
		const Vector3 &p_tile_tint,
		const Vector3 &p_light_direction) {
	opennova::TerrainTilePageBinding unavailable;
	if (!is_ready()) {
		return unavailable;
	}

	const std::shared_ptr<const AsyncState::SourceSnapshot> source_snapshot =
			async_->sources();
	if (source_snapshot == nullptr) return unavailable;
	opennova::terrain::TerrainTilePageSourceView sources =
			source_snapshot->view({}, {}, nullptr);

	opennova::TerrainTileCompositionRequest request;
	request.page.sector_origin_x = p_draw.sector_x * 512;
	request.page.sector_origin_z = p_draw.sector_z * 512;
	request.page.page_local_x = p_draw.local_page_x;
	request.page.page_local_z = p_draw.local_page_z;
	request.page.page_lod_level = static_cast<uint8_t>(p_draw.page_lod_level);
	request.tile_index = p_draw.tile_index;
	request.source_origin_x = p_draw.source_page_x;
	request.source_origin_z = p_draw.source_page_z;
	if (opennova::TerrainTileCompositionCache::page_world_span(
			request.page.page_lod_level) == 0) {
		return unavailable;
	}
	opennova::TerrainTileContentStamp content{
			_content_stamp(p_tile_tint, p_light_direction, sources)};
	request.content = content;
	// Permanent scorch identity: the page's insertion-ordered overlap stamp,
	// walked from the registry's sector buckets with no entry list built.
	// The entries are built only on the miss path below. A page the registry
	// cannot route gets its base page with no overlay; scorch is an optional
	// overlay and never a reason to drop the tile binding.
	const opennova::terrain::TerrainScorchPageStamp scorch_stamp =
			scorch_registry_.stamp(request.page);
	if (scorch_stamp.valid) {
		request.content.value = mix_value_bytes(
				request.content.value, scorch_stamp.content_stamp);
	}

	++frame_requests_;
	const std::optional<opennova::TerrainTileCompositionDecision> decision =
			cache_.request(request);
	if (!decision.has_value()) {
		++cache_misses_;
		++frame_capacity_fallbacks_;
		return unavailable;
	}
	if (!decision->job.has_value()) {
		if (decision->binding.ready) {
			++cache_hits_;
			if (decision->binding.stale) {
				++frame_stale_hits_;
			} else {
				++frame_ready_hits_;
			}
			_record_frame_selected_ready(decision->binding);
		}
		return decision->binding;
	}

	++cache_misses_;
	++compose_jobs_;
	++frame_compose_jobs_;
	const opennova::TerrainTileCompositionJob &job = *decision->job;
	if (decision->binding.ready) {
		// Stale-serving: the layer keeps its published payload (and its ready
		// bookkeeping) while the replacement generation composes.
		++frame_stale_hits_;
		_record_frame_selected_ready(decision->binding);
	} else {
		// request() may have invalidated or evicted the previously published
		// page in this layer. Do not report it ready if composition/upload
		// fails.
		ready_generations_[job.target.layer] = 0;
		ready_page_output_hashes_[job.target.layer] = 0;
	}
	opennova::terrain::TerrainScorchPagePlan scorch_plan;
	if (scorch_stamp.valid) scorch_plan = scorch_registry_.plan(request.page);
	if (!async_->enqueue(job, source_snapshot, sources.tile_overlay_tint,
			sources.light_bytes, std::move(scorch_plan),
			diagnostic_frame_id_, capture_diagnostics_)) {
		cache_.invalidate(job.target.page);
		++frame_capacity_fallbacks_;
		return unavailable;
	}
	return decision->binding;
}

std::optional<opennova::TerrainTilePageBinding>
TerrainTileCacheDevice::best_ready(
		const opennova::TerrainTileResidentPoint &p_point) {
	if (!is_ready()) {
		return std::nullopt;
	}
	return cache_.best_ready(p_point);
}

Dictionary TerrainTileCacheDevice::get_diagnostics() const {
	Dictionary diagnostics;
	int ready_pages = 0;
	std::vector<std::size_t> ready_layers;
	for (std::size_t layer = 0; layer < ready_generations_.size(); ++layer) {
		if (ready_generations_[layer] != 0) {
			++ready_pages;
			ready_layers.push_back(layer);
		}
	}
	std::sort(ready_layers.begin(), ready_layers.end(),
			[this](std::size_t left, std::size_t right) {
				const auto &a = ready_page_keys_[left];
				const auto &b = ready_page_keys_[right];
				if (a.sector_origin_x != b.sector_origin_x) {
					return a.sector_origin_x < b.sector_origin_x;
				}
				if (a.sector_origin_z != b.sector_origin_z) {
					return a.sector_origin_z < b.sector_origin_z;
				}
				if (a.page_local_x != b.page_local_x) {
					return a.page_local_x < b.page_local_x;
				}
				if (a.page_local_z != b.page_local_z) {
					return a.page_local_z < b.page_local_z;
				}
				return a.page_lod_level < b.page_lod_level;
			});
	uint64_t resident_output_hash = UINT64_C(1469598103934665603);
	for (std::size_t layer : ready_layers) {
		resident_output_hash = mix_value_bytes(resident_output_hash,
				ready_page_output_hashes_[layer]);
	}
	diagnostics["available"] = is_ready();
	diagnostics["dimension"] =
			opennova::TerrainTileCompositionCache::kDimension;
	diagnostics["capacity"] =
			opennova::TerrainTileCompositionCache::kCapacity;
	diagnostics["ready_pages"] = ready_pages;
	diagnostics["resident_output_pages"] = ready_pages;
	diagnostics["resident_output_hash"] =
			static_cast<int64_t>(resident_output_hash);
	diagnostics["compose_jobs"] = static_cast<int64_t>(compose_jobs_);
	diagnostics["cache_hits"] = static_cast<int64_t>(cache_hits_);
	diagnostics["cache_misses"] = static_cast<int64_t>(cache_misses_);
	diagnostics["upload_failures"] = static_cast<int64_t>(upload_failures_);
	diagnostics["scorch_textures_ready"] = scorch_textures_ready_;
	diagnostics["scorch_records"] = static_cast<int64_t>(scorch_registry_.size());
	diagnostics["scorch_generation"] =
			static_cast<int64_t>(scorch_registry_.generation());
	diagnostics["scorch_records_rejected"] =
			static_cast<int64_t>(scorch_records_rejected_);
	diagnostics["scorch_page_invalidations"] =
			static_cast<int64_t>(scorch_page_invalidations_);
	diagnostics["frame_requests"] = static_cast<int64_t>(frame_requests_);
	diagnostics["frame_ready_hits"] = static_cast<int64_t>(frame_ready_hits_);
	diagnostics["frame_stale_hits"] = static_cast<int64_t>(frame_stale_hits_);
	diagnostics["frame_selected_ready_pages"] =
			static_cast<int64_t>(frame_selected_ready_pages_);
	diagnostics["frame_compose_jobs"] =
			static_cast<int64_t>(frame_compose_jobs_);
	diagnostics["frame_compose_us"] =
			static_cast<int64_t>(frame_compose_us_);
	diagnostics["frame_uploads"] = static_cast<int64_t>(frame_uploads_);
	diagnostics["pending_jobs"] = static_cast<int64_t>(
			async_->pending_jobs());
	diagnostics["active_jobs"] = static_cast<int64_t>(
			async_->current_epoch_active_jobs());
	diagnostics["worker_count"] = static_cast<int64_t>(
			AsyncState::kWorkerCount);
	diagnostics["upload_budget"] = static_cast<int64_t>(
			AsyncState::kUploadBudgetPerFrame);
	diagnostics["frame_capacity_fallbacks"] =
			static_cast<int64_t>(frame_capacity_fallbacks_);
	diagnostics["frame_output_pages"] =
			static_cast<int64_t>(frame_output_pages_);
	diagnostics["frame_output_hash"] =
			static_cast<int64_t>(frame_output_hash_);
	diagnostics["source_revision"] = static_cast<int64_t>(source_revision_);
	diagnostics["tile_overlay_required"] = tile_overlay_required_;
	diagnostics["tile_overlay_available"] = tile_overlay_ready_;
	return diagnostics;
}

} // namespace godot
