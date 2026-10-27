// The body's eye restamp: the CameraOffset the movement resolver and the views read
// (AiSystem::infantry_restamp_eye, its local head anchor and both capsule legs) and the sample-less
// player's four-turn capsule leg (player_body_capsule_eye). Split out of infantry.cpp by
// responsibility (the oversize-TU ratchet, A2-43).

#include <runtime/world/ai.h>

#include <algorithm>
#include <cmath>

#include <base/io/bam.h>
#include <base/io/fixed.h>
#include <runtime/world/geom.h>
#include <runtime/world/infantry.h>
#include <runtime/world/infantry_internal.h>
#include <runtime/world/player_view.h> // player_view_floor_eye_to_terrain (the on-foot local eye leg)
#include <runtime/world/pose_provider.h>
#include <runtime/world/world.h>

namespace opennova::world {

namespace {
// One eye angle's x87 pair: the BAM word times the radians per BAM, its sine and
// cosine scaled by 2^22 and truncated (`_ftol2_sse`).
struct EyeTurn {
    int32_t s = 0;
    int32_t c = 0;
};
EyeTurn eye_turn(int32_t word) {
    const double a = static_cast<double>(word) * io::kRadiansPerBamX87;
    return {static_cast<int32_t>(std::sin(a) * io::kQ22One),
            static_cast<int32_t>(std::cos(a) * io::kQ22One)};
}
} // namespace

void player_body_capsule_eye(int32_t extent, int32_t lean, int32_t yaw, int32_t body_heading,
                             int32_t roll, int32_t body_pitch, int32_t out[3]) {
    // [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - `neg ecx` @0x4B69A5, the
    //  lean's pair @0x4B69AB..0x4B69D4 (`fld dbl_7C3608` @0x4B69AF, the cosine
    //  @0x4B69CC), the twist `sub edx, [esi+8Ch]` @0x4B69DC,
    //  Roll @0x4B6A68, +0x90 @0x4B6A8F, +0x8C @0x4B6AB3]
    const EyeTurn l = eye_turn(io::bam_sub(0, lean));
    const EyeTurn tw = eye_turn(io::bam_sub(yaw, body_heading));
    const EyeTurn r = eye_turn(roll);
    const EyeTurn p = eye_turn(body_pitch);
    const EyeTurn h = eye_turn(body_heading);
    // The lean's arc in the y / z plane, x 0 [orig: @0x4B6A0C..0x4B6A2A].
    const int32_t x0 = 0;
    const int32_t y0 = io::mul_q22(l.s, extent);
    const int32_t z0 = io::mul_q22(l.c, extent);
    // About z by the twist [orig: @0x4B6A32..0x4B6A6D, x @0x4B6ADB].
    const int32_t x1 = io::bam_sub(io::mul_q22(tw.c, x0), io::mul_q22(tw.s, y0));
    const int32_t y1 = io::bam_add(io::mul_q22(tw.c, y0), io::mul_q22(tw.s, x0));
    // About x by Roll [orig: @0x4B6AE1..0x4B6B19, y @0x4B6B39].
    const int32_t y2 = io::bam_sub(io::mul_q22(r.c, y1), io::mul_q22(r.s, z0));
    const int32_t z2 = io::bam_add(io::mul_q22(r.c, z0), io::mul_q22(r.s, y1));
    // About y by the body pitch [orig: @0x4B6B1D..0x4B6B37, @0x4B6B43..0x4B6B5D].
    const int32_t x3 = io::bam_sub(io::mul_q22(p.c, x1), io::mul_q22(p.s, z2));
    const int32_t z3 = io::bam_add(io::mul_q22(p.c, z2), io::mul_q22(p.s, x1));
    // About z by the body heading [orig: @0x4B6B60..0x4B6B96].
    out[0] = io::bam_sub(io::mul_q22(h.c, x3), io::mul_q22(h.s, y2));
    out[1] = io::bam_add(io::mul_q22(h.c, y2), io::mul_q22(h.s, x3));
    // [orig: the floor @0x4B6B98..0x4B6BA0; the stores @0x4B6BA5..0x4B6BAB]
    out[2] = z3 < 0x2000 ? 0x2000 : z3;
}

void AiSystem::infantry_restamp_eye(AiEntity &e, World &world, const RootMotionFrame &frame,
                                    bool mounted_site) {
    // The local on-foot path terrain-floors the absolute eye; the mounted path
    // does not. The capsule fallback retains the org1/org2 extent and lean
    // formulas. [orig: carrier selector @0x4B6386; mounted local selector
    // @0x4B66CD; org2 @0x4B6908..0x4B696C / @0x4B6BB3..0x4B6CC8;
    // org1 @0x4BF078..0x4BF14C]
    InfantryState &inf = e.inf;
    // The capsule legs are the player body's (org2: every player body, the local
    // one when its head sample is lost) or the NPC's (org1).
    const bool org2 = infantry_org2_body(world, e, world.registry.get(e.handle));
    int32_t head_world[3] = {};
    if (inf.is_local_player && world.pose_provider != nullptr &&
            world.pose_provider->resolve_skeletal_anchor(
                    world, e.handle, SkeletalAnchor::Head, head_world)) {
        float head[3] = {
            static_cast<float>(from_fixed(io::bam_sub(head_world[0], e.self->position.x.q16))),
            static_cast<float>(from_fixed(io::bam_sub(head_world[1], e.self->position.y.q16))),
            static_cast<float>(from_fixed(io::bam_sub(head_world[2], e.self->position.z.q16)))
        };
        const Entity *reg = world.registry.get(e.handle);
        // The terrain floor is an ABSOLUTE-space rule, so apply it to the
        // absolute eye and fold the correction back into the delta. It is
        // also on-foot only: the mounted leg is unfloored in retail
        // [orig: the 0x2000 floors @0x4b68e7/@0x4b6b98 belong to the
        //  capsule legs, and @0x4b6908 has no floor of any kind].
        if (!mounted_site) {
            float abs_eye[3] = {static_cast<float>(from_fixed(e.self->position.x.q16) + head[0]),
                                static_cast<float>(from_fixed(e.self->position.y.q16) + head[1]),
                                static_cast<float>(from_fixed(e.self->position.z.q16) + head[2])};
            player_view_floor_eye_to_terrain(
                    terrain,
                    reg != nullptr &&
                            (reg->flags & kEntityFlagIndoors) != 0,
                    abs_eye);
            head[0] = abs_eye[0] - from_fixed(e.self->position.x.q16);
            head[1] = abs_eye[1] - from_fixed(e.self->position.y.q16);
            head[2] = abs_eye[2] - from_fixed(e.self->position.z.q16);
        }
        inf.eye_offset_x = to_fixed(head[0]);
        inf.eye_offset_y = to_fixed(head[1]);
        inf.eye_offset_z = to_fixed(head[2]);
        inf.dbg_eye_source = 1;
    } else {
        inf.dbg_eye_source = 2;
        // Player extent cap: on foot @0x4B698C, mounted @0x4B66D9.
        // [orig: @0x4B698C / @0x4B66D9; floors @0x4B6B98 / @0x4B68E7]
        const int32_t extent = frame.capsule_top - frame.capsule_bottom;
        const int32_t delta = org2 ? std::min(extent, 0xD000)
                                                  : std::max(extent, 0x9000);
        // A player body's eye is the four-turn capsule leg, lateral words included,
        // on foot and seated alike: the mounted site's arm is the same leg over the
        // same words (D-NET-330).
        // [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - on foot
        //  @0x4B698C..0x4B6BAB; seated @0x4B66D9..0x4B68FC, the body heading
        //  `mov edi, [esi+8Ch]` @0x4B668B, the twist @0x4B672E..0x4B6731, Roll
        //  @0x4B67B9, +0x90 @0x4B67E0, `fimul [esi+8Ch]` @0x4B6804, the floor
        //  @0x4B68E7, the stores @0x4B68F5..0x4B68FC]
        if (org2) {
            int32_t eye[3] = {};
            player_body_capsule_eye(delta, inf.lean_angle, e.heading, inf.body_heading, e.roll,
                                    e.body_pitch, eye);
            inf.eye_offset_x = eye[0];
            inf.eye_offset_y = eye[1];
            inf.eye_offset_z = eye[2];
            return;
        }
        // The NPC (org1) restamp reads the Q22 sine table at the rounded top
        // ten bits of the lean and of the heading.
        // [orig: org1 @0x4BF09F..0x4BF14C (lean +0xB0 @0x4BF09F..0x4BF0BA,
        //  heading +0x10 @0x4BF0EE..0x4BF114)]
        const int lean_index = io::bam_table_index(static_cast<uint32_t>(inf.lean_angle) + 0x200000u);
        const int32_t lean_cos = static_cast<int32_t>(io::bam_table_cos(lean_index) * io::kQ22One);
        inf.eye_offset_z =
            static_cast<int32_t>((static_cast<int64_t>(delta) * lean_cos) >> 22);
        const int32_t lean_sin =
            static_cast<int32_t>(io::bam_table_sin(lean_index) * io::kQ22One);
        const int32_t lat_raw = static_cast<int32_t>(
            (static_cast<int64_t>(delta) * lean_sin) >> 22);
        const int32_t lat = (lat_raw * 3) >> 2;
        const int yaw_index =
            io::bam_table_index(static_cast<uint32_t>(e.heading) + 0x200000u);
        const int32_t yaw_sin =
            static_cast<int32_t>(io::bam_table_sin(yaw_index) * io::kQ22One);
        const int32_t yaw_cos =
            static_cast<int32_t>(io::bam_table_cos(yaw_index) * io::kQ22One);
        inf.eye_offset_x = static_cast<int32_t>(
            (static_cast<int64_t>(lat) * yaw_sin) >> 22);
        inf.eye_offset_y = -static_cast<int32_t>(
            (static_cast<int64_t>(lat) * yaw_cos) >> 22);
    }
}

} // namespace opennova::world
