#pragma once

// Permanent terrain scorch records and their page-local compile plan.
//
// Retail keeps one append-only 4096-record list. PolyTrn_RenderTile walks the
// records in insertion order after mission .til overlays and before the
// heightfield DOT3/static-model alpha contributions. The Godot device owns
// texture loading and page upload; this module owns the portable record,
// routing, overlap, mip, and content-identity rules.
// [orig: Terrain_LoadScorchTextures @0x604CE0; Terrain_AddScorchRecord
// @0x605C90; Terrain_AddScorchForEffectKind @0x6060D0; Terrain_AddScorchSized
// @0x606180; PolyTrn_RenderTile
// @0x60DF39..0x60E0AF]

#include <runtime/terrain/terrain_tile_composition_cache.h>
#include <runtime/terrain/texture_preprocess.h>
#include <runtime/terrain_query/terrain_scorch_record.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace opennova::terrain {

// The record, capacity, extents, and the two retail producers live in the
// terrain_query seam header above (world reaches them there, ADR 0020).
// Slot 3 is deliberately empty in the retail table.
std::string_view terrain_scorch_texture_name(uint8_t texture_index) noexcept;

struct TerrainScorchTexture {
	// Retail asks D3DXFilterTexture for box mips and stops at a 4x4 terminal
	// level (64 -> 64,32,16,8,4; 256 -> ... -> 4).
	std::vector<Rgba8Image> mips;
	bool is_valid() const noexcept;
};

TerrainScorchTexture build_terrain_scorch_texture(
		const Rgba8Image &base);

struct TerrainScorchPagePlan {
	std::vector<TerrainScorchEntry> entries;
	uint64_t content_stamp = 0;
	bool valid = false;
};

// The identity half of a page plan: the same insertion-ordered overlap walk
// reduced to its content stamp, with no entry list built. Invalid only for a
// page level the cache cannot route.
struct TerrainScorchPageStamp {
	uint64_t content_stamp = 0;
	uint32_t entry_count = 0;
	bool valid = false;
};

class TerrainScorchRegistry {
public:
	bool append(const TerrainScorchEntry &entry);
	void clear() noexcept;
	std::size_t size() const noexcept { return entries_.size(); }
	bool full() const noexcept { return entries_.size() >= kTerrainScorchCapacity; }
	const std::vector<TerrainScorchEntry> &entries() const noexcept {
		return entries_;
	}
	// Monotonic identity of the record list: advances on every accepted
	// append and on clear, never on a rejected record. Starts at 1.
	uint64_t generation() const noexcept { return generation_; }

	// Per-frame page identity. Visits only the records bucketed into the
	// 512-unit sectors the page touches, in insertion order, and allocates
	// nothing; stamp(page).content_stamp == plan(page).content_stamp always.
	TerrainScorchPageStamp stamp(const TerrainTilePageKey &page) const;
	// The page's insertion-ordered overlap list for composition. Built on a
	// cache miss only; the per-frame path uses stamp().
	TerrainScorchPagePlan plan(const TerrainTilePageKey &page) const;

	static bool overlaps_page(const TerrainScorchEntry &entry,
			const TerrainTilePageKey &page) noexcept;

private:
	// The insertion-ordered walk over the page's candidate records. Appends
	// overlapping entries to `entries` when it is non-null; returns false for
	// an unroutable page level.
	bool collect(const TerrainTilePageKey &page,
			std::vector<TerrainScorchEntry> *entries,
			uint64_t &content_stamp, uint32_t &count) const;

	std::vector<TerrainScorchEntry> entries_;
	// Record indices per 512-unit sector cell (packed sector x/z), each list
	// ascending, i.e. in insertion order. A record whose inclusive Q16 bounds
	// touch several cells is listed in each; the walk merges by index.
	std::unordered_map<uint64_t, std::vector<uint32_t>> sector_records_;
	uint64_t generation_ = 1;
};

// Applies the already-page-filtered list in insertion order. RGB uses the
// retail DESTCOLOR/SRCCOLOR multiply blend; alpha doubles the existing target
// alpha because stage alpha selects opaque diffuse. Sampling is wrap +
// max-quality anisotropic/trilinear over the exact box mip chain.
bool compose_terrain_scorches(
		const TerrainTileCompositionJob &job,
		const TerrainScorchPagePlan &plan,
		const std::array<TerrainScorchTexture,
				kTerrainScorchTextureSlots> &textures,
		Rgba8Image &page) noexcept;

} // namespace opennova::terrain
