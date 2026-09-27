#pragma once

// Portable port of retail's composed terrain page cache: the 128 32-byte
// records PolyTrn_RenderTile keys pages by, the per-frame claim of the least
// recently used record, the time-of-day refresh, the spatial invalidations,
// and the spatial lookup. The Godot layer owns the texture array and composes
// every job the frame's sweep returns before the terrain draws.
// [orig: PolyTrn_RenderTile @ 0x60DA70; the record array dword_319A2E0
// (lod +0x00, packed coordinate +0x04, sector +0x08/+0x0C, last use +0x10,
// compose frame +0x14, TOD epoch +0x18, render target +0x1C);
// docs/terrain/terrain-re.md]

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
	uint8_t page_lod_level = 0; // 0 = shared flat page; 1..4 => 512, 256, 128, 64u
};

inline bool same_page(const TerrainTilePageKey &a, const TerrainTilePageKey &b) noexcept {
	return a.sector_origin_x == b.sector_origin_x &&
			a.sector_origin_z == b.sector_origin_z &&
			a.page_local_x == b.page_local_x &&
			a.page_local_z == b.page_local_z &&
			a.page_lod_level == b.page_lod_level;
}

// A content identity the static-shadow planner memoizes its per-page plans
// under (not part of the page identity: retail keys pages spatially).
struct TerrainTileContentStamp {
	uint64_t value = 0;
};

struct TerrainTilePageLayout {
	int texture_dimension = 0;
	int world_span = 0;
	float texels_per_world_unit = 0.0f;

	int texel_footprint(int world_units) const noexcept;
};

// Highest-quality retail c7/c8 page projection reduced into presentation
// world. g_FoliageWindSwayVS transforms the original (pre-wind) vertex into
// D3D world (Godot Z, Y, X), then c7/c8 undo the packed page origin and scale
// by 1/(1024 >> lod). The reduced result is therefore exactly
// ((world_x-origin_x), (world_z-origin_z)) * inverse_world_span.
// [orig: Foliage_RenderDetailPatches @0x60A1DE..0x60A34F; c7/c8 uploads
// @0x6006AB..0x600704; g_FoliageWindSwayVS source @0x7DE648 (assembled @0x5ff691)]
struct TerrainTilePageProjection {
	float world_origin_x = 0.0f;
	float world_origin_z = 0.0f;
	float inverse_world_span = 0.0f;
	float world_span = 0.0f;

	std::array<float, 2> project(float world_x, float world_z) const noexcept;
};

struct TerrainTilePageBinding {
	TerrainTilePageKey page;
	uint16_t layer = 0;
	uint64_t generation = 0;
	bool ready = false;
};

struct TerrainTileCompositionJob {
	TerrainTilePageBinding target;
	int32_t tile_index = -1;
	int32_t source_origin_x = 0; // exact 0..1023 source-atlas origin
	int32_t source_origin_z = 0;
	TerrainTilePageLayout layout;
};

// One visible patch's page: the record identity (level, packed source
// coordinate, routed sector) plus the mesh tile it draws.
struct TerrainTileCompositionRequest {
	TerrainTilePageKey page;
	int32_t tile_index = -1;
	int32_t source_origin_x = 0;
	int32_t source_origin_z = 0;
};

struct TerrainTileCompositionDecision {
	TerrainTilePageBinding binding;
	std::optional<TerrainTileCompositionJob> job;
};

// Portable scheduling policy for high-quality CPU page work. A demand frame is
// the render compiler frame that made the page visible; sequence is the FIFO
// order within that frame. Payload storage stays in the device binding.
struct TerrainTileCompositionDemand {
	uint16_t layer = 0;
	uint64_t generation = 0;
	uint64_t frame_id = 0;
	uint64_t sequence = 0;
};

struct TerrainTileCompositionDemandEnqueueResult {
	bool accepted = false;
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

// A world point a page consumer (MATCHTERRAIN material, detail foliage) needs
// the resident page for.
struct TerrainTileResidentPoint {
	float world_x = 0.0f;
	float world_z = 0.0f;
};

class TerrainTileCompositionCache {
public:
	// The active-quality retail cache layout. Low-quality 128x128 pages are a
	// separate future policy, not a runtime mutation of this cache instance.
	// [orig: 128-record scan bound @ 0x60db40 over dword_319A2E0; the page RT
	// dimension is the active-quality global dword_31A00D4 (256).]
	static constexpr int kCapacity = 128;
	static constexpr int kDimension = 256;
	// Creation-time clear of every page render target. Retail clears each of
	// the 128 tile RTs to D3DCOLOR 0xFFFF6060 (ARGB: A=FF R=FF G=60 B=60) with
	// z 0.99994999 as it allocates them, then seeds the slot key/UV sentinels
	// with 0x12345678 and the indices with -1; one extra square target of
	// dimension dword_31A00D0 (the model-shadow target) follows and is not
	// this cache's. The Godot device binding fills its blank layers with this
	// colour. [orig: Terrain_CreateTileCacheTargets @ 0x604DD0, GTexRT_SelectThunk(0x12345678,
	// rt, -40864, 0.99994999) per slot]
	static constexpr uint32_t kTileClearColorArgb = 0xFFFF6060u;

	// [orig: span = 1024 >> lodLevel, PolyTrn_RenderTile
	// @ 0x60DBA5..0x60DBAA.]
	static constexpr int page_world_span(uint8_t page_lod_level) noexcept {
		return page_lod_level <= 4
				? 1024 >> page_lod_level
				: 0;
	}

	// Returns the max-quality page projection. Terrain's flat mesh decoder
	// zeros primary UVs; zero_primary_uv reduces that stream to a zero scale,
	// while shadow/other geometric projections keep the full page extent.
	// The retail failed-vertex-
	// shader fallback that used inverse-view rows is deliberately not exposed.
	static std::optional<TerrainTilePageProjection> page_projection(
			const TerrainTilePageKey &page, bool zero_primary_uv = false) noexcept;

	// One PolyTrn_RenderFrame: the frame counter advances, and the records the
	// frame claims stamp `tod_epoch` (g_EnvTodEpoch).
	// [orig: dword_319FC04 += 1 @ 0x60EAE8]
	void begin_frame(uint32_t tod_epoch) noexcept;
	uint32_t frame() const noexcept { return frame_; }

	// PolyTrn_RenderTile's cache half. A resident record with the same
	// identity is a hit: its last use refreshes and no job returns. A miss
	// claims the record least recently used, but only one unused in this
	// frame and the previous one, and returns its composition job; with no
	// such record the page is not composed this frame (null).
	std::optional<TerrainTileCompositionDecision> request(
			const TerrainTileCompositionRequest &request);
	// Terrain_EvictOldestTodStaleTile: retires the record composed longest ago among
	// those stamped with an older TOD epoch and composed more than one frame
	// ago, so the next sweep recomposes it under the current light. False when
	// no record qualifies.
	bool evict_one_tod_stale() noexcept;
	// PolyTrn_RenderFrame's page sweep over the frame's visible list, in list
	// order: every miss claims a record. A sweep that composes nothing retires
	// one TOD-stale record and sweeps again, so a visible page baked under an
	// older light recomposes that same frame. Returns every claimed job; the
	// embedder composes them all before the terrain draws.
	std::vector<TerrainTileCompositionJob> sweep(
			const std::vector<TerrainTileCompositionRequest> &visible);
	// PolyTrn_BindStageTextures: the exact-identity record a patch draws with
	// (refreshing its last use), or null when the page is not composed.
	std::optional<TerrainTilePageBinding> bind(
			const TerrainTileCompositionRequest &request) noexcept;
	// TerrainTile_CacheLookup (MATCHTERRAIN) / Terrain_FindSectorPatchRT
	// (detail foliage): the first resident record, in record order, whose
	// packed coordinate matches the point's at granularity 32, then 64 ... 512
	// units within the point's sector. The record may be coarser or finer than
	// the point's own page, and a coarse-granularity match can return a page
	// that does not contain the point.
	std::optional<TerrainTilePageBinding> lookup(
			const TerrainTileResidentPoint &point) noexcept;
	// The composed page a layer holds, if any (diagnostics; no last-use stamp).
	std::optional<TerrainTilePageBinding> resident_layer(uint16_t layer) const noexcept;

	// Publishes only the still-current target generation. Eviction,
	// invalidation, or a newer claim makes an older job fail closed.
	// Device bindings call can_publish() immediately before mutating a leased
	// texture layer; cache ownership stays on the render thread, so a true
	// result remains current until that binding calls publish().
	bool can_publish(const TerrainTileCompositionJob &job) const noexcept;
	bool publish(const TerrainTileCompositionJob &job) noexcept;
	// Retires one page whose composition failed.
	bool invalidate(const TerrainTilePageKey &page) noexcept;
	// The inclusive fixed-point page test the scorch append and the destroyed-
	// entity invalidation share. A rectangle exactly on a shared page edge
	// retires both pages even though half-open raster coverage affects one.
	static bool page_overlaps_q16(const TerrainTilePageKey &page,
			int32_t minimum_x_q16, int32_t minimum_z_q16,
			int32_t maximum_x_q16, int32_t maximum_z_q16) noexcept;
	// Retires every resident page the rectangle overlaps.
	std::size_t invalidate_overlapping_q16(
			int32_t minimum_x_q16, int32_t minimum_z_q16,
			int32_t maximum_x_q16, int32_t maximum_z_q16) noexcept;
	// Terrain_ResetTileCache: every record empties and becomes claimable; the
	// frame counter keeps running (it is never reset). Per-layer generations
	// advance so outstanding pre-reset jobs stay stale.
	// [orig: Terrain_ResetTileCache @ 0x605FB0..0x605FD2 / Terrain_ResetTileCache_0 @ 0x60C640]
	void invalidate_all() noexcept;

private:
	struct Slot {
		bool lod_valid = false;   // +0x00 != -1
		bool resident = false;    // +0x04 != -1
		bool ready = false;       // composed into its layer
		TerrainTilePageKey page;
		int32_t tile_index = -1;
		int32_t source_origin_x = 0;
		int32_t source_origin_z = 0;
		uint32_t last_use = 0;       // +0x10
		uint32_t compose_frame = 0;  // +0x14
		uint32_t tod_epoch = 0;      // +0x18
		uint64_t generation = 0;
	};

	static bool same_identity(const Slot &slot,
			const TerrainTileCompositionRequest &request) noexcept;
	TerrainTilePageBinding binding(const Slot &slot, uint16_t layer) const noexcept;
	void retire(Slot &slot) noexcept;

	std::array<Slot, kCapacity> slots_{};
	uint32_t frame_ = 0;
	uint32_t tod_epoch_ = 0;
};

} // namespace opennova
