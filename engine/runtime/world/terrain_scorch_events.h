#pragma once

#include <terrain_query/terrain_scorch_record.h>

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

// The simulation-side mirror of retail's append-only 4096-record registry.
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
	void clear_pending() noexcept { pending_.clear(); }
	void reset() noexcept;

	std::size_t record_count() const noexcept { return record_count_; }
	uint64_t rejected_count() const noexcept { return rejected_count_; }

private:
	bool append_resolved(const terrain::TerrainScorchResolved &resolved,
			uint32_t tick);

	std::vector<TerrainScorchEvent> pending_;
	std::size_t record_count_ = 0;
	uint64_t rejected_count_ = 0;
};

} // namespace opennova::world
