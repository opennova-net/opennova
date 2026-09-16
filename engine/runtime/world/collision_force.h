// The ammo-driven hit reaction on a person: the burn state from
// ammo.secondary_anim (+224, `hitType`) and the collision force from
// ammo.kz_physics (+225, `forceType`) — one retail function, both halves.
// [orig: Entity_ApplyCollisionForce @0x4AF4A0; org1 @0x4BBF8F; org2 @0x4B70D9]
#pragma once
#include <cstdint>
namespace opennova::world {
struct World;
struct Entity;
struct EntityHandle;
struct Vec3;
struct InfantryState;
class IRootMotionSource;
// hit_type != 0 stamps the burn state (skipped on a dead body); force_type
// then runs regardless of hit_type and of the dead bit: 1 = walk push, 2 =
// drift, 3 = the local-player hit blackout (env hit dim), 4 = direct push.
// The velocity triple and the burn byte live on the infantry body, so an
// entity without one (no AiEntity) has no target for either half.
void apply_collision_force(World &world, Entity &target, uint8_t hit_type,
        uint8_t force_type, const Vec3 &source, EntityHandle attacker);
// Caller owns the body selection cadence: every 4 ticks for player bodies,
// every 16 ticks for NPCs. Player timers only advance on the 16-tick boundary.
int select_infantry_burn(InfantryState &inf, const IRootMotionSource *motion,
        bool player, uint32_t tick);
} // namespace opennova::world
