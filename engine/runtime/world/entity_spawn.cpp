#include <runtime/world/entity_spawn.h>
#include <runtime/world/angle.h>
#include <algorithm>

namespace opennova::world {

void entity_reset_to_spawn_state(Entity &e) {
    // [orig: Entity_ResetToSpawnState @0x4B9610] Save the current pose and
    // flags before clearing death state.
    e.spawn_position = e.position;
    e.spawn_heading = bam_heading_from_mission_yaw_deg(e.yaw);
    e.spawn_flags = (e.flags | e.engine_flags) & ~kEntityFlagDead;
    // Both portable flag views represent the same retail dword.
    e.flags &= ~2u;
    e.engine_flags &= ~2u;
    // The +0x124 damage-disabled state is NOT touched here: the deploy leg that
    // calls this reset seeds its 620-tick spawn protection afterwards
    // [orig: no [esi+124h] store anywhere in @0x4B9610; the seed is
    //  Server_ProcessPlayerDeath @0x517937/@0x517952/@0x517960].
    e.health = std::max(e.health, e.health_max);
    e.alive = e.health > 0;
    e.mana = e.mana_max;
    e.section_mask = 0;
    e.last_attacker = {};
    e.dragger = {};
    e.dragger_spawn_id = 0;
    e.roll = 0;
    // The World overload selects 153 when the class has that clip and advances
    // both channels. A fresh row without a motor seeds the fallback state.
    // [orig: Entity_ResetToSpawnState anim reseed @0x4B9714]
    e.net_anim_state = 44;
    e.net_anim_pending = 0;
    e.net_anim_phase = 0;
    e.net_stance_bits = 0; // stance latches reset with the body [orig: the @0x4b9610 reseed]
}

} // namespace opennova::world
