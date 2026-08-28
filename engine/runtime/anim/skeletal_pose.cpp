// Time-domain skeletal pose evaluation — moved from the shell binding's
// skeletal evaluator (ADR 0028); the Godot binding's residue was Transform3D
// boxing around exactly this math.
#include <runtime/anim/skeletal_pose.h>

#include <runtime/anim/aim_overlay.h>

#include <cmath>

namespace opennova::anim {

namespace {

Vec3 vec3_lerp(const Vec3 &a, const Vec3 &b, float t) {
    return Vec3{a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t};
}

struct Mat3 {
    float m[3][3] = {};
};

Mat3 rows_to_mat3(const float rot[9]) {
    Mat3 o{};
    o.m[0][0] = rot[0]; o.m[0][1] = rot[1]; o.m[0][2] = rot[2];
    o.m[1][0] = rot[3]; o.m[1][1] = rot[4]; o.m[1][2] = rot[5];
    o.m[2][0] = rot[6]; o.m[2][1] = rot[7]; o.m[2][2] = rot[8];
    return o;
}

// Cofactor inverse with the binding's exact 1e-9 determinant guard; a failed
// invert leaves `out` untouched (the binding ignored the result over a
// zero-initialized matrix, and the caller's determinant guard catches the
// degenerate product downstream).
bool mat3_invert(const Mat3 &m, Mat3 &out) {
    const float a00 = m.m[0][0], a01 = m.m[0][1], a02 = m.m[0][2];
    const float a10 = m.m[1][0], a11 = m.m[1][1], a12 = m.m[1][2];
    const float a20 = m.m[2][0], a21 = m.m[2][1], a22 = m.m[2][2];
    const float b01 = a22 * a11 - a12 * a21;
    const float b11 = -a22 * a10 + a12 * a20;
    const float b21 = a21 * a10 - a11 * a20;
    const float det = a00 * b01 + a01 * b11 + a02 * b21;
    if (std::fabs(det) < 1e-9f) {
        return false;
    }
    const float inv_det = 1.0f / det;
    out.m[0][0] = b01 * inv_det;
    out.m[0][1] = (-a22 * a01 + a02 * a21) * inv_det;
    out.m[0][2] = (a12 * a01 - a02 * a11) * inv_det;
    out.m[1][0] = b11 * inv_det;
    out.m[1][1] = (a22 * a00 - a02 * a20) * inv_det;
    out.m[1][2] = (-a12 * a00 + a02 * a10) * inv_det;
    out.m[2][0] = b21 * inv_det;
    out.m[2][1] = (-a21 * a00 + a01 * a20) * inv_det;
    out.m[2][2] = (a11 * a00 - a01 * a10) * inv_det;
    return true;
}

Mat3 mat3_mul(const Mat3 &a, const Mat3 &b) {
    Mat3 o{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            o.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] +
                    a.m[r][2] * b.m[2][c];
        }
    }
    return o;
}

float mat3_determinant(const Mat3 &m) {
    return m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[2][1] * m.m[1][2]) -
            m.m[1][0] * (m.m[0][1] * m.m[2][2] - m.m[2][1] * m.m[0][2]) +
            m.m[2][0] * (m.m[0][1] * m.m[1][2] - m.m[1][1] * m.m[0][2]);
}

Vec3 mat3_column(const Mat3 &m, int c) {
    return Vec3{m.m[0][c], m.m[1][c], m.m[2][c]};
}

void mat3_set_column(Mat3 &m, int c, const Vec3 &v) {
    m.m[0][c] = v.x;
    m.m[1][c] = v.y;
    m.m[2][c] = v.z;
}

float vec3_dot(const Vec3 &a, const Vec3 &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 vec3_sub_scaled(const Vec3 &a, const Vec3 &b, float s) {
    return Vec3{a.x - b.x * s, a.y - b.y * s, a.z - b.z * s};
}

bool vec3_normalize(Vec3 &v) {
    const float len = std::sqrt(vec3_dot(v, v));
    if (len == 0.0f) return false;
    v.x /= len;
    v.y /= len;
    v.z /= len;
    return true;
}

// Column-wise Gram-Schmidt, the same normalization the binding's
// Basis::orthonormalized() applied to the relativized bind product.
void mat3_orthonormalize(Mat3 &m) {
    Vec3 x = mat3_column(m, 0);
    Vec3 y = mat3_column(m, 1);
    Vec3 z = mat3_column(m, 2);
    vec3_normalize(x);
    y = vec3_sub_scaled(y, x, vec3_dot(x, y));
    vec3_normalize(y);
    z = vec3_sub_scaled(z, x, vec3_dot(x, z));
    z = vec3_sub_scaled(z, y, vec3_dot(y, z));
    vec3_normalize(z);
    mat3_set_column(m, 0, x);
    mat3_set_column(m, 1, y);
    mat3_set_column(m, 2, z);
}

} // namespace

void eval_clip_pose(const Clip &clip, double playhead_seconds,
                    std::vector<PoseBone> &r_pose) {
    r_pose.clear();
    if (clip.frames.empty() || clip.bones.empty()) return;

    const int frame_count = static_cast<int>(clip.frame_count);
    const double fps = clip.fps > 0 ? static_cast<double>(clip.fps) : 30.0;
    const double frame_time = playhead_seconds * fps;

    int a = 0;
    int b = 0;
    double frac = 0.0;
    // The pose table holds frame_count + 1 keys (the header counts INTERVALS;
    // the sampler bakes every channel key). Loops cycle the frame_count
    // interval windows (the seam key ~= key 0; the original's loop-wrap window
    // is unwalked); one-shots play every window and HOLD the true final key —
    // a one-frame clip is one full window of motion, not a static pose.
    // [orig: BoneAnim_FindKeyframeAtTime @0x410220 — hold-last past the
    // summed durations]
    const int last_pose = static_cast<int>(clip.frames.size()) - 1;
    if (clip.loops() && frame_count > 1) {
        double m = std::fmod(frame_time, static_cast<double>(frame_count));
        if (m < 0.0) {
            m += static_cast<double>(frame_count);
        }
        a = static_cast<int>(std::floor(m));
        frac = m - a;
        b = (a + 1) % frame_count;
    } else if (frame_time <= 0.0) {
        a = b = 0;
    } else if (frame_time >= last_pose) {
        a = b = last_pose;
    } else {
        a = static_cast<int>(std::floor(frame_time));
        frac = frame_time - a;
        b = a + 1;
    }

    const std::vector<BoneSample> &fa = clip.frames[a];
    const std::vector<BoneSample> &fb = clip.frames[b];
    const size_t bone_count = clip.bones.size();
    if (fa.size() < bone_count || fb.size() < bone_count) return;
    r_pose.resize(bone_count);
    for (size_t i = 0; i < bone_count; ++i) {
        const BoneSample &sa = fa[i];
        const BoneSample &sb = fb[i];
        r_pose[i].rotation = quat_slerp(sa.local_rotation, sb.local_rotation,
                static_cast<float>(frac));
        r_pose[i].origin = vec3_lerp(sa.local_position, sb.local_position,
                static_cast<float>(frac));
    }
}

void blend_poses(const std::vector<PoseBone> &source,
                 const std::vector<PoseBone> &target, float weight,
                 std::vector<PoseBone> &r_pose) {
    if (source.size() != target.size()) {
        r_pose = target;
        return;
    }
    if (weight <= 0.0f) {
        r_pose = source;
        return;
    }
    if (weight >= 1.0f) {
        r_pose = target;
        return;
    }
    r_pose.resize(target.size());
    for (size_t i = 0; i < target.size(); ++i) {
        r_pose[i].rotation =
                quat_slerp(source[i].rotation, target[i].rotation, weight);
        r_pose[i].origin = vec3_lerp(source[i].origin, target[i].origin, weight);
    }
}

int model_bone_index_from_name(const std::string &name) {
    if (name.size() < 4 || (name[0] != 'B' && name[0] != 'b') ||
            (name[1] != 'N' && name[1] != 'n') ||
            name[2] < '0' || name[2] > '9' || name[3] < '0' || name[3] > '9')
        return -1;
    return (name[2] - '0') * 10 + (name[3] - '0') - 1;
}

uint8_t overlay_class_for_bone_name(const std::string &name) {
    const int index = model_bone_index_from_name(name);
    // "BN01 Hips" -> bone index 0; classes exist for BN01..BN19, everything
    // else takes the body matrix (the retail default case).
    if (index >= 0 && index < 19) {
        return kOverlayClassByBoneIndex[index];
    }
    return kOverlayBody;
}

void bind_rest_local(const ClipBone &bone, const ClipBone *parent,
                     float r_rotation_rows[9], Vec3 &r_origin) {
    r_origin = Vec3{bone.rest_position[0], bone.rest_position[1],
            bone.rest_position[2]};
    const Mat3 bone_mat = rows_to_mat3(bone.rest_rotation);
    Mat3 product;
    if (parent == nullptr) {
        product = bone_mat;
    } else {
        const Mat3 parent_mat = rows_to_mat3(parent->rest_rotation);
        Mat3 parent_inv{};
        mat3_invert(parent_mat, parent_inv);
        product = mat3_mul(bone_mat, parent_inv);
    }
    // The .bad bind 3x3 is a row-vector engine matrix; the relativized product
    // TRANSPOSES into the column-vector basis the pose pipeline composes with
    // (the binding realized the same conversion by feeding the product's rows
    // into Godot's column-axes Basis constructor). Guard/orthonormalize the
    // transposed basis exactly like the binding did.
    Mat3 rest;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            rest.m[r][c] = product.m[c][r];
    if (mat3_determinant(rest) == 0.0f) {
        rest = Mat3{};
        rest.m[0][0] = rest.m[1][1] = rest.m[2][2] = 1.0f;
    } else {
        mat3_orthonormalize(rest);
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            r_rotation_rows[r * 3 + c] = rest.m[r][c];
}

void quat_to_mat3_rows(const Quat &q, float r_rows[9]) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    r_rows[0] = 1.0f - 2.0f * (yy + zz);
    r_rows[1] = 2.0f * (xy - wz);
    r_rows[2] = 2.0f * (xz + wy);
    r_rows[3] = 2.0f * (xy + wz);
    r_rows[4] = 1.0f - 2.0f * (xx + zz);
    r_rows[5] = 2.0f * (yz - wx);
    r_rows[6] = 2.0f * (xz - wy);
    r_rows[7] = 2.0f * (yz + wx);
    r_rows[8] = 1.0f - 2.0f * (xx + yy);
}

} // namespace opennova::anim
