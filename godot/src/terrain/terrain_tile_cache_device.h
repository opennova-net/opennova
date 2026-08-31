#pragma once

// Godot device adapter for the engine-owned terrain tile-composition cache.
// Page identity, LRU, invalidation, and pixel composition remain portable;
// this class owns only source extraction, Texture2DArray upload, and counters.

#include <godot_cpp/classes/texture2d_array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/terrain/terrain_frame.h>
#include <runtime/terrain/terrain_scorch.h>
#include <runtime/terrain/terrain_tile_composer.h>
#include <runtime/terrain/terrain_tile_composition_cache.h>
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
	// Byte-level capture diagnostics (the full-page FNV output hash) copy and
	// re-walk every composed 256 KB page — capture/test instrumentation, not
	// steady-state work. Default OFF; the render probes opt in.
	void set_capture_diagnostics(bool p_enabled) {
		capture_diagnostics_ = p_enabled;
	}
	bool is_capture_diagnostics_enabled() const {
		return capture_diagnostics_;
	}
	// Appends one already-resolved permanent record and retires exactly the
	// occupied cache pages its inclusive Q16 bounds touch.
	bool append_terrain_scorch(
			const opennova::terrain::TerrainScorchEntry &p_entry);
	// Mission/replay reset: remove the permanent list and make every resident
	// page cold so no prior compiled scorch survives the lifecycle boundary.
	void clear_terrain_scorches();

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
	void _invalidate_page(const opennova::TerrainTilePageKey &p_page);
	void _retire_ready_scorch_overlaps(
			const opennova::terrain::TerrainScorchEntry &p_entry);
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
	opennova::terrain::TerrainScorchRegistry scorch_registry_;
	bool scorch_textures_ready_ = false;
	uint64_t scorch_records_rejected_ = 0;
	uint64_t scorch_page_invalidations_ = 0;
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
	uint64_t frame_output_pages_ = 0;
	uint64_t frame_output_hash_ = 0;
};

} // namespace godot
