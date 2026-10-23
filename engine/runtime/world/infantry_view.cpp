// The player body's (org2) view phase, one kernel for the authority's body tick and a client's
// decoded rows (replication::row_org2_view_phase), split out of infantry.cpp and
// client_replica_legs.cpp, which each carried a copy.
#include <runtime/world/infantry_view.h>

#include <base/io/bam.h>

#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h> // infantry_anim_flags
#include <runtime/world/world.h>

namespace opennova::world {

namespace {

// The leg model's constants: the re-plant hysteresis (|d| past 0x38E38E0, and past 0x15555540 or
// on the leg's 64-tick window), the quarter-step chase clamped to 0x3000000, the twist held within
// 0x30000000 of the Yaw [orig: the deltas off the legs @0x4B4993 (L) / @0x4B499B (R), the tests
// @0x4B49A6 / @0x4B49B5, the L window `add ebx, -32` @0x4B49AD, the R window `test ebp, ebp`
// @0x4B49D0 (ebp = tick & 0x3F @0x4B4680), the movement re-plant @0x4B49DD / @0x4B49E3, the
// chases @0x4B49E9..0x4B4AA3 (the rate clamp @0x4B49FB, the twist @0x4B4A23, L's from @0x4B4A49)].
constexpr int32_t kLegReplantMin = 0x38E38E0;
constexpr int32_t kLegReplantSnap = 0x15555540;
constexpr int32_t kLegChaseClamp = 0x3000000;
constexpr int32_t kLegTwistLimit = 0x30000000;
// The skip branch's Yaw clamp (120 degrees), the pitch clamp's two limits (80 @0x4B4B76, prone 40
// @0x4B4B7D).
constexpr int32_t kViewYawLimit = 0x55555500;
constexpr int32_t kPitchLimit = 0x38E38E00;
constexpr int32_t kPronePitchLimit = 0x1C71C700;

// A vehicle (its def's type 1, Entity_HasActiveParent's test) in its crash state (+0x2EC).
bool crashed_vehicle(const Entity *e) {
    return e != nullptr && e->has_item_def && e->item_type == 1u && e->veh.crashed != 0;
}

} // namespace

// The view phase, in its pass's order, with no locality test:
// - under a canopy (Flags 0x20) the body heading (+0x8C) chases a sixteenth of the way to the Yaw
//   and both re-plant targets (+0x2E4 / +0x2E8) take it;
// - on a ladder, carried or under a canopy (Flags 0x100060) the leg model is skipped: the Yaw is
//   held within 0x55555500 (120 degrees) of the body heading, unless its mount's config (+0x86C)
//   is 3 or its ground entity is a vehicle in its crash state (`yaw_clamp_skipped`), and both legs
//   (+0x2D4 / +0x2D8) take the body heading, their targets left as they stand;
// - otherwise the LEGS chase the Yaw and the body heading is their midpoint: a movement state
//   (+0x2BC's flag bit 0) re-plants both targets on the Yaw, an idle one each leg past 0x38E38E0
//   off it and either past 0x15555540 or on its 64-tick window (the left's 32 ticks behind, both
//   on the raw tick); each leg steps a quarter, clamped to 0x3000000, and is held within
//   0x30000000 of the Yaw;
// - then the Pitch is held within 0x38E38E00 (80 degrees) of savedLivePitch (+0x90), 0x1C71C700
//   (40) under the prone local, unless the mount is a vehicle in its crash state
//   (`pitch_clamp_skipped`).
// The local body's clamp also lands on g_LocalPlayerLookYaw, which our `yaw` word is for it.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the canopy chase @0x4B4945..0x4B496C, the
//  skip test @0x4B4972..0x4B4978, the mount config test @0x4B4AC6..0x4B4ADE, the ground test
//  @0x4B4AE0..0x4B4AF9 (Entity_HasActiveParent @0x402BB0 called @0x4B4AE9), the clamp
//  @0x4B4AFB..0x4B4B59, the legs @0x4B4B5F..0x4B4B6B, the selector @0x4B497E..0x4B4984, the idle
//  windows @0x4B4991..0x4B49E3 (the key @0x4B467A..0x4B4683), the chases @0x4B49E9..0x4B4AA3, the
//  midpoint @0x4B4AA9..0x4B4ABB, the limit select @0x4B4B71..0x4B4B7D, the mount's crash test
//  @0x4B4B82..0x4B4BA0, the pitch clamp @0x4B4BA2..0x4B4BC6]
void infantry_org2_view_phase(const Org2ViewWords &w, const Org2ViewGates &g) {
    if ((g.flags & kEntityFlagParachute) != 0u) {
        w.body_heading = io::bam_add(w.body_heading,
                io::bam_sar(io::bam_add(io::bam_sub(w.yaw, w.body_heading), 8), 4));
        w.leg_target[0] = w.leg_target[1] = w.body_heading;
    }
    if ((g.flags & (kEntityFlagLadderContact | kEntityFlagMounted | kEntityFlagParachute)) != 0u) {
        if (!g.yaw_clamp_skipped) {
            const int32_t d = io::bam_sub(w.yaw, w.body_heading);
            if (d > kViewYawLimit) w.yaw = io::bam_add(w.body_heading, kViewYawLimit);
            if (d < -kViewYawLimit) w.yaw = io::bam_sub(w.body_heading, kViewYawLimit);
        }
        w.leg_yaw[0] = w.leg_yaw[1] = w.body_heading;
    } else {
        const int32_t yaw = w.yaw;
        if ((infantry_anim_flags(g.anim_state) & 0x1u) != 0u) {
            w.leg_target[1] = yaw;
            w.leg_target[0] = yaw;
        } else {
            const int32_t dl = io::bam_abs(io::bam_sub(yaw, w.leg_yaw[1]));
            if ((dl > kLegReplantSnap || ((g.tick - 32u) & 63u) == 0u) && dl > kLegReplantMin)
                w.leg_target[1] = yaw;
            const int32_t dr = io::bam_abs(io::bam_sub(yaw, w.leg_yaw[0]));
            if ((dr > kLegReplantSnap || (g.tick & 63u) == 0u) && dr > kLegReplantMin)
                w.leg_target[0] = yaw;
        }
        for (int leg = 0; leg < 2; ++leg) {
            const int32_t ldiff = io::bam_sub(w.leg_target[leg], w.leg_yaw[leg]);
            int32_t lstep = io::bam_sar(io::bam_add(ldiff, 2), 2);
            if (lstep > kLegChaseClamp) lstep = kLegChaseClamp;
            if (lstep < -kLegChaseClamp) lstep = -kLegChaseClamp;
            w.leg_yaw[leg] = io::bam_add(w.leg_yaw[leg], lstep);
            const int32_t twist = io::bam_sub(w.leg_yaw[leg], yaw);
            if (twist > kLegTwistLimit)
                w.leg_yaw[leg] = io::bam_add(yaw, kLegTwistLimit);
            else if (twist < -kLegTwistLimit)
                w.leg_yaw[leg] = io::bam_sub(yaw, kLegTwistLimit);
        }
        w.body_heading = io::bam_add(
                w.leg_yaw[1], io::bam_sar(io::bam_sub(w.leg_yaw[0], w.leg_yaw[1]), 1));
    }
    if (g.pitch_clamp_skipped) return;
    const int32_t limit = g.prone ? kPronePitchLimit : kPitchLimit;
    if (io::bam_sub(w.pitch, w.body_pitch) > limit) w.pitch = io::bam_add(w.body_pitch, limit);
    if (io::bam_sub(w.pitch, w.body_pitch) < -limit) w.pitch = io::bam_sub(w.body_pitch, limit);
}

// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the head's Flags @0x4B411B, the prone local
//  @0x4B416C..0x4B4183, its living-mount local @0x4B41A2..0x4B41C0, the clear @0x4B46FC..0x4B471B]
bool infantry_org2_prone_local(bool prone_order, uint32_t flags, bool living_mount_with_def) {
    return prone_order &&
            (flags & (kEntityFlagInAir | kEntityFlagDrowning | kEntityFlagLadderContact)) == 0u &&
            !living_mount_with_def && (flags & kEntityFlagMounted) == 0u;
}

// The authority's player body, ahead of its scoped drift and its think cadence, seated or not:
// its Yaw and Pitch the local body's look accumulators (the mouse) or any other body's own words
// (a remote player's the 0x0C read-apply and the network chase). The mount is +0x16C, the ground
// +0x28, which the prologue points at the mount while one is held; the prone local is the
// body's stance (its MoveOrder bit 0x100).
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the ground = mount store
//  @0x4B41A2..0x4B41B4, the phase @0x4B4945..0x4B4BC6 (no locality test but the
//  g_LocalPlayerLookYaw mirrors @0x4B4B16..0x4B4B2A / @0x4B4B45..0x4B4B59, their local tests'
//  `jnz` @0x4B4B1C / @0x4B4B4B)]
void infantry_org2_view_tick(const World &world, AiEntity &e, const Entity *tick_entity,
                             uint32_t tick) {
    InfantryState &inf = e.inf;
    const bool local = inf.is_local_player;
    const uint32_t flags =
            tick_entity != nullptr ? (tick_entity->flags | tick_entity->engine_flags) : 0u;
    const Entity *mount = tick_entity != nullptr && tick_entity->mounted
            ? world.registry.get(tick_entity->mount_target)
            : nullptr;
    const Entity *ground = mount != nullptr ? mount
            : tick_entity != nullptr        ? world.registry.get(tick_entity->ground_target)
                                            : nullptr;
    Org2ViewGates gates;
    gates.flags = flags;
    gates.anim_state = inf.anim_state;
    gates.tick = tick;
    gates.prone = infantry_org2_prone_local(inf.stance == InfantryState::Stance::kProne, flags,
            mount != nullptr && tick_entity->health > 0 && mount->has_item_def);
    gates.yaw_clamp_skipped =
            (mount != nullptr && mount->emplaced_config_valid && mount->emplaced_config == 3) ||
            crashed_vehicle(ground);
    gates.pitch_clamp_skipped = crashed_vehicle(mount);
    int32_t &yaw = local ? inf.target_heading : e.heading;
    int32_t &pitch = local ? inf.look_pitch : e.pitch;
    infantry_org2_view_phase(
            Org2ViewWords{yaw, pitch, inf.body_heading, inf.leg_yaw, inf.leg_target, e.body_pitch},
            gates);
    // Our local body keeps its look in the input accumulators: the render / aim words take the
    // clamped look, retail's one +0x10 / +0x14 pair.
    if (local) {
        e.heading = inf.target_heading;
        e.pitch = inf.look_pitch;
    }
}

} // namespace opennova::world
