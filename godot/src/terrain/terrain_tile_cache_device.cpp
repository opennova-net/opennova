// Device leg only: Godot Image/Texture2DArray marshalling, the frame's
// synchronous compose-and-upload, and upload counters. Page identity, the
// per-frame sweep, and invalidation — the witnessed cache semantics — live in
// engine/runtime/terrain/terrain_tile_composition_cache.{h,cpp}, held here as
// a member (see the class header's witness block and
// docs/terrain/terrain-re.md; retail's device-side twin is the D3D
// tile-texture pool the record maps).
#include "terrain/terrain_tile_cache_device.h"
#include "util/data_format.h"

#include "terrain/terrain_data.h"
#include "terrain/terrain_image_convert.h"
#include "terrain/terrain_surface_inputs.h"
#include "terrain/terrain_tile_info.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/color.hpp>
#include <base/io/hash.h>

#include <chrono>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace godot {
using opennova::TerrainTileCompositionDemandQueue;

namespace {

Ref<Image> image_from_rgba8(const opennova::terrain::Rgba8Image &p_source) {
	if (!p_source.is_valid()) {
		return {};
	}
	return Image::create_from_data(
			static_cast<int32_t>(p_source.width),
			static_cast<int32_t>(p_source.height), false,
			Image::FORMAT_RGBA8, to_packed_bytes(p_source.pixels));
}

uint8_t quantize_unorm(float p_value) {
	return static_cast<uint8_t>(std::clamp(
			static_cast<int>(std::lround(
					std::clamp(p_value, 0.0f, 1.0f) * 255.0f)), 0, 255));
}

// The page's resident-output identity: the page key plus its pixels. The
// pixel fold consumes eight bytes per step (a 256 KB page per upload, up to
// two uploads a frame, on the main thread) — only relative equality of these
// hashes is ever read (the tile-cache diagnostics).
uint64_t page_output_hash(
		const opennova::TerrainTileCompositionJob &p_job,
		const opennova::terrain::Rgba8Image &p_pixels) {
	uint64_t hash = opennova::io::kFnv1a64Offset;
	hash = opennova::io::fnv1a64_value(hash, p_job.target.page.sector_origin_x);
	hash = opennova::io::fnv1a64_value(hash, p_job.target.page.sector_origin_z);
	hash = opennova::io::fnv1a64_value(hash, p_job.target.page.page_local_x);
	hash = opennova::io::fnv1a64_value(hash, p_job.target.page.page_local_z);
	hash = opennova::io::fnv1a64_value(hash, p_job.target.page.page_lod_level);
	const uint8_t *bytes = p_pixels.pixels.data();
	const std::size_t size = p_pixels.pixels.size();
	std::size_t index = 0;
	for (; index + 8 <= size; index += 8) {
		uint64_t word = 0;
		std::memcpy(&word, bytes + index, sizeof(word));
		hash = (hash ^ word) * UINT64_C(1099511628211);
		hash ^= hash >> 29;
	}
	for (; index < size; ++index) hash = opennova::io::fnv1a64_byte(hash, bytes[index]);
	return hash;
}

// The web build's pthread pool is fixed when the page loads (the Web export
// preset's threads/emscripten_pool_size), and a thread the pool cannot serve
// never starts while this frame blocks in wait_idle(); so the web composes on
// a small fixed budget the pool is sized for, and the desktop on the hardware.
opennova::terrain::TerrainTileCompositionWorker::Threads composition_threads() {
	if (OS::get_singleton()->has_feature("web")) {
		opennova::terrain::TerrainTileCompositionWorker::Threads web;
		web.page_workers = 2;
		web.lane_threads = 2;
		return web;
	}
	return opennova::terrain::TerrainTileCompositionWorker::Threads::for_hardware();
}

} // namespace

TerrainTileCacheDevice::TerrainTileCacheDevice() :
		async_(std::make_unique<opennova::terrain::TerrainTileCompositionWorker>(
				composition_threads())) {}

TerrainTileCacheDevice::~TerrainTileCacheDevice() = default;

void TerrainTileCacheDevice::_reset_shadow_epoch_diagnostics() {
	shadow_epoch_raster_jobs_ = 0;
	shadow_epoch_pages_with_draws_ = 0;
	shadow_epoch_projection_draws_ = 0;
	shadow_epoch_plan_failures_ = 0;
	shadow_epoch_unsupported_draw_count_ = 0;
	shadow_epoch_unsupported_attribution_truncated_ = 0;
	shadow_epoch_alpha_changed_bytes_ = 0;
	shadow_epoch_rgb_changed_bytes_ = 0;
	shadow_epoch_base_nonzero_alpha_bytes_ = 0;
}

bool TerrainTileCacheDevice::_refresh_shadow_snapshot() {
	if (static_shadow_rasterizer_ == nullptr) {
		shadow_snapshot_.reset();
		return true;
	}
	std::shared_ptr<const opennova::terrain::TerrainStaticShadowCompilationSnapshot> snapshot =
			static_shadow_rasterizer_->compilation_snapshot();
	if (snapshot == nullptr) {
		_invalidate_all();
		shadow_snapshot_.reset();
		_reset_shadow_epoch_diagnostics();
		return false;
	}
	// A planner state change is NOT an invalidation: retail samples the
	// casters when a page composes and keeps the page until the record is
	// claimed again, retired by its TOD epoch, or invalidated spatially.
	shadow_snapshot_ = std::move(snapshot);
	return true;
}

bool TerrainTileCacheDevice::rebuild(
		const Ref<TerrainData> &p_data,
		const Ref<TerrainSurfaceInputs> &p_surface_inputs,
		const Ref<TerrainTileInfo> &p_tile_info_override) {
	clear();
	if (p_data.is_null() || p_surface_inputs.is_null()) {
		return false;
	}
	auto snapshot = std::make_shared<opennova::terrain::TerrainTileCompositionWorker::SourceSnapshot>();

	opennova::terrain::Rgba8Image colormap;
	opennova::terrain::Rgba8Image heightfield_normal;
	const Ref<Image> live_colormap = p_data->get_colormap_image();
	const bool have_colormap = live_colormap.is_valid() && !live_colormap->is_empty()
			? image_to_rgba8(live_colormap, colormap)
			: texture_to_rgba8(p_data->get_colormap(), colormap);
	const bool have_normal = texture_to_rgba8(
			p_surface_inputs->get_heightfield_normal_texture(),
			heightfield_normal);
	if (!have_colormap || !have_normal) {
		return false;
	}
	snapshot->colormap =
			opennova::terrain::build_terrain_tile_quadrant_source(colormap);
	snapshot->heightfield_normal =
			opennova::terrain::build_terrain_tile_quadrant_source(heightfield_normal);
	if (!snapshot->colormap.is_valid() || !snapshot->heightfield_normal.is_valid()) {
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
		// The scorch set loads through the stage loader (Terrain_LoadScorchTextures).
		const Ref<Texture2D> texture = p_data->load_source_texture(
				String::utf8(name.data(), static_cast<int>(name.size())),
				opennova::renderer::TextureLoader::Stage);
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
	if (tile_info_declared && tile_info.is_null()) {
		// A TRN-declared .til that could not be resolved or parsed is not the
		// same as a mission authored without tile overlays. Publishing only the
		// base sources here would make the missing overlay invisible to capture.
		tile_overlay_required_ = true;
		return false;
	}
	if (tile_info.is_valid() && tile_info->get_entry_count() > 0) {
		tile_overlay_required_ = true;
		opennova::terrain::Rgba8Image tilestrip;
		if (!texture_to_rgba8(p_data->get_tilestrip_tex(), tilestrip)) {
			return false;
		}
		snapshot->tilestrip =
				opennova::terrain::build_terrain_tile_set_mips(tilestrip);
		if (snapshot->tilestrip.empty()) {
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
	cache_.invalidate_all();
	texture_.unref();
	tile_overlay_required_ = false;
	tile_overlay_ready_ = false;
	sources_ready_ = false;
	source_revision_ = 0;
	ready_generations_.fill(0);
	ready_page_keys_.fill(opennova::TerrainTilePageKey{});
	ready_page_output_hashes_.fill(0);
	frame_visible_.clear();
	frame_bindings_.clear();
	compose_jobs_ = 0;
	cache_hits_ = 0;
	cache_misses_ = 0;
	upload_failures_ = 0;
	scorch_registry_.clear();
	scorch_textures_ready_ = false;
	scorch_records_rejected_ = 0;
	scorch_page_invalidations_ = 0;
	shadow_snapshot_.reset();
	shadow_raster_jobs_ = 0;
	shadow_raster_failures_ = 0;
	shadow_alpha_changed_bytes_ = 0;
	shadow_rgb_changed_bytes_ = 0;
	shadow_base_nonzero_alpha_bytes_ = 0;
	_reset_shadow_epoch_diagnostics();
	diagnostic_frame_active_ = false;
	diagnostic_frame_id_ = 0;
	frame_requests_ = 0;
	frame_ready_hits_ = 0;
	frame_selected_ready_pages_ = 0;
	frame_compose_jobs_ = 0;
	frame_compose_us_ = 0;
	frame_compose_page_us_ = 0;
	frame_compose_shadow_plan_us_ = 0;
	frame_compose_shadow_build_us_ = 0;
	frame_compose_wait_us_ = 0;
	frame_upload_us_ = 0;
	frame_uploads_ = 0;
	frame_capacity_fallbacks_ = 0;
	frame_shadow_alpha_changed_bytes_ = 0;
	frame_shadow_rgb_changed_bytes_ = 0;
	frame_shadow_base_nonzero_alpha_bytes_ = 0;
	frame_output_pages_ = 0;
	frame_output_hash_ = 0;
}

void TerrainTileCacheDevice::begin_frame(uint64_t p_frame_id, uint32_t p_tod_epoch) {
	if (diagnostic_frame_active_ && diagnostic_frame_id_ == p_frame_id) {
		return;
	}
	diagnostic_frame_active_ = true;
	diagnostic_frame_id_ = p_frame_id;
	frame_requests_ = 0;
	frame_ready_hits_ = 0;
	frame_selected_ready_pages_ = 0;
	frame_compose_jobs_ = 0;
	frame_compose_us_ = 0;
	frame_compose_page_us_ = 0;
	frame_compose_shadow_plan_us_ = 0;
	frame_compose_shadow_build_us_ = 0;
	frame_compose_wait_us_ = 0;
	frame_upload_us_ = 0;
	frame_uploads_ = 0;
	frame_capacity_fallbacks_ = 0;
	frame_shadow_alpha_changed_bytes_ = 0;
	frame_shadow_rgb_changed_bytes_ = 0;
	frame_shadow_base_nonzero_alpha_bytes_ = 0;
	frame_output_pages_ = 0;
	frame_output_hash_ = opennova::io::kFnv1a64Offset;
	_refresh_shadow_snapshot();
	cache_.begin_frame(p_tod_epoch);
}

void TerrainTileCacheDevice::_invalidate_all() {
	async_->cancel(false);
	cache_.invalidate_all();
	ready_generations_.fill(0);
	ready_page_output_hashes_.fill(0);
}

void TerrainTileCacheDevice::set_static_shadow_rasterizer(
		TerrainStaticShadowPageRasterizer *p_rasterizer) {
	if (static_shadow_rasterizer_ == p_rasterizer) {
		return;
	}
	static_shadow_rasterizer_ = p_rasterizer;
	shadow_snapshot_.reset();
	// Existing layers were published under a different alpha producer (or no
	// producer). Keep the allocated array but make every page cold.
	_invalidate_all();
	_reset_shadow_epoch_diagnostics();
}

void TerrainTileCacheDevice::invalidate_static_shadow_pages() {
	// Provider control changes alter the final page-alpha result independently
	// of the terrain source images. Retire every page immediately so foliage
	// cannot observe a prior enabled/suppression state before terrain
	// recomposes the replacement pages.
	_invalidate_all();
	shadow_snapshot_.reset();
	_reset_shadow_epoch_diagnostics();
}

bool TerrainTileCacheDevice::append_terrain_scorch(
		const opennova::terrain::TerrainScorchEntry &p_entry) {
	if (!scorch_textures_ready_ || !scorch_registry_.append(p_entry)) {
		++scorch_records_rejected_;
		return false;
	}
	scorch_page_invalidations_ += cache_.invalidate_overlapping_q16(
			p_entry.minimum_x_q16, p_entry.minimum_z_q16,
			p_entry.maximum_x_q16, p_entry.maximum_z_q16);
	return true;
}

std::size_t TerrainTileCacheDevice::invalidate_region(
		int32_t p_minimum_x_q16, int32_t p_minimum_z_q16,
		int32_t p_maximum_x_q16, int32_t p_maximum_z_q16) {
	return cache_.invalidate_overlapping_q16(p_minimum_x_q16, p_minimum_z_q16,
			p_maximum_x_q16, p_maximum_z_q16);
}

void TerrainTileCacheDevice::clear_terrain_scorches() {
	if (scorch_registry_.size() == 0) return;
	// A reset removes every record and every page they were composed into.
	_invalidate_all();
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
	// Every page starts as the retail creation-time clear colour (the engine
	// constant is D3DCOLOR ARGB), so an unpublished layer reads as retail's
	// freshly allocated tile RT rather than black.
	const uint32_t clear_argb =
			opennova::TerrainTileCompositionCache::kTileClearColorArgb;
	blank->fill(Color(
			static_cast<float>((clear_argb >> 16) & 0xFFu) / 255.0f,
			static_cast<float>((clear_argb >> 8) & 0xFFu) / 255.0f,
			static_cast<float>(clear_argb & 0xFFu) / 255.0f,
			static_cast<float>((clear_argb >> 24) & 0xFFu) / 255.0f));
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

void TerrainTileCacheDevice::_upload_completed() {
	if (texture_.is_null()) return;
	while (true) {
		std::optional<opennova::terrain::TerrainTileCompositionWorker::Completion> ready =
				async_->take_completion();
		if (!ready.has_value()) break;
		opennova::terrain::TerrainTileCompositionWorker::Completion &completion = *ready;
		frame_compose_us_ += completion.compose_us;
		frame_compose_page_us_ += completion.page_us;
		frame_compose_shadow_plan_us_ += completion.shadow_plan_us;
		frame_compose_shadow_build_us_ +=
				completion.shadow_diagnostics.frame_triangle_build_us;
		const opennova::TerrainTileCompositionJob &job = completion.job;
		// Validate the claim before touching its Texture2DArray layer. The cache
		// is render-thread-owned, so it cannot become stale between this check
		// and publish() below.
		if (!cache_.can_publish(job)) continue;
		if (completion.shadow_attempted) {
			++shadow_raster_jobs_;
			++shadow_epoch_raster_jobs_;
			shadow_epoch_pages_with_draws_ +=
					completion.shadow_diagnostics.frame_pages_with_draws;
			shadow_epoch_projection_draws_ +=
					completion.shadow_diagnostics.frame_projection_draws;
			shadow_epoch_plan_failures_ +=
					completion.shadow_diagnostics.frame_plan_failures;
			shadow_epoch_unsupported_draw_count_ +=
					completion.shadow_diagnostics.frame_unsupported_draw_count;
			shadow_epoch_unsupported_attribution_truncated_ +=
					completion.shadow_diagnostics.
							frame_unsupported_attribution_truncated;
			shadow_epoch_alpha_changed_bytes_ +=
					completion.shadow_alpha_changed_bytes;
			shadow_epoch_rgb_changed_bytes_ +=
					completion.shadow_rgb_changed_bytes;
			shadow_epoch_base_nonzero_alpha_bytes_ +=
					completion.shadow_base_nonzero_alpha_bytes;
			if (static_shadow_rasterizer_ != nullptr) {
				static_shadow_rasterizer_->merge_async_diagnostics(
						completion.shadow_diagnostics);
			}
			shadow_alpha_changed_bytes_ +=
					completion.shadow_alpha_changed_bytes;
			shadow_rgb_changed_bytes_ +=
					completion.shadow_rgb_changed_bytes;
			shadow_base_nonzero_alpha_bytes_ +=
					completion.shadow_base_nonzero_alpha_bytes;
			frame_shadow_alpha_changed_bytes_ +=
					completion.shadow_alpha_changed_bytes;
			frame_shadow_rgb_changed_bytes_ +=
					completion.shadow_rgb_changed_bytes;
			frame_shadow_base_nonzero_alpha_bytes_ +=
					completion.shadow_base_nonzero_alpha_bytes;
		}
		if (!completion.success) {
			if (completion.shadow_attempted) ++shadow_raster_failures_;
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
		++frame_uploads_;
		++frame_output_pages_;
		ready_generations_[job.target.layer] = job.target.generation;
		ready_page_keys_[job.target.layer] = job.target.page;
		ready_page_output_hashes_[job.target.layer] =
				page_output_hash(job, completion.pixels);
		if (capture_diagnostics_) {
			frame_output_hash_ = opennova::io::fnv1a64_value(frame_output_hash_,
					job.target.page.sector_origin_x);
			frame_output_hash_ = opennova::io::fnv1a64_value(frame_output_hash_,
					job.target.page.sector_origin_z);
			frame_output_hash_ = opennova::io::fnv1a64_value(frame_output_hash_,
					job.target.page.page_local_x);
			frame_output_hash_ = opennova::io::fnv1a64_value(frame_output_hash_,
					job.target.page.page_local_z);
			frame_output_hash_ = opennova::io::fnv1a64_value(frame_output_hash_,
					job.target.page.page_lod_level);
			for (uint8_t value : completion.pixels.pixels) {
				frame_output_hash_ = opennova::io::fnv1a64_byte(frame_output_hash_, value);
			}
		}
	}
}

const std::vector<opennova::TerrainTilePageBinding> &
TerrainTileCacheDevice::compose_frame(
		const opennova::TerrainDrawList &p_draw_list,
		const Vector3 &p_tile_tint,
		const Vector3 &p_light_direction) {
	frame_bindings_.assign(p_draw_list.patches.size(), opennova::TerrainTilePageBinding{});
	if (!is_ready()) {
		return frame_bindings_;
	}
	const std::shared_ptr<const opennova::terrain::TerrainTileCompositionWorker::SourceSnapshot>
			source_snapshot = async_->sources();
	if (source_snapshot == nullptr) {
		return frame_bindings_;
	}
	std::shared_ptr<const opennova::terrain::TerrainStaticShadowCompilationSnapshot>
			shadow_snapshot = shadow_snapshot_;
	if (static_shadow_rasterizer_ != nullptr) {
		if (shadow_snapshot == nullptr && !_refresh_shadow_snapshot()) {
			++shadow_raster_failures_;
			return frame_bindings_;
		}
		shadow_snapshot = shadow_snapshot_;
		if (!shadow_snapshot->planner.is_enabled()) shadow_snapshot.reset();
	}
	// The page inputs sampled at compose time: the tile tint as the
	// D3DCOLOR retail passes as the overlay diffuse, and the environment
	// light tuple packed into texture-basis (g2,g0,g1) bytes for the DOT3 pass.
	std::array<float, 3> tint{};
	tint[0] = quantize_unorm(p_tile_tint.x) / 255.0f;
	tint[1] = quantize_unorm(p_tile_tint.y) / 255.0f;
	tint[2] = quantize_unorm(p_tile_tint.z) / 255.0f;
	const opennova::terrain::TerrainTileLightEpoch light =
			opennova::terrain::terrain_tile_light_epoch_from_environment_tuple(
					p_light_direction.x, p_light_direction.y,
					p_light_direction.z);

	frame_visible_.clear();
	frame_visible_.reserve(p_draw_list.patches.size());
	for (const opennova::TerrainPatchDraw &draw : p_draw_list.patches) {
		frame_visible_.push_back(opennova::terrain_tile_composition_request(draw));
	}
	frame_requests_ += frame_visible_.size();
	const std::vector<opennova::TerrainTileCompositionJob> jobs =
			cache_.sweep(frame_visible_);
	const uint32_t shadow_material_time = shadow_snapshot != nullptr
			? static_shadow_rasterizer_->material_time_ms()
			: 0u;
	for (const opennova::TerrainTileCompositionJob &job : jobs) {
		++compose_jobs_;
		++frame_compose_jobs_;
		ready_generations_[job.target.layer] = 0;
		ready_page_output_hashes_[job.target.layer] = 0;
		opennova::terrain::TerrainScorchPagePlan scorch_plan =
				scorch_registry_.plan(job.target.page);
		if (!async_->enqueue(job, source_snapshot, tint, light,
				std::move(scorch_plan), diagnostic_frame_id_, shadow_snapshot,
				shadow_material_time, capture_diagnostics_)) {
			cache_.invalidate(job.target.page);
		}
	}
	// Retail composes every missing visible page inside the sweep, before the
	// batch draws bind them. (retail PolyTrn_RenderFrame @ 0x60F080..0x60F0E3)
	const auto wait_started = std::chrono::steady_clock::now();
	async_->wait_idle();
	const auto wait_done = std::chrono::steady_clock::now();
	_upload_completed();
	frame_compose_wait_us_ += static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(wait_done - wait_started).count());
	frame_upload_us_ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - wait_done).count());

	std::array<bool, opennova::TerrainTileCompositionCache::kCapacity> selected{};
	for (std::size_t index = 0; index < frame_visible_.size(); ++index) {
		const std::optional<opennova::TerrainTilePageBinding> bound =
				cache_.bind(frame_visible_[index]);
		if (!bound.has_value()) {
			++frame_capacity_fallbacks_;
			continue;
		}
		frame_bindings_[index] = *bound;
		if (!selected[bound->layer]) {
			selected[bound->layer] = true;
			++frame_selected_ready_pages_;
		}
	}
	frame_ready_hits_ = frame_requests_ - std::min<uint64_t>(
			frame_requests_, frame_compose_jobs_ + frame_capacity_fallbacks_);
	cache_hits_ += frame_ready_hits_;
	cache_misses_ += frame_compose_jobs_ + frame_capacity_fallbacks_;
	return frame_bindings_;
}

std::optional<opennova::TerrainTilePageBinding>
TerrainTileCacheDevice::lookup(
		const opennova::TerrainTileResidentPoint &p_point) {
	if (!is_ready()) {
		return std::nullopt;
	}
	return cache_.lookup(p_point);
}

Dictionary TerrainTileCacheDevice::get_diagnostics() const {
	Dictionary diagnostics;
	int ready_pages = 0;
	std::vector<std::size_t> ready_layers;
	for (std::size_t layer = 0; layer < ready_generations_.size(); ++layer) {
		const std::optional<opennova::TerrainTilePageBinding> resident =
				cache_.resident_layer(static_cast<uint16_t>(layer));
		if (resident.has_value() && ready_generations_[layer] != 0 &&
				resident->generation == ready_generations_[layer]) {
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
	uint64_t resident_output_hash = opennova::io::kFnv1a64Offset;
	for (std::size_t layer : ready_layers) {
		resident_output_hash = opennova::io::fnv1a64_value(resident_output_hash,
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
	diagnostics["shadow_raster_available"] =
			static_shadow_rasterizer_ != nullptr;
	diagnostics["shadow_raster_jobs"] =
			static_cast<int64_t>(shadow_raster_jobs_);
	diagnostics["shadow_raster_failures"] =
			static_cast<int64_t>(shadow_raster_failures_);
	diagnostics["shadow_alpha_changed_bytes"] =
			static_cast<int64_t>(shadow_alpha_changed_bytes_);
	diagnostics["shadow_rgb_changed_bytes"] =
			static_cast<int64_t>(shadow_rgb_changed_bytes_);
	diagnostics["shadow_base_nonzero_alpha_bytes"] =
			static_cast<int64_t>(shadow_base_nonzero_alpha_bytes_);
	diagnostics["shadow_epoch_raster_jobs"] =
			static_cast<int64_t>(shadow_epoch_raster_jobs_);
	diagnostics["shadow_epoch_pages_with_draws"] =
			static_cast<int64_t>(shadow_epoch_pages_with_draws_);
	diagnostics["shadow_epoch_projection_draws"] =
			static_cast<int64_t>(shadow_epoch_projection_draws_);
	diagnostics["shadow_epoch_plan_failures"] =
			static_cast<int64_t>(shadow_epoch_plan_failures_);
	diagnostics["shadow_epoch_unsupported_draw_count"] =
			static_cast<int64_t>(shadow_epoch_unsupported_draw_count_);
	diagnostics["shadow_epoch_unsupported_attribution_truncated"] =
			static_cast<int64_t>(
					shadow_epoch_unsupported_attribution_truncated_);
	diagnostics["shadow_epoch_alpha_changed_bytes"] =
			static_cast<int64_t>(shadow_epoch_alpha_changed_bytes_);
	diagnostics["shadow_epoch_rgb_changed_bytes"] =
			static_cast<int64_t>(shadow_epoch_rgb_changed_bytes_);
	diagnostics["shadow_epoch_base_nonzero_alpha_bytes"] =
			static_cast<int64_t>(shadow_epoch_base_nonzero_alpha_bytes_);
	diagnostics["frame_requests"] = static_cast<int64_t>(frame_requests_);
	diagnostics["frame_ready_hits"] = static_cast<int64_t>(frame_ready_hits_);
	diagnostics["frame_selected_ready_pages"] =
			static_cast<int64_t>(frame_selected_ready_pages_);
	diagnostics["frame_compose_jobs"] =
			static_cast<int64_t>(frame_compose_jobs_);
	diagnostics["frame_compose_us"] =
			static_cast<int64_t>(frame_compose_us_);
	diagnostics["frame_compose_page_us"] =
			static_cast<int64_t>(frame_compose_page_us_);
	diagnostics["frame_compose_shadow_plan_us"] =
			static_cast<int64_t>(frame_compose_shadow_plan_us_);
	diagnostics["frame_compose_shadow_build_us"] =
			static_cast<int64_t>(frame_compose_shadow_build_us_);
	diagnostics["frame_compose_wait_us"] =
			static_cast<int64_t>(frame_compose_wait_us_);
	diagnostics["frame_upload_us"] = static_cast<int64_t>(frame_upload_us_);
	diagnostics["frame_uploads"] = static_cast<int64_t>(frame_uploads_);
	diagnostics["pending_jobs"] = static_cast<int64_t>(
			async_->pending_jobs());
	diagnostics["active_jobs"] = static_cast<int64_t>(
			async_->current_epoch_active_jobs());
	diagnostics["worker_count"] = static_cast<int64_t>(
			async_->worker_count());
	diagnostics["frame_capacity_fallbacks"] =
			static_cast<int64_t>(frame_capacity_fallbacks_);
	diagnostics["frame_shadow_alpha_changed_bytes"] =
			static_cast<int64_t>(frame_shadow_alpha_changed_bytes_);
	diagnostics["frame_shadow_rgb_changed_bytes"] =
			static_cast<int64_t>(frame_shadow_rgb_changed_bytes_);
	diagnostics["frame_shadow_base_nonzero_alpha_bytes"] =
			static_cast<int64_t>(frame_shadow_base_nonzero_alpha_bytes_);
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
