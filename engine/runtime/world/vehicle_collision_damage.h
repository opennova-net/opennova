#pragma once

#include <cstdint>

namespace opennova::world {

// VEHICLE COLLISION DAMAGE — what happens when a vehicle runs a person over
// [orig: Entity_ApplyVehicleCollisionDamage @0x4E6620].
//
// STAGED, NOT WIRED (2026-08-25 tidy): the roadkill leg is unported. Retail
// reaches it from ONE site — the damage-queue dispatcher's kind-1 arm, which
// selects this handler (over Entity_ApplyWeaponDamage /
// GameEvent_HandleMedicInteraction) for a queued vehicle-collision damage entry
// [orig: Projectile_ProcessExplosionQueue @0x4EAD80, the jumptable @0x4EADC6
// case 1 -> the callback store @0x4EAE1C, gated on
// AnimMap_IsSlotActive(playerClass, 4)]. The live owner-to-be is the engine's
// damage-queue dispatch in engine/runtime/world/round_sim.cpp (the vehicle
// movers queue the kind-1 entries), and the hit itself — the health subtract
// @0x4e672e, the death-anim pick Entity_ComputeAnimSlotIndex(cause 2, quadrant
// from the atan2 @0x4e668d..0x4e66b1) @0x4e6735, the +0x178 attacker store
// @0x4e66c6, the death callback @0x4e6752 and Score_ProcessKillEvent @0x4e6773
// — lands with that arm. The predicates below are the witnessed gates it will
// call. Consumed by tests/world/vehicle_collision_damage_test.cpp only until
// then (scripts/lint/orphan_header_check.py).
//
// This is roadkill, and it is the one damage path with no weapon behind it:
// the attacker is resolved through the vehicle's parent chain so the DRIVER
// gets the kill, not the hull.

// Only a PERSON takes collision damage [orig: the `itemDef->type ==
// ItemType_Person` test @0x4E6633 (jnz @0x4e663e)]. A vehicle hitting another
// vehicle goes through the physics contact path instead, not this one.
inline constexpr int32_t kCollisionDamageTargetType = 0; // ItemType_Person

// Two flags gate it, and both are refusals rather than conditions
// [orig: the `& 2` test @0x4E6647 and the `& 0x4000000` test @0x4E665F]:
//   * flag 0x2 — already dead/dying: a corpse is not run over twice;
//   * flag 0x4000000 — immune to this path entirely.
inline constexpr uint32_t kEntityFlagDeadOrDying = 0x2u;
inline constexpr uint32_t kEntityFlagNoCollisionDamage = 0x4000000u;

// The self-hit guard: an entity never applies collision damage to itself
// [orig: the `targetEntity != attacker` test @0x4E6657, attacker = the
// source's +0x20 entity]. Without it a driver
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
// @0x4E66CC..0x4E6707]. So a dead driver's vehicle still credits whoever
// killed the driver, rather than crediting a corpse or nobody. The walk is
// exactly TWO hops, not a loop: the attacker's own +0x178 link @0x4e66e0..
// 0x4e66ea, then that entity's +0x178 under the same test @0x4e66ff..
// 0x4e6707 — this predicate is what each hop tests.
//
// Flag 0x100 stops that walk — an entity carrying it is a terminal attribution
// even when dead.
inline constexpr uint32_t kEntityFlagTerminalAttribution = 0x100u;

inline bool attribution_walks_past(int32_t parent_health, uint32_t parent_flags) {
	return parent_health <= 0 &&
			(parent_flags & kEntityFlagTerminalAttribution) == 0u;
}

// THE KILL EVENT FIRES ON THE TRANSITION, not on the state
// [orig: `if (Health <= 0 && originalHealth > 0)` @0x4E6766; the whole hit is
// already inside the `originalHealth > 0` gate @0x4e66be]. Both halves are
// needed: without the second, every subsequent collision with an already-dead
// body would re-award the kill.
inline bool collision_kill_fires(int32_t health_before, int32_t health_after) {
	return health_after <= 0 && health_before > 0;
}

// The damage flag the target latches when run over [orig: the `|= 0x400` on
// +0x2C @0x4E6740], set BEFORE the death callback so a handler sees it.
inline constexpr uint32_t kDamageFlagCollision = 0x400u;

// The death callback's cause code for a collision [orig: the literal 3 passed
// at @0x4E6752, `deathCallback(target, 3, 0)`].
inline constexpr int kDeathCauseCollision = 3;

} // namespace opennova::world
