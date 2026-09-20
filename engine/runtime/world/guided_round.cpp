#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>
#include <runtime/world/throwables.h>
#include <base/io/fixed.h>
#include <algorithm>

namespace opennova::world {
EntityHandle guided_heat_target(World &, const Entity &, const AiEntity &, const AmmoTableEntry &);
namespace {
// The RAW Position copies: the Javelin launch seed, the owner's class
// get-target callback and the flare decoy.
// [orig: jvln launch @0x445E40..0x445E97; sub_4B0DD0 @0x4B0DF5..0x4B0E12;
//  flare decoy @0x446232..0x44626B]
void target_position(const Entity &target, int32_t out[3]) {
    out[0] = to_fixed(target.position.x);
    out[1] = to_fixed(target.position.y);
    out[2] = to_fixed(target.position.z);
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
GuidedInputs inputs(World &world, const LiveRound &r, bool authority, bool launch) {
    GuidedInputs in;
    in.authority = authority; in.session = world.rules.mp_session;
    const Entity *owner = world.registry.get(r.owner);
    in.owner = owner != nullptr;
    if (owner) {
        in.owner_target = owner->last_fire_target.packed;
        if (const AiEntity *body = world.ai.for_handle(owner->handle)) {
            in.owner_aim = true; in.owner_ai = true;
            if (body->slot.f[3]) in.owner_target = uint16_t(body->slot.f[3] - 1);
            std::copy_n(body->inf.aim_point, 3, in.aim);
        }
        if (const Entity *aim = world.registry.get(EntityHandle{in.owner_target})) {
            in.owner_aim = true;
            target_position(*aim, in.aim);
        }
    }
    const uint16_t target_id = r.guided.target == 0xFFFF && r.guided.phase == 0 ? in.owner_target : r.guided.target;
    if (const Entity *target = world.registry.get(EntityHandle{target_id})) {
        in.target_present = true; in.target_alive = target->health > 0;
        target_sample(world, *target, launch, in.target_origin);
    }
    return in;
}
void copy_pose(const LiveRound &r, GuidedFlightState &s) {
    s.pos[0] = to_fixed(r.pos.x); s.pos[1] = to_fixed(r.pos.y); s.pos[2] = to_fixed(r.pos.z);
    s.yaw_bam = r.yaw_bam; s.pitch_bam = r.pitch_bam; s.roll_bam = r.roll_bam;
    s.velocity[0] = to_fixed(r.vel.x); s.velocity[1] = to_fixed(r.vel.y); s.velocity[2] = to_fixed(r.vel.z);
}
void copy_motion(LiveRound &r) {
    r.yaw_bam = r.guided.yaw_bam; r.pitch_bam = r.guided.pitch_bam; r.roll_bam = r.guided.roll_bam;
    r.vel = {float(r.guided.velocity[0]) / 65536.0f, float(r.guided.velocity[1]) / 65536.0f, float(r.guided.velocity[2]) / 65536.0f};
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
                const auto target = guided_heat_target(world, *owner, *body, ammo);
                body->slot.f[3] = target.valid() ? int32_t(target.packed) + 1 : 0;
                owner->last_fire_target = target;
                in.acquisition_ran = true; in.acquired = target.valid();
                in.acquired_target = target.packed;
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
            std::copy_n(s.pos, 3, pose.pos);
            pose.heading = s.yaw_bam; pose.pitch = s.pitch_bam; pose.roll = s.roll_bam;
            const EntityHandle chosen = guided_heat_target(world, seeker, pose, ammo);
            in.acquired_target = chosen.packed;
            if (const Entity *target = world.registry.get(chosen)) {
                in.acquired = true; in.target_present = true; in.target_alive = target->health > 0;
                target_sample(world, *target, false, in.target_origin);
                target_position(*target, in.acquired_origin);
            }
        }
    }
    if (guided_inputs_provider) guided_inputs_provider(r, in);
    const auto events = GuidedFlight::motor(s, r.guided_family, parameters(ammo), in);
    if (events.detonate) r.det_at_expiry = true;
    if (authority && events.groups) guided_updates.push_back({r.shooter_handle, r.shot_seq, events.groups, s});
    copy_motion(r);
}
} // namespace opennova::world
