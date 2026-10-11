#include <runtime/world/entity_spawn.h>
#include <runtime/world/angle.h>
#include <runtime/world/world.h>
#include <algorithm>

namespace opennova::world {

int32_t max_health_with_difficulty(const World &world, const Entity &e) {
    if (!e.has_item_def) return 0; // @0x43B8AB
    return max_health_with_difficulty(world, e, e.health_max);
}

int32_t max_health_with_difficulty(const World &world, const Entity &e, int32_t hp) {
    hp = retail_signed_i16(hp); // movzx @0x43B8B4, movsx @0x43B8EC
    if (world.rules.mp_session || !e.handle.valid() || e.handle != world.cached.local_player)
        return hp; // @0x43B8AD, @0x43B8C0
    if (world.rules.difficulty == -1) return retail_signed_i16(hp + hp); // @0x43B8CE..0x43B8D8
    if (world.rules.difficulty == 1) return retail_signed_i16(hp / 2);   // @0x43B8DC..0x43B8E9
    return hp;
}

void entity_reset_to_spawn_state(Entity &e) {
    entity_reset_to_spawn_state(e, e.health_max);
}

void entity_reset_to_spawn_state(Entity &e, int32_t health_ceiling) {
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
    // The reset's one raise [orig: Entity_ResetToSpawnState @0x4B97BC -> Entity_RaiseHealthToMax
    // @0x43C290: `if (Health < max) Health = max`, a signed word compare].
    if (retail_signed_i16(e.health) < health_ceiling) e.health = health_ceiling;
    e.alive = e.health > 0;
    e.mana = e.mana_max;
    e.section_mask = 0;
    e.last_attacker = {};
    e.dragger = {};
    e.dragger_spawn_id = 0;
    e.medic_reviving = false; // [orig: the +0x1E0 clear at Game_InitNewRound @0x422740]
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
