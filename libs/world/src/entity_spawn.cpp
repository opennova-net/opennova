#include "world/entity_spawn.h"

namespace opennova::world {

void entity_reset_to_spawn_state(Entity &e) {
    // [orig: Entity_ResetToSpawnState @ 0x4B9610] backs up the current Position into the
    // entity's spawn-point fields (pad9[124/128/132]) ...
    e.spawn_position = e.position;
    // ... then `entity->Flags &= ~2u` clears entity+36 bit 1 — the movement gate
    // (net-re §5.2b / §5.6). See entity_spawn.h for the deferred remainder of the reset.
    e.flags &= ~2u;
}

} // namespace opennova::world
