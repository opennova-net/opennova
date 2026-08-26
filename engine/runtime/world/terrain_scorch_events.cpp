#include "world/terrain_scorch_events.h"

#include <crt/crt_rng.h>

namespace opennova::world {
namespace {

bool consumes_crt_roll(int scorch_id) noexcept {
	return scorch_id == 1 || scorch_id == 2 || scorch_id == 7;
}

uint16_t producer_roll(int scorch_id) noexcept {
	return consumes_crt_roll(scorch_id) ? crt_rand15() : 0;
}

} // namespace

bool TerrainScorchEvents::emit_standard(int32_t center_x_q16,
		int32_t center_y_q16, int scorch_id, uint32_t tick) {
	// Both retail routers choose the random texture before entering the capped
	// registry append. Record 4097 therefore still advances the CRT stream
	// [orig: rand() @ 0x6060f7 / @ 0x606122 precede Terrain_AddScorchRecord
	// @ 0x606105 / @ 0x606130; sized router @ 0x6061aa..0x6061b8].
	const terrain::TerrainScorchResolved resolved =
			terrain::resolve_standard_terrain_scorch(
					center_x_q16, center_y_q16, scorch_id,
					producer_roll(scorch_id));
	return append_resolved(resolved, tick);
}

bool TerrainScorchEvents::emit_sized(int32_t center_x_q16,
		int32_t center_y_q16, int scorch_id, int32_t half_extent_q16,
		uint32_t tick) {
	const terrain::TerrainScorchResolved resolved =
			terrain::resolve_sized_terrain_scorch(
					center_x_q16, center_y_q16, scorch_id,
					half_extent_q16, producer_roll(scorch_id));
	return append_resolved(resolved, tick);
}

bool TerrainScorchEvents::append_resolved(
		const terrain::TerrainScorchResolved &resolved, uint32_t tick) {
	if (!resolved.valid) return false;
	if (record_count_ >= terrain::kTerrainScorchCapacity) {
		++rejected_count_;
		return false;
	}
	pending_.push_back(TerrainScorchEvent{
			resolved.entry, tick, static_cast<uint64_t>(record_count_)});
	++record_count_;
	return true;
}

void TerrainScorchEvents::reset() noexcept {
	pending_.clear();
	record_count_ = 0;
	rejected_count_ = 0;
}

} // namespace opennova::world
