#pragma once

#include <cstdint>

namespace opennova::world {

// VEHICLE COLLISION DAMAGE — what happens when a vehicle runs a person over
// [orig: Entity_ApplyVehicleCollisionDamage @0x4E6620].
//
// This is roadkill, and it is the one damage path with no weapon behind it:
// the attacker is resolved through the vehicle's parent chain so the DRIVER
// gets the kill, not the hull.

// Only a PERSON takes collision damage [orig: the ItemType_Person test
// @0x4E663A]. A vehicle hitting another vehicle goes through the physics
// contact path instead, not this one.
inline constexpr int32_t kCollisionDamageTargetType = 0; // ItemType_Person

// Two flags gate it, and both are refusals rather than conditions
// [orig: the `& 2` test @0x4E6644 and the `& 0x4000000` test @0x4E6656]:
//   * flag 0x2 — already dead/dying: a corpse is not run over twice;
//   * flag 0x4000000 — immune to this path entirely.
inline constexpr uint32_t kEntityFlagDeadOrDying = 0x2u;
inline constexpr uint32_t kEntityFlagNoCollisionDamage = 0x4000000u;

// The self-hit guard: an entity never applies collision damage to itself
// [orig: the `targetEntity != attacker` test @0x4E6656]. Without it a driver
// whose own hull resolves back to them takes their own roadkill.
inline bool collision_damage_applies(int32_t target_type, uint32_t target_flags,
		bool target_is_attacker) {
	if (target_type != kCollisionDamageTargetType) return false;
	if ((target_flags & kEntityFlagDeadOrDying) != 0u) return false;
	if (target_is_attacker) return false;
	if ((target_flags & kEntityFlagNoCollisionDamage) != 0u) return false;
	return true;
}

// ATTRIBUTION WALKS UP, NOT DOWN. The credited attacker is resolved through
// the parent chain, and when that parent is itself DEAD the walk continues to
// ITS attacker [orig: the `attacker->Health <= 0 && !(Flags & 0x100)` arm
// @0x4E6674..]. So a dead driver's vehicle still credits whoever killed the
// driver, rather than crediting a corpse or nobody.
//
// Flag 0x100 stops that walk — an entity carrying it is a terminal attribution
// even when dead.
inline constexpr uint32_t kEntityFlagTerminalAttribution = 0x100u;

inline bool attribution_walks_past(int32_t parent_health, uint32_t parent_flags) {
	return parent_health <= 0 &&
			(parent_flags & kEntityFlagTerminalAttribution) == 0u;
}

// THE KILL EVENT FIRES ON THE TRANSITION, not on the state
// [orig: `if (Health <= 0 && originalHealth > 0)` @0x4E66C6]. Both halves are
// needed: without the second, every subsequent collision with an already-dead
// body would re-award the kill.
inline bool collision_kill_fires(int32_t health_before, int32_t health_after) {
	return health_after <= 0 && health_before > 0;
}

// The damage flag the target latches when run over [orig: the `|= 0x400`
// @0x4E66A9], set BEFORE the death callback so a handler sees it.
inline constexpr uint32_t kDamageFlagCollision = 0x400u;

// The death callback's cause code for a collision [orig: the literal 3 passed
// at @0x4E66B6].
inline constexpr int kDeathCauseCollision = 3;

} // namespace opennova::world
