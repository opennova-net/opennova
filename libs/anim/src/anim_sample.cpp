// Portable .bad skeletal sampler in engine-native (Y-up) space. The world-accumulate ->
// parent-local extraction mirrors the proven pre-repo Godot port (adm
// adm_import_plugin.cpp) and the original engine's pose chain. See anim_sample.h.

#include "anim/anim_sample.h"

#include "bad/bad.h"

#include <cmath>

namespace opennova::anim {

namespace {

constexpr Quat kIdentityQuat = {1.0f, 0.0f, 0.0f, 0.0f};
constexpr Vec3 kZeroVec = {0.0f, 0.0f, 0.0f};

Vec3 vec_add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 vec_sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

}  // namespace

Quat quat_normalize(Quat q) {
    const float n2 = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
    if (n2 <= 1e-24f) {
        return kIdentityQuat;
    }
    const float inv = 1.0f / std::sqrt(n2);
    return {q.w * inv, q.x * inv, q.y * inv, q.z * inv};
}

Quat quat_mul(Quat a, Quat b) {
    return {
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
}

Quat quat_inv(Quat q) {
    const Quat n = quat_normalize(q);
    return {n.w, -n.x, -n.y, -n.z};
}

// Rotate v by q (== quat_to_matrix(q) * v in animation_build.py).
Vec3 quat_rotate(Quat q, Vec3 v) {
    const Quat n = quat_normalize(q);
    const float xx = n.x * n.x, yy = n.y * n.y, zz = n.z * n.z;
    const float xy = n.x * n.y, xz = n.x * n.z, yz = n.y * n.z;
    const float wx = n.w * n.x, wy = n.w * n.y, wz = n.w * n.z;
    return {
        (1.0f - 2.0f * (yy + zz)) * v.x + (2.0f * (xy - wz)) * v.y + (2.0f * (xz + wy)) * v.z,
        (2.0f * (xy + wz)) * v.x + (1.0f - 2.0f * (xx + zz)) * v.y + (2.0f * (yz - wx)) * v.z,
        (2.0f * (xz - wy)) * v.x + (2.0f * (yz + wx)) * v.y + (1.0f - 2.0f * (xx + yy)) * v.z,
    };
}

Quat bad_channel_quat(float x, float y, float z, float w) {
    // Engine-native: the stored (x, y, z, w) becomes our w-first {w, x, y, z}, no axis
    // swap. (The Blender path swaps to Z-up; Godot/the engine are Y-up, so we don't.)
    return quat_normalize({w, x, y, z});
}

Quat mat3_to_quat(const float m[9]) {
    // Row-major 3x3 -> quat (Shepperd). m[r*3+c].
    const float trace = m[0] + m[4] + m[8];
    Quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {0.25f * s, (m[7] - m[5]) / s, (m[2] - m[6]) / s, (m[3] - m[1]) / s};
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q = {(m[7] - m[5]) / s, 0.25f * s, (m[1] + m[3]) / s, (m[2] + m[6]) / s};
    } else if (m[4] > m[8]) {
        const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q = {(m[2] - m[6]) / s, (m[1] + m[3]) / s, 0.25f * s, (m[5] + m[7]) / s};
    } else {
        const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q = {(m[3] - m[1]) / s, (m[2] + m[6]) / s, (m[5] + m[7]) / s, 0.25f * s};
    }
    return quat_normalize(q);
}

Quat quat_slerp(Quat a, Quat b, float t) {
    a = quat_normalize(a);
    b = quat_normalize(b);
    float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (dot < 0.0f) {  // shortest path (hemisphere flip)
        b = {-b.w, -b.x, -b.y, -b.z};
        dot = -dot;
    }
    if (dot > 0.9995f) {  // near-parallel: nlerp (matches the engine's small-angle fast path)
        return quat_normalize({a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t,
                               a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t});
    }
    const float theta = std::acos(dot);
    const float inv_sin = 1.0f / std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) * inv_sin;
    const float wb = std::sin(t * theta) * inv_sin;
    return quat_normalize({a.w * wa + b.w * wb, a.x * wa + b.x * wb,
                           a.y * wa + b.y * wb, a.z * wa + b.z * wb});
}

namespace {

// Per-bone WORLD rotation at integer tick `tick`, faithful to BoneAnim_FindKeyframeAtTime @0x410220:
// walk THIS bone's keyframe duration table (frame_lengths) to find the keyframe whose
// [accumulated, accumulated+duration) window holds `tick`, then slerp rot[i] -> rot[i+1] (wrap to 0)
// by the in-window fraction. Compressed clips key each bone sparsely with per-bone-different
// durations; a flat rotations[tick] index would snap a short bone to identity past its keyframe
// count. For a dense uniform clip (every keyframe duration == 1) this returns rotations[tick].
Quat sample_bone_world_rot(const BadChannel &ch, uint32_t tick) {
    if (ch.rotations == nullptr || ch.frame_count == 0) {
        return kIdentityQuat;
    }
    const uint32_t count = ch.frame_count;
    const auto kf = [&](uint32_t i) {
        const BadQuaternion &r = ch.rotations[i];
        return bad_channel_quat(r.x, r.y, r.z, r.w);
    };
    if (count == 1) {
        return kf(0);
    }
    uint32_t acc = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t dur = (ch.frame_lengths != nullptr && ch.frame_lengths[i] > 0) ? ch.frame_lengths[i] : 1u;
        if (acc + dur > tick) {
            const float blend = static_cast<float>(tick - acc) / static_cast<float>(dur);
            const uint32_t next = (i + 1 < count) ? i + 1 : 0u;  // wrap to keyframe 0 (loop)
            return quat_slerp(kf(i), kf(next), blend);
        }
        acc += dur;
    }
    return kf(count - 1);  // tick past the bone's summed duration: hold the last keyframe
}

}  // namespace

std::vector<Vec3> positions_from_model(const BadFile &bind_bad,
                                       const std::vector<int> &model_parents,
                                       const std::vector<Vec3> &model_rel_positions) {
    std::vector<Vec3> out;
    out.reserve(model_rel_positions.size());
    for (size_t i = 0; i < model_rel_positions.size(); ++i) {
        const Vec3 &rel = model_rel_positions[i];
        const Vec3 flipped = {-rel.x, rel.y, rel.z};
        const int parent = (i < model_parents.size()) ? model_parents[i] : -1;
        if (parent < 0 || static_cast<size_t>(parent) == i ||
                static_cast<size_t>(parent) >= bind_bad.num_bones) {
            out.push_back(flipped);
            continue;
        }
        const float *m = bind_bad.bones[parent].rotation;  // row-major 3x3
        out.push_back({
                m[0] * flipped.x + m[1] * flipped.y + m[2] * flipped.z,
                m[3] * flipped.x + m[4] * flipped.y + m[5] * flipped.z,
                m[6] * flipped.x + m[7] * flipped.y + m[8] * flipped.z,
        });
    }
    return out;
}

Clip sample_clip(const BadFile &bad, const std::vector<Vec3> &shared_rest_origins, bool model_bind,
                 const BadFile *bind_source, const std::vector<int> &model_parents) {
    Clip clip;
    clip.fps = bad.fps;
    clip.flags = bad.flags;
    clip.frame_count = bad.frame_count;

    const size_t bad_bone_count = bad.num_bones;
    const bool translated = (bad.flags & 0x02u) != 0;
    // Model-table mode: the MODEL's bone table defines the rig -- row count, hierarchy, and
    // pivots; the .bad's own bone count/parents/positions are never read (BadBone.position is
    // a lossy DCC export -- 12 of 43 JO viewmodel rigs ship zeroed/stale values and retail
    // renders them all). [orig: BoneAnim_BuildWorldMatrices @0x40c400 -- the FK loop runs to
    // modelDef+52 and reads parent (+20) and pivot (+36) from each modelDef+56 row.]
    const bool model_table =
            !model_parents.empty() && model_parents.size() == shared_rest_origins.size();
    const size_t bone_count = model_table ? model_parents.size() : bad_bone_count;
    // Legacy path: the FK accumulation uses ONE shared skeleton's bone offsets across all of a
    // model's clips. Use the caller-supplied shared origins when present; otherwise fall back
    // to this clip's own bind positions (correct only for the bind/reset clip itself).
    const bool use_shared = model_table || shared_rest_origins.size() == bone_count;

    clip.bones.resize(bone_count);
    std::vector<Vec3> rest_origins(bone_count, kZeroVec);
    // model_bind: every channel rotation composes against the SKELETON's per-bone bind 3x3.
    // The bind operand is the rig-wide bind source (the .adm slot-0 / reset .bad -- the
    // channel+44 override), NOT this clip's own bone records; those are only the original's
    // fallback when no override exists. Composing a clip against its own bind self-cancels
    // at its start frame -- the 2026-07-08 misreading that froze the FP rig at its T-pose.
    // [orig: AnimChannel_ComputeBoneMatrices @0x410da0 (Transpose(bind) x channel; bind =
    // *(channel+44) ? *(channel+44) : playing anim @0x410de3); AnimMap_RegisterEntity
    // @0x40bb60 pins channel+44 to the .adm slot-0 .bad once, at entity registration.]
    const BadFile &bind_bad = (model_bind && bind_source != nullptr) ? *bind_source : bad;
    std::vector<Quat> bind_rot;
    if (model_bind) {
        bind_rot.resize(bone_count, kIdentityQuat);
    }
    for (size_t b = 0; b < bone_count; ++b) {
        const BadBone *bone = (b < bad_bone_count) ? &bad.bones[b] : nullptr;
        clip.bones[b].name = (bone != nullptr) ? bone->name : "";
        // Hierarchy: the model table when supplied, else this .bad's records. The table stores
        // the root row's parent as ITSELF (the original's in-place multiply against the
        // model-origin root pivot is a no-op) -- normalize self/invalid to -1.
        int parent = model_table ? model_parents[b]
                                 : ((bone != nullptr) ? bone->parent_index : -1);
        if (parent < 0 || static_cast<size_t>(parent) >= bone_count ||
                static_cast<size_t>(parent) == b) {
            parent = -1;
        }
        clip.bones[b].parent_index = parent;
        // Engine-native: bone bind position used as-is (parent-local origin). When the caller
        // supplies shared origins (the .3di model's bone pivots -- see NovaSkeletalAnim), they win
        // over the BadBone.position field, which is a lossy export (roughly half the .bad corpus
        // triplicates X into all three slots, destroying Y/Z). The original engine likewise sources
        // bone pivots from the model, not the .bad. [orig: BoneAnim_BuildWorldMatrices @0x40c400 /
        // build_world_bone_matrices @0x40c770 read the model bone table (modelDef+56); the .bad
        // supplies only rotations via AnimChannel_ComputeBoneMatrices @0x410da0.]
        const Vec3 origin = use_shared
                ? shared_rest_origins[b]
                : Vec3{bone->position[0], bone->position[1], bone->position[2]};
        if (model_bind) {
            // Faithful bind: pure translation. The bind-inverse the original bakes into the
            // composed bone matrices is T(-pivot) with NO rotation; the stored bind 3x3 only
            // serves as the zero-reference the channels are measured against.
            // [orig: BoneAnim_BuildWorldMatrices @0x40c400 T(-p) * local.]
            //
            // Everything stays in the NATIVE model frame (pivots, deltas, translations). The
            // original's builders emit S*A^T*S with x-negated pivots/translations -- that
            // diag(-1,1,1) conjugation is its model->render frame map, which the embedder realizes
            // instead by building the FP mesh in the native frame and mapping the whole rig to
            // the camera in one container transform. [orig: the copy loops @0x40c4d8..0x40c57c
            // / @0x40c84c..0x40c8f5.]
            for (int k = 0; k < 9; ++k) {
                clip.bones[b].rest_rotation[k] = (k % 4 == 0) ? 1.0f : 0.0f;
            }
            // Bind operand from the rig's skeleton .bad (bone-index aligned; the original
            // walks the bind source's records by the same bone index, count from ITS header
            // [orig: @0x410deb..0x410e4e boneData+20/boneData+24]).
            if (b < bind_bad.num_bones) {
                bind_rot[b] = mat3_to_quat(bind_bad.bones[b].rotation);
            }
        } else if (bone != nullptr) {
            // Carry the raw BadBone BIND rotation so the embedder can build the Skeleton3D rest from
            // it; the rest ORIGIN follows the same source as the FK (model pivots when shared), so
            // the Skeleton3D bind and the FK agree. (Model-table rows past the .bad's records keep
            // the identity default.)
            for (int k = 0; k < 9; ++k) {
                clip.bones[b].rest_rotation[k] = bone->rotation[k];
            }
        }
        rest_origins[b] = origin;
        clip.bones[b].rest_position[0] = origin.x;
        clip.bones[b].rest_position[1] = origin.y;
        clip.bones[b].rest_position[2] = origin.z;
    }

    // The channel tables carry frame_count + 1 keys — the header counts INTERVALS
    // (fence-post): a 16-frame idle stores 17 keys, and a ONE-frame clip stores its
    // entire motion as key 0 -> key 1 (the M4 viewmodel fire kick, m4_1f). Bake a
    // pose at every key tick, 0..frame_count inclusive; stopping at frame_count - 1
    // dropped the final key — invisible on loops (the seam key ~= key 0) but it
    // erased a one-frame clip's whole motion into a static pose.
    // [orig: BoneAnim_FindKeyframeAtTime @0x410220 walks every channel key; the
    //  final window holds key_last]
    clip.frames.resize(clip.frame_count + 1);
    for (uint32_t f = 0; f <= clip.frame_count; ++f) {
        std::vector<BoneSample> &frame = clip.frames[f];
        frame.resize(bone_count);

        for (size_t b = 0; b < bone_count; ++b) {
            // Per-bone WORLD rotation at tick f, walking THIS bone's own keyframe duration table
            // (sample_bone_world_rot / BoneAnim_FindKeyframeAtTime @0x410220) -- NOT a flat
            // rotations[f] index, which snaps sparsely-keyed bones to identity on compressed clips.
            Quat world_rot = kIdentityQuat;
            if (b < bad.num_channels) {
                world_rot = sample_bone_world_rot(bad.channels[b], f);
            } else if (model_table) {
                // Model rows past the .bad's channels take row 0's already-COMPOSED rotation --
                // the original copies bone 0's finished matrix into every extra slot before the
                // FK [orig: the padding loop @0x40c5a1]. Rows 0..b-1 are complete (ascending b);
                // with no channels at all the identity stands.
                frame[b].world_rotation = (bad.num_channels > 0) ? frame[0].world_rotation
                                                                 : kIdentityQuat;
                Vec3 pad_translation = kZeroVec;  // orig sums uninitialized stack here (UB): D-INF-15
                const int pad_parent = clip.bones[b].parent_index;
                if (pad_parent < 0) {
                    frame[b].world_position = vec_add(rest_origins[b], pad_translation);
                } else {
                    const BoneSample &pp = frame[static_cast<size_t>(pad_parent)];
                    frame[b].world_position = vec_add(
                            vec_add(pp.world_position, quat_rotate(pp.world_rotation, rest_origins[b])),
                            pad_translation);
                }
                continue;
            }
            if (model_bind) {
                // Compose against the SKELETON bind: the .bad stores each bind 3x3 TRANSPOSED
                // relative to the channel quats (on-data: mat3(stored) x channel-at-reset ==
                // identity on every clip's own records), so with our row-major read the
                // witnessed `Transpose(bind 3x3) x channel` is mat3(stored) x channel --
                // quat form: q(stored) * channel. Identity at the skeleton's own reset
                // clip; the pose that carries the rig from the bind (T-pose) into the
                // clip's stance everywhere else -- the FP hold at anim_wpn_idle. The
                // operand order was pinned VISUALLY on the ak47 rig: this order renders
                // the two-armed hold; the conjugate (channel * bind, oscarmike's
                // pose x inverse-bind shape) collapses the rig -- their pipeline differs
                // because its Skeleton3D rest carries the bind rotations, ours bakes the
                // whole composition into the pose over identity rests.
                // [orig: AnimChannel_ComputeBoneMatrices @0x410da0; sense pin D-INF-14.]
                world_rot = quat_normalize(quat_mul(bind_rot[b], world_rot));
            }

            Vec3 translation = kZeroVec;
            if (translated && bad.translations != nullptr && b < bad_bone_count) {
                // The translation block is laid out by the FILE's own bone count (its stride),
                // regardless of the rig's row count in model-table mode. It carries exactly
                // frame_count rows (no fence-post key): the final key's pose HOLDS the last
                // row — a zero fallback would pop translated clips' last key to the origin.
                // (The original's translation read at the final key window is unwalked; the
                // hold mirrors the rotation walk's hold-last shape.)
                const uint32_t tf = (bad.frame_count > 0 && f >= bad.frame_count)
                        ? bad.frame_count - 1 : f;
                const size_t idx = static_cast<size_t>(tf) * bad_bone_count + b;
                if (idx < bad.num_translations) {
                    const float *t = bad.translations[idx];
                    translation = {t[0], t[1], t[2]};
                }
            }

            const int parent = clip.bones[b].parent_index;
            BoneSample &out = frame[b];
            out.world_rotation = world_rot;
            if (parent < 0) {
                out.world_position = vec_add(rest_origins[b], translation);
            } else {
                const BoneSample &p = frame[static_cast<size_t>(parent)];
                out.world_position = vec_add(
                    vec_add(p.world_position, quat_rotate(p.world_rotation, rest_origins[b])),
                    translation);
            }
        }

        // Second pass: parent-relative (local) transforms.
        for (size_t b = 0; b < bone_count; ++b) {
            const int parent = clip.bones[b].parent_index;
            BoneSample &out = frame[b];
            if (parent < 0) {
                out.local_rotation = quat_normalize(out.world_rotation);
                out.local_position = out.world_position;
            } else {
                const BoneSample &p = frame[static_cast<size_t>(parent)];
                out.local_rotation = quat_normalize(quat_mul(quat_inv(p.world_rotation), out.world_rotation));
                out.local_position = quat_rotate(quat_inv(p.world_rotation),
                                                 vec_sub(out.world_position, p.world_position));
            }
        }
    }

    return clip;
}

}  // namespace opennova::anim
