#pragma once

// Portable request/compiler for retail's composed terrain page cache. The
// module owns page identity and cache decisions; the Godot layer owns the
// texture array and executes returned composition jobs.
// [orig: PolyTrn_RenderTile @ 0x60DA70; 128-slot cache dword_319A2E4;
// docs/tiles/til-re.md]

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace opennova {

struct TerrainTilePageKey {
	int32_t sector_origin_x = 0;
	int32_t sector_origin_z = 0;
	int32_t page_local_x = 0; // exact origin inside the routed 512u sector
	int32_t page_local_z = 0;
	uint8_t page_lod_level = 0; // 1..4 => 512, 256, 128, 64u
};

struct TerrainTileContentStamp {
	uint64_t value = 0;
};

struct TerrainTilePageLayout {
	int texture_dimension = 0;
	int world_span = 0;
	float texels_per_world_unit = 0.0f;

	int texel_footprint(int world_units) const noexcept;
};

struct TerrainTilePageBinding {
	TerrainTilePageKey page;
	uint16_t layer = 0;
	uint64_t generation = 0;
	bool ready = false;
	// ready with a superseded payload: the layer serves its last-published
	// pixels while the replacement generation composes. Never true when ready
	// is false; cleared by publish and by explicit invalidation/eviction.
	bool stale = false;
};

struct TerrainTileCompositionJob {
	TerrainTilePageBinding target;
	int32_t tile_index = -1;
	int32_t source_origin_x = 0; // exact 0..1023 source-atlas origin
	int32_t source_origin_z = 0;
	TerrainTileContentStamp content;
	TerrainTilePageLayout layout;
};

struct TerrainTileCompositionRequest {
	TerrainTilePageKey page;
	int32_t tile_index = -1;
	int32_t source_origin_x = 0;
	int32_t source_origin_z = 0;
	TerrainTileContentStamp content;
};

struct TerrainTileCompositionDecision {
	TerrainTilePageBinding binding;
	std::optional<TerrainTileCompositionJob> job;
};

// Portable scheduling policy for high-quality CPU page work. A demand frame is
// the render compiler frame that made the page visible; sequence is the FIFO
// order within that frame. Payload storage stays in the device adapter.
struct TerrainTileCompositionDemand {
	uint16_t layer = 0;
	uint64_t generation = 0;
	uint64_t frame_id = 0;
	uint64_t sequence = 0;
};

struct TerrainTileCompositionDemandEnqueueResult {
	bool accepted = false;
	bool rejected_stale = false;
	std::vector<uint64_t> removed_sequences;
};

class TerrainTileCompositionDemandQueue {
public:
	TerrainTileCompositionDemandEnqueueResult enqueue(
			const TerrainTileCompositionDemand &demand,
			std::size_t maximum_size);
	std::vector<uint64_t> remove_older_generations(
			uint16_t layer, uint64_t generation);
	std::optional<TerrainTileCompositionDemand> take_next();
	void clear() noexcept { demands_.clear(); }
	std::size_t size() const noexcept { return demands_.size(); }
	bool empty() const noexcept { return demands_.empty(); }

private:
	std::vector<TerrainTileCompositionDemand> demands_;
};

// A world point plus its routed sector identity. The sector fields prevent a
// repeated source quadrant from borrowing a cache page from another world
// placement; world_x/world_z select among that sector's nested ready pages.
struct TerrainTileResidentPoint {
	int32_t sector_origin_x = 0;
	int32_t sector_origin_z = 0;
	float world_x = 0.0f;
	float world_z = 0.0f;
};

class TerrainTileCompositionCache {
public:
	// The active-quality retail cache layout. Low-quality 128x128 pages are a
	// separate future policy, not a runtime mutation of this cache instance.
	// [orig: 128-slot scan bound @ 0x60db40 over the dword_319A2E4 slot
	// array; the page RT dimension is the active-quality global dword_31A00D4
	// (256).]
	static constexpr int kCapacity = 128;
	static constexpr int kDimension = 256;

	// [orig: span = 1024 >> lodLevel @ 0x60dbf6/0x60dd87 in
	// PolyTrn_RenderTile.]
	static constexpr int page_world_span(uint8_t page_lod_level) noexcept {
		return page_lod_level >= 1 && page_lod_level <= 4
				? 1024 >> page_lod_level
				: 0;
	}

	// Starts the binding lifetime for one deferred render frame. Repeating the
	// same id is idempotent; a different id releases the prior frame's pins.
	// Successful request()/best_ready() bindings are protected from layer
	// reuse until the next frame begins. If all 128 layers are pinned, a new
	// miss returns null rather than invalidating an earlier draw binding.
	void begin_frame(uint64_t frame_id) noexcept;

	// Returns null only for a page level outside 1..4. A returned job reserves
	// its layer/generation until publish(); repeated requests while that job is
	// pending return the same not-ready binding without duplicating the job.
	// Exact hits and successful best_ready() lookups refresh strict LRU age.
	// A valid miss also returns null when every layer is pinned by begin_frame().
	std::optional<TerrainTileCompositionDecision> request(
			const TerrainTileCompositionRequest &request);
	// Publishes only the still-current target generation. Eviction,
	// invalidation, or a newer request makes an older job fail closed.
	// Device adapters call can_publish() immediately before mutating a leased
	// texture layer; cache ownership stays on the render thread, so a true
	// result remains current until that adapter calls publish().
	bool can_publish(const TerrainTileCompositionJob &job) const noexcept;
	bool publish(const TerrainTileCompositionJob &job) noexcept;
	bool invalidate(const TerrainTilePageKey &page) noexcept;
	void invalidate_all() noexcept;
	// Drops every resident identity and frame pin for mission/device changes.
	// Per-layer generations advance so outstanding pre-clear jobs stay stale.
	void clear() noexcept;
	// Chooses the finest ready half-open containing page. During an active
	// frame, only pages selected by request() in that same frame are eligible;
	// retained prior-frame pages must not leak stale LOD/content into foliage.
	// Without begin_frame(), all residents remain eligible for cold diagnostics.
	// Same-level overlap uses newest local access, then the lowest layer, solely
    // for deterministic cache behavior; no retail tie-break is claimed for that
	// otherwise-degenerate case.
	std::optional<TerrainTilePageBinding> best_ready(
			const TerrainTileResidentPoint &point) noexcept;

private:
	struct Slot {
		bool occupied = false;
		bool ready = false;
		bool pending = false;
		// The published payload predates the current identity (see
		// TerrainTilePageBinding::stale). Only a content/source re-target of a
		// ready slot sets it; explicit invalidation and eviction drop the
		// payload outright instead (fail-closed wins over stale-serving).
		bool stale = false;
		TerrainTilePageKey page;
		int32_t tile_index = -1;
		int32_t source_origin_x = 0;
		int32_t source_origin_z = 0;
		TerrainTileContentStamp content;
		uint64_t generation = 0;
		uint64_t last_touch = 0;
		bool pinned = false;
		// Spatial consumers may borrow only pages selected by request() in the
		// active deferred frame. The explicit boolean keeps frame id 0 valid and
		// prevents a later reused id from reviving an older selection.
		uint64_t selected_frame_id = 0;
		bool selected_in_frame = false;
	};

	std::array<Slot, kCapacity> slots_{};
	uint16_t used_ = 0;
	uint64_t touch_clock_ = 0;
	uint64_t frame_id_ = 0;
	bool frame_active_ = false;
};

} // namespace opennova
