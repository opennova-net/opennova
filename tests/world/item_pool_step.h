// One entity-update step of an item row's pool, shared by the item rigs
// (destruction, match, item events): every pool-1 row's own visit
// (World::update_pool1_slot), or the pool-2/3 cohort walk. Header-only test
// infrastructure.
// [orig: Entity_UpdatePool1Slot @0x4B8DD0; Entity_UpdateAllEntities @0x4C2244 /
//  @0x4C230C]
#pragma once

#include <runtime/world/destruction.h>
#include <runtime/world/system.h>
#include <runtime/world/world.h>

#include <cstddef>

namespace test_world {

inline void step_item_pool(opennova::world::World &w, int pool) {
	namespace ow = opennova::world;
	if (pool != 1) {
		ow::tick_item_event_pool(w, pool);
		return;
	}
	ow::TickContext ctx;
	ctx.world = &w;
	ctx.is_authority = true;
	ctx.logic_tick = w.logic_tick;
	for (std::size_t slot = 0; slot < w.registry.pool_capacity(1); ++slot)
		if (ow::Entity *row = w.registry.get(ow::EntityHandle::make(1, static_cast<int>(slot))))
			w.update_pool1_slot(*row, ctx);
}

} // namespace test_world
