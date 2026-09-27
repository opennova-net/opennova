// Authored squib sweeps, independent of ballistic round flight.
// [orig: sub_448CE0 @0x448CE0, Entity_InitArcMovement @0x449810,
// Entity_ProcessProjectileTravel @0x448D50]
#include <runtime/world/destruction.h>
#include <runtime/world/world.h>
#include <runtime/world/collision.h>
#include <runtime/world/angle.h>
#include <base/io/bam.h>
#include "collision_detail.h"
#include <algorithm>
#include <cmath>

namespace opennova::world {
namespace {
FixedVec3 fixed(const Vec3 &v) {
    return {int32_t(v.x * 65536), int32_t(v.y * 65536), int32_t(v.z * 65536)};
}
Vec3 floating(const FixedVec3 &v) {
    return {v.x / 65536.0f, v.y / 65536.0f, v.z / 65536.0f};
}
int32_t mul(int32_t a, int32_t b) {
    return int32_t(uint32_t((uint64_t(int64_t(a) * b) + 0x8000u) >> 16));
}
FixedVec3 rotate(const CollisionMatrix &m, const FixedVec3 &v) {
    const int32_t in[] = {v.x,v.y,v.z};
    int32_t out[3];
    m.rotate_point(in, out);
    return {out[0],out[1],out[2]};
}
CollisionMatrix rotation(int32_t yaw, int32_t pitch) {
    // The original double is 30.5 ppm above the ideal radians/BAM constant.
    // [orig: Math_BuildFixedPointRotationMatrixYXZ @0x615400, dbl_7C3608]
    constexpr double radians = 1.4629627251502471e-9;
    const int32_t cy=int32_t(std::cos(yaw*radians)*4194304.0);
    const int32_t sy=int32_t(std::sin(yaw*radians)*4194304.0);
    const int32_t cp=int32_t(std::cos(pitch*radians)*4194304.0);
    const int32_t sp=int32_t(std::sin(pitch*radians)*4194304.0);
    CollisionMatrix m;
    m.m[0]=int32_t((int64_t(cp)*cy)>>22); m.m[1]=-sy;
    m.m[2]=int32_t((int64_t(-sp)*cy)>>22);
    m.m[4]=int32_t((int64_t(cp)*sy)>>22); m.m[5]=cy;
    m.m[6]=int32_t((int64_t(-sp)*sy)>>22);
    m.m[8]=sp; m.m[10]=cp;
    return m;
}
CollisionMatrix compose(const CollisionMatrix &a, const CollisionMatrix &b) {
    // [orig: Matrix_Multiply3x4_FixedPoint @0x613940]
    CollisionMatrix out;
    for (int r=0;r<3;++r) for (int c=0;c<3;++c) {
        uint64_t sum = 0x200000u;
        for (int k=0;k<3;++k) sum += uint64_t(int64_t(a.m[4*r+k]) * b.m[4*k+c]);
        out.m[4*r+c] = int32_t(uint32_t(sum >> 22));
    }
    return out;
}
}
void squib_event(World &world, Entity &entity, int phase) {
    entity.class_think_ticks = 0x7FFFFFF;
    if (phase != 4) return;
    entity.death_anim_state = 0;
    emit_item_state(world, entity, 0);
    const auto *traits = world.tables.item_death_traits.get(entity.item_id);
    if (!traits || !entity.script_next_ssn) return;
    Entity *target = nullptr;
    for (size_t i=0;i<world.registry.pool_capacity(3);++i) {
        Entity *row = world.registry.get(EntityHandle::make(3, uint16_t(i)));
        if (row && row->item_id && row->net_id == entity.script_next_ssn) { target=row; break; }
    }
    // Retail reads beyond the pool on a missing target, and divides by zero
    // for a nonpositive authored step/rate. Leave malformed emitters parked.
    if (!target || traits->squib_distance_q16 <= 0 || entity.deathtime_ticks <= 0) return;
    auto &state = entity.squib;
    const auto *target_traits = world.tables.item_death_traits.get(target->item_id);
    state.next_ssn = target_traits && !target_traits->squib_ammo.empty() ?
            entity.script_next_ssn : target->script_next_ssn;
    entity.death_anim_state = state.next_ssn;
    const FixedVec3 source = fixed(entity.position), end = fixed(target->position);
    FixedVec3 direction{io::bam_sub(end.x,source.x), io::bam_sub(end.y,source.y),
            io::bam_sub(end.z,source.z)};
    const int32_t distance = detail::vec_len_ftol(direction.x,direction.y,direction.z);
    state.remaining = distance >= traits->squib_distance_q16 ?
            distance / traits->squib_distance_q16 : 1;
    state.step = distance >= traits->squib_distance_q16 ?
            FixedVec3{direction.x/state.remaining, direction.y/state.remaining,
                    direction.z/state.remaining} : FixedVec3{};
    state.origin = source;
    state.interval = entity.deathtime_ticks;
    state.last_tick = world.logic_tick;
    CollisionMatrix first = rotation(0,0);
    int32_t heading = 0;
    if (distance) {
        direction = {int32_t(int64_t(direction.x)*65536/distance),
                int32_t(int64_t(direction.y)*65536/distance),
                int32_t(int64_t(direction.z)*65536/distance)};
        heading = int32_t(std::atan2(double(direction.y), double(direction.x)) *
                io::kBamPerRadian); // [orig: dbl_7C19D8 = 2^31/pi]
        if (direction.x > 0) first = rotation(0,-1073741760);
        else {
            heading = io::bam_add(heading,2147483520);
            first = rotation(0,1073741760);
        }
    } else direction = {0,65536,0};
    const auto second = rotation(io::bam_sub(0, io::bam_add(
            heading, bam_from_degrees_wrapped(entity.pitch))),0);
    state.direction = rotate(compose(first,second), direction);
    const int32_t twice = io::bam_add(distance,distance);
    state.center = {io::bam_sub(io::bam_add(source.x,end.x)/2,mul(twice,state.direction.x)),
            io::bam_sub(io::bam_add(source.y,end.y)/2,mul(twice,state.direction.y)),
            io::bam_sub(io::bam_add(source.z,end.z)/2,mul(twice,state.direction.z))};
    if (!traits->squib_ammo.empty())
        state.ammo_index = std::max(0, world.tables.ammo.index_of(traits->squib_ammo.c_str()));
    if (!state.ammo_index || !world.tables.ammo.by_index(state.ammo_index)) return;
    Entity clone = entity; // the represented fields of retail's first 0x310 bytes
    clone.death_tick = world.logic_tick;
    world.registry.spawn(1, clone);
}
void tick_squib(World &world, Entity &entity) {
    if (!entity.death_tick || entity.squib.interval <= 0) return;
    auto &state = entity.squib;
    const auto *ammo = world.tables.ammo.by_index(state.ammo_index);
    if (!ammo) return;
    const auto *weapon = entity.equipped_adm_index ? world.tables.weapons.by_index(entity.equipped_adm_index) : nullptr;
    const Vec3 center = floating(state.center);
    int32_t elapsed = io::bam_sub(int32_t(world.logic_tick),int32_t(state.last_tick));
    while (elapsed != 0 && elapsed >= state.interval) {
        // The seed is observed, not advanced, even during catch-up.
        const auto spread = weapon_calc_random_spread_offset(state.spread_q16,
                world.throwables.fan_prng_state,0,false);
            const auto matrix = rotation(spread.yaw_bam,spread.pitch_bam);
        const FixedVec3 direction = rotate(matrix,state.direction);
        const FixedVec3 end{io::bam_add(state.origin.x,int32_t(uint32_t(direction.x)*100u)),
                io::bam_add(state.origin.y,int32_t(uint32_t(direction.y)*100u)),
                io::bam_add(state.origin.z,int32_t(uint32_t(direction.z)*100u))};
        CollisionWorld fallback;
        CollisionWorld *collision = world.collision ? world.collision : &fallback;
        fallback.terrain = world.tables.terrain;
        auto hit = collision->trace_squib(world,entity.handle,state.origin,end);
        int tag = 0;
        FixedVec3 point = end;
        if (hit.hit()) {
            if (hit.hit_class != ProjectileHitClass::Terrain) {
                point = {io::bam_add(state.origin.x,mul(direction.x,hit.distance_q16)),
                        io::bam_add(state.origin.y,mul(direction.y,hit.distance_q16)),
                        io::bam_add(state.origin.z,mul(direction.z,hit.distance_q16))};
            }
            tag = hit.hit_class == ProjectileHitClass::Water ? 11 : hit.surface_type + 4;
            if (hit.hit_class == ProjectileHitClass::Person) {
                LiveRound round;
                round.owner = entity.primary_occupant;
                round.shooter_handle = round.owner.packed;
                round.adm_index = entity.equipped_adm_index;
                round.ammo_index = state.damage_ammo_index;
                round.pos = entity.position;
                round.yaw_bam = bam_heading_from_mission_yaw_deg(entity.yaw);
                round.pitch_bam = bam_from_degrees_wrapped(entity.pitch);
                round.roll_bam = bam_from_degrees_wrapped(entity.roll);
                FixedVec3 velocity{entity.veh.vel_x,entity.veh.vel_y,entity.veh.slide_z};
                world.round_sim.process_damage_hit(world,round,hit,velocity);
                entity.veh.vel_x=velocity.x; entity.veh.vel_y=velocity.y; entity.veh.slide_z=velocity.z;
            }
        }
        if (tag > 0) {
            RoundImpact impact;
            impact.position=floating(point); impact.direction=floating(direction);
            impact.ammo_index=state.ammo_index; impact.effect_tag=tag;
            impact.tick=world.logic_tick; impact.source_order=world.round_sim.next_impact_order++;
            // The ordinary impact presenter applies its full-volume positional sound.
            if (world.round_sim.impacts.size() < RoundSim::kMaxPendingImpacts)
                world.round_sim.impacts.push_back(impact);
        }
        state.origin = {io::bam_add(state.origin.x,state.step.x),
                io::bam_add(state.origin.y,state.step.y),io::bam_add(state.origin.z,state.step.z)};
        elapsed = io::bam_sub(elapsed,state.interval);
        const int32_t old = state.remaining;
        state.remaining = io::bam_sub(old,1);
        if (old) {
            const char *sound = weapon ?
                    weapon->action_fsm.actions[weapon_action::kFire].soundsetend : ammo->ai_launch_set.c_str();
            world.out.fire_sounds.play_with_distance_delay(sound,center,entity.bms_id,entity.handle.packed);
        } else {
            const int32_t next = state.next_ssn;
            const int32_t bms_id = entity.bms_id;
            const uint16_t packed = entity.handle.packed;
            world.commands.remove_ssn(EntityTarget(entity.handle));
            if (next) world.commands.kill_ssn(EntityTarget(uint16_t(next)));
            else if (weapon) world.out.fire_sounds.play_with_distance_delay(
                    weapon->action_fsm.soundtrailoff,center,bms_id,packed);
            // Retail may continue reading freed pool memory during catch-up.
            // Stop at destruction; never let a later allocation inherit that work.
            return;
        }
    }
    state.last_tick = uint32_t(io::bam_sub(int32_t(world.logic_tick),elapsed));
    if (weapon && weapon->action_fsm.soundfireloop[0]) {
        SoundEmitterEvent sound;
        sound.source_spawn_id=entity.registry_spawn_id; sound.source_handle=entity.handle.packed;
        sound.source_bms_id=entity.bms_id; sound.pos=center; sound.emitted_tick=world.logic_tick;
        sound.lifetime_ticks=10; sound.pitch_q16=65536; sound.volume_q8_8=65535;
        sound.set_name=weapon->action_fsm.soundfireloop;
        world.out.sound_emitters.publish(std::move(sound));
    }
}
} // namespace opennova::world
