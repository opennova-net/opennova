#include <runtime/world/entity_spawn.h>
#include <runtime/world/angle.h>
#include <runtime/world/world.h>
#include <algorithm>

namespace opennova::world {

// [orig: Entity_GetMaxHealthWithDifficulty @0x43B8A0]
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

} // namespace opennova::world
