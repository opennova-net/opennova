// Infantry hit-reaction state from ammo.secondary_anim (+224).
// [orig: Entity_ApplyCollisionForce @0x4AF4A0; org1 @0x4BBF8F; org2 @0x4B70DE]
#pragma once
#include <cstdint>
namespace opennova::world {
struct World;
struct Entity;
struct EntityHandle;
struct Vec3;
struct InfantryState;
class IRootMotionSource;
void apply_infantry_burn(World &world, Entity &target, uint8_t hit_type,
        const Vec3 &source, EntityHandle attacker);
// Caller owns the body selection cadence: every 4 ticks for player bodies,
// every 16 ticks for NPCs. Player timers only advance on the 16-tick boundary.
int select_infantry_burn(InfantryState &inf, const IRootMotionSource *motion,
        bool player, uint32_t tick);
} // namespace opennova::world
