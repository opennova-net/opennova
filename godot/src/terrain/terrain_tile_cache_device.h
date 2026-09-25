#pragma once

// Godot device adapter for the engine-owned terrain tile-composition cache.
// Page identity, the per-frame sweep, invalidation, pixel composition and the
// composition thread pool remain portable; this class owns only source
// extraction, the frame's synchronous compose-and-upload, Texture2DArray
// upload, and counters.

#include <godot_cpp/classes/texture2d_array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/terrain/terrain_frame.h>
#include <runtime/terrain/terrain_scorch.h>
#include <runtime/terrain/terrain_static_shadow_alpha.h>
#include <runtime/terrain/terrain_static_shadow_planner.h>
#include <runtime/terrain/terrain_tile_composer.h>
#include <runtime/terrain/terrain_tile_composition_cache.h>
#include <runtime/terrain/terrain_tile_composition_worker.h>
#include <runtime/terrain_query/terrain_field_store.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace godot {

class TerrainData;
class TerrainSurfaceInputs;
class TerrainTileInfo;

// Non-owning producer seam between mission/ObjectData geometry resolution and
// the page-cache device. Only immutable portable snapshots cross to workers;
// the device owns generation validation and the render-thread upload commit.
class TerrainStaticShadowPageRasterizer {
public:
	virtual ~TerrainStaticShadowPageRasterizer() = default;
	virtual std::shared_ptr<const opennova::terrain::TerrainStaticShadowCompilationSnapshot>
	compilation_snapshot() const = 0;
	// The frame-shared Render_ShaderTickMs the provider's begin_frame received.
	// The device stamps it on every composition job it enqueues so a worker
	// samples caster material animation at the requesting frame, exactly as
	// retail evaluates tile-model materials inside the tile render.
	virtual uint32_t material_time_ms() const noexcept = 0;
	virtual void merge_async_diagnostics(
			const opennova::terrain::TerrainStaticShadowPlannerDiagnostics
					&p_diagnostics) noexcept = 0;
};

class TerrainTileCacheDevice {
public:
	TerrainTileCacheDevice();
	~TerrainTileCacheDevice();
	TerrainTileCacheDevice(const TerrainTileCacheDevice &) = delete;
	TerrainTileCacheDevice &operator=(const TerrainTileCacheDevice &) = delete;

	bool rebuild(const Ref<TerrainData> &p_data,
			const Ref<TerrainSurfaceInputs> &p_surface_inputs,
			const Ref<TerrainTileInfo> &p_tile_info_override,
			bool p_tile_overlay_enabled);
	void clear();
	// One PolyTrn_RenderFrame: the cache frame advances and this frame's
	// claims stamp the given TOD epoch (Env_TodEpoch).
	void begin_frame(uint64_t p_frame_id, uint32_t p_tod_epoch);
	// Byte-level capture diagnostics (full-page FNV output hash, pre/post
	// shadow byte diffs) copy and re-walk every composed 256 KB page — that
	// is capture/test instrumentation, not steady-state work. Default OFF;
	// the render probes and shadow GUT suites opt in.
	void set_capture_diagnostics(bool p_enabled) {
		capture_diagnostics_ = p_enabled;
	}
	bool is_capture_diagnostics_enabled() const {
		return capture_diagnostics_;
	}
	void set_static_shadow_rasterizer(
			TerrainStaticShadowPageRasterizer *p_rasterizer);
	void invalidate_static_shadow_pages();
	// Appends one already-resolved permanent record and retires exactly the
	// occupied cache pages its inclusive Q16 bounds touch.
	bool append_terrain_scorch(
			const opennova::terrain::TerrainScorchEntry &p_entry);
	// Retires the pages a destroyed entity's bounds touch (the same inclusive
	// Q16 test); they recompose the next time they are visible.
	std::size_t invalidate_region(int32_t p_minimum_x_q16, int32_t p_minimum_z_q16,
			int32_t p_maximum_x_q16, int32_t p_maximum_z_q16);
	// Mission/replay reset: remove the permanent list and make every resident
	// page cold so no prior compiled scorch survives the lifecycle boundary.
	void clear_terrain_scorches();

	// The frame's page sweep over its draw list, composed and uploaded before
	// the terrain draws; one binding per patch, not ready where the patch
	// draws with no page this frame.
	const std::vector<opennova::TerrainTilePageBinding> &compose_frame(
			const opennova::TerrainDrawList &p_draw_list,
			const Vector3 &p_tile_tint,
			const Vector3 &p_light_direction);
	std::optional<opennova::TerrainTilePageBinding> lookup(
			const opennova::TerrainTileResidentPoint &p_point);

	Ref<Texture2DArray> get_texture() const { return texture_; }
	Dictionary get_diagnostics() const;
	bool is_ready() const { return texture_.is_valid() && sources_ready_; }

private:
	void _upload_completed();
	bool _refresh_shadow_snapshot();
	void _reset_shadow_epoch_diagnostics();
	void _invalidate_all();
	bool _allocate_texture();

	opennova::TerrainTileCompositionCache cache_;
	// The page-composition thread pool (engine): demand / completion queues,
	// the source snapshot, the epoch cancel.
	std::unique_ptr<opennova::terrain::TerrainTileCompositionWorker> async_;
	Ref<Texture2DArray> texture_;
	bool tile_overlay_required_ = false;
	bool tile_overlay_ready_ = false;
	bool sources_ready_ = false;
	uint64_t source_revision_ = 0;
	uint64_t next_source_revision_ = 1;
	// The generation each layer's uploaded pixels belong to, and their hash
	// (diagnostics: a layer counts as resident output while the cache still
	// holds that generation there).
	std::array<uint64_t, opennova::TerrainTileCompositionCache::kCapacity>
			ready_generations_{};
	std::array<opennova::TerrainTilePageKey,
			opennova::TerrainTileCompositionCache::kCapacity> ready_page_keys_{};
	std::array<uint64_t, opennova::TerrainTileCompositionCache::kCapacity>
			ready_page_output_hashes_{};
	std::vector<opennova::TerrainTileCompositionRequest> frame_visible_;
	std::vector<opennova::TerrainTilePageBinding> frame_bindings_;
	uint64_t compose_jobs_ = 0;
	uint64_t cache_hits_ = 0;
	uint64_t cache_misses_ = 0;
	uint64_t upload_failures_ = 0;
	opennova::terrain::TerrainScorchRegistry scorch_registry_;
	bool scorch_textures_ready_ = false;
	uint64_t scorch_records_rejected_ = 0;
	uint64_t scorch_page_invalidations_ = 0;
	TerrainStaticShadowPageRasterizer *static_shadow_rasterizer_ = nullptr;
	std::shared_ptr<const opennova::terrain::TerrainStaticShadowCompilationSnapshot>
			shadow_snapshot_;
	uint64_t shadow_raster_jobs_ = 0;
	uint64_t shadow_raster_failures_ = 0;
	uint64_t shadow_alpha_changed_bytes_ = 0;
	uint64_t shadow_rgb_changed_bytes_ = 0;
	uint64_t shadow_base_nonzero_alpha_bytes_ = 0;
	uint64_t shadow_epoch_raster_jobs_ = 0;
	uint64_t shadow_epoch_pages_with_draws_ = 0;
	uint64_t shadow_epoch_projection_draws_ = 0;
	uint64_t shadow_epoch_plan_failures_ = 0;
	uint64_t shadow_epoch_unsupported_draw_count_ = 0;
	uint64_t shadow_epoch_unsupported_attribution_truncated_ = 0;
	uint64_t shadow_epoch_alpha_changed_bytes_ = 0;
	uint64_t shadow_epoch_rgb_changed_bytes_ = 0;
	uint64_t shadow_epoch_base_nonzero_alpha_bytes_ = 0;
	bool capture_diagnostics_ = false;
	bool diagnostic_frame_active_ = false;
	uint64_t diagnostic_frame_id_ = 0;
	uint64_t frame_requests_ = 0;
	uint64_t frame_ready_hits_ = 0;
	// Distinct composed pages this frame's patches bind.
	uint64_t frame_selected_ready_pages_ = 0;
	uint64_t frame_compose_jobs_ = 0;
	uint64_t frame_compose_us_ = 0;
	uint64_t frame_compose_page_us_ = 0;
	uint64_t frame_compose_shadow_plan_us_ = 0;
	uint64_t frame_uploads_ = 0;
	uint64_t frame_capacity_fallbacks_ = 0;
	uint64_t frame_shadow_alpha_changed_bytes_ = 0;
	uint64_t frame_shadow_rgb_changed_bytes_ = 0;
	uint64_t frame_shadow_base_nonzero_alpha_bytes_ = 0;
	uint64_t frame_output_pages_ = 0;
	uint64_t frame_output_hash_ = 0;
};

} // namespace godot
