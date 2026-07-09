// Third-person body aim overlay -- see aim_overlay.h.
// [orig: Entity_BuildBoneTransformMatrices @ 0x4b1290; docs/world/world-wac-ai-re.md section 14]

#include "anim/aim_overlay.h"

#include <io/bam.h>

namespace opennova::anim {

using io::bam_add;
using io::bam_dbl;
using io::bam_sar;
using io::bam_sub;

// The original mutates entity Yaw/Pitch/Roll and calls the fixed-point matrix builder
// per overlay; here each build is one AimOverlayAngles record. Differences like
// (body - aim) are int32 BAM subtractions (wraparound is the shortest-arc delta) and the
// blend steps are arithmetic right shifts, exactly as the sar-based original — routed
// through io/bam.h so the x86 wrap/sar semantics hold without signed-overflow UB.
void compute_aim_overlay_angles(const AimOverlayInputs &in,
                                AimOverlayAngles out[kOverlayClassCount]) {
    const int32_t A = in.aim_yaw;
    const int32_t P = in.aim_pitch;
    const int32_t B = in.body_yaw;
    const int32_t Q = in.body_pitch;
    // States 41/42 (roll_left/right) zero the roll sources before any build.
    // [orig: @ 0x4b17d3..0x4b17d9]
    const int32_t R = in.rolling ? 0 : in.roll;
    const int32_t TR = in.rolling ? 0 : in.torso_roll;
    const int32_t LN = in.lean;
    const int32_t PB = in.pitch_blend;
    const int32_t HLD = in.head_look_decay;

    // Built unconditionally at the top of the original, before the branch split.
    // v142 (head/full aim): Roll = torsoRoll + lean/2 while Yaw/Pitch stay the aim pair.
    // [orig: @ 0x4b17e5 + 0x4b180a. The local-player fine pitch kick
    //  (dword_3346FA8 << 17, @ 0x4b17fb) is unported -- its writer is unwitnessed.]
    out[kOverlayHead] = {A, P, bam_add(TR, bam_sar(LN, 1))};
    // v132 (body): bodyHeading / bodyPitch / original Roll. [orig: @ 0x4b182e]
    out[kOverlayBody] = {B, Q, R};
    // v143 / v144 (leg chains): the leg chase yaws with body pitch. [orig: @ 0x4b1845 /
    //  0x4b185c -- the fields misnamed torsoYaw/torsoPitch in the IDB; section 3.3]
    out[kOverlayLegR] = {in.leg_yaw_r, Q, R};
    out[kOverlayLegL] = {in.leg_yaw_l, Q, R};

    if (in.aim_state) {
        // The aim branch (g_animStateFlagsTable & 0x40). [orig: @ 0x4b1bbe..0x4b1ce4]
        // elbow (arms): 3/4 aim yaw, full aim pitch + head-look + 2x pitch blend.
        out[kOverlayArm] = {bam_add(A, bam_sar(bam_sub(B, A), 2)),
                            bam_add(bam_add(P, HLD), bam_dbl(PB)),
                            bam_add(R, LN)};
        // v137 (upper spine): 3/4 aim yaw, full aim pitch + pitch blend.
        out[kOverlayUpperSpine] = {bam_add(A, bam_sar(bam_sub(B, A), 2)),
                                   bam_add(P, PB),
                                   bam_add(R, LN)};
        // rootMatrix (clavicles) and v138 (neck) are v137 copies on foot.
        // [orig: qmemcpy @ 0x4b1c8f / 0x4b1caa]
        out[kOverlayClavicle] = out[kOverlayUpperSpine];
        out[kOverlayNeck] = out[kOverlayUpperSpine];
        // spineMatrix (lower spine, the first bend segment): half yaw, quarter pitch.
        // [orig: @ 0x4b1cb4..0x4b1ce4]
        out[kOverlaySpine] = {bam_add(A, bam_sar(bam_sub(B, A), 1)),
                              bam_add(bam_add(Q, bam_sar(bam_sub(P, Q), 2)), bam_sar(PB, 1)),
                              bam_add(R, bam_sar(LN, 1))};
    } else {
        // The reduced branch: spine/clavicles/neck stay on the body; only arms and the
        // head track aim (head keeps v142 from above). [orig: @ 0x4b1cf1..0x4b1dfa]
        out[kOverlaySpine] = out[kOverlayBody];
        out[kOverlayUpperSpine] = out[kOverlayBody];
        out[kOverlayClavicle] = out[kOverlayBody];
        out[kOverlayNeck] = out[kOverlayBody];
        if (in.arms_locked) {
            // Flags & 0x100000: arms ride the body matrix too. [orig: @ 0x4b1d48]
            out[kOverlayArm] = out[kOverlayBody];
        } else {
            // [orig: @ 0x4b1da4..0x4b1dce -- head-look decays to a quarter here]
            out[kOverlayArm] = {bam_add(A, bam_sar(bam_sub(B, A), 2)),
                                bam_add(bam_add(P, bam_sar(HLD, 2)), bam_dbl(PB)),
                                bam_add(R, LN)};
        }
    }
}

void apply_aim_overlay(const std::vector<int> &parent_index,
                       const Quat deltas[kOverlayClassCount],
                       const uint8_t *bone_class,
                       std::vector<Quat> &local_rotation) {
    const size_t n = local_rotation.size();
    if (parent_index.size() < n || bone_class == nullptr) return;

    // FK the input pose, apply the per-class world delta, return to parent-local.
    // Positions are untouched: re-anchoring each child at its parent (the original's
    // pivot recomposition) preserves parent-relative origins by construction.
    std::vector<Quat> world(n);
    std::vector<Quat> world_out(n);
    for (size_t k = 0; k < n; ++k) {
        const int p = parent_index[k];
        const Quat w = (p >= 0 && static_cast<size_t>(p) < k)
                               ? quat_mul(world[static_cast<size_t>(p)], local_rotation[k])
                               : local_rotation[k];
        world[k] = quat_normalize(w);
        const uint8_t cls = bone_class[k] < kOverlayClassCount ? bone_class[k]
                                                               : static_cast<uint8_t>(kOverlayBody);
        world_out[k] = quat_normalize(quat_mul(deltas[cls], world[k]));
        local_rotation[k] = (p >= 0 && static_cast<size_t>(p) < k)
                                    ? quat_normalize(quat_mul(quat_inv(world_out[static_cast<size_t>(p)]),
                                                              world_out[k]))
                                    : world_out[k];
    }
}

} // namespace opennova::anim
