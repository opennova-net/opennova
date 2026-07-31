// Third-person body aim overlay -- the "torso bend". Structural translation of the
// per-bone overlay-matrix system: every bone's world orientation is the sampled anim
// pose times ONE of nine per-segment orientation matrices, each a blend of the aim
// angles vs the body angles; the blend gradient across the spine/head is the visible
// waist bend, and the legs follow their own lagged chase yaws.
//
// [orig: Entity_BuildBoneTransformMatrices @ 0x4b1290 -- the witness record is
//  docs/world/world-wac-ai-re.md section 14 (D-INF-11). The switch there is indexed by
//  (modelBoneIdx - 1) via the byte table @ 0x4b25c0; Hex-Rays shows the case labels
//  shifted -1. Bone 0 (BN01 Hips) takes the default (pure body) matrix.]
//
// All angles are BAM32 (2^32 = full turn); blends are exact int32 arithmetic with the
// original's arithmetic shift steps, so BAM wraparound behaves identically. Conversion
// to embedder-space bases happens in the embedder layer (single-sourced there); this header owns
// the blend math and the bone-class map only.

#ifndef OPENNOVA_ANIM_AIM_OVERLAY_H
#define OPENNOVA_ANIM_AIM_OVERLAY_H

#include <cstdint>
#include <vector>

#include "anim/anim_sample.h"

namespace opennova::anim {

// One slot per distinct overlay matrix the original builds (its stack locals, in the
// witnessed naming): v132 body, spineMatrix, v137, rootMatrix, elbowMatrix, v143, v144,
// v138, v142. [orig: 0x4b17ad..0x4b1dfa]
enum OverlayClass : uint8_t {
    kOverlayBody = 0,    // v132: bodyHeading / bodyPitch (hips, accessories, default)
    kOverlaySpine,       // spineMatrix: the first bend segment (BN02 Lower Spine)
    kOverlayUpperSpine,  // v137 (BN03 Upper Spine)
    kOverlayClavicle,    // rootMatrix (BN04/BN05; = v137 on foot, differs when mounted)
    kOverlayArm,         // elbowMatrix (upper arms, forearms, hands, incl. stow bone)
    kOverlayLegR,        // v143: right leg chain, yaw = leg chase R (+0x2d4)
    kOverlayLegL,        // v144: left leg chain, yaw = leg chase L (+0x2d8)
    kOverlayNeck,        // v138 (BN14; = v137 on foot, full aim when seated)
    kOverlayHead,        // v142: full aim (BN15)
    kOverlayClassCount
};

// Model bone index (== BN## - 1, the .bad file order) -> overlay class.
// [orig: the bone-index switch @ 0x4b1f3a; map witnessed in section 14.2. Bones >= 19
// (helmet/gear accessories) take kOverlayBody, matching the default case.]
inline constexpr uint8_t kOverlayClassByBoneIndex[19] = {
    kOverlayBody,        // 0  BN01 Hips (default case)
    kOverlaySpine,       // 1  BN02 Lower Spine
    kOverlayUpperSpine,  // 2  BN03 Upper Spine
    kOverlayClavicle,    // 3  BN04 R Clavicle
    kOverlayClavicle,    // 4  BN05 L Clavicle
    kOverlayArm,         // 5  BN06 R UpperArm
    kOverlayArm,         // 6  BN07 L UpperArm
    kOverlayLegR,        // 7  BN08 R Thigh
    kOverlayLegL,        // 8  BN09 L Thigh
    kOverlayArm,         // 9  BN10 R Forearm
    kOverlayArm,         // 10 BN11 L Forearm
    kOverlayLegR,        // 11 BN12 R Calf
    kOverlayLegL,        // 12 BN13 L Calf
    kOverlayNeck,        // 13 BN14 Neck
    kOverlayHead,        // 14 BN15 Head
    kOverlayArm,         // 15 BN16 L Hand
    kOverlayArm,         // 16 BN17 R Hand (the mounted stow-hide bone; visible => arm)
    kOverlayLegR,        // 17 BN18 R Foot
    kOverlayLegL,        // 18 BN19 L Foot
};

// The upper-body WEAPON-CHANNEL mask: model bone indices (BN## - 1) whose sampled
// channel matrices are HARD-OVERWRITTEN by the entity's secondary AnimMap channel
// (clavicles, upper arms, forearms, neck, head, both hands) before the aim overlay
// composes on top. The legs and spine keep the primary (locomotion) channel.
// [orig: the mask build @ 0x4b14db inside Entity_BuildBoneTransformMatrices
//  @ 0x4b1290, gated Flags & 0x100 + not-mounted + primary-state flag 0x40
//  @ 0x4b14a7; witness docs/world/world-wac-ai-re.md section 14.8.6]
inline constexpr int kWeaponChannelMaskBones[] = {3, 4, 5, 6, 9, 10, 13, 14, 15, 16};

inline constexpr bool weapon_channel_masks_bone(int model_bone_index) {
    for (int b : kWeaponChannelMaskBones)
        if (b == model_bone_index) return true;
    return false;
}

// Mounted overlay selection belongs to animation because it chooses which of the
// retail body/aim/counter-lean matrices each skeletal class consumes.  World/embedder
// layers translate their seat facts into this small input; rendering and collision
// then call the same builder.
// [orig: parentSlot 2/3/5 dispatch @ 0x4b1868..0x4b1ba9]
enum class MountMode : uint8_t {
    OnFoot = 0,
    Seated,
    Gunner,
};

// Compose the secondary weapon channel in the same space as the original's matrix
// mask: FK both parent-local rotation poses, select PRIMARY or WEAPON per bone in
// world space, then convert the complete mixed hierarchy back to parent-local. The
// caller keeps the primary pose's local origins; the later model-pivot re-anchor owns
// translation. Recomputing every local (including unmasked children below a masked
// parent) preserves each selected absolute rotation exactly.
// [orig: channel-A matrix overwrite @ 0x4b16a7, before the overlay pivot re-anchor;
//  witness docs/world/world-wac-ai-re.md section 14.8.6]
void splice_weapon_channel_rotations(const std::vector<int> &parent_index,
                                     const uint8_t *masked_bone,
                                     const std::vector<Quat> &weapon_local_rotation,
                                     std::vector<Quat> &primary_local_rotation);

// The entity angle state feeding the overlay, all BAM32.
// [orig fields: Yaw/Pitch/Roll +0x10/+0x14/+0x18, bodyHeading/bodyPitch +0x8c/+0x90,
//  leg chase yaws +0x2d4/+0x2d8, torsoRoll +0x2dc, leanAngle +0xb0, pitchBlend +0x380,
//  pitchKickAccum +0x36c]
struct AimOverlayInputs {
    int32_t aim_yaw = 0;
    int32_t aim_pitch = 0;
    int32_t roll = 0;
    int32_t body_yaw = 0;
    int32_t body_pitch = 0;
    int32_t leg_yaw_r = 0;
    int32_t leg_yaw_l = 0;
    int32_t torso_roll = 0;
    int32_t lean = 0;
    int32_t pitch_blend = 0;
    int32_t pitch_kick_accum = 0;
    // g_animStateFlagsTable[animStateId] & 0x40: the aim-overlay branch. Prone walks
    // (0x603), rolls, and deaths lack it and take the reduced branch.
    bool aim_state = false;
    // animStateId 41/42 (roll_left/right): the clip owns the whole body -- roll overlays
    // zeroed. [orig: @ 0x4b17d3]
    bool rolling = false;
    // entity Flags & 0x100000: the non-aim branch keeps even the arms on the body
    // matrix. Semantics unconfirmed (swim-family suspected); embedders pass false.
    bool arms_locked = false;
    // The target item definition's authored phrase_set dword (+0x86c).  Validity is
    // independent of its value because zero is a witnessed gunner configuration;
    // an absent target definition must not silently select that branch.
    MountMode mount_mode = MountMode::OnFoot;
    bool mount_config_valid = false;
    int32_t mount_config = 0;
};

struct AimOverlayAngles {
    int32_t yaw = 0;
    int32_t pitch = 0;
    int32_t roll = 0;
};

// Compute the nine per-class overlay orientations from the entity state -- the exact
// int32 BAM blends and mounted matrix-selection branches.  A Gunner with unknown
// config deliberately preserves the existing on-foot result: retail always has a
// target item definition, so only a valid +0x86c value may enter its config switch.
// [orig: mounted @ 0x4b1868..0x4b1ba9; aim @ 0x4b1bbe; non-aim @ 0x4b1cf1;
// top-of-function v142/v143/v144 @ 0x4b17ad..0x4b185c; docs section 14.3]
void compute_aim_overlay_angles(const AimOverlayInputs &in,
                                AimOverlayAngles out[kOverlayClassCount]);

// The HELD-WEAPON ATTACHMENT orientation — the third-person gun's own basis.
//
// This is NOT one of the nine bone classes and must not be taken from any of them: the
// head class carries full-aim PITCH and the arm class carries BLENDED yaw, so either one
// aims the rifle visibly off-axis. The original builds it separately and the bone loop
// never reads it — it exists only to orient the weapon model, which is drawn RIGID (one
// matrix stamped into every bone slot) at the bone-16 attach point.
//
// Its shape in the original is an idiom rather than a matrix blend: the code temporarily
// OVERWRITES the entity's own Yaw/Pitch/Roll with the triple below and builds a transform
// from the entity, which is why the result reads as an entity orientation and not as an
// overlay delta.
//   aim state (g_animStateFlagsTable & 0x40):
//     yaw = aim yaw (PURE — not the 3/4 blend the arms use)
//     pitch = aim pitch + pitchKickAccum + 2*pitchBlend      [orig: @ 0x4b1bdc..0x4b1bf8]
//     roll = roll + lean
//   non-aim state: the same, WITHOUT the pitch-kick term    [orig: @ 0x4b1dd9..0x4b1dfa,
//                                                            and the sibling @ 0x4b1d60]
//
// MOUNTED bodies are deliberately NOT handled here. The original does not compute a
// triple for them at all — it copies an already-built matrix, either the body or the arm
// one, per mount branch [orig: qmemcpy @ 0x4b193e / 0x4b19cf / 0x4b1ab2 / 0x4b1b35 /
// 0x4b1b94]. Correlating each of those five branches to a mount mode is unfinished, so
// this returns the ON-FOOT result regardless and callers must not draw a held weapon for
// a mounted body on the strength of it. In practice the exposure is small: the draw gate
// hides the weapon outright for control/gunner/driver seats, leaving only the passenger
// seat [orig: Entity_CanFireWeapon @ 0x4dcb10].
AimOverlayAngles compute_held_weapon_attach_angles(const AimOverlayInputs &in);

// Apply per-bone world-orientation deltas to a parent-local pose, re-anchoring children
// on their parents -- the pose-space equivalent of the original's world-matrix overlay +
// pivot recomposition (modelDef+56 pivot table): with parent-local transforms the pivot
// re-anchor preserves every local origin, so only local rotations change:
//   W[k] = FK(pose);  F[k] = delta[class[k]] * W[k];  L'[k] = inv(F[parent]) * F[k].
// deltas are node-frame rotations (embedder frame), one per OverlayClass; bone_class is per
// bone (values >= kOverlayClassCount clamp to kOverlayBody).
// [orig: the multiply @ 0x4b1fe0 + pivot block @ 0x4b201e..0x4b2162]
void apply_aim_overlay(const std::vector<int> &parent_index,
                       const Quat deltas[kOverlayClassCount],
                       const uint8_t *bone_class,
                       std::vector<Quat> &local_rotation);

} // namespace opennova::anim

#endif // OPENNOVA_ANIM_AIM_OVERLAY_H
