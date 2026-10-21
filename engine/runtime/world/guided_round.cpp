#include <runtime/world/round_sim.h>
#include <runtime/world/radar_contacts.h>
#include <runtime/world/world.h>
#include <runtime/world/throwables.h>
#include <base/io/fixed.h>
#include <algorithm>

namespace opennova::world {
namespace {
// The RAW Position copies: the Javelin launch seed, the owner's class
// get-target callback and the flare decoy.
// [orig: jvln launch @0x445E40..0x445E97; PersonClass_GetTarget @0x4B0DF5..0x4B0E12;
//  flare decoy @0x446232..0x44626B]
void target_position(const Entity &target, int32_t out[3]) {
    out[0] = target.position.x.q16;
    out[1] = target.position.y.q16;
    out[2] = target.position.z.q16;
}
// Every IN-FLIGHT refresh samples the target's aim origin instead: a person's
// phased CameraOffset point, a model's TARGET userpoint, else its bbox centre.
// [orig: Entity_ComputeWeaponFireOrigin @0x43B4B0 -- stng @0x4463A3, hlfr
//  @0x446772, jvln @0x446C2D / @0x446D82 / @0x446DF6 / @0x446E8E / @0x446F74]
void target_sample(World &world, const Entity &target, bool launch, int32_t out[3]) {
    if (launch) target_position(target, out);
    else world.ai.weapon_aim_origin(world, target, out);
}
GuidedAmmo parameters(const AmmoTableEntry &ammo) {
    return {ammo.velocity, ammo.turnrate_maxpit, ammo.turnrate_maxyaw, ammo.boresight_maxang};
}
// A round the missile holds (or would take from its owner): the projectile array's slot
// is always there, so it is present; it lives while its Health word is above 0, and both
// the raw copy and the in-flight sample read its Position. [orig: Entity_UpdateGuidedMissile_0
//  @0x446060 - the liveness tests @0x4461A6..0x4461BA (authority) / @0x44634E..0x446361,
//  the sample @0x4463A3; PersonClass_GetTarget @0x4B0DD0 (the raw copy @0x4B0DF1..0x4B0E12)]
void round_target_inputs(World &world, int32_t slot, GuidedInputs &in) {
    in.target_present = true;
    in.target_alive = round_target_health(world, slot) > 0;
    round_target_origin(world, slot, in.target_origin);
}
GuidedInputs inputs(World &world, const LiveRound &r, bool authority, bool launch) {
    GuidedInputs in;
    in.authority = authority; in.session = world.rules.mp_session;
    const Entity *owner = world.registry.get(r.owner);
    in.owner = owner != nullptr;
    if (owner) {
        TargetRef owner_target = owner->last_fire_target;
        if (const AiEntity *body = world.ai.for_handle(owner->handle)) {
            in.owner_aim = true; in.owner_ai = true;
            if (body->slot.f[3]) owner_target = AiSlot::target_of(body->slot.f[3]);
            std::copy_n(body->inf.aim_point, 3, in.aim);
        }
        in.owner_target = owner_target.entity.packed;
        in.owner_target_round = owner_target.round;
        if (owner_target.round >= 0) {
            in.owner_aim = true;
            round_target_origin(world, owner_target.round, in.aim);
        } else if (const Entity *aim = world.registry.get(owner_target.entity)) {
            in.owner_aim = true;
            target_position(*aim, in.aim);
        }
    }
    const bool held = r.guided.target != 0xFFFF || r.guided.target_round >= 0;
    const bool from_owner = !held && r.guided.phase == 0;
    const uint16_t target_id = from_owner ? in.owner_target : r.guided.target;
    const int32_t target_round = from_owner ? in.owner_target_round : r.guided.target_round;
    if (target_round >= 0) {
        round_target_inputs(world, target_round, in);
    } else if (const Entity *target = world.registry.get(EntityHandle{target_id})) {
        in.target_present = true; in.target_alive = target->health > 0;
        target_sample(world, *target, launch, in.target_origin);
    }
    return in;
}
void copy_pose(const LiveRound &r, GuidedFlightState &s) {
    s.pos[0] = r.pos.x.q16; s.pos[1] = r.pos.y.q16; s.pos[2] = r.pos.z.q16;
    s.yaw_bam = r.yaw_bam; s.pitch_bam = r.pitch_bam; s.roll_bam = r.roll_bam;
    s.velocity[0] = r.vel.x.q16; s.velocity[1] = r.vel.y.q16; s.velocity[2] = r.vel.z.q16;
}
void copy_motion(LiveRound &r) {
    r.yaw_bam = r.guided.yaw_bam; r.pitch_bam = r.guided.pitch_bam; r.roll_bam = r.guided.roll_bam;
    r.vel.set_q16({r.guided.velocity[0], r.guided.velocity[1], r.guided.velocity[2]}); // the words (D-NET-507)
}
}
// [orig: EntitySlot_FindByNetId @0x4E70C0; dispatch's positive lifetime gate @0x4D6960]
LiveRound *RoundSim::find_guided(int16_t id) {
    for (auto &r : rounds) {
        if (r.shot_seq != uint16_t(id)) continue;
        return r.active && r.max_age_ticks > r.age_ticks && r.guided_family != GuidedFamily::None ? &r : nullptr;
    }
    return nullptr;
}
void RoundSim::init_guided(World &world, LiveRound &r, const AmmoTableEntry &ammo) {
    switch (r.motor) {
        case ThrowClass::kStinger: r.guided_family = GuidedFamily::Stinger; break;
        case ThrowClass::kHellfire: r.guided_family = GuidedFamily::Hellfire; break;
        case ThrowClass::kJavelin: r.guided_family = GuidedFamily::Javelin; break;
        default: return;
    }
    r.spin_yaw = r.spin_pitch = r.spin_roll = 0;
    copy_pose(r, r.guided);
    auto in = inputs(world, r, r.consequence_mode == RoundConsequenceMode::Authoritative, true);
    if (r.guided_family == GuidedFamily::Stinger && in.owner_ai) {
        if (Entity *owner = world.registry.get(r.owner)) {
            if (AiEntity *body = world.ai.for_handle(r.owner)) {
                // The stng init queries over its shooter (the roll's attacker the
                // shooter's +368), stores the winner, an entity or a round, into the
                // shooter's +0xC and the missile's +0x2D4 takes that word.
                // [orig: the stng init StngClass_InitFromDef @0x445EF0: the AiSlot test
                //  @0x445EFB, Entity_SpawnWeaponEffect @0x445F02, the +0xC store
                //  @0x445F10, the +0x2D4 store @0x445F22]
                const TargetRef target = guided_heat_target(world, *owner, *body, &body->slot, ammo,
                        world.registry.get(owner->primary_occupant));
                body->slot.f[3] = AiSlot::target_word(target);
                owner->last_fire_target = target;
                in.acquisition_ran = true; in.acquired = target.valid();
                in.acquired_target = target.entity.packed;
                in.acquired_round = target.round;
            }
        }
    }
    if (guided_inputs_provider) guided_inputs_provider(r, in);
    GuidedFlight::launch(r.guided, r.guided_family, parameters(ammo), in);
    copy_motion(r);
}
// Class binding is the stng table row @0x82AD24; the generic projectile
// position add remains in Projectile_UpdatePhysics @0x444AED.
// The class motor runs before the stock sweep; a bound motor suppresses the
// stock gravity/drag tail. [orig: Projectile_UpdatePhysics @0x4EA06A/0x4EAA4C]
void RoundSim::tick_guided(World &world, LiveRound &r, const AmmoTableEntry &ammo, bool authority) {
    auto &s = r.guided;
    copy_pose(r, s); s.age = r.age_ticks - 1;
    auto in = inputs(world, r, authority, false);
    if (authority && r.guided_family == GuidedFamily::Stinger &&
        ((s.timer >= 0 && s.timer <= 1) || ((s.flags & 2) && (!in.target_present || !in.target_alive)))) {
        in.acquisition_ran = true;
        if (const Entity *owner = world.registry.get(r.owner)) {
            Entity seeker = *owner;
            seeker.position = r.pos;
            AiEntity pose;
            pose.self = &seeker; // the round's words, s.pos (copy_pose)
            pose.heading = s.yaw_bam; pose.pitch = s.pitch_bam; pose.roll = s.roll_bam;
            // The in-flight query walks the ring; its attacker's +368 is the missile's
            // source, the shooter [orig: Entity_UpdateGuidedMissile_0 @0x446060 -> Entity_SpawnWeaponEffect
            //  @0x545840 (the calls @0x44620B / @0x44628A; the query entity @0x5458B7, its
            //  +368 @0x5458EA)].
            const TargetRef pick = guided_heat_target(world, seeker, pose, nullptr, ammo, owner);
            const EntityHandle chosen = pick.entity;
            in.acquired_target = chosen.packed;
            if (pick.round >= 0) {
                // An air flare is a decoy: the missile steers at its raw Position and keeps
                // no target; a ground flare (its +0x1C4 the jump thunk, no shell
                // projectile) the missile keeps as its target, group 3 sending its word
                // as 0xFFFF [orig: Entity_UpdateGuidedMissile_0 @0x446060 (the decoy
                //  @0x446232..0x44626B; the kept target @0x44632B..0x446345);
                //  Entity_IsShellProjectile @0x4E4040].
                const LiveRound &flare = rounds[static_cast<size_t>(pick.round)];
                in.acquired = true;
                if (flare.motor == ThrowClass::kAirFlare) {
                    in.acquired_flare = true;
                    round_target_origin(world, pick.round, in.acquired_origin);
                } else {
                    in.acquired_round = pick.round;
                    in.acquired_present = true;
                    round_target_origin(world, pick.round, in.acquired_sample);
                }
            } else if (const Entity *target = world.registry.get(chosen)) {
                in.acquired = true; in.acquired_present = true;
                target_sample(world, *target, false, in.acquired_sample);
                target_position(*target, in.acquired_origin);
            }
        }
    }
    if (guided_inputs_provider) guided_inputs_provider(r, in);
    const auto events = GuidedFlight::motor(s, r.guided_family, parameters(ammo), in);
    // The proximity notice: inside 400 u of the steer point the held target's vehicle
    // brain (+0x64; a round's is the allocation's zero, a person carries none) takes event
    // 12 at once (timer 0.0, channel 0, the brain's index), AI_HandleCommand's case-0x0C
    // evade arm. [orig: Entity_UpdateGuidedMissile_0 @0x446060 - the range test
    //  @0x44657F..0x446587, the +0x2D4 and +0x64 tests @0x446589..0x446598, the entry
    //  @0x44659A..0x4465CC, AIEvent_QueueEntry @0x455DA0 (the call @0x4465D3)]
    if (events.proximity && s.target_round < 0) {
        AiEntity *brain = world.ai.for_handle(EntityHandle{s.target});
        if (brain != nullptr && !brain->inf.active && brain->brain.f[AiBrain::kOwner] != 0) {
            AiEventEntry notice{};
            notice.f[0] = 12;
            notice.f[1] = world.ai.index_of(*brain) << 16;
            notice.set_timer(0.0f);
            world.ai.events.queue(notice);
        }
    }
    // The Stinger's lock note, at the pose the motor read [orig:
    // Entity_UpdateGuidedMissile_0 @0x4465db..0x446622 -> Radar_TrackMissile].
    if (events.threat_note)
        radar_note_guided_missile(world, r, static_cast<uint32_t>(&r - rounds.data()));
    if (events.detonate) r.det_at_expiry = true;
    if (authority && events.groups) guided_updates.push_back({r.shooter_handle, r.shot_seq, events.groups, s});
    copy_motion(r);
}
} // namespace opennova::world
