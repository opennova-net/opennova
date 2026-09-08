#pragma once

#include <cmath>
#include <cstdint>

namespace opennova::world {

// The kind-1 death quadrant ends with the source/target atan2. [orig: @0x4E66B1]
// Historical damage-queue predicates. The IDB name
// Entity_ApplyVehicleCollisionDamage at 0x4E6620 describes the kind-1 melee
// callback, not the movement resolver roadkill arm. Its source argument is
// a damage record, not a vehicle entity. The live roadkill predicates below
// are consumed by collision_resolve.cpp at Entity_MovementCollisionResolver.
// These kind-1 predicates remain pinned by vehicle_collision_damage_test.
// [orig: @0x4E6620]
// [orig: @0x4EAD80]
// [orig: @0x4EADC6]
// [orig: @0x4EAE1C]
// [orig: @0x4e672e]
// [orig: @0x4e668d]
// [orig: @0x4e6735]
// [orig: @0x4e66c6]
// [orig: @0x4e6752]
// [orig: @0x4e6773]
// Only a PERSON takes kind-1 damage [orig: the `itemDef->type ==
// ItemType_Person` test @0x4E6633 (jnz @0x4e663e)]. A vehicle hitting another
// vehicle goes through the physics contact path instead, not this one.
inline constexpr int32_t kCollisionDamageTargetType = 0; // ItemType_Person

// Two flags gate it, and both are refusals rather than conditions
// [orig: the `& 2` test @0x4E6647 and the `& 0x4000000` test @0x4E665F]:
//   * flag 0x2 — already dead/dying: a corpse is not run over twice;
//   * flag 0x4000000 — immune to this path entirely.
inline constexpr uint32_t kEntityFlagDeadOrDying = 0x2u;
inline constexpr uint32_t kEntityFlagNoCollisionDamage = 0x4000000u;

// The self-hit guard: an entity never applies kind-1 damage to itself
// [orig: the `targetEntity != attacker` test @0x4E6657, attacker = the
// source's +0x20 entity]. Without it a driver
// whose own hull resolves back to them takes their own hit.
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
// @0x4E66CC..0x4E6707]. So a dead source still credits whoever
// killed the source, rather than crediting a corpse or nobody. The walk is
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

// The damage flag the target latches on this hit [orig: the `|= 0x400` on
// +0x2C @0x4E6740], set BEFORE the death callback so a handler sees it.
inline constexpr uint32_t kDamageFlagCollision = 0x400u;

// The death callback's cause code for a collision [orig: the literal 3 passed
// at @0x4E6752, `deathCallback(target, 3, 0)`].
inline constexpr int kDeathCauseCollision = 3;

// THE RUN-OVER KILL — the movement resolver's own leg, run for the body being
// resolved against the LAST candidate that pushed it in the first force pass
// [orig: Entity_MovementCollisionResolver @0x4b2bd0, the block
// @0x4b37c2..0x4b39f7; the pusher is the var_80 store inside the pass-0 force
// fold @0x4b30a9]. Every gate is a refusal:
//   * the pusher is a VEHICLE (itemDef type 1 @0x4b37dc) that is not the
//     victim's ground link (@0x4b37e8 — a rider is never crushed by its ride)
//     and is not dead (@0x4b37f3);
//   * the victim has health and is not dead (@0x4b37fd / @0x4b380b);
//   * BOTH the pusher's own tick displacement and its displacement RELATIVE to
//     the victim exceed 0x27B0 (the planar lengths @0x4b3815..0x4b3890, the
//     compares @0x4b38e5..0x4b38f8) — a parked hull nudged into never kills;
//   * a same-team pusher kills only when either side's AI slot carries the
//     BERSERK behavior bit 0x200 (@0x4b389d..0x4b38d2);
//   * the authority alone kills (@0x4b38d8);
//   * an indestructible victim (Flags 0x4000000) survives (@0x4b3901).
// The kill: death anim = cause 2 (explosive) at bone 2 by the approach
// quadrant, +0x178 credits the pusher's OCCUPANT, Health = 0, then
// Score_ProcessKillEvent (@0x4b39b2..0x4b39e2).
inline constexpr int32_t kRunOverSpeedThreshold = 0x27B0;
inline constexpr int kRunOverDeathBone = 2;
inline constexpr int kRunOverDeathCause = 2; // explosive family (death_grenade_*)

inline bool run_over_kill_applies(bool pusher_is_vehicle, bool pusher_is_victims_ground,
		bool pusher_dead, int32_t victim_health, bool victim_dead,
		int32_t pusher_move_planar, int32_t relative_move_planar,
		bool same_team, bool either_berserk, bool is_authority,
		bool victim_indestructible) {
	if (!pusher_is_vehicle || pusher_is_victims_ground || pusher_dead) return false;
	if (victim_health <= 0 || victim_dead) return false;
	if (same_team && !either_berserk) return false;
	if (!is_authority) return false;
	if (relative_move_planar <= kRunOverSpeedThreshold) return false;
	if (pusher_move_planar <= kRunOverSpeedThreshold) return false;
	if (victim_indestructible) return false;
	return true;
}

// The approach quadrant [orig: @0x4b395d..0x4b39a7 — fpatan(rel dy, rel dx)
// x 2^32/2pi, `(Yaw - that + 0x1FFFFFFF) >> 30` as an UNSIGNED shift].
inline int run_over_quadrant(int32_t victim_yaw_bam, int32_t rel_dx, int32_t rel_dy) {
	const double a = std::atan2(static_cast<double>(rel_dy), static_cast<double>(rel_dx));
	const int32_t bam = static_cast<int32_t>(static_cast<int64_t>(a * 683565275.5764316));
	const uint32_t q = (static_cast<uint32_t>(victim_yaw_bam) - static_cast<uint32_t>(bam) +
			0x1FFFFFFFu) >> 30;
	return static_cast<int>(q);
}

} // namespace opennova::world
