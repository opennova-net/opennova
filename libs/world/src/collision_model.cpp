#include "world/collision.h"

// Split out of collision.cpp (quality campaign W3-2). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// CollisionModel bounds and the fixed-point matrix operations: the section AABB /
// bound-sphere derivation plus the Q22 transform, inverse and pose builders.

#include "world/angle.h"
#include "world/dir_table.h"
#include <algorithm>
#include <cmath>

#include "collision_detail.h"

#include "world/world.h"

namespace opennova::world {

using namespace detail; // the shared fixed-point helpers, unqualified as before

// ----------------------------------------------------------------------------
// Model finalize: derive the per-section AABB + bound sphere from its volumes.
// [orig: precomputed on the runtime COBJ records by the model loader; the queries
// read them at +68..+104.]
// ----------------------------------------------------------------------------
void CollisionModel::finalize_sections() {
    for (CollisionSection &s : sections) {
        if (s.vehicle_volume_start < 0) {
            for (int32_t i = 0; i < s.volume_count; ++i) {
                const int32_t type = volumes[s.volume_start + i].type;
                if (type == 7 || type == 12) {
                    s.vehicle_volume_start = i;
                    break;
                }
            }
        }
        // Exact COBJ bounds win. Skeletal/person rows can also carry only an
        // authored med/radius (no BVOL/CFAC), so preserve a positive radius even
        // for legacy callers that did not set authored_bounds explicitly.
        if (s.authored_bounds || (s.volume_count <= 0 && s.radius > 0)) continue;

        // The bound SPHERE is synthesized only from a section's BVOL run.
        // Vertex-derived AABBs (the CFAC mesh row) must never mint one: retail
        // reads the AUTHORED COBJ mid/radius raw [orig:
        // Physics_RaycastAgainstBoneSections @ 0x4e4670], and every JO person
        // model authors its mesh row with radius 0 + sentinel inverted bounds
        // (Indo01 sec 19: min +10000/max -10000) — a derived whole-mesh sphere
        // there becomes a phantom shootable "bone" on the pose slot.
        bool synthesize_sphere = false;
        if (s.volume_count > 0) {
            synthesize_sphere = true;
            const CollisionVolume &v0 = volumes[s.volume_start];
            s.min_x = v0.min_x; s.max_x = v0.max_x;
            s.min_y = v0.min_y; s.max_y = v0.max_y;
            s.min_z = v0.min_z; s.max_z = v0.max_z;
            for (int32_t i = 1; i < s.volume_count; ++i) {
                const CollisionVolume &v = volumes[s.volume_start + i];
                if (v.min_x < s.min_x) s.min_x = v.min_x;
                if (v.max_x > s.max_x) s.max_x = v.max_x;
                if (v.min_y < s.min_y) s.min_y = v.min_y;
                if (v.max_y > s.max_y) s.max_y = v.max_y;
                if (v.min_z < s.min_z) s.min_z = v.min_z;
                if (v.max_z > s.max_z) s.max_z = v.max_z;
            }
        } else if (s.vertex_count > 0) {
            const CollisionVertex &v0 = vertices[s.vertex_start];
            s.min_x = s.max_x = v0.p[0];
            s.min_y = s.max_y = v0.p[1];
            s.min_z = s.max_z = v0.p[2];
            for (int32_t i = 1; i < s.vertex_count; ++i) {
                const CollisionVertex &v = vertices[s.vertex_start + i];
                if (v.p[0] < s.min_x) s.min_x = v.p[0];
                if (v.p[0] > s.max_x) s.max_x = v.p[0];
                if (v.p[1] < s.min_y) s.min_y = v.p[1];
                if (v.p[1] > s.max_y) s.max_y = v.p[1];
                if (v.p[2] < s.min_z) s.min_z = v.p[2];
                if (v.p[2] > s.max_z) s.max_z = v.p[2];
            }
        } else if (s.face_vertex_count > 0) {
            const CollisionFaceVertex &v0 = face_vertices[s.face_vertex_start];
            s.min_x = s.max_x = static_cast<int32_t>(v0.x) << 8;
            s.min_y = s.max_y = static_cast<int32_t>(v0.y) << 8;
            s.min_z = s.max_z = static_cast<int32_t>(v0.z) << 8;
            for (int32_t i = 1; i < s.face_vertex_count; ++i) {
                const CollisionFaceVertex &v = face_vertices[s.face_vertex_start + i];
                const int32_t p[3] = {static_cast<int32_t>(v.x) << 8,
                                      static_cast<int32_t>(v.y) << 8,
                                      static_cast<int32_t>(v.z) << 8};
                if (p[0] < s.min_x) s.min_x = p[0];
                if (p[0] > s.max_x) s.max_x = p[0];
                if (p[1] < s.min_y) s.min_y = p[1];
                if (p[1] > s.max_y) s.max_y = p[1];
                if (p[2] < s.min_z) s.min_z = p[2];
                if (p[2] > s.max_z) s.max_z = p[2];
            }
        } else {
            s.min_x = s.max_x = s.min_y = s.max_y = s.min_z = s.max_z = 0;
            s.center[0] = s.center[1] = s.center[2] = 0;
            // Preserve the negative host/test sentinel for an absent synthetic
            // section. Authored sections use a non-negative radius, including
            // retail's meaningful zero-radius whole-body mesh row.
            if (s.radius >= 0) s.radius = 0;
            continue;
        }
        if (synthesize_sphere) {
            const int32_t hx = (s.max_x - s.min_x) >> 1;
            const int32_t hy = (s.max_y - s.min_y) >> 1;
            const int32_t hz = (s.max_z - s.min_z) >> 1;
            s.center[0] = s.min_x + hx;
            s.center[1] = s.min_y + hy;
            s.center[2] = s.min_z + hz;
            s.radius = vec_len_ftol(hx, hy, hz);
        }
    }
    // Model-level bounds = union of the section AABBs — the runtime collision
    // header min/max the render occlusion consumes (+24..+44).
    min[0] = min[1] = min[2] = 0;
    max[0] = max[1] = max[2] = 0;
    bool first = true;
    for (const CollisionSection &s : sections) {
        if (s.volume_count <= 0 && s.vertex_count <= 0 && s.face_vertex_count <= 0 &&
            !s.authored_bounds && s.radius <= 0)
            continue;
        if (first) {
            min[0] = s.min_x; max[0] = s.max_x;
            min[1] = s.min_y; max[1] = s.max_y;
            min[2] = s.min_z; max[2] = s.max_z;
            first = false;
            continue;
        }
        if (s.min_x < min[0]) min[0] = s.min_x;
        if (s.max_x > max[0]) max[0] = s.max_x;
        if (s.min_y < min[1]) min[1] = s.min_y;
        if (s.max_y > max[1]) max[1] = s.max_y;
        if (s.min_z < min[2]) min[2] = s.min_z;
        if (s.max_z > max[2]) max[2] = s.max_z;
    }

    // Cache the model-derived fallback sphere once. Per-query broad phases only
    // apply entity scale to this value; they never rescan sections or take a
    // square root. Combining the farthest extent on each axis is conservative
    // even when the extrema come from different sections. Each component is at
    // most 2^31, so the three squares total at most 3*2^62 and fit in u64.
    uint64_t farthest[3] = {};
    for (const CollisionSection &s : sections) {
        const int32_t lo[3] = {s.min_x, s.min_y, s.min_z};
        const int32_t hi[3] = {s.max_x, s.max_y, s.max_z};
        for (int axis = 0; axis < 3; ++axis) {
            farthest[axis] = std::max(farthest[axis], int32_magnitude(lo[axis]));
            farthest[axis] = std::max(farthest[axis], int32_magnitude(hi[axis]));
        }
    }
    const uint64_t squared = farthest[0] * farthest[0] +
                             farthest[1] * farthest[1] +
                             farthest[2] * farthest[2];
    fallback_bound_radius_q16 =
            std::max<uint64_t>(ceil_sqrt_u64(squared), 0x10000u);
}

// ----------------------------------------------------------------------------
// Matrix helpers (row-major 3x4, Q22 rotation rows, 16.16 translation at
// [3]/[7]/[11], [15] flags).
// ----------------------------------------------------------------------------

// [orig: Math_FixedPointTransformPoint22 @ 0x615810 — rotate THEN translate.]
void CollisionMatrix::transform_point(const int32_t in[3], int32_t out[3]) const {
    int32_t r[3];
    rotate_point(in, r);
    out[0] = r[0] + m[3];
    out[1] = r[1] + m[7];
    out[2] = r[2] + m[11];
}

// [orig: Math_TransformPointFixedPoint22 @ 0x412e90 — rotate only.]
void CollisionMatrix::rotate_point(const int32_t in[3], int32_t out[3]) const {
    out[0] = static_cast<int32_t>((static_cast<int64_t>(in[1]) * m[1] +
                                   static_cast<int64_t>(in[0]) * m[0] +
                                   static_cast<int64_t>(in[2]) * m[2] + 0x200000) >> 22);
    out[1] = static_cast<int32_t>((static_cast<int64_t>(in[1]) * m[5] +
                                   static_cast<int64_t>(in[0]) * m[4] +
                                   static_cast<int64_t>(in[2]) * m[6] + 0x200000) >> 22);
    out[2] = static_cast<int32_t>((static_cast<int64_t>(in[1]) * m[9] +
                                   static_cast<int64_t>(in[0]) * m[8] +
                                   static_cast<int64_t>(in[2]) * m[10] + 0x200000) >> 22);
}

static float retail_x87_mul3(float a0, float b0, float a1, float b1,
                             float a2, float b2) {
    volatile double value = static_cast<double>(a0) * b0;
    value = value + static_cast<double>(a1) * b1;
    value = value + static_cast<double>(a2) * b2;
    return static_cast<float>(value);
}

static float retail_x87_mul3_add(float a0, float b0, float a1, float b1,
                                 float a2, float b2, float add) {
    volatile double value = static_cast<double>(a0) * b0;
    value = value + static_cast<double>(a1) * b1;
    value = value + static_cast<double>(a2) * b2;
    value = value + add;
    return static_cast<float>(value);
}

bool collision_matrix_apply_render_pose(const CollisionMatrix &entity_world,
                                        const float pose[16],
                                        CollisionMatrix &out) {
    if (pose == nullptr) return false;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(pose[i])) return false;

    constexpr float kInvQ22 = 1.0f / 4194304.0f;
    constexpr float kInv16 = 1.0f / 65536.0f;
    // Fixed mission matrix -> row-vector render float, including the
    // (-mission-y, mission-z, mission-x) axis map.
    // [orig: Math_FixedPointToFloatMatrix4x4_Swizzled @ 0x611080]
    float entity[16] = {};
    entity[0] = entity_world.m[5] * kInvQ22;
    entity[1] = -entity_world.m[9] * kInvQ22;
    entity[2] = -entity_world.m[1] * kInvQ22;
    entity[4] = -entity_world.m[6] * kInvQ22;
    entity[5] = entity_world.m[10] * kInvQ22;
    entity[6] = entity_world.m[2] * kInvQ22;
    entity[8] = -entity_world.m[4] * kInvQ22;
    entity[9] = entity_world.m[8] * kInvQ22;
    entity[10] = entity_world.m[0] * kInvQ22;
    entity[12] = -entity_world.m[7] * kInv16;
    entity[13] = entity_world.m[11] * kInv16;
    entity[14] = entity_world.m[3] * kInv16;
    entity[15] = 1.0f;

    // Row vectors: model point * PANM pose * entity world. Retail performs the
    // adds in this operand order with x87 precision-control set to 53-bit, then
    // stores one binary32 result. The explicit binary64 sequence differs from
    // a normal float loop by one Q22 unit for reachable rotations.
    // [orig: Math_MultiplyMatrix4x4_Float @ 0x611750, as called by
    // Model_TransformBoneMatrices @ 0x58e390]
    float final[16] = {};
    final[0] = retail_x87_mul3(
        pose[1], entity[4], pose[0], entity[0], pose[2], entity[8]);
    final[1] = retail_x87_mul3(
        pose[2], entity[9], pose[1], entity[5], pose[0], entity[1]);
    final[2] = retail_x87_mul3(
        pose[0], entity[2], pose[2], entity[10], pose[1], entity[6]);
    final[4] = retail_x87_mul3(
        pose[4], entity[0], pose[6], entity[8], pose[5], entity[4]);
    final[5] = retail_x87_mul3(
        pose[6], entity[9], pose[5], entity[5], pose[4], entity[1]);
    final[6] = retail_x87_mul3(
        pose[4], entity[2], pose[6], entity[10], pose[5], entity[6]);
    final[8] = retail_x87_mul3(
        pose[8], entity[0], pose[10], entity[8], pose[9], entity[4]);
    final[9] = retail_x87_mul3(
        pose[10], entity[9], pose[9], entity[5], pose[8], entity[1]);
    final[10] = retail_x87_mul3(
        pose[8], entity[2], pose[10], entity[10], pose[9], entity[6]);
    final[12] = retail_x87_mul3_add(
        pose[12], entity[0], pose[14], entity[8], pose[13], entity[4], entity[12]);
    final[13] = retail_x87_mul3_add(
        pose[14], entity[9], pose[13], entity[5], pose[12], entity[1], entity[13]);
    final[14] = retail_x87_mul3_add(
        pose[12], entity[2], pose[14], entity[10], pose[13], entity[6], entity[14]);
    final[15] = 1.0f;
    for (float v : final)
        if (!std::isfinite(v)) return false;

    constexpr double kQ22 = 4194304.0;
    constexpr double kFixed16 = 65536.0;
    const auto ftol_checked = [](double v, int32_t &dst) {
        if (!std::isfinite(v) ||
            v < static_cast<double>(INT32_MIN) ||
            v > static_cast<double>(INT32_MAX))
            return false;
        dst = static_cast<int32_t>(v); // trunc toward zero [orig: _ftol2_sse]
        return true;
    };

    CollisionMatrix converted;
    // Render float -> fixed mission matrix, the exact inverse swizzle. Only the
    // FINAL float matrix is quantized, like BoneCallback_Generic.
    // [orig: Math_FloatMatrixToFixedPoint22 @ 0x611140]
    if (!ftol_checked(final[10] * kQ22, converted.m[0]) ||
        !ftol_checked(-final[2] * kQ22, converted.m[1]) ||
        !ftol_checked(final[6] * kQ22, converted.m[2]) ||
        !ftol_checked(final[14] * kFixed16, converted.m[3]) ||
        !ftol_checked(-final[8] * kQ22, converted.m[4]) ||
        !ftol_checked(final[0] * kQ22, converted.m[5]) ||
        !ftol_checked(-final[4] * kQ22, converted.m[6]) ||
        !ftol_checked(-final[12] * kFixed16, converted.m[7]) ||
        !ftol_checked(final[9] * kQ22, converted.m[8]) ||
        !ftol_checked(-final[1] * kQ22, converted.m[9]) ||
        !ftol_checked(final[5] * kQ22, converted.m[10]) ||
        !ftol_checked(final[13] * kFixed16, converted.m[11]))
        return false;
    converted.m[12] = converted.m[13] = converted.m[14] = converted.m[15] = 0;
    out = converted;
    return true;
}

// [orig: Matrix_Transpose3x3WithNegateCol3 @ 0x6136d0]
void CollisionMatrix::invert_into(CollisionMatrix &out) const {
    out.m[3] = -m[3];
    out.m[7] = -m[7];
    out.m[11] = -m[11];
    out.m[0] = m[0]; out.m[1] = m[4]; out.m[2] = m[8];
    out.m[4] = m[1]; out.m[5] = m[5]; out.m[6] = m[9];
    out.m[8] = m[2]; out.m[9] = m[6]; out.m[10] = m[10];
    out.m[12] = out.m[13] = out.m[14] = out.m[15] = 0;
}

bool CollisionMatrix::invert_uniform_scale_into(CollisionMatrix &out,
                                                int32_t scale_q16) const {
    // [orig: Math_BuildInverseFixedPointMatrix3x3 @ 0x613e10]
    const int64_t scaled_q22 = static_cast<int64_t>(scale_q16) << 6;
    if (scaled_q22 < INT32_MIN || scaled_q22 > INT32_MAX) return false;
    const int64_t scale_sq_q22 = (scaled_q22 * scaled_q22) >> 22;
    if (scale_sq_q22 == 0) return false;
    const int64_t reciprocal_sq_q22 = (int64_t{1} << 44) / scale_sq_q22;
    if (reciprocal_sq_q22 < INT32_MIN || reciprocal_sq_q22 > INT32_MAX) return false;
    if (m[3] == INT32_MIN || m[7] == INT32_MIN || m[11] == INT32_MIN) return false;

    CollisionMatrix candidate;
    candidate.m[3] = -m[3];
    candidate.m[7] = -m[7];
    candidate.m[11] = -m[11];
    const int source[3][3] = {{0, 1, 2}, {4, 5, 6}, {8, 9, 10}};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int64_t value =
                (static_cast<int64_t>(m[source[col][row]]) * reciprocal_sq_q22) >> 22;
            if (value < INT32_MIN || value > INT32_MAX) return false;
            candidate.m[source[row][col]] = static_cast<int32_t>(value);
        }
    }
    candidate.m[15] = 0x400000;
    out = candidate;
    return true;
}

CollisionMatrix collision_matrix_from_heading(int32_t heading_bam, const int32_t pos[3]) {
    int32_t c, s;
    quantized_dir(heading_bam, c, s); // Q22 [orig: outMillis table indexing]
    CollisionMatrix out;
    // Rows map local -> world: local +X = forward along heading (the infantry root
    // integration convention: wx = fwd*c - lat*s, wy = fwd*s + lat*c).
    out.m[0] = c;  out.m[1] = -s; out.m[2] = 0;  out.m[3] = pos[0];
    out.m[4] = s;  out.m[5] = c;  out.m[6] = 0;  out.m[7] = pos[1];
    out.m[8] = 0;  out.m[9] = 0;  out.m[10] = 1 << 22; out.m[11] = pos[2];
    out.m[15] = 0;
    return out;
}

// The full placement matrix — Rz(heading)·Ry(-pitch)·Rx(roll), Q22 rows composed
// with the original's per-product >>22 truncations and exact-zero stage skips.
// [orig: Math_BuildFixedPointMatrixFromEulerAngles @ 0x613f40, fed by the spawn
// euler pack {[3] = (90 − yaw) BAM, [4] = pitch BAM, [5] = roll BAM}
// (Entity_SpawnFromBMSRecord @ 0x40eb66); the entity's collision/bone matrices
// carry this same orientation, so a rolled rock's collision shell leans WITH
// its visual — the 00TRg through-shot root cause once statics kept yaw only.]
CollisionMatrix collision_matrix_from_euler(int32_t heading_bam, int32_t pitch_bam,
                                            int32_t roll_bam, const int32_t pos[3]) {
    static constexpr double kRadPerBam = 6.283185307179586 / 4294967296.0;
    static constexpr double kQ22 = 4194304.0;
    const auto trig = [](int32_t bam, int32_t &s, int32_t &c) {
        const double a = static_cast<double>(bam) * kRadPerBam;
        s = static_cast<int32_t>(std::sin(a) * kQ22); // trunc [orig: _ftol2_sse]
        c = static_cast<int32_t>(std::cos(a) * kQ22);
    };
    const auto q = [](int64_t v) { return static_cast<int32_t>(v >> 22); };

    int32_t t[12] = {0}; // stage 1: RotX(roll) [orig: @ 0x613f7b-0x613fc9]
    if (roll_bam != 0) {
        int32_t sr, cr;
        trig(roll_bam, sr, cr);
        t[0] = 1 << 22;
        t[5] = cr; t[6] = -sr;
        t[9] = sr; t[10] = cr;
    } else {
        t[0] = t[5] = t[10] = 1 << 22;
    }

    int32_t p2[12]; // stage 2: RotY(pitch) applied [orig: @ 0x613fd7-0x614099]
    const int32_t *cur = t;
    if (pitch_bam != 0) {
        int32_t sp, cp;
        trig(pitch_bam, sp, cp);
        p2[0] = cp;
        p2[1] = q(static_cast<int64_t>(t[9]) * -sp);
        p2[2] = q(static_cast<int64_t>(t[10]) * -sp);
        p2[3] = 0;
        p2[4] = t[4]; p2[5] = t[5]; p2[6] = t[6]; p2[7] = 0;
        p2[8] = sp;
        p2[9] = q(static_cast<int64_t>(t[9]) * cp);
        p2[10] = q(static_cast<int64_t>(t[10]) * cp);
        p2[11] = 0;
        cur = p2;
    }

    CollisionMatrix out; // stage 3: Rz(heading) applied [orig: @ 0x6140b2-0x6141da]
    if (heading_bam != 0) {
        int32_t sz, cz;
        trig(heading_bam, sz, cz);
        out.m[0] = q(static_cast<int64_t>(cur[0]) * cz);
        out.m[1] = q(static_cast<int64_t>(cur[1]) * cz) + q(static_cast<int64_t>(cur[5]) * -sz);
        out.m[2] = q(static_cast<int64_t>(cur[2]) * cz) + q(static_cast<int64_t>(cur[6]) * -sz);
        out.m[4] = q(static_cast<int64_t>(cur[0]) * sz);
        out.m[5] = q(static_cast<int64_t>(cur[1]) * sz) + q(static_cast<int64_t>(cur[5]) * cz);
        out.m[6] = q(static_cast<int64_t>(cur[2]) * sz) + q(static_cast<int64_t>(cur[6]) * cz);
        out.m[8] = cur[8]; out.m[9] = cur[9]; out.m[10] = cur[10];
    } else {
        out.m[0] = cur[0]; out.m[1] = cur[1]; out.m[2] = cur[2];
        out.m[4] = cur[4]; out.m[5] = cur[5]; out.m[6] = cur[6];
        out.m[8] = cur[8]; out.m[9] = cur[9]; out.m[10] = cur[10];
    }
    out.m[3] = pos[0]; out.m[7] = pos[1]; out.m[11] = pos[2];
    out.m[12] = out.m[13] = out.m[14] = out.m[15] = 0;
    return out;
}

} // namespace opennova::world
