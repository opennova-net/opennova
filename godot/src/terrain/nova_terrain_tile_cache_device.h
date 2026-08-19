#pragma once

// Godot device adapter for the engine-owned terrain tile-composition cache.
// Page identity, LRU, invalidation, and pixel composition remain portable;
// this class owns only source extraction, Texture2DArray upload, and counters.

#include <godot_cpp/classes/texture2d_array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <terrain/terrain_frame.h>
#include <terrain/terrain_static_shadow_alpha.h>
#include <terrain/terrain_static_shadow_planner.h>
#include <terrain/terrain_tile_composer.h>
#include <terrain/terrain_tile_composition_cache.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace godot {

class TerrainData;
class TerrainSurfaceInputs;
class TerrainTileInfo;

// Main-thread-owned provider state copied once per semantic shadow epoch.
// Worker threads clone only the portable planner and keep the receiver storage
// alive; no Godot Object or rendering API crosses the worker boundary.
struct TerrainStaticShadowReceiverStorage {
	std::vector<uint16_t> heightmap;
	std::array<int, 16 * 16> sector_grid{};
};

struct TerrainStaticShadowCompilationSnapshot {
	uint64_t revision = 0;
	std::shared_ptr<const TerrainStaticShadowReceiverStorage> receiver_storage;
	opennova::terrain::TerrainStaticShadowPlanner planner;
};

// Non-owning producer seam between mission/ObjectData geometry resolution and
// the page-cache device. Only immutable portable snapshots cross to workers;
// the device owns generation validation and the render-thread upload commit.
class TerrainStaticShadowPageRasterizer {
public:
	virtual ~TerrainStaticShadowPageRasterizer() = default;
	virtual std::shared_ptr<const TerrainStaticShadowCompilationSnapshot>
	compilation_snapshot() const = 0;
	// Main-thread only: the memoized per-page plan on the LIVE planner. The
	// provider mutates planner state only before the device's request loop
	// (rasterizer begin_frame), so within a frame this is state-identical to
	// what workers compute from compilation_snapshot().
	virtual opennova::terrain::TerrainStaticShadowPagePlanResult plan_page(
			const opennova::TerrainTilePageKey &p_page) = 0;
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
	void begin_frame(uint64_t p_frame_id);
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

	opennova::TerrainTilePageBinding request(
			const opennova::TerrainPatchDraw &p_draw,
			const Vector3 &p_tile_tint,
			const Vector3 &p_light_direction);
	std::optional<opennova::TerrainTilePageBinding> best_ready(
			const opennova::TerrainTileResidentPoint &p_point);

	Ref<Texture2DArray> get_texture() const { return texture_; }
	Dictionary get_diagnostics() const;
	bool is_ready() const { return texture_.is_valid() && sources_ready_; }

private:
	struct AsyncState;
	void _drain_completed();
	bool _refresh_shadow_snapshot();
	void _reset_shadow_epoch_diagnostics();
	void _invalidate_page(const opennova::TerrainTilePageKey &p_page);
	bool _allocate_texture();
	void _record_frame_selected_ready(
			const opennova::TerrainTilePageBinding &p_binding);
	uint64_t _content_stamp(
			const Vector3 &p_tile_tint,
			const Vector3 &p_light_direction,
			opennova::terrain::TerrainTilePageSourceView &r_sources) const;

	opennova::TerrainTileCompositionCache cache_;
	std::unique_ptr<AsyncState> async_;
	Ref<Texture2DArray> texture_;
	bool tile_overlay_required_ = false;
	bool tile_overlay_ready_ = false;
	bool sources_ready_ = false;
	uint64_t source_revision_ = 0;
	uint64_t next_source_revision_ = 1;
	std::array<uint64_t, opennova::TerrainTileCompositionCache::kCapacity>
			ready_generations_{};
	std::array<opennova::TerrainTilePageKey,
			opennova::TerrainTileCompositionCache::kCapacity> ready_page_keys_{};
	std::array<uint64_t, opennova::TerrainTileCompositionCache::kCapacity>
			ready_page_output_hashes_{};
	uint64_t compose_jobs_ = 0;
	uint64_t cache_hits_ = 0;
	uint64_t cache_misses_ = 0;
	uint64_t upload_failures_ = 0;
	TerrainStaticShadowPageRasterizer *static_shadow_rasterizer_ = nullptr;
	std::shared_ptr<const TerrainStaticShadowCompilationSnapshot>
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
	// Requests served by a stale-marked payload while its replacement
	// composes. Never counted as ready hits, so capture settle gates
	// (frame_ready_hits == frame_requests) still demand exactness.
	uint64_t frame_stale_hits_ = 0;
	uint64_t frame_selected_ready_pages_ = 0;
	std::array<bool, opennova::TerrainTileCompositionCache::kCapacity>
			frame_selected_ready_layers_{};
	uint64_t frame_compose_jobs_ = 0;
	uint64_t frame_compose_us_ = 0;
	uint64_t frame_uploads_ = 0;
	uint64_t frame_capacity_fallbacks_ = 0;
	uint64_t frame_shadow_alpha_changed_bytes_ = 0;
	uint64_t frame_shadow_rgb_changed_bytes_ = 0;
	uint64_t frame_shadow_base_nonzero_alpha_bytes_ = 0;
	uint64_t frame_output_pages_ = 0;
	uint64_t frame_output_hash_ = 0;
};

} // namespace godot
