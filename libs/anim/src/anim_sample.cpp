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

Clip sample_clip(const BadFile &bad, const std::vector<Vec3> &shared_rest_origins) {
    Clip clip;
    clip.fps = bad.fps;
    clip.flags = bad.flags;
    clip.frame_count = bad.frame_count;

    const size_t bone_count = bad.num_bones;
    const bool translated = (bad.flags & 0x02u) != 0;
    // The FK accumulation uses ONE shared skeleton's bone offsets across all of a model's
    // clips. Use the caller-supplied shared origins when present; otherwise fall back to this
    // clip's own bind positions (correct only for the bind/reset clip itself).
    const bool use_shared = shared_rest_origins.size() == bone_count;

    clip.bones.resize(bone_count);
    std::vector<Vec3> rest_origins(bone_count, kZeroVec);
    for (size_t b = 0; b < bone_count; ++b) {
        const BadBone &bone = bad.bones[b];
        clip.bones[b].name = bone.name;
        int parent = bone.parent_index;
        if (parent < 0 || static_cast<size_t>(parent) >= bone_count) {
            parent = -1;
        }
        clip.bones[b].parent_index = parent;
        // Engine-native: bone bind position used as-is (parent-local origin).
        rest_origins[b] = use_shared
                ? shared_rest_origins[b]
                : Vec3{bone.position[0], bone.position[1], bone.position[2]};
        // Carry the raw BadBone BIND pose so the host can build the Skeleton3D rest from it.
        for (int k = 0; k < 9; ++k) {
            clip.bones[b].rest_rotation[k] = bone.rotation[k];
        }
        clip.bones[b].rest_position[0] = bone.position[0];
        clip.bones[b].rest_position[1] = bone.position[1];
        clip.bones[b].rest_position[2] = bone.position[2];
    }

    clip.frames.resize(clip.frame_count);
    for (uint32_t f = 0; f < clip.frame_count; ++f) {
        std::vector<BoneSample> &frame = clip.frames[f];
        frame.resize(bone_count);

        for (size_t b = 0; b < bone_count; ++b) {
            // Per-bone WORLD rotation at tick f, walking THIS bone's own keyframe duration table
            // (sample_bone_world_rot / BoneAnim_FindKeyframeAtTime @0x410220) -- NOT a flat
            // rotations[f] index, which snaps sparsely-keyed bones to identity on compressed clips.
            Quat world_rot = kIdentityQuat;
            if (b < bad.num_channels) {
                world_rot = sample_bone_world_rot(bad.channels[b], f);
            }

            Vec3 translation = kZeroVec;
            if (translated && bad.translations != nullptr) {
                const size_t idx = static_cast<size_t>(f) * bone_count + b;
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
