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
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <base/io/hash.h>
#include <runtime/terrain/terrain_tile_quadrant_update.h>

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

} // namespace

TerrainTileCacheDevice::TerrainTileCacheDevice() :
		async_(std::make_unique<opennova::terrain::TerrainTileCompositionWorker>()) {}

TerrainTileCacheDevice::~TerrainTileCacheDevice() {
	async_->cancel(true);
	_release_device_texture();
}

Ref<TextureLayered> TerrainTileCacheDevice::get_texture() const {
	if (device_texture_.is_valid()) return device_texture_;
	return texture_;
}

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
		const Ref<TerrainTileInfo> &p_tile_info_override, bool p_page_targets) {
	clear();
	if (p_data.is_null() || p_surface_inputs.is_null()) {
		return false;
	}
	// The page tier under the session's terrain texture detail
	// (TerrainTileCompositionCache::page_dimension).
	cache_.set_dimension(opennova::TerrainTileCompositionCache::page_dimension(
			p_surface_inputs->get_terrain_texdetail()));
	auto snapshot = std::make_shared<opennova::terrain::TerrainTileCompositionWorker::SourceSnapshot>();

	opennova::terrain::Rgba8Image colormap;
	opennova::terrain::Rgba8Image heightfield_normal;
	// The live map an editor's paint left (TerrainData::get_colormap_image), else the loaded texture.
	const Ref<Image> live_colormap = p_data->get_colormap_image();
	const bool have_colormap = live_colormap.is_valid() && !live_colormap->is_empty()
			? image_to_rgba8(live_colormap, colormap)
			: p_data->texture_rgba8(p_data->get_colormap(), colormap);
	// The surface inputs' own atlas, which their texture was made of (nothing read back).
	heightfield_normal = p_surface_inputs->get_heightfield_normal_image();
	const bool have_normal = heightfield_normal.is_valid();
	if (!have_colormap || !have_normal) {
		return false;
	}
	// The colormap quadrants under the session's texcompression_level (DXT1
	// below 2 on the reference card, D-RMAT-24); the heightfield normals stay
	// 0x100001.
	snapshot->colormap = std::make_shared<const opennova::terrain::TerrainTileQuadrantSource>(
			opennova::terrain::build_terrain_tile_quadrant_source(colormap,
					opennova::terrain::terrain_colormap_quadrant_flags(p_surface_inputs->get_texcompression_level())));
	snapshot->heightfield_normal = std::make_shared<const opennova::terrain::TerrainTileQuadrantSource>(
			opennova::terrain::build_terrain_tile_quadrant_source(heightfield_normal));
	if (!snapshot->colormap->is_valid() || !snapshot->heightfield_normal->is_valid()) {
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
	auto scorch_textures = std::make_shared<
			std::array<opennova::terrain::TerrainScorchTexture, opennova::terrain::kTerrainScorchTextureSlots>>();
	for (const uint8_t texture_index :
			{uint8_t{0}, uint8_t{1}, uint8_t{2}, uint8_t{4}}) {
		const std::string_view name =
				opennova::terrain::terrain_scorch_texture_name(texture_index);
		// The scorch set loads through the stage loader (Terrain_LoadScorchTextures), its pixels
		// decoded alone: no device texture is made of them here.
		const Ref<Image> image = p_data->load_source_image(
				String::utf8(name.data(), static_cast<int>(name.size())),
				opennova::renderer::TextureLoader::Stage);
		opennova::terrain::Rgba8Image base;
		if (!image_to_rgba8(image, base)) {
			scorch_ready = false;
			break;
		}
		(*scorch_textures)[texture_index] =
				opennova::terrain::build_terrain_scorch_texture(base);
		if (!(*scorch_textures)[texture_index].is_valid()) {
			scorch_ready = false;
			break;
		}
	}
	if (!scorch_ready) {
		for (auto &slot : *scorch_textures) {
			slot = opennova::terrain::TerrainScorchTexture{};
		}
	}
	snapshot->scorch_textures = std::move(scorch_textures);
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
		if (!p_data->texture_rgba8(p_data->get_tilestrip_tex(), tilestrip)) {
			return false;
		}
		snapshot->tilestrip = std::make_shared<const std::vector<opennova::terrain::Rgba8Image>>(
				opennova::terrain::build_terrain_tile_set_mips(tilestrip));
		if (snapshot->tilestrip->empty()) {
			return false;
		}
		snapshot->tile_info = tile_info->to_native();
		snapshot->tile_overlay_ready = true;
		tile_overlay_ready_ = true;
	}

	source_revision_ = next_source_revision_++;
	if (!p_page_targets) {
		// The sources are the mission textures stage's, which load on both arms; the page targets
		// are the render subsystems', which a dedicated host's start jumps over (engine
		// terrain/terrain_load_legs.h builds_tile_cache_targets carries the witness).
		async_->install_sources(std::move(snapshot));
		return false;
	}
	sources_ready_ = _allocate_texture();
	if (sources_ready_) {
		async_->install_sources(std::move(snapshot));
		// Only the scorch overlay is gated on its own sources resolving; the
		// base page cache is ready either way.
		scorch_textures_ready_ = scorch_ready;
		// The device pass's shaders and targets build with the load, not in the first
		// frame that composes a shadowed page.
		if (device_texture_.is_valid()) (void)_device_shadow_ready();
	}
	return sources_ready_;
}

void TerrainTileCacheDevice::clear() {
	async_->cancel(true);
	cache_.invalidate_all();
	texture_.unref();
	_release_device_texture();
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
	colormap_region_updates_ = 0;
	colormap_pages_retired_ = 0;
	tile_entry_updates_ = 0;
	tile_entry_pages_retired_ = 0;
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
	frame_compose_shadow_walk_us_ = 0;
	frame_compose_raster_ = {};
	frame_compose_wait_us_ = 0;
	frame_upload_us_ = 0;
	frame_uploads_ = 0;
	frame_capacity_fallbacks_ = 0;
	frame_shadow_alpha_changed_bytes_ = 0;
	frame_shadow_rgb_changed_bytes_ = 0;
	frame_shadow_base_nonzero_alpha_bytes_ = 0;
	frame_output_pages_ = 0;
	frame_output_hash_ = 0;
	shadow_device_jobs_ = 0;
	frame_shadow_device_jobs_ = 0;
	frame_shadow_device_draws_ = 0;
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
	frame_compose_shadow_walk_us_ = 0;
	frame_compose_raster_ = {};
	frame_compose_wait_us_ = 0;
	frame_upload_us_ = 0;
	frame_uploads_ = 0;
	frame_capacity_fallbacks_ = 0;
	frame_shadow_alpha_changed_bytes_ = 0;
	frame_shadow_rgb_changed_bytes_ = 0;
	frame_shadow_base_nonzero_alpha_bytes_ = 0;
	frame_output_pages_ = 0;
	frame_output_hash_ = opennova::io::kFnv1a64Offset;
	frame_shadow_device_jobs_ = 0;
	frame_shadow_device_draws_ = 0;
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

void TerrainTileCacheDevice::set_device_shadow_enabled(bool p_enabled) {
	if (device_shadow_enabled_ == p_enabled) return;
	device_shadow_enabled_ = p_enabled;
	_invalidate_all();
}

PackedByteArray TerrainTileCacheDevice::read_device_layer(int p_layer) const {
	if (rd_ == nullptr || !page_rid_.is_valid() || p_layer < 0 ||
			p_layer >= opennova::TerrainTileCompositionCache::kCapacity) {
		return PackedByteArray();
	}
	return rd_->texture_get_data(page_rid_, static_cast<uint32_t>(p_layer));
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

std::optional<std::size_t> TerrainTileCacheDevice::update_heightfield_normal_region(
		const opennova::terrain::Rgba8Image &p_normals,
		const std::vector<opennova::terrain::CoordsRect> &p_written) {
	const std::shared_ptr<const opennova::terrain::TerrainTileCompositionWorker::SourceSnapshot> current =
			sources_ready_ ? async_->sources() : nullptr;
	if (!current || !current->heightfield_normal || !p_normals.is_valid()) {
		return std::nullopt;
	}
	if (p_written.empty()) {
		return std::size_t{0};
	}
	// The source's chains shared; the quadrants a box reaches made again in new ones, over its texels alone, a
	// change kept for each box.
	auto normals = std::make_shared<opennova::terrain::TerrainTileQuadrantSource>(*current->heightfield_normal);
	std::vector<opennova::terrain::TerrainTileQuadrantChange> changes;
	if (opennova::terrain::update_terrain_tile_quadrants(*normals, p_normals, p_written, changes) == 0) {
		return std::nullopt;
	}
	auto next = std::make_shared<opennova::terrain::TerrainTileCompositionWorker::SourceSnapshot>(*current);
	next->heightfield_normal = normals;
	async_->install_sources(std::move(next));
	// Every page that samples a changed normal, by its key and the source origin it was claimed with; a page
	// composing from the old quadrants now holds a retired generation, its completion dropped.
	return cache_.invalidate_sampling(
			[&](const opennova::TerrainTilePageKey &page, int32_t source_x, int32_t source_z) {
				return opennova::terrain::terrain_tile_page_samples_any(changes, *normals, page.page_lod_level,
						source_x, source_z, cache_.dimension());
			});
}

std::size_t TerrainTileCacheDevice::invalidate_region(
		int32_t p_minimum_x_q16, int32_t p_minimum_z_q16,
		int32_t p_maximum_x_q16, int32_t p_maximum_z_q16) {
	return cache_.invalidate_overlapping_q16(p_minimum_x_q16, p_minimum_z_q16,
			p_maximum_x_q16, p_maximum_z_q16);
}

bool TerrainTileCacheDevice::update_colormap_region(const uint8_t *p_rgba, uint32_t p_width, uint32_t p_height,
		int x, int y, int w, int h) {
	const std::shared_ptr<const opennova::terrain::TerrainTileCompositionWorker::SourceSnapshot> current =
			sources_ready_ ? async_->sources() : nullptr;
	if (!current || !current->colormap) {
		return false;
	}
	// The source's chains shared; the quadrants the rect reaches made again in new ones.
	auto colormap = std::make_shared<opennova::terrain::TerrainTileQuadrantSource>(*current->colormap);
	opennova::terrain::TerrainTileQuadrantChange change;
	if (opennova::terrain::update_terrain_tile_quadrants(*colormap, p_rgba, p_width, p_height, x, y, w, h,
				&change) == 0) {
		return false;
	}
	auto next = std::make_shared<opennova::terrain::TerrainTileCompositionWorker::SourceSnapshot>(*current);
	next->colormap = colormap;
	async_->install_sources(std::move(next));
	// Every page that samples a changed texel, by its key and the source origin it was claimed with; a page
	// composing from the old quadrants now holds a retired generation, its completion dropped.
	const std::size_t retired = cache_.invalidate_sampling(
			[&](const opennova::TerrainTilePageKey &page, int32_t source_x, int32_t source_z) {
				return opennova::terrain::terrain_tile_page_samples(change, *colormap, page.page_lod_level, source_x,
						source_z, cache_.dimension());
			});
	++colormap_region_updates_;
	colormap_pages_retired_ += retired;
	return true;
}

std::optional<std::size_t> TerrainTileCacheDevice::set_tile_entries(const opennova::TilFile &p_file) {
	const std::shared_ptr<const opennova::terrain::TerrainTileCompositionWorker::SourceSnapshot> current =
			sources_ready_ ? async_->sources() : nullptr;
	if (!current) {
		return std::nullopt;
	}
	// Tiles over sources built without the tile set (a terrain drawn with none) need it built: a rebuild.
	if (!p_file.entries.empty() && !current->tile_overlay_ready) {
		return std::nullopt;
	}
	const std::vector<opennova::terrain::TerrainTileSquareQ16> squares =
			opennova::terrain::terrain_tile_overlay_changes(current->tile_info, p_file);
	if (squares.empty()) {
		return std::size_t{0};
	}
	// The parts shared (their pointers copied), the placement alone new.
	auto next = std::make_shared<opennova::terrain::TerrainTileCompositionWorker::SourceSnapshot>(*current);
	next->tile_info = p_file;
	tile_overlay_required_ = !p_file.entries.empty();
	async_->install_sources(std::move(next));
	// A page composing from the old placement now holds a retired generation: its completion is dropped.
	const auto clamp = [](int64_t value) {
		return static_cast<int32_t>(std::clamp<int64_t>(value, INT32_MIN, INT32_MAX));
	};
	std::size_t retired = 0;
	for (const opennova::terrain::TerrainTileSquareQ16 &square : squares) {
		retired += cache_.invalidate_overlapping_q16(clamp(square.minimum_x), clamp(square.minimum_z),
				clamp(square.maximum_x), clamp(square.maximum_z));
	}
	++tile_entry_updates_;
	tile_entry_pages_retired_ += retired;
	return retired;
}

void TerrainTileCacheDevice::clear_terrain_scorches() {
	if (scorch_registry_.size() == 0) return;
	// A reset removes every record and every page they were composed into.
	_invalidate_all();
	scorch_registry_.clear();
	scorch_records_rejected_ = 0;
}

bool TerrainTileCacheDevice::_allocate_device_texture(RenderingDevice *p_rd) {
	const BitField<RenderingDevice::TextureUsageBits> usage =
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	if (!p_rd->texture_is_format_supported_for_usage(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
				usage)) {
		return false;
	}
	Ref<RDTextureFormat> format;
	format.instantiate();
	format->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D_ARRAY);
	format->set_width(cache_.dimension());
	format->set_height(cache_.dimension());
	format->set_depth(1);
	format->set_array_layers(opennova::TerrainTileCompositionCache::kCapacity);
	format->set_mipmaps(1);
	format->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
	format->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	format->set_usage_bits(usage);
	Ref<RDTextureView> view;
	view.instantiate();
	const RID page_array = p_rd->texture_create(format, view);
	if (!page_array.is_valid()) return false;
	// Every page starts as the retail creation-time clear colour (the D3DCOLOR ARGB the
	// engine keeps), as the uploaded array's layers do.
	const uint32_t clear_argb = opennova::TerrainTileCompositionCache::kTileClearColorArgb;
	const Color clear(static_cast<float>((clear_argb >> 16) & 0xFFu) / 255.0f,
			static_cast<float>((clear_argb >> 8) & 0xFFu) / 255.0f,
			static_cast<float>(clear_argb & 0xFFu) / 255.0f,
			static_cast<float>((clear_argb >> 24) & 0xFFu) / 255.0f);
	if (p_rd->texture_clear(page_array, clear, 0, 1, 0,
				opennova::TerrainTileCompositionCache::kCapacity) != OK) {
		p_rd->free_rid(page_array);
		return false;
	}
	rd_ = p_rd;
	page_rid_ = page_array;
	device_texture_.instantiate();
	device_texture_->set_texture_rd_rid(page_rid_);
	return true;
}

void TerrainTileCacheDevice::_release_device_texture() {
	shadow_pass_.release();
	shadow_pass_retired_ = false;
	shadow_pass_failure_.clear();
	if (device_texture_.is_valid()) {
		device_texture_->set_texture_rd_rid(RID());
		device_texture_.unref();
	}
	if (rd_ != nullptr && page_rid_.is_valid() && RenderingServer::get_singleton() != nullptr &&
			rd_->texture_is_valid(page_rid_)) {
		rd_->free_rid(page_rid_);
	}
	page_rid_ = RID();
	rd_ = nullptr;
}

void TerrainTileCacheDevice::_retire_device_shadow(const std::string &p_reason) {
	if (shadow_pass_retired_) return;
	shadow_pass_retired_ = true;
	shadow_pass_failure_ = p_reason;
	shadow_pass_.release();
	UtilityFunctions::push_warning(vformat(
			"Terrain static shadow: the device pass is unavailable (%s); the CPU raster draws "
			"the casters.",
			String::utf8(p_reason.c_str())));
}

bool TerrainTileCacheDevice::_device_shadow_ready() {
	if (shadow_pass_retired_ || !device_texture_.is_valid() || rd_ == nullptr) return false;
	RenderingServer *server = RenderingServer::get_singleton();
	if (server == nullptr || !server->is_on_render_thread()) return false;
	if (shadow_pass_.is_prepared()) return true;
	if (shadow_pass_.prepare(rd_, page_rid_, static_cast<uint32_t>(cache_.dimension()))) return true;
	_retire_device_shadow(shadow_pass_.failure());
	return false;
}

bool TerrainTileCacheDevice::_allocate_texture() {
	// The device form wherever the device runs on this thread (the default single-safe
	// renderer: the main thread is the render thread).
	RenderingServer *server = RenderingServer::get_singleton();
	RenderingDevice *device = server != nullptr ? server->get_rendering_device() : nullptr;
	if (device != nullptr && server->is_on_render_thread() && _allocate_device_texture(device)) {
		return true;
	}
	TypedArray<Ref<Image>> layers;
	const Ref<Image> blank = Image::create(cache_.dimension(), cache_.dimension(), false,
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
	if (texture_.is_null() && !device_texture_.is_valid()) return;
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
		frame_compose_shadow_walk_us_ +=
				completion.shadow_diagnostics.frame_triangle_walk_us;
		{
			const opennova::terrain::TerrainStaticShadowRasterStats &raster =
					completion.shadow_diagnostics.frame_raster;
			frame_compose_raster_.setup_us += raster.setup_us;
			frame_compose_raster_.prepare_us += raster.prepare_us;
			frame_compose_raster_.pixel_us += raster.pixel_us;
			frame_compose_raster_.pixel_lane_sum_us += raster.pixel_lane_sum_us;
			frame_compose_raster_.pixel_lane_max_us =
					std::max(frame_compose_raster_.pixel_lane_max_us, raster.pixel_lane_max_us);
			frame_compose_raster_.resolve_us += raster.resolve_us;
		}
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
		if (device_texture_.is_valid()) {
			// The device form: the composed page into its layer, then (a shadowed page) the
			// casters drawn into it ahead of the frame's terrain draw.
			if (!completion.pixels.is_valid() ||
					rd_->texture_update(page_rid_, job.target.layer,
							to_packed_bytes(completion.pixels.pixels)) != OK) {
				++upload_failures_;
				cache_.invalidate(job.target.page);
				continue;
			}
			const std::shared_ptr<const opennova::terrain::TerrainStaticShadowDeviceJob> shadow =
					completion.device_shadow;
			if (shadow != nullptr && !shadow->draws.empty()) {
				if (!_device_shadow_ready() || !shadow_pass_.record(*shadow, job.target.layer)) {
					// A page the device could not draw fails as a raster that failed: it is
					// retired and recomposes, on the CPU raster from here on.
					_retire_device_shadow(shadow_pass_.failure().empty() ? "the pass is gone"
																		 : shadow_pass_.failure());
					++shadow_raster_failures_;
					++upload_failures_;
					cache_.invalidate(job.target.page);
					continue;
				}
				++shadow_device_jobs_;
				++frame_shadow_device_jobs_;
				frame_shadow_device_draws_ += shadow->draws.size();
				if (capture_diagnostics_) {
					// Capture only: the layer as the device left it (a readback that waits for
					// the device), the shadow's byte changes measured against the upload.
					const PackedByteArray after = rd_->texture_get_data(page_rid_, job.target.layer);
					const std::size_t count = completion.pixels.pixels.size();
					if (static_cast<std::size_t>(after.size()) >= count) {
						uint64_t alpha_changed = 0;
						uint64_t rgb_changed = 0;
						uint64_t base_nonzero = 0;
						for (std::size_t byte = 0; byte < count; ++byte) {
							const uint8_t before = completion.pixels.pixels[byte];
							const uint8_t now = after[static_cast<int64_t>(byte)];
							if ((byte & 3u) == 3u) {
								if (before != 0) ++base_nonzero;
								if (now != before) ++alpha_changed;
							} else if (now != before) {
								++rgb_changed;
							}
							completion.pixels.pixels[byte] = now;
						}
						shadow_alpha_changed_bytes_ += alpha_changed;
						shadow_rgb_changed_bytes_ += rgb_changed;
						shadow_base_nonzero_alpha_bytes_ += base_nonzero;
						shadow_epoch_alpha_changed_bytes_ += alpha_changed;
						shadow_epoch_rgb_changed_bytes_ += rgb_changed;
						shadow_epoch_base_nonzero_alpha_bytes_ += base_nonzero;
						frame_shadow_alpha_changed_bytes_ += alpha_changed;
						frame_shadow_rgb_changed_bytes_ += rgb_changed;
						frame_shadow_base_nonzero_alpha_bytes_ += base_nonzero;
					}
				}
			}
		} else {
			const Ref<Image> image = image_from_rgba8(completion.pixels);
			if (image.is_null()) {
				++upload_failures_;
				cache_.invalidate(job.target.page);
				continue;
			}
			texture_->update_layer(image, job.target.layer);
		}
		if (!cache_.publish(job)) {
			++upload_failures_;
			cache_.invalidate(job.target.page);
			continue;
		}
		++frame_uploads_;
		++frame_output_pages_;
		ready_generations_[job.target.layer] = job.target.generation;
		ready_page_keys_[job.target.layer] = job.target.page;
		// A device-drawn page outside a capture hashes its upload with its casters' count:
		// only relative equality of these hashes is read.
		uint64_t output_hash = page_output_hash(job, completion.pixels);
		if (completion.device_shadow != nullptr && !capture_diagnostics_) {
			output_hash = opennova::io::fnv1a64_value(output_hash,
					completion.device_shadow->triangle_count);
		}
		ready_page_output_hashes_[job.target.layer] = output_hash;
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
	// A shadowed page's casters are the device's where its pass runs, the worker's CPU
	// raster where it does not.
	const bool device_shadow = device_shadow_enabled_ && shadow_snapshot != nullptr &&
			!jobs.empty() && _device_shadow_ready();
	for (const opennova::TerrainTileCompositionJob &job : jobs) {
		++compose_jobs_;
		++frame_compose_jobs_;
		ready_generations_[job.target.layer] = 0;
		ready_page_output_hashes_[job.target.layer] = 0;
		opennova::terrain::TerrainScorchPagePlan scorch_plan =
				scorch_registry_.plan(job.target.page);
		if (!async_->enqueue(job, source_snapshot, tint, light,
				std::move(scorch_plan), diagnostic_frame_id_, shadow_snapshot,
				shadow_material_time, capture_diagnostics_, device_shadow)) {
			cache_.invalidate(job.target.page);
		}
	}
	// Retail composes every missing visible page inside the sweep, before the
	// batch draws bind them. (engine: runtime/terrain/terrain_tile_composition_cache.cpp)
	const auto wait_started = std::chrono::steady_clock::now();
	async_->wait_idle();
	const auto wait_done = std::chrono::steady_clock::now();
	_upload_completed();
	frame_compose_wait_us_ += static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(wait_done - wait_started).count());
	frame_upload_us_ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - wait_done).count());
	// The device's caster buffers for geometry no caster holds any more.
	if (frame_shadow_device_jobs_ != 0) shadow_pass_.prune();

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

std::optional<opennova::TerrainTilePageBinding>
TerrainTileCacheDevice::find_sector_patch(
		const opennova::TerrainTileSectorPatchPoint &p_point) {
	if (!is_ready()) {
		return std::nullopt;
	}
	return cache_.find_sector_patch(p_point);
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
	// The layers holding resident composed pages, in page-key order.
	PackedInt32Array resident_layers;
	for (std::size_t layer : ready_layers) resident_layers.push_back(static_cast<int32_t>(layer));
	diagnostics["resident_layers"] = resident_layers;
	diagnostics["dimension"] = cache_.dimension();
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
	diagnostics["frame_compose_shadow_walk_us"] =
			static_cast<int64_t>(frame_compose_shadow_walk_us_);
	diagnostics["frame_compose_raster_setup_us"] =
			static_cast<int64_t>(frame_compose_raster_.setup_us);
	diagnostics["frame_compose_raster_prepare_us"] =
			static_cast<int64_t>(frame_compose_raster_.prepare_us);
	diagnostics["frame_compose_raster_pixel_us"] =
			static_cast<int64_t>(frame_compose_raster_.pixel_us);
	diagnostics["frame_compose_raster_pixel_lane_sum_us"] =
			static_cast<int64_t>(frame_compose_raster_.pixel_lane_sum_us);
	diagnostics["frame_compose_raster_pixel_lane_max_us"] =
			static_cast<int64_t>(frame_compose_raster_.pixel_lane_max_us);
	diagnostics["frame_compose_raster_resolve_us"] =
			static_cast<int64_t>(frame_compose_raster_.resolve_us);
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
	diagnostics["colormap_region_updates"] = static_cast<int64_t>(colormap_region_updates_);
	diagnostics["colormap_pages_retired"] = static_cast<int64_t>(colormap_pages_retired_);
	diagnostics["tile_entry_updates"] = static_cast<int64_t>(tile_entry_updates_);
	diagnostics["tile_entry_pages_retired"] = static_cast<int64_t>(tile_entry_pages_retired_);
	diagnostics["page_array_device"] = device_texture_.is_valid();
	diagnostics["shadow_device_pass"] = shadow_pass_.is_prepared();
	diagnostics["shadow_device_failure"] = String::utf8(shadow_pass_failure_.c_str());
	diagnostics["shadow_device_jobs"] = static_cast<int64_t>(shadow_device_jobs_);
	diagnostics["frame_shadow_device_jobs"] = static_cast<int64_t>(frame_shadow_device_jobs_);
	diagnostics["frame_shadow_device_draws"] = static_cast<int64_t>(frame_shadow_device_draws_);
	diagnostics["shadow_device_geometries"] =
			static_cast<int64_t>(shadow_pass_.cached_geometries());
	diagnostics["shadow_device_textures"] = static_cast<int64_t>(shadow_pass_.cached_textures());
	diagnostics["tile_overlay_required"] = tile_overlay_required_;
	diagnostics["tile_overlay_available"] = tile_overlay_ready_;
	return diagnostics;
}

} // namespace godot
