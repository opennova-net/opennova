// Portable .bad skeletal sampler in engine-native (Y-up) space. The world-accumulate ->
// parent-local extraction mirrors the proven Godot port (oscarmike opennova_adm
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
            // Channel rotation -> engine-native world rotation (identity if missing).
            Quat world_rot = kIdentityQuat;
            if (b < bad.num_channels) {
                const BadChannel &ch = bad.channels[b];
                if (f < ch.frame_count && ch.rotations != nullptr) {
                    const BadQuaternion &rq = ch.rotations[f];
                    world_rot = bad_channel_quat(rq.x, rq.y, rq.z, rq.w);
                }
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
