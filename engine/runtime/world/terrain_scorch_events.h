#pragma once

#include <runtime/terrain_query/terrain_scorch_record.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::world {

// One permanent terrain-scorch insertion resolved at its retail producer.
// `mission_bounds` uses the simulation ground plane (x,y); the renderer-facing
// drain performs the one x,y -> x,-z coordinate fold.
struct TerrainScorchEvent {
	terrain::TerrainScorchEntry mission_bounds;
	uint32_t tick = 0;
	uint64_t source_order = 0;
};

// One destroyed entity's cached terrain-page invalidation, in the mission
// ground plane (x,y): the entity's position +-2 bound radii on both axes. The
// renderer-facing drain folds it to terrain x,-z like the scorch bounds.
// [orig: Terrain_InvalidateTileCacheRegion @0x605C10, reached through
// j_j_Terrain_InvalidateTileCacheRegion @0x610900 from
// Entity_ProcessDestructibleDeath @0x43FC12..0x43FC5E,
// Entity_ProcessBld2Destruction @0x43F192..0x43F1DA (the bld2 collapse),
// the crane collapse's twin block Entity_ProcessCraneCollapse
// @0x440036..0x44007E and Entity_SpawnSectionEntity
// @0x44062E..0x440670]
struct TerrainPageInvalidationEvent {
	int32_t minimum_x_q16 = 0;
	int32_t minimum_y_q16 = 0;
	int32_t maximum_x_q16 = 0;
	int32_t maximum_y_q16 = 0;
};

// The simulation-side mirror of retail's append-only 4096-record registry
// [orig: Terrain_AddScorchRecord @0x605c90 (4096-record append + cached-tile invalidation);
//  producers Terrain_AddScorchForEffectKind @0x6060d0 (rand() @0x6060f7/@0x606122 -> append
//  @0x606105/@0x606130) and Terrain_AddScorchSized @0x606180 (@0x6061aa..0x6061b8)].
// It resolves the CRT-selected texture at producer time and retains only rows
// not yet handed to the renderer. Draining presentation rows never re-opens
// registry capacity; only a mission reset does.
class TerrainScorchEvents {
public:
	bool emit_standard(int32_t center_x_q16, int32_t center_y_q16,
			int scorch_id, uint32_t tick);

    bool emit_sized(int32_t center_x_q16, int32_t center_y_q16,
            int scorch_id, int32_t half_extent_q16, uint32_t tick);

	const std::vector<TerrainScorchEvent> &pending() const noexcept {
		return pending_;
	}
	// The destroyed-entity page invalidation (no registry record, no CRT).
	void emit_page_invalidation(int32_t center_x_q16, int32_t center_y_q16,
			int32_t bound_radius_q16);
	const std::vector<TerrainPageInvalidationEvent> &pending_page_invalidations()
			const noexcept {
		return pending_page_invalidations_;
	}
	void clear_pending() noexcept {
		pending_.clear();
		pending_page_invalidations_.clear();
	}
	void reset() noexcept;

	std::size_t record_count() const noexcept { return record_count_; }
	uint64_t rejected_count() const noexcept { return rejected_count_; }

private:
	bool append_resolved(const terrain::TerrainScorchResolved &resolved,
			uint32_t tick);

	std::vector<TerrainScorchEvent> pending_;
	std::vector<TerrainPageInvalidationEvent> pending_page_invalidations_;
	std::size_t record_count_ = 0;
	uint64_t rejected_count_ = 0;
};

} // namespace opennova::world
