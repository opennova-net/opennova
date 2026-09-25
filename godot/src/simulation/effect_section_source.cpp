#include "simulation/effect_section_source.h"

#include "simulation/simulation_internal.h"

#include <runtime/world/collision.h>
#include <runtime/world/geom.h>

namespace godot {

EffectSectionSource::EffectSectionSource(Simulation *p_simulation) :
		simulation_(p_simulation) {
	if (simulation_ != nullptr && simulation_->kernel_)
		occlusion_ = &simulation_->kernel_->occlusion;
}

// The building blink volumes containing the spawn point, queried in the
// mission frame (retail Entity_QueryBlinkBoxesAtPoint at the descriptor
// position, CEffectWorld_SpawnEmitterAtPosition @ 0x5F6F5A).
void EffectSectionSource::blink_hits(const Vector3 &p_godot_position,
		std::array<uint32_t, 4> &r_hits) const {
	r_hits.fill(0u);
	if (simulation_ == nullptr || !simulation_->kernel_)
		return;
	// Godot (x, y, z) -> mission (x, -z, y), 16.16.
	const int32_t position[3] = {opennova::world::to_fixed(p_godot_position.x),
			opennova::world::to_fixed(-p_godot_position.z),
			opennova::world::to_fixed(p_godot_position.y)};
	opennova::world::BlinkAccum accum;
	simulation_->kernel_->collision.query_blink_boxes_at_point(
			simulation_->kernel_->world, position, accum);
	for (std::size_t i = 0; i < r_hits.size(); ++i)
		r_hits[i] = accum.hits[i];
}

// The raw word by the hit's pool-2 entity index: no forced bits (retail
// sub_5F6D10 reads g_BuildingSectionVisMask directly).
std::uint32_t EffectSectionSource::section_mask(std::int32_t p_pool_entity_index) const {
	return occlusion_ != nullptr
			? occlusion_->section_mask(opennova::world::EntityHandle::make(2, p_pool_entity_index))
			: 0u;
}

} // namespace godot
