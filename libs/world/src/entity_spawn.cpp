#include "world/entity_spawn.h"

namespace opennova::world {

void entity_reset_to_spawn_state(Entity &e) {
    // [orig: Entity_ResetToSpawnState @ 0x4B9610] backs up the current Position into the
    // entity's spawn-point fields (pad9[124/128/132]) ...
    e.spawn_position = e.position;
    // ... then `entity->Flags &= ~2u` clears entity+36 bit 1 — the movement gate
    // (net-re §5.2b / §5.6). See entity_spawn.h for the deferred remainder of the reset.
    e.flags &= ~2u;
    e.damage_state = 0;
    // Spawn body-anim state 44 (idle2, or 153 when the class table maps it — class table
    // unmodeled) + a fresh anim channel — the wire bytes 14/15 a freshly deployed player
    // replicates. [orig: Entity_ResetToSpawnState anim reseed @0x4b9714]
    e.net_anim_state = 44;
    e.net_anim_pending = 0;
    e.net_anim_phase = 0;
    e.net_stance_bits = 0; // stance latches reset with the body [orig: the @0x4b9610 reseed]
}

} // namespace opennova::world
