#include <runtime/world/infantry_internal.h>
#include <runtime/world/carrier_motion.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>
#include <base/io/bam.h>
#include <algorithm>
#include <cmath>

namespace opennova::world {
// [orig: org1 Entity_UpdateInfantryAI @0x4BA45D..0x4BA891;
// org2 Entity_UpdateInfantryPlayerBody @0x4B52A0..0x4B5726]
static void follow_ground_carrier(AiEntity &e, World &world, int32_t bottom, bool player_body) {
    Entity *self = world.registry.get(e.handle);
    // Seat posing owns mounted bodies; only the free-standing delta was missing.
    if (self == nullptr || self->mounted) return;
    const Entity *carrier = world.registry.get(self->ground_target);
    if (carrier == nullptr || !carrier->saved_live_valid) return;
    int32_t pos[3], yaw, pitch, roll;
    carrier_pose_fixed(*carrier, pos, yaw, pitch, roll);
    if (player_body) {
        const double dx = double(e.pos[0]) - pos[0];
        const double dy = double(e.pos[1]) - pos[1];
        const double dz = double(e.pos[2]) - pos[2];
        const int32_t distance = static_cast<int32_t>(std::min(
                std::sqrt(dx * dx + dy * dy + dz * dz), 2147418112.0));
        // Only org2 drops an unmounted ground link outside the carrier radius.
        // [orig: @0x4B52A7..0x4B52FF]
        if (distance > to_fixed(carrier->bound_radius)) {
            self->ground_target = {};
            return;
        }
    }
    CarrierMotionPose p;
    std::copy_n(e.pos, 3, p.pos);
    // Both inline twins rotate about the animation capsule midpoint, then
    // restore the bias. The standalone shared operation has no such bias.
    // [orig: org1 @0x4BA509 / @0x4BA79B; org2 @0x4B53A1 / @0x4B5649]
    p.pos[2] = io::bam_sub(p.pos[2], bottom >> 1);
    p.yaw_bam = e.heading;
    follow_carrier_motion(*carrier, p);
    p.pos[2] = io::bam_add(p.pos[2], bottom >> 1);
    std::copy_n(p.pos, 3, e.pos);
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
    // An NPC looking at a target on another carrier keeps its world aim.
    // Body/leg adoption and physical transport still occur. [orig: @0x4BA82C]
    const Entity *target = world.registry.get(inf.combat_target);
    if (player_body || target == nullptr || target->ground_target == self->ground_target) {
        e.heading = p.yaw_bam;
        inf.aim_pitch = io::bam_add(inf.aim_pitch, dpitch);
        if (!player_body) inf.aim_heading = io::bam_add(inf.aim_heading, dyaw);
    }
    if (player_body) inf.carrier_pitch_lag = io::bam_add(inf.carrier_pitch_lag, dpitch);
    else e.pitch = io::bam_add(e.pitch, dpitch);
}
bool infantry_follow_carrier(AiEntity &e, World &world, int32_t bottom, bool player_body) {
    const int32_t before[3] = {e.pos[0], e.pos[1], e.pos[2]};
    follow_ground_carrier(e, world, bottom, player_body);
    const Entity *self = world.registry.get(e.handle);
    const bool moved = e.pos[0] != before[0] || e.pos[1] != before[1] || e.pos[2] != before[2];
    if (!player_body || self == nullptr || self->mounted) return moved;
    // Org2 feeds carrier pitch gradually into look, including the residual
    // after walking off. The local input mirror must retain this motor write.
    // [orig: entity+0x2E0, @0x4B57CD..0x4B57E5]
    int32_t &lag = e.inf.carrier_pitch_lag;
    const int32_t step = io::bam_add(lag, 16) >> 5;
    e.pitch = io::bam_add(e.pitch, step);
    e.inf.look_pitch = io::bam_add(e.inf.look_pitch, step);
    lag = io::bam_sub(lag, step);
    lag = io::bam_sub(lag, lag >> 31);
    return moved;
}
} // namespace opennova::world
