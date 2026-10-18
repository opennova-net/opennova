// The client leg of the projectile damage pass over a wire-built person (see
// wire_person_hit.h).
#include <runtime/world/wire_person_hit.h>

#include <runtime/world/ammo_table.h>
#include <runtime/world/collision.h>
#include <runtime/world/damage_pair.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/throwables.h>
#include <runtime/world/world.h>

namespace opennova::world {

namespace {

// One side of the friendly-fire gate, as Projectile_DamagePairEligible reads
// it off an entity: the team byte (+354), the Player bit of Flags (+0x24 &
// 0x100) and the AI slot's BERSERK bit (slot +4 & 0x200)
// [orig: Projectile_DamagePairEligible @0x4E74F0 -- @0x4E7516, @0x4E7528,
//  @0x4E752C..0x4E754E].
struct PairSide {
	bool known = false;
	uint8_t team = 0;
	bool player = false;
	bool berserk = false;
};

} // namespace

// [orig: Projectile_ProcessDamageOnTarget @0x4E7FB0, its in-session
//  non-authority leg]
void RoundSim::process_wire_person_hit(World &world, LiveRound &r,
		const ProjectileHit &collision, const FixedVec3 &velocity_q16) {
	if (!wire_person_body) return;
	const AmmoTableEntry *ammo = world.tables.ammo.by_index(r.ammo_index);
	if (ammo == nullptr) return;
	// The struck section the person handler's ray carries (ray[31]): the hit
	// bone, the torso stand-in reading section 1, as the registry leg reads it.
	const int16_t section =
			static_cast<int16_t>(collision.bone_index < 0 ? 1 : collision.bone_index);

	// The round's owner (+0x170), which the client resolves to its own pool
	// entity: this peer's own body for its own rounds, the decoded shooter's
	// for a relayed round [orig: NetPacket_DeserializeRoundEvent @0x42F491].
	PairSide shooter;
	const Entity *owner = world.registry.get(r.owner);
	WireActor actor;
	const bool actor_known = owner == nullptr && r.shooter_handle != EntityHandle::kInvalid &&
			wire_actor_provider && wire_actor_provider(r.shooter_handle, actor);
	if (actor_known && actor.is_local) owner = world.registry.get(world.cached.local_player);
	if (owner != nullptr) {
		shooter.known = true;
		shooter.team = static_cast<uint8_t>(owner->team);
		shooter.player = ((owner->flags | owner->engine_flags) & kEntityFlagPlayer) != 0;
		shooter.berserk = damage_pair_berserk(world, *owner);
	} else if (r.shooter_handle != EntityHandle::kInvalid) {
		const bool person = wire_person_body(r.shooter_handle, [&](WirePersonBody &body) {
			shooter.known = true;
			shooter.team = body.team;
			shooter.player = (body.flags & kEntityFlagPlayer) != 0;
			shooter.berserk = body.berserk;
		});
		// A decoded shooter that is no person (a vehicle or emplaced gun row)
		// carries its team and Flags word on its row, and no AI slot.
		if (!person && actor_known) {
			shooter.known = true;
			shooter.team = actor.team;
			shooter.player = (actor.flags & kEntityFlagPlayer) != 0;
		}
	}

	wire_person_body(collision.wire_person_handle, [&](WirePersonBody &victim) {
		// A null ItemDef returns before the damage calculation [orig: @0x4E7FCB].
		if (!victim.has_item_def) return;
		// In a session a non-authority peer computes no damage
		// [orig: Weapon_CalcImpactDamage @0x4EC92A..0x4EC93A]; the armor,
		// Indestructible and damage_state clamps keep the zero
		// (@0x4E7FF6..0x4E803D). The Health clamp then takes the signed word
		// when it is lower and NoDie keeps the damage under it
		// (@0x4E8059..0x4E8064, @0x4E8069..0x4E8076): a client holds a
		// person's Health positive or zeroed (a dead record or an 0x13), so
		// only a zeroed NoDie body records -1.
		const int32_t health = victim.health_zero ? 0 : 1;
		int32_t damage = 0;
		if (damage > health) damage = health;
		if ((victim.item_attrib & kItemAttribNoDie) != 0 && damage >= health)
			damage = health - 1;
		// The friendly-fire gate, asked when the round has an owner; a
		// protected pair leaves with nothing landed [orig: @0x4E8079..0x4E808F
		// -> Projectile_DamagePairEligible @0x4E74F0]. A person is no placed
		// device, so the gate's device leg never applies to it.
		if (shooter.known &&
				damage_pair_protected_rule(world.rules.no_friendly_fire, shooter.player,
						victim.team, shooter.team, victim.berserk, shooter.berserk,
						ThrowClass::kNone))
			return;
		// The shot relations (@0x4E809C) and the health write (@0x4E81B1) are
		// the authority's; the kill leg needs damage >= Health > 0
		// (@0x4E80F7..0x4E8112), which a zero never meets, so
		// Score_ProcessKillEvent (@0x4E8133) never runs here.
		// LABEL_36: the record every event-1 callback reads: this round, the
		// damage, the struck section and the target [orig:
		// Projectile_CopyEntityToHitRecord @0x4E81BA..0x4E81C0, +0x30 @0x4E81C9,
		// +0x38 @0x4E81D2, +0x48 @0x4E81D7].
		copy_round_to_hit_record(hit_record, r, velocity_q16);
		hit_record.damage = damage;
		hit_record.section = section;
		hit_record.target_wire = victim.wire_handle;
		// The attacker stamp, unless the def carries EWEAP [orig: @0x4E81E3
		// `test byte ptr [ecx+54h], 20h`, the jnz @0x4E81E7, +0x178 @0x4E81F3,
		// +0x17C @0x4E81F9].
		if ((victim.item_attrib & kItemAttribEweap) == 0) {
			victim.last_attacker = r.shooter_handle;
			victim.last_attacker_ammo = r.ammo_index;
		}
		// Player_OnDamageReceived is the local player's alone (@0x4E8213..
		// 0x4E822B): a wire-built person never takes it.
	});
}

} // namespace opennova::world
