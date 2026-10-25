// The client leg of the projectile damage pass over a wire-built person (see
// wire_person_hit.h).
#include <runtime/world/wire_person_hit.h>

#include <base/io/fixed.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/collision.h>
#include <runtime/world/collision_force.h>
#include <runtime/world/damage_pair.h>
#include <runtime/world/destruction.h>
#include <runtime/world/geom.h>
#include <runtime/world/infantry.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/throwables.h>
#include <runtime/world/world.h>

#include <string>

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

// The round's owner as the class callbacks read it through the record's
// round (+0x40 -> +0x170): its wire identity and, when it is known, its
// commandGroup. Every entity a client holds for an owner is client-built (its
// own body included: the 0x0C slot memset @0x42E7F1, and no client message
// stores +0x11C), so a known owner's commandGroup is 0 whatever the joiner's
// world keeps for its own body.
struct RoundOwner {
	uint16_t wire = 0xFFFF;
	bool known = false;
	int32_t group = 0;
};

// The ammo's hit emitter (ammo +72) into the body's +0x1CC slot: the slot's
// own emitter is released (every spawn into it tags the body, so a held one
// is the body's), then the effect spawns at the body's position, oriented
// (0, 0, -0.5), tagged with the body and never re-posed -- the descriptor the
// explosion queue's victim emitter builds (destruction.h
// spawn_victim_hit_emitter), here keyed to the wire-built body.
// [orig: OrganicClass_HandleEvent @0x4074C7..0x40754A -- the release
//  @0x4074DB..0x4074F9, the descriptor @0x4074FF..0x40753A (flags 1 @0x40752E,
//  the tag @0x407536, orientation z 0xFFFF8000 @0x40753A), the store
//  @0x40754A]
void spawn_wire_person_hit_emitter(const WirePersonBody &body, const std::string &effect,
		DestructionEvents &events) {
	DestructionEffectEvent release;
	release.family = 1;
	release.attach_wire_handle = body.wire_handle;
	release.attach_spawn_origin = kSpawnOriginNone;
	release.release = true;
	events.effects.push_back(std::move(release));
	DestructionEffectEvent spawn;
	spawn.effect = effect;
	spawn.pos = {io::fp16_16_to_float(body.pos[0]), io::fp16_16_to_float(body.pos[1]),
			io::fp16_16_to_float(body.pos[2])};
	spawn.dir = {0.0f, 0.0f, -0.5f};
	spawn.family = 1;
	spawn.attach_wire_handle = body.wire_handle;
	spawn.attach_spawn_origin = kSpawnOriginNone;
	spawn.section_tagged = true;
	spawn.positioned = true;
	events.effects.push_back(std::move(spawn));
}

// The person class callbacks' round legs, which run while the record holds a
// round: the death clip from the record's section and the round's approach
// quadrant, the ammo's collision force from the round, the hit emitter, the
// torso-stack body roll, Entity_OnDamageReceived's tail and the emitter's
// 173 clip. The authority's dismemberment cut sits between the last two and
// never runs on a client [orig: @0x407598 / @0x40793B].
// [orig: OrganicClass_HandleEvent @0x40740F..0x4076D5; the plyr twin
//  Entity_HandleDamageAndTriggerZones @0x407777..0x407A7C]
void wire_person_round_legs(World &world, WirePersonBody &body, const HitRecord &record,
		const RoundOwner &owner) {
	const AmmoTableEntry *ammo = world.tables.ammo.by_index(record.round_ammo_index);
	if (ammo == nullptr) return;
	// The ammo's +72 word names the hit emitter [orig: @0x40743B..0x407453].
	const std::string &effect = ammo->secondary_effect;
	// The approach quadrant against the body's Yaw [orig: @0x407478].
	const int quadrant = death_quadrant_from_round(body.heading_bam,
			static_cast<float>(from_fixed(record.round_vel_q16.x)),
			static_cast<float>(from_fixed(record.round_vel_q16.y)));
	// [orig: Entity_ComputeAnimSlotIndex(entity, hitRecord[14], quadrant, 1)
	//  @0x407483]
	body.death_anim_state = static_cast<int16_t>(
			compute_death_anim_state(record.section, quadrant, death_cause::kBullet));
	// The ammo's hit type (+224) and force (+225) from the round's position,
	// against the round owner's commandGroup [orig: Entity_ApplyCollisionForce
	// call @0x4074BA]. Force 3 is the local player's; a wire-built body is
	// never the local player.
	const int32_t xy[2] = {body.pos[0], body.pos[1]};
	collision_force_body(xy, body.heading_bam, body.flags,
			owner.known ? &owner.group : nullptr, body.command_group,
			ammo->secondary_anim, ammo->kz_physics, record.round_pos.q16(),
			body.hit_type, body.move_timer, body.move_timer, body.vel);
	if (!effect.empty())
		spawn_wire_person_hit_emitter(body, effect, world.out.destruction);
	// [orig: the torso-stack gate @0x40755E]
	if ((body.flags & kEntityFlagDead) == 0)
		hit_body_roll(body.body_roll, record.section, quadrant);
	// Entity_OnDamageReceived(body, ammo, owner): its head is the local
	// player's; the tail marks any body another entity hit [orig: the call
	// @0x407588 -> @0x4AF85B..0x4AF878].
	if (body.wire_handle != owner.wire) {
		body.was_hit = true;                                 // [orig: @0x4AF866]
		if (body.damage_timer < 25) body.damage_timer += 10; // [orig: @0x4AF86D..0x4AF872]
		body.damage_attacker = owner.wire;                   // [orig: @0x4AF878]
	}
	// [orig: @0x4076D3..0x4076D5]
	if (!effect.empty()) body.death_anim_state = 173;
}

// The org0 / org1 class callback, event 1, over a wire-built body
// [orig: OrganicClass_HandleEvent @0x407310, event 1 @0x4073BF..0x4076D5].
void wire_org_class_event1(World &world, WirePersonBody &body, const HitRecord &record,
		const RoundOwner &owner) {
	if ((body.flags & kEntityFlagPlayer) == 0) {
		// The alert: the body's AI slot and its trigger group go red
		// [orig: @0x4073D8..0x4073DB; TriggerGroup_SetAlertRed @0x4073EA].
		body.ai_alert = 2;
		world.script.relations.group(body.command_group).alert = TriggerRelations::kAlertRed;
	}
	// A Player-flag body's Score_ProcessNetworkKillEvent (@0x407405) scores
	// only on the authority [orig: @0x4FD494].
	if (record.has_round) wire_person_round_legs(world, body, record, owner);
}

// The plyr class callback, event 1, over a wire-built body
// [orig: Entity_HandleDamageAndTriggerZones @0x407720, event 1 @0x407742..
//  0x407A86, the tail @0x407ACD..0x407C71].
void wire_plyr_class_event1(World &world, WirePersonBody &body, const HitRecord &record,
		const RoundOwner &owner) {
	// A body already flagged dead returns at once [orig: @0x40772F].
	if ((body.flags & kEntityFlagDead) != 0) return;
	if ((body.flags & kEntityFlagPlayer) == 0) {
		// A plyr body without the Player bit takes the org alert
		// [orig: @0x407755..0x40776D].
		body.ai_alert = 2;
		world.script.relations.group(body.command_group).alert = TriggerRelations::kAlertRed;
	}
	// A Player body's Score_ProcessNetworkKillEvent, with the round's owner
	// or none (@0x4078ED / @0x407906), scores only on the authority
	// [orig: @0x4FD494].
	if (record.has_round) wire_person_round_legs(world, body, record, owner);
	// The health tail: a body whose Health is gone restages its death clip
	// (the record-less selection when none is staged) as the current
	// state, clears the pending one, and a mounted body leaves its seat
	// through Entity_SendDetachPacket on a client, which sends only for the
	// local player's own body: a wire-built body sends nothing
	// [orig: @0x407ACD..0x407B01; Entity_ComputeAnimSlotIndex(entity, 0, 0, 1)
	//  @0x407ADF; the detach @0x407B0B..0x407B33 -> Entity_SendDetachPacket
	//  @0x435510, its local-player gate @0x43552A].
	if (body.health_zero) {
		if (body.death_anim_state == 0)
			body.death_anim_state = static_cast<int16_t>(
					compute_death_anim_state(0, 0, death_cause::kBullet));
		body.anim_current = body.death_anim_state;
		body.anim_pending = 0;
	}
	// Event 1 keeps the cause bits (1 is in the no-clear set, @0x407B3B..
	// 0x407B4F). The think re-arm brackets the waypoint tail, which walks the
	// AI slot's path (+0x94): the client allocates the slot zeroed and no
	// client message writes the word, so the walk never runs
	// [orig: @0x407B5E, the path test @0x407B64..0x407B6C, @0x407C71].
	body.spawn_phase = 64;
}

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
	RoundOwner round_owner;
	round_owner.wire = r.shooter_handle;
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
		round_owner.known = true;
	} else if (r.shooter_handle != EntityHandle::kInvalid) {
		const bool person = wire_person_body(r.shooter_handle, [&](WirePersonBody &body) {
			shooter.known = true;
			shooter.team = body.team;
			shooter.player = (body.flags & kEntityFlagPlayer) != 0;
			shooter.berserk = body.berserk;
			round_owner.known = true;
		});
		// A decoded shooter that is no person (a vehicle or emplaced gun row)
		// carries its team and Flags word on its row, and no AI slot.
		if (!person && actor_known) {
			shooter.known = true;
			shooter.team = actor.team;
			shooter.player = (actor.flags & kEntityFlagPlayer) != 0;
			round_owner.known = true;
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
		// The class callback, event 1 (+0x1C8 @0x4E81FF..0x4E820E).
		if (victim.plyr_class)
			wire_plyr_class_event1(world, victim, hit_record, round_owner);
		else
			wire_org_class_event1(world, victim, hit_record, round_owner);
		// Player_OnDamageReceived is the local player's alone (@0x4E8213..
		// 0x4E822B): a wire-built person never takes it.
	});
}

} // namespace opennova::world
