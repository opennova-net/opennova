#include <runtime/world/infantry_internal.h>
#include <formats/def/def.h>
#include <runtime/world/carrier_motion.h>
#include <runtime/world/infantry_view.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>
#include <base/io/bam.h>
#include <algorithm>
#include <cmath>

namespace opennova::world {
// The org1 deck ride. [orig: Entity_UpdateInfantryAI @0x4BA45D..0x4BA891]
static void follow_ground_carrier(AiEntity &e, World &world, int32_t bottom) {
    Entity *self = world.registry.get(e.handle);
    if (self == nullptr) return;
    // An org1 rider rides its parent: the motor head points groundEntity (+0x28)
    // at the parent (+0x16C), so the deck ride below turns its look and aim with
    // the carrier before the seat pose overwrites its position and body frame.
    // [orig: Entity_UpdateInfantryAI @0x4B9A0D..0x4B9A11]
    if (self->mounted) self->ground_target = self->mount_target;
    const Entity *carrier = world.registry.get(self->ground_target);
    if (carrier == nullptr || !carrier->saved_live_valid) return;
    int32_t pos[3], yaw, pitch, roll;
    carrier_pose_fixed(*carrier, pos, yaw, pitch, roll);
    CarrierMotionPose p;
    std::copy_n(e.self->position.words().data(), 3, p.pos);
    // Both inline twins rotate about the animation capsule midpoint, then
    // restore the bias. The standalone shared operation has no such bias.
    // [orig: org1 @0x4BA509 / @0x4BA79B; org2 @0x4B53A1 / @0x4B5649]
    p.pos[2] = io::bam_sub(p.pos[2], bottom >> 1);
    p.yaw_bam = e.heading;
    follow_carrier_motion(*carrier, p);
    p.pos[2] = io::bam_add(p.pos[2], bottom >> 1);
    e.self->position.set_q16({p.pos[0], p.pos[1], p.pos[2]});
    const int32_t dyaw = io::bam_sub(yaw, carrier->saved_live_yaw);
    const int32_t dpitch = p.pitch_bam;
    InfantryState &inf = e.inf;
    inf.body_heading = io::bam_add(inf.body_heading, dyaw);
    inf.target_heading = io::bam_add(inf.target_heading, dyaw);
    for (int i = 0; i < 2; ++i) {
        inf.leg_yaw[i] = io::bam_add(inf.leg_yaw[i], dyaw);
        inf.leg_target[i] = io::bam_add(inf.leg_target[i], dyaw);
    }
    e.body_pitch = io::bam_add(e.body_pitch, dpitch);
    e.roll = io::bam_add(e.roll, p.roll_bam);
    // An NPC looking at a target on another carrier keeps its world aim, and a
    // rider of a turret parent (def+0x58 & 0x1000) leaves its look to the
    // turret. Body/leg adoption and physical transport still occur.
    // [orig: turret gate @0x4BA812..0x4BA82A; target gate @0x4BA82C]
    const Entity *parent = self->mounted ? world.registry.get(self->mount_target) : nullptr;
    const bool turret_parent = parent != nullptr && parent->has_item_def &&
            (parent->item_attrib2 & def::DEF_ITEM_ATTRIB2_ISTURRET) != 0;
    const Entity *target = world.registry.get(inf.combat_target);
    if (!turret_parent && (target == nullptr || target->ground_target == self->ground_target)) {
        e.heading = p.yaw_bam;
        inf.aim_pitch = io::bam_add(inf.aim_pitch, dpitch);
        inf.aim_heading = io::bam_add(inf.aim_heading, dyaw);
    }
    e.pitch = io::bam_add(e.pitch, dpitch);
}

// The org2 deck ride, which a seated body runs too: its ground (+0x28) is its mount, the
// update's head having pointed it there, and only a body with no mount (+0x16C) drops a ground
// outside the ground's bound radius. The ground's position delta and its attitude's rotation
// carry the body's position (a seated body's seat carry later overwrites it), and the turn
// (infantry_org2_ride_turn) its words. Then the driver's hold: a body that is its ground's first
// occupant on a PlayerControl def, with no free-look bit and its ground no crashed vehicle, takes
// back its Yaw and look.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the ground @0x4B5288, the mounted test
//  @0x4B529F, the radius drop @0x4B52A7..0x4B52FF, the ground test @0x4B5302, the translation
//  @0x4B5329..0x4B535D, the rotation @0x4B537B..0x4B5653, the turn @0x4B5656..0x4B5723; the hold
//  @0x4B572E..0x4B57AA; the ground = mount store @0x4B41A2..0x4B41B4]
static void org2_ride(AiEntity &e, World &world, int32_t bottom) {
    Entity *self = world.registry.get(e.handle);
    if (self == nullptr) return;
    const Entity *ground = world.registry.get(self->ground_target);
    if (ground == nullptr || !ground->saved_live_valid) return;
    int32_t pos[3], yaw, pitch, roll;
    carrier_pose_fixed(*ground, pos, yaw, pitch, roll);
    if (!self->mounted) {
        const double dx = double(e.self->position.x.q16) - pos[0];
        const double dy = double(e.self->position.y.q16) - pos[1];
        const double dz = double(e.self->position.z.q16) - pos[2];
        const int32_t distance = static_cast<int32_t>(std::min(
                std::sqrt(dx * dx + dy * dy + dz * dz), 2147418112.0));
        if (distance > ground->bound_radius.q16) {
            self->ground_target = {};
            return;
        }
    }
    InfantryState &inf = e.inf;
    // The local body's look is its input accumulator, which its Yaw mirrors here.
    int32_t *look_yaw = inf.is_local_player ? &inf.target_heading : nullptr;
    const int32_t yaw_before = e.heading;
    const int32_t look_before = inf.target_heading;
    CarrierMotionPose p;
    std::copy_n(e.self->position.words().data(), 3, p.pos);
    p.pos[2] = io::bam_sub(p.pos[2], bottom >> 1);
    p.yaw_bam = e.heading;
    follow_carrier_motion(*ground, p);
    p.pos[2] = io::bam_add(p.pos[2], bottom >> 1);
    e.self->position.set_q16({p.pos[0], p.pos[1], p.pos[2]});
    const Entity *mount = self->mounted ? world.registry.get(self->mount_target) : nullptr;
    const bool turret_mount = mount != nullptr && mount->has_item_def &&
            (mount->item_attrib2 & def::DEF_ITEM_ATTRIB2_ISTURRET) != 0;
    const Org2RideWords w{e.heading, look_yaw, inf.body_heading, inf.leg_yaw, inf.leg_target,
            e.body_pitch, inf.carrier_pitch_lag, inf.aim_pitch, e.roll};
    infantry_org2_ride_turn(w, io::bam_sub(yaw, ground->saved_live_yaw), p.pitch_bam, p.roll_bam,
            turret_mount);
    if (!infantry_org2_ride_holds(ground->primary_occupant == self->handle,
                ground->has_item_def ? ground->item_attrib : 0u, self->net_move_input,
                crashed_vehicle(ground)))
        return;
    int32_t ground_ground_dyaw = 0;
    if (const Entity *gg = world.registry.get(ground->ground_target)) {
        if (gg->saved_live_valid) {
            int32_t gpos[3], gyaw, gpitch, groll;
            carrier_pose_fixed(*gg, gpos, gyaw, gpitch, groll);
            ground_ground_dyaw = io::bam_sub(gyaw, gg->saved_live_yaw);
        }
    }
    infantry_org2_ride_hold(w, yaw_before, look_before, ground_ground_dyaw);
}

bool infantry_follow_carrier(AiEntity &e, World &world, int32_t bottom, bool player_body) {
    const int32_t before[3] = {e.self->position.x.q16, e.self->position.y.q16, e.self->position.z.q16};
    if (player_body) org2_ride(e, world, bottom);
    else follow_ground_carrier(e, world, bottom);
    const Entity *self = world.registry.get(e.handle);
    const bool moved = e.self->position.x.q16 != before[0] || e.self->position.y.q16 != before[1] || e.self->position.z.q16 != before[2];
    if (!player_body || self == nullptr) return moved;
    // Org2 feeds the ride's pitch lag into the Pitch a thirty-second a tick, the residual after
    // walking off included, on foot or seated, unless its ground (the mount, seated) is a crashed
    // vehicle. The local input mirror must retain this motor write.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the gate @0x4B57B0..0x4B57CB, entity+0x2E0
    //  @0x4B57CD..0x4B57E5]
    if (crashed_vehicle(world.registry.get(self->ground_target))) return moved;
    const int32_t step = infantry_org2_pitch_lag_step(e.inf.carrier_pitch_lag);
    e.pitch = io::bam_add(e.pitch, step);
    e.inf.look_pitch = io::bam_add(e.inf.look_pitch, step);
    return moved;
}
} // namespace opennova::world
