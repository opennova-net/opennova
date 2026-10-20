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
struct FixedVec3;
struct InfantryState;
struct AmmoTableEntry;
class IRootMotionSource;
// hit_type != 0 stamps the burn state (skipped on a dead body); force_type
// then runs regardless of hit_type and of the dead bit: 1 = walk push, 2 =
// drift, 3 = the local-player hit blackout (env hit dim), 4 = direct push.
// The velocity triple and the burn byte live on the infantry body, so an
// entity without one (no AiEntity) has no target for either half.
void apply_collision_force(World &world, Entity &target, uint8_t hit_type,
        uint8_t force_type, const FixedVec3 &source, EntityHandle attacker);

// The body half of the same function over the fields it writes: the hit-type
// stamp and its moveTimer, then the force modes 1, 2 and 4's velocity kicks.
// `pos` and `heading` are the body's Position x/y (16.16) and Yaw (BAM32),
// `flags` its Flags word; `owner_group` is the attacker's commandGroup, or
// null for an ownerless event. The moveTimer is the player bit's or the
// non-player's field (the two the world's body keeps apart). Force 3 (the
// local player's hit blackout) is the caller's: the return is its damage
// multiplier, 3 when the source lies within 90 degrees of the yaw, else 4.
// [orig: Entity_ApplyCollisionForce @0x4AF4A0]
int collision_force_body(const int32_t pos[2], int32_t heading, uint32_t flags,
        const int32_t *owner_group, int32_t group, uint8_t hit_type, uint8_t force_type,
        const FixedVec3 &source, uint8_t &r_hit_type, int32_t &r_player_move_timer,
        int32_t &r_move_timer, int32_t r_vel[3]);

// The hit-time body roll: a projectile hit on a torso-stack bone (< 5) tips
// bodyRoll (+0x94) about 8 degrees toward the shot, front quadrant positive,
// rear negative; the side quadrants leave it. The callers gate on the body's
// dead bit. [orig: OrganicClass_HandleEvent @0x40755E..0x407575;
// Entity_HandleDamageAndTriggerZones @0x4078C6..0x407918]
void hit_body_roll(int32_t &r_body_roll, int32_t bone, int quadrant);

// The ammo REACTION notify, the sibling function beside the force applicator
// [orig: Entity_OnDamageReceived @0x4AF800]. Its head is the LOCAL player's
// cosmetic half and is what lives here: an ammo whose kz_physics byte (+225)
// reads 3 raises the white hit flash to a FLOOR of 128 [orig: @0x4af828..0x4af82a],
// and an ammo with a non-zero kz_damage word (+46) adds 20 to the camera shake
// [orig: @0x4af830..0x4af84b] -- the shake arm sits ahead of any damage gate,
// so an explosive NEAR-MISS that deals nothing still shakes. `ammo` may be null
// (retail's `ammoSource != 0` head gate).
// Its TAIL -- wasHit / damageTimer / lastAttacker on any victim
// [orig: @0x4af85b..0x4af878] -- is ported on the RoundHit drain,
// AiSystem::apply_round_hits, so it is deliberately NOT repeated here.
void entity_on_damage_received(World &world, const Entity &victim,
        const AmmoTableEntry *ammo);
// Caller owns the body selection cadence: every 4 ticks for player bodies,
// every 16 ticks for NPCs. Player timers only advance on the 16-tick boundary.
int select_infantry_burn(InfantryState &inf, const IRootMotionSource *motion,
        bool player, uint32_t tick);
} // namespace opennova::world
