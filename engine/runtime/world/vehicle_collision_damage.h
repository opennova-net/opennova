#pragma once

#include <cmath>
#include <cstdint>

namespace opennova::world {

// Two retail contact-damage legs share these predicates.
//
// THE KIND-1 (KNIFE) KILL-ZONE CALLBACK. The explosion drain installs
// Entity_ApplyVehicleCollisionDamage for entry type 1 [orig: @0x4EAE1C]; the
// IDB name is a misnomer, the entry type being the knife's kztype 1 (the
// instantkillzone spawn queues it @0x4EC20E). Its source argument is the queue
// entry, not a vehicle entity. Ported as entity_apply_melee_damage
// (world/destruction.cpp). [orig: Entity_ApplyVehicleCollisionDamage @0x4E6620]
//
// THE RUN-OVER KILL AND BUMP SOUND of the movement resolver, ported in
// CollisionWorld::resolve_entity (world/collision_resolve.cpp).
// [orig: Entity_MovementCollisionResolver @0x4B2BD0, @0x4B37C2..0x4B3A5C]

// Only a PERSON takes kind-1 damage [orig: the `itemDef->type ==
// ItemType_Person` test @0x4E6633 (jnz @0x4e663e)]; entity item_type carries
// the retail ItemDef type value.
inline constexpr int32_t kCollisionDamageTargetType = 3; // ItemType_Person

// Two flags gate it, and both are refusals rather than conditions
// [orig: the `& 2` test @0x4E6647 and the `& 0x4000000` test @0x4E665F]:
//   * flag 0x2 — already dead/dying;
//   * flag 0x4000000 — immune to this path entirely.
inline constexpr uint32_t kEntityFlagDeadOrDying = 0x2u;
inline constexpr uint32_t kEntityFlagNoCollisionDamage = 0x4000000u;

// The self-hit guard: an entity never applies kind-1 damage to itself
// [orig: the `targetEntity != attacker` test @0x4E6657, attacker = the
// source's +0x20 entity]. Without it the attacker standing inside his own
// knife's radius takes his own hit.
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
// [orig: `if (Health <= 0 && originalHealth > 0)` @0x4E6757..0x4E6766; the
// whole hit is already inside the `originalHealth > 0` gate @0x4e66be]. Both
// halves are needed: without the second, every later hit on an already-dead
// body would re-award the kill.
inline bool collision_kill_fires(int32_t health_before, int32_t health_after) {
	return health_after <= 0 && health_before > 0;
}

// The damage flag the target latches on this hit — the knife cause bit the
// death classifier and the revive window read [orig: the `|= 0x400` on +0x2C
// @0x4E6740], set BEFORE the death callback so a handler sees it.
inline constexpr uint32_t kDamageFlagCollision = 0x400u;

// The death callback's event code for a kind-1 hit [orig: the literal 3 passed
// at @0x4E6749, `deathCallback(target, 3, 0)` @0x4E6752].
inline constexpr int kDeathCauseCollision = 3;

// The kind-1 death anim: cause 1 at bone 2 by the approach quadrant
// [orig: Entity_ComputeAnimSlotIndex(target, 2, quadrant, 1) @0x4E6723..0x4E6735].
inline constexpr int kCollisionDeathBone = 2;
inline constexpr int kCollisionDeathCause = 1;

// The approach quadrant these legs share: (Yaw - atan2BAM(dy, dx) + bias) >> 30
// as an UNSIGNED shift, where (dx, dy) points from the victim toward the
// damage source (fpatan over 16.16 deltas, x 2^32/2pi, fistp). The blast and
// run-over legs bias by 0x1FFFFFFF, the kind-1 callback by 0x20000000.
inline int approach_quadrant(int32_t victim_yaw_bam, int32_t dx, int32_t dy, uint32_t bias) {
	const double a = std::atan2(static_cast<double>(dy), static_cast<double>(dx));
	const int32_t bam = static_cast<int32_t>(static_cast<int64_t>(a * 683565275.5764316));
	const uint32_t q = (static_cast<uint32_t>(victim_yaw_bam) - static_cast<uint32_t>(bam) +
			bias) >> 30;
	return static_cast<int>(q);
}

// [orig: `sub edi, edx; add edi, 20000000h` @0x4E66AF..0x4E66B1, `shr edi, 1Eh`
// @0x4E6725 over the entry-minus-target delta @0x4E666A..0x4E6672]
inline constexpr uint32_t kCollisionQuadrantBias = 0x20000000u;

// THE RUN-OVER KILL — the movement resolver's own leg, run for the body being
// resolved against the LAST candidate that pushed it in the first force pass
// [orig: Entity_MovementCollisionResolver @0x4b2bd0, the block
// @0x4b37c2..0x4b39f7; the pusher is the var_80 store inside the pass-0 force
// fold @0x4b30a9]. Every gate is a refusal. The shared head, which the bump
// sound below also needs:
//   * the pusher is a VEHICLE (itemDef type 1 @0x4b37dc) that is not the
//     victim's ground link (@0x4b37e8 — a rider is never crushed by its ride)
//     and is not dead (@0x4b37f3);
//   * the victim has health and is not dead (@0x4b37fd / @0x4b380b).
inline bool run_over_contact_applies(bool pusher_is_vehicle, bool pusher_is_victims_ground,
		bool pusher_dead, int32_t victim_health, bool victim_dead) {
	if (!pusher_is_vehicle || pusher_is_victims_ground || pusher_dead) return false;
	if (victim_health <= 0 || victim_dead) return false;
	return true;
}

// The kill leg's own refusals after that head:
//   * BOTH the pusher's own tick displacement and its displacement RELATIVE to
//     the victim exceed 0x27B0 (the planar lengths @0x4b3815..0x4b3890, the
//     compares @0x4b38e5..0x4b38f8) — a parked hull nudged into never kills;
//   * a same-team pusher kills only when either side's AI slot carries the
//     BERSERK behavior bit 0x200 (@0x4b389d..0x4b38d2);
//   * the authority alone kills (@0x4b38d8);
//   * an indestructible victim (Flags 0x4000000) survives (@0x4b3901);
//   * a PLAYER victim (Flags 0x100) whose damage-disabled word (entity+0x124:
//     spawn protection or the dead latch) is set survives (@0x4b3918..0x4b392a),
//     and so does one whose player slot carries the spectator latch
//     (slot +0x188D7 through Entity_ValidatePtr @0x4b3933..0x4b3946).
// The kill: death anim = cause 2 (explosive) at bone 2 by the approach
// quadrant (Entity_ComputeAnimSlotIndex @0x4b39b2), +0x178 credits the
// pusher's OCCUPANT (@0x4b39d1), Health = 0 (@0x4b39d7), then
// Score_ProcessKillEvent (@0x4b39e2).
inline constexpr int32_t kRunOverSpeedThreshold = 0x27B0;
inline constexpr int kRunOverDeathBone = 2;
inline constexpr int kRunOverDeathCause = 2; // explosive family (death_grenade_*)

inline bool run_over_kill_applies(bool pusher_is_vehicle, bool pusher_is_victims_ground,
		bool pusher_dead, int32_t victim_health, bool victim_dead,
		int32_t pusher_move_planar, int32_t relative_move_planar,
		bool same_team, bool either_berserk, bool is_authority,
		bool victim_indestructible, bool victim_player_protected) {
	if (!run_over_contact_applies(pusher_is_vehicle, pusher_is_victims_ground, pusher_dead,
			victim_health, victim_dead))
		return false;
	if (same_team && !either_berserk) return false;
	if (!is_authority) return false;
	if (relative_move_planar <= kRunOverSpeedThreshold) return false;
	if (pusher_move_planar <= kRunOverSpeedThreshold) return false;
	if (victim_indestructible) return false;
	if (victim_player_protected) return false;
	return true;
}

// The approach quadrant [orig: @0x4b395d..0x4b39a7]: the fpatan operands are
// the VICTIM's tick displacement minus the pusher's (`sub edi, [esp+var_7C]`
// @0x4b395d, `sub ebp, ebx` @0x4b3963), so a pusher driving head-on into a
// victim's face reads quadrant 0; then `(Yaw - that + 0x1FFFFFFF) >> 30` as an
// UNSIGNED shift.
inline int run_over_quadrant(int32_t victim_yaw_bam, int32_t victim_minus_pusher_dx,
		int32_t victim_minus_pusher_dy) {
	return approach_quadrant(victim_yaw_bam, victim_minus_pusher_dx, victim_minus_pusher_dy,
			0x1FFFFFFFu);
}

// THE BUMP SOUND — past the kill leg, whether or not it killed, the same
// contact with BOTH planar displacements above 0x3F8 plays the pusher's sound
// profile slot 38 at the victim, once per suppression window
// [orig: @0x4b39fd..0x4b3a0c (the thresholds), the ItemDef +0x268 profile
// handle and the +0x864 page slot +0x98 tests @0x4b3a0e..0x4b3a2f,
// Server_TrackEntityInTable(id, 0x1F) @0x4b3a3c, Sound_PlayWithDistanceAttenuation
// at the victim @0x4b3a4e]. The authority is not consulted.
inline constexpr int32_t kRunOverSoundThreshold = 0x3F8;
inline constexpr int32_t kRunOverSoundSuppressTicks = 0x1F;

inline bool run_over_sound_applies(int32_t pusher_move_planar, int32_t relative_move_planar) {
	return pusher_move_planar > kRunOverSoundThreshold &&
			relative_move_planar > kRunOverSoundThreshold;
}

} // namespace opennova::world
