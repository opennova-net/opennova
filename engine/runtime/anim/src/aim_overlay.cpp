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
    const int32_t HLD = in.pitch_kick_accum;

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

    // Controller/driver slots 2/5: the seated clip owns the torso and arms, while
    // neck/head retain the full-aim matrix and the prebuilt leg chains remain intact.
    // [orig: @ 0x4b1b4c..0x4b1ba9]
    if (in.mount_mode == MountMode::Seated) {
        out[kOverlaySpine] = out[kOverlayBody];
        out[kOverlayUpperSpine] = out[kOverlayBody];
        out[kOverlayClavicle] = out[kOverlayBody];
        out[kOverlayArm] = out[kOverlayBody];
        out[kOverlayNeck] = out[kOverlayHead];
        return;
    }

    // Gunner slot 3 reads the mounted target's authored phrase_set dword from its
    // item definition +0x86c.  Config zero is a real branch, so invalid metadata
    // must bypass the switch rather than alias it.
    // [orig: config read @ 0x4b1884; switch @ 0x4b188c..0x4b1b35]
    if (in.mount_mode == MountMode::Gunner && in.mount_config_valid) {
        const int32_t config = in.mount_config;
        if (config == 0) {
            // Config 0 counter-lean. [orig: @ 0x4b189e..0x4b193e]
            const AimOverlayAngles arm = {
                bam_sub(B, bam_sar(bam_sub(A, B), 2)),
                bam_add(bam_sar(PB, 1), bam_sub(bam_dbl(Q), P)),
                bam_add(R, LN)};
            out[kOverlaySpine] = out[kOverlayBody];
            out[kOverlayUpperSpine] = out[kOverlayBody];
            out[kOverlayClavicle] = arm;
            out[kOverlayArm] = arm;
            out[kOverlayNeck] = out[kOverlayHead];
            return;
        }
        if (config == 3 || config == 5 || config == 7) {
            // These emplacements lock the entire witnessed upper skeleton to body.
            // [orig: tests @ 0x4b195a; copies @ 0x4b1ac9..0x4b1b35]
            out[kOverlaySpine] = out[kOverlayBody];
            out[kOverlayUpperSpine] = out[kOverlayBody];
            out[kOverlayClavicle] = out[kOverlayBody];
            out[kOverlayArm] = out[kOverlayBody];
            out[kOverlayNeck] = out[kOverlayBody];
            out[kOverlayHead] = out[kOverlayBody];
            return;
        }
        if (config == 6) {
            // Config 6 is body-locked too, except head deliberately keeps full aim.
            // [orig: test @ 0x4b1963; copies @ 0x4b1975..0x4b19cf]
            out[kOverlaySpine] = out[kOverlayBody];
            out[kOverlayUpperSpine] = out[kOverlayBody];
            out[kOverlayClavicle] = out[kOverlayBody];
            out[kOverlayArm] = out[kOverlayBody];
            out[kOverlayNeck] = out[kOverlayBody];
            return;
        }

        // Every other valid nonzero phrase_set uses the second witnessed
        // counter-lean recipe. [orig: @ 0x4b19ed..0x4b1ab2]
        const AimOverlayAngles arm = {
            bam_sub(B, bam_sar(bam_sub(A, B), 2)),
            bam_sub(bam_add(Q, bam_sar(PB, 1)), bam_sar(bam_sub(P, Q), 1)),
            bam_add(R, LN)};
        const AimOverlayAngles upper = {
            bam_sub(B, bam_sar(bam_sub(A, B), 4)),
            bam_add(Q, bam_sar(PB, 1)),
            bam_add(R, bam_sar(LN, 1))};
        out[kOverlaySpine] = out[kOverlayBody];
        out[kOverlayUpperSpine] = upper;
        out[kOverlayClavicle] = arm;
        out[kOverlayArm] = arm;
        out[kOverlayNeck] = out[kOverlayHead];
        return;
    }

    if (in.aim_state) {
        // The aim branch (g_animStateFlagsTable & 0x40). [orig: @ 0x4b1bbe..0x4b1ce4]
        // elbow (arms): 3/4 aim yaw, full aim pitch + pitch-kick + 2x pitch blend.
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
            // [orig: @ 0x4b1da4..0x4b1dce -- the pitch kick decays to a quarter here]
            out[kOverlayArm] = {bam_add(A, bam_sar(bam_sub(B, A), 2)),
                                bam_add(bam_add(P, bam_sar(HLD, 2)), bam_dbl(PB)),
                                bam_add(R, LN)};
        }
    }
}

// See the aim_overlay.h contract: the attach basis, built from the entity's own angle
// triple rather than blended from the body/aim pair.
AimOverlayAngles compute_held_weapon_attach_angles(const AimOverlayInputs &in) {
    const int32_t R = in.rolling ? 0 : in.roll;
    // Pure aim yaw — the arms take 3/4 of the body->aim delta here, the weapon does not.
    // [orig: entity->Yaw = Yaw @ 0x4b1bf2 / @ 0x4b1df4]
    const int32_t yaw = in.aim_yaw;
    // [orig: entity->Roll = savedRoll + leanAngle @ 0x4b1bdc / @ 0x4b1de5]
    const int32_t roll = bam_add(R, in.lean);
    // The pitch-kick term is present only in the aim branch.
    // [orig: aim @ 0x4b1bf5 savedPitch + pitchKickAccum + 2*pitchBlend;
    //  non-aim @ 0x4b1df7 savedPitch + 2*pitchBlend]
    const int32_t pitch = in.aim_state
                                  ? bam_add(bam_add(in.aim_pitch, in.pitch_kick_accum),
                                            bam_dbl(in.pitch_blend))
                                  : bam_add(in.aim_pitch, bam_dbl(in.pitch_blend));
    return {yaw, pitch, roll};
}

void splice_weapon_channel_rotations(const std::vector<int> &parent_index,
                                     const uint8_t *masked_bone,
                                     const std::vector<Quat> &weapon_local_rotation,
                                     std::vector<Quat> &primary_local_rotation) {
    const size_t n = primary_local_rotation.size();
    if (parent_index.size() < n || weapon_local_rotation.size() != n ||
        masked_bone == nullptr) {
        return;
    }

    // Both AnimMap channels compute complete absolute bone rotations before the
    // mask is applied. Select in that space, then re-localize the ENTIRE hierarchy:
    // an unmasked accessory below a masked hand must retain its primary absolute
    // orientation rather than inheriting the weapon hand's rotation.
    std::vector<Quat> primary_world(n);
    std::vector<Quat> weapon_world(n);
    std::vector<Quat> mixed_world(n);
    for (size_t k = 0; k < n; ++k) {
        const int p = parent_index[k];
        const bool has_parent = p >= 0 && static_cast<size_t>(p) < k;
        primary_world[k] = quat_normalize(
            has_parent ? quat_mul(primary_world[static_cast<size_t>(p)],
                                  primary_local_rotation[k])
                       : primary_local_rotation[k]);
        weapon_world[k] = quat_normalize(
            has_parent ? quat_mul(weapon_world[static_cast<size_t>(p)],
                                  weapon_local_rotation[k])
                       : weapon_local_rotation[k]);
        mixed_world[k] = masked_bone[k] != 0 ? weapon_world[k] : primary_world[k];
        primary_local_rotation[k] =
            has_parent
                ? quat_normalize(quat_mul(quat_inv(mixed_world[static_cast<size_t>(p)]),
                                          mixed_world[k]))
                : mixed_world[k];
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
