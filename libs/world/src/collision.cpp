// World-object collision queries + the per-tick proximity/blink machinery.
// Faithful structural translations from Jointops.exe (imagebase 0x400000);
// witness record docs/world/world-wac-ai-re.md §15. See collision.h for the
// per-function [orig] map.

#include "world/collision.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <io/bam.h>
#include <terrain/height_field.h>
#include <terrain/terrain_raycast.h>

#include "world/angle.h"
#include "world/dir_table.h"
#include "world/world.h"

namespace opennova::world {

namespace {

inline int32_t abs32(int32_t v) { return opennova::io::bam_abs(v); }

// [orig: dbl_7C19D8 = 2^31/pi — BAM per radian]
constexpr double kBamPerRadian = 683565275.5764316;
// [orig: dbl_7C57B8 = -2^31/pi — the NEGATED BAM-per-radian the ladder-contact
// leg multiplies its atan2 results by (the target-relative subtraction then
// yields target + atan*BAM).]
constexpr double kNegBamPerRadian = -683565275.5764316;
// [orig: dbl_7C3608 = 2*pi/2^32 — radians per BAM32]
constexpr double kRadianPerBam = 1.4629180792671596e-9;
// [orig: flt_7C19E0 = 2147352576.0 — every sqrt is min-clamped to this before
// _ftol2_sse so the int cast can't overflow]
constexpr double kFtolClamp = 2147352576.0;

int32_t sqrt_ftol(double squared_len) {
    double len = std::sqrt(squared_len);
    if (len > kFtolClamp) len = kFtolClamp;
    return static_cast<int32_t>(len);
}

// Distance of `p` from the ray line through `start` along normalized `dir`
// (16.16), computed exactly like the original: project (float sqrt + ftol).
// [orig: the shared projection block in raycast_entity_collision @ 0x4139a4 /
// Entity_RaycastCollisionModel @ 0x4131a1 / Entity_FindNearestByRay @ 0x413d02]
int32_t ray_line_distance(const int32_t start[3], const int32_t dir[3], const int32_t p[3]) {
    const int64_t t = (static_cast<int64_t>(dir[1]) * (p[1] - start[1]) +
                       static_cast<int64_t>(dir[0]) * (p[0] - start[0]) +
                       static_cast<int64_t>(dir[2]) * (p[2] - start[2])) >> 16;
    int32_t d[3];
    for (int i = 0; i < 3; ++i) {
        const int32_t closest =
            start[i] + static_cast<int32_t>((static_cast<int64_t>(dir[i]) * t + 0x8000) >> 16);
        d[i] = abs32(closest - p[i]);
    }
    return sqrt_ftol(static_cast<double>(d[0]) * d[0] + static_cast<double>(d[1]) * d[1] +
                     static_cast<double>(d[2]) * d[2]);
}

int32_t vec_len_ftol(int32_t x, int32_t y, int32_t z) {
    return sqrt_ftol(static_cast<double>(x) * x + static_cast<double>(y) * y +
                     static_cast<double>(z) * z);
}

int32_t person_effective_radius(int32_t section, int32_t authored_radius,
                                int32_t extra_radius) {
    const int32_t scale = section == 14 ? 65 : 45;
    int32_t effective =
            extra_radius + 0xCCC +
            static_cast<int32_t>(static_cast<int64_t>(scale) * authored_radius / 100);
    if ((section == 15 || section == 16) && effective > 0x3000)
        effective = 0x3000;
    return effective;
}

} // namespace

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

        if (s.volume_count > 0) {
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
            s.radius = 0;
            continue;
        }
        const int32_t hx = (s.max_x - s.min_x) >> 1;
        const int32_t hy = (s.max_y - s.min_y) >> 1;
        const int32_t hz = (s.max_z - s.min_z) >> 1;
        s.center[0] = s.min_x + hx;
        s.center[1] = s.min_y + hy;
        s.center[2] = s.min_z + hz;
        s.radius = vec_len_ftol(hx, hy, hz);
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
}

// ----------------------------------------------------------------------------
// Matrix helpers (row-major 3x4, Q22 rotation rows, 16.16 translation at
// [3]/[7]/[11], [15] flags).
// ----------------------------------------------------------------------------

// [orig: Math_TransformPointWithTranslation22 @ 0x412f60 — translate THEN rotate:
// used with the inverse matrix (t = -t_fwd) so local = R^T * (p - t_fwd).]
static void transform_translate_then_rotate(const int32_t m[16], const int32_t in[3],
                                            int32_t out[3]) {
    const int32_t tx = in[0] + m[3];
    const int32_t ty = in[1] + m[7];
    const int32_t tz = in[2] + m[11];
    out[0] = static_cast<int32_t>((static_cast<int64_t>(ty) * m[1] +
                                   static_cast<int64_t>(tx) * m[0] +
                                   static_cast<int64_t>(tz) * m[2] + 0x200000) >> 22);
    out[1] = static_cast<int32_t>((static_cast<int64_t>(ty) * m[5] +
                                   static_cast<int64_t>(tx) * m[4] +
                                   static_cast<int64_t>(tz) * m[6] + 0x200000) >> 22);
    out[2] = static_cast<int32_t>((static_cast<int64_t>(ty) * m[9] +
                                   static_cast<int64_t>(tx) * m[8] +
                                   static_cast<int64_t>(tz) * m[10] + 0x200000) >> 22);
}

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

// ----------------------------------------------------------------------------
// Point-vs-blink query. [orig: Entity_TestCollisionSections @ 0x4aef90]
// ----------------------------------------------------------------------------
bool collision_test_blink(const CollisionTargetView &target, const CollisionPoint *points,
                          const int32_t *radii, int32_t num_points, BlinkAccum &blink) {
    if (target.model == nullptr || target.matrices == nullptr || !target.is_building)
        return false; // [orig: itemDef type != 5 -> return 0 @ 0x4aefb2]
    const CollisionModel &model = *target.model;
    if (model.sections.empty()) return false;

    // Broad phase: any point within bound_radius + point radius per axis. [orig: @ 0x4aefcf]
    int32_t hit_point = -1;
    for (int32_t i = 0; i < num_points; ++i) {
        const int32_t combined = target.bound_radius + radii[i];
        if (abs32(points[i].x - target.pos[0]) <= combined &&
            abs32(points[i].y - target.pos[1]) <= combined &&
            abs32(points[i].z - target.pos[2]) <= combined) {
            hit_point = i;
            break;
        }
    }
    if (hit_point < 0) return false;

    bool found = false;
    int32_t face_counter = -1; // [orig: faceIdx — counts type-8 volumes across sections]
    for (size_t si = 0; si < model.sections.size(); ++si) {
        const CollisionSection &sec = model.sections[si];
        const CollisionMatrix &mat = target.matrices[si];
        if (sec.volume_count == 0 || mat.disabled()) continue; // [orig: @ 0x4af0ae]

        CollisionMatrix inv;
        if (target.uniform_scale_q16 != 0) {
            if (!mat.invert_uniform_scale_into(inv, target.uniform_scale_q16)) continue;
        } else {
            mat.invert_into(inv);
        }
        // The type-8 ordinal counter restarts from the section-entry value for
        // EACH point, so the same volume keeps the same ordinal across points;
        // the last point's count carries into the next section. [orig: the
        // savedFaceIdx save @ 0x4af0d3 / per-point reset @ 0x4af165]
        const int32_t section_entry_counter = face_counter;
        for (int32_t pi = 0; pi < num_points; ++pi) {
            int32_t local[3];
            const int32_t pw[3] = {points[pi].x, points[pi].y, points[pi].z};
            transform_translate_then_rotate(inv.m, pw, local);
            const int32_t r = radii[pi];
            if (local[0] - r > sec.max_x || local[0] + r < sec.min_x ||
                local[1] - r > sec.max_y || local[1] + r < sec.min_y ||
                local[2] - r > sec.max_z || local[2] + r < sec.min_z)
                continue; // [orig: section AABB reject @ 0x4af109-0x4af155]

            face_counter = section_entry_counter;
            for (int32_t vi = 0; vi < sec.volume_count; ++vi) {
                const CollisionVolume &vol = model.volumes[sec.volume_start + vi];
                if (vol.type != 8) continue; // [orig: @ 0x4af188 — blink volumes only]
                ++face_counter;
                if (local[0] - r > vol.max_x || local[0] + r < vol.min_x ||
                    local[1] - r > vol.max_y || local[1] + r < vol.min_y ||
                    local[2] - r > vol.max_z || local[2] + r < vol.min_z)
                    continue; // [orig: volume AABB @ 0x4af1da]

                // Inside every plane (dot>>14 + dist - r < 0). [orig: @ 0x4af208-0x4af25d]
                bool inside = true;
                for (int32_t k = 0; k < vol.plane_count; ++k) {
                    const CollisionPlane &pl = model.planes[vol.plane_start + k];
                    const int32_t dot = static_cast<int32_t>(
                        (static_cast<int64_t>(local[1]) * pl.ny +
                         static_cast<int64_t>(local[0]) * pl.nx +
                         static_cast<int64_t>(local[2]) * pl.nz) >> 14);
                    if (dot + pl.dist - r >= 0) { inside = false; break; }
                }
                if (!inside) continue;

                // [orig: @ 0x4af27f — g_BlinkFlagsAccum |= flags ^ 6; packed hit when
                // hit_count < 4 && faceIdx < 16]
                blink.flags |= vol.flags ^ 6u;
                if (face_counter < 16)
                    blink.add_hit(static_cast<int32_t>(si), target.pool_index);
                found = true;
            }
        }
    }
    return found;
}

// ----------------------------------------------------------------------------
// Ray record + segment-vs-solid clip. [orig: Entity_RaycastCollisionModel @ 0x413060]
// ----------------------------------------------------------------------------
void CollisionRay::refresh() {
    // [orig: the prologue of raycast_entity_collision @ 0x4137c1-0x413890 and the
    // 0x413060 hit tail @ 0x41370c — midpoint/half extents + float-normalized dir]
    int32_t d[3];
    for (int i = 0; i < 3; ++i) {
        d[i] = end[i] - start[i];
        mid[i] = start[i] + (d[i] >> 1);
        half[i] = abs32(d[i] >> 1);
    }
    const double len = std::sqrt(static_cast<double>(d[0]) * d[0] +
                                 static_cast<double>(d[1]) * d[1] +
                                 static_cast<double>(d[2]) * d[2]);
    if (len > 0.0) {
        const double inv = 65536.0 / len; // [orig: flt_7C32BC / len]
        dir[0] = static_cast<int32_t>(d[0] * inv);
        dir[1] = static_cast<int32_t>(d[1] * inv);
        dir[2] = static_cast<int32_t>(d[2] * inv);
    } else {
        dir[0] = dir[1] = dir[2] = 0;
    }
}

void CollisionRay::refresh_bounds() {
    // [orig: the hit tail @ 0x41370c-0x41374e — mid/half from the clipped
    // start/end; dir keeps the construction-time normalization]
    for (int i = 0; i < 3; ++i) {
        const int32_t h = (end[i] - start[i]) >> 1;
        mid[i] = start[i] + h;
        half[i] = abs32(h);
    }
}

bool collision_raycast_model(const CollisionTargetView &target, CollisionRay &ray,
                             CollisionModelHit *out_hit) {
    if (out_hit != nullptr) *out_hit = CollisionModelHit{};
    if (target.model == nullptr || target.matrices == nullptr) return false;
    const CollisionModel &model = *target.model;
    bool hit_found = false;
    CollisionModelHit nearest_hit;

    for (size_t si = 0; si < model.sections.size(); ++si) {
        const CollisionSection &sec = model.sections[si];
        const CollisionMatrix &mat = target.matrices[si];
        if (sec.volume_count == 0 || mat.disabled()) continue; // [orig: @ 0x4130fe]

        CollisionMatrix inv;
        mat.invert_into(inv);
        int32_t lstart[3], ldir[3], lend[3];
        transform_translate_then_rotate(inv.m, ray.start, lstart); // [orig: @ 0x413127]
        inv.rotate_point(ray.dir, ldir);                           // [orig: @ 0x41313d]

        // Section bound-sphere vs ray line. [orig: @ 0x41314f-0x41324a]
        if (ray_line_distance(lstart, ldir, sec.center) > sec.radius) continue;

        transform_translate_then_rotate(inv.m, ray.end, lend); // [orig: @ 0x413268]

        bool clipped_any = false;
        CollisionModelHit section_hit;
        for (int32_t vi = 0; vi < sec.volume_count; ++vi) {
            const CollisionVolume &vol = model.volumes[sec.volume_start + vi];
            if (vol.type != 1) continue; // [orig: @ 0x413298 — solid type-1 only]

            // Volume bound-sphere reject. [orig: @ 0x4132b6-0x41341c]
            const int32_t hx = (vol.max_x - vol.min_x) >> 1;
            const int32_t hy = (vol.max_y - vol.min_y) >> 1;
            const int32_t hz = (vol.max_z - vol.min_z) >> 1;
            const int32_t sphere_r = vec_len_ftol(hx, hy, hz);
            const int32_t c[3] = {vol.min_x + hx, vol.min_y + hy, vol.min_z + hz};
            if (ray_line_distance(lstart, ldir, c) > sphere_r) continue;

            // Convex clip of [lstart, lend] against the plane run. [orig: @ 0x413445-0x41363e]
            int32_t cs[3] = {lstart[0], lstart[1], lstart[2]};
            int32_t ce[3] = {lend[0], lend[1], lend[2]};
            bool miss = false;
            int32_t entry_plane = -1;
            for (int32_t k = 0; k < vol.plane_count; ++k) {
                const CollisionPlane &pl = model.planes[vol.plane_start + k];
                const int32_t d0 = static_cast<int32_t>(
                                       (static_cast<int64_t>(cs[1]) * pl.ny +
                                        static_cast<int64_t>(cs[0]) * pl.nx +
                                        static_cast<int64_t>(cs[2]) * pl.nz) >> 14) +
                                   pl.dist;
                const int32_t d1 = static_cast<int32_t>(
                                       (static_cast<int64_t>(ce[1]) * pl.ny +
                                        static_cast<int64_t>(ce[0]) * pl.nx +
                                        static_cast<int64_t>(ce[2]) * pl.nz) >> 14) +
                                   pl.dist;
                if (d0 >= 0 && d1 >= 0) { miss = true; break; } // both outside one plane
                if (d0 < 0 && d1 < 0) continue;                 // both inside this plane
                const int32_t t =
                    static_cast<int32_t>((static_cast<int64_t>(d0) << 16) / (d0 - d1));
                if (d0 >= 0) {
                    for (int i = 0; i < 3; ++i)
                        cs[i] += static_cast<int32_t>(
                            (static_cast<int64_t>(t) * (ce[i] - cs[i]) + 0x8000) >> 16);
                    entry_plane = vol.plane_start + k;
                } else {
                    for (int i = 0; i < 3; ++i)
                        ce[i] = cs[i] + static_cast<int32_t>(
                                            (static_cast<int64_t>(t) * (ce[i] - cs[i]) + 0x8000) >> 16);
                }
            }
            if (miss) continue;
            // Entry point = the clipped start. [orig: @ 0x413646-0x41365e]
            lend[0] = cs[0]; lend[1] = cs[1]; lend[2] = cs[2];
            clipped_any = true;
            section_hit.section_index = static_cast<int32_t>(si);
            section_hit.volume_index = sec.volume_start + vi;
            section_hit.normal_q16[0] = 0;
            section_hit.normal_q16[1] = 0;
            section_hit.normal_q16[2] = 0;
            if (entry_plane >= 0) {
                const CollisionPlane &pl = model.planes[entry_plane];
                const int32_t local_normal[3] = {
                    static_cast<int32_t>(pl.nx) << 2,
                    static_cast<int32_t>(pl.ny) << 2,
                    static_cast<int32_t>(pl.nz) << 2,
                };
                mat.rotate_point(local_normal, section_hit.normal_q16);
            }
        }

        if (clipped_any) {
            mat.transform_point(lend, ray.end); // [orig: @ 0x4136ac]
            hit_found = true;
            nearest_hit = section_hit;
        }
    }

    if (hit_found) ray.refresh_bounds(); // [orig: @ 0x41370c mid/half recompute — dir untouched]
    if (hit_found && out_hit != nullptr) *out_hit = nearest_hit;
    return hit_found;
}

namespace {

// The odd-even point-in-triangle on the normal's projection plane.
// [orig: Math_PointInTriangle2D @ 0x414050 — the Q8 int16 vertex table << 8
// (16.16), the axis flag picking the coordinate pair (1=XY, 2=XZ, 4=YZ), the
// three edges v0->v1->v2->v0 walked with the crossing count capped at 2;
// inside == exactly ONE crossing of the +u ray from the test point.]
bool point_in_triangle_2d(const int32_t p[3], const CollisionFace &face,
                          const CollisionFaceVertex *verts) {
    int32_t tu, tv;
    int32_t u[3], v[3];
    switch (face.axis) {
        case 1:
            tu = p[0]; tv = p[1];
            for (int i = 0; i < 3; ++i) {
                const CollisionFaceVertex &vt = verts[face.v[i]];
                u[i] = static_cast<int32_t>(vt.x) << 8;
                v[i] = static_cast<int32_t>(vt.y) << 8;
            }
            break;
        case 2:
            tu = p[0]; tv = p[2];
            for (int i = 0; i < 3; ++i) {
                const CollisionFaceVertex &vt = verts[face.v[i]];
                u[i] = static_cast<int32_t>(vt.x) << 8;
                v[i] = static_cast<int32_t>(vt.z) << 8;
            }
            break;
        case 4:
            tu = p[1]; tv = p[2];
            for (int i = 0; i < 3; ++i) {
                const CollisionFaceVertex &vt = verts[face.v[i]];
                u[i] = static_cast<int32_t>(vt.y) << 8;
                v[i] = static_cast<int32_t>(vt.z) << 8;
            }
            break;
        default: // [orig: no-flag falls through with stale locals — never authored]
            return false;
    }
    int crossings = 0;
    bool prev_above = v[0] >= tv;
    for (int e = 0; e < 3 && crossings < 2; ++e) {
        const int32_t su = u[e], sv = v[e];
        const int32_t nu = u[(e + 1) % 3], nv = v[(e + 1) % 3];
        if (prev_above != (nv >= tv)) {
            // quadrant bits: 1 = edge start left of the point, 2 = edge end left.
            const int quadrant = (su < tu ? 1 : 0) | (nu >= tu ? 0 : 2);
            if (quadrant != 3 &&
                (quadrant == 0 ||
                 static_cast<int32_t>(static_cast<int64_t>(nu - su) * (tv - sv) / (nv - sv)) +
                                 su >=
                         tu))
                ++crossings;
        }
        prev_above = nv >= tv;
    }
    return crossings == 1;
}

} // namespace

// ----------------------------------------------------------------------------
// The projectile face-mesh raycast. [orig: Physics_RaycastAgainstBoneCollision
// @ 0x4e4cb0 — see the header note for the witnessed gate-by-gate map.]
// ----------------------------------------------------------------------------
bool collision_raycast_faces(const CollisionTargetView &target, const int32_t start[3],
                             const int32_t end[3], uint32_t ammo_flags, RayFaceHit &out) {
    if (target.model == nullptr || target.matrices == nullptr) return false;
    const CollisionModel &model = *target.model;
    if (model.faces.empty()) return false;

    // Segment length + float-normalized direction — the caller's rayState[6..9]
    // [orig: the velocityMagnitude sqrt + flt_7C32BC normalize @ 0x4ea0fc].
    int32_t d[3], ndir[3];
    for (int i = 0; i < 3; ++i) d[i] = end[i] - start[i];
    const double dlen = std::sqrt(static_cast<double>(d[0]) * d[0] +
                                  static_cast<double>(d[1]) * d[1] +
                                  static_cast<double>(d[2]) * d[2]);
    if (dlen <= 0.0) return false;
    const int32_t seg_len = static_cast<int32_t>(dlen);
    const double ninv = 65536.0 / dlen;
    for (int i = 0; i < 3; ++i) ndir[i] = static_cast<int32_t>(d[i] * ninv);

    int32_t best = seg_len; // [orig: rayState[29] preseeded with rayState[9] @ 0x4e534f]
    bool hit = false;

    for (size_t si = 0; si < model.sections.size(); ++si) {
        const CollisionSection &sec = model.sections[si];
        if (sec.face_count <= 0) continue;
        const CollisionMatrix &mat = target.matrices[si];
        if (mat.polygon_disabled()) continue; // [orig: matrix+60 & 3 @ 0x4e4f12]

        CollisionMatrix inv;
        if (target.uniform_scale_q16 != 0) {
            if (!mat.invert_uniform_scale_into(inv, target.uniform_scale_q16)) continue;
        } else {
            mat.invert_into(inv); // [orig: Matrix_Transpose3x3WithNegateCol3 @ 0x4e4f41]
        }
        int32_t ls[3], le[3], ldir[3];
        transform_translate_then_rotate(inv.m, start, ls); // [orig: @ 0x4e4f57]
        transform_translate_then_rotate(inv.m, end, le);   // [orig: @ 0x4e4f6d]
        inv.rotate_point(ndir, ldir);                      // [orig: @ 0x4e4f86]

        int32_t smin[3], smax[3];
        for (int i = 0; i < 3; ++i) {
            smin[i] = ls[i] < le[i] ? ls[i] : le[i]; // [orig: @ 0x4e4f98-0x4e4fe4]
            smax[i] = ls[i] < le[i] ? le[i] : ls[i];
        }

        const CollisionFaceVertex *verts = model.face_vertices.data() + sec.face_vertex_start;
        for (int32_t fi = 0; fi < sec.face_count; ++fi) {
            const CollisionFace &face = model.faces[sec.face_start + fi];
            // Face AABB reject + the never-hit and foliage gates. [orig: @ 0x4e5073]
            if (smin[0] > face.max[0] || smax[0] < face.min[0] || smin[1] > face.max[1] ||
                smax[1] < face.min[1] || smin[2] > face.max[2] || smax[2] < face.min[2])
                continue;
            if ((face.flags & 0x100u) != 0) continue;
            if (face.material == 17 && (ammo_flags & 0x4000000u) != 0) continue;
            // Plane side at both endpoints (Q14 dot, signed >> 14). [orig: @ 0x4e50b6 shrd]
            const int32_t d0 =
                    static_cast<int32_t>((static_cast<int64_t>(ls[0]) * face.normal[0] +
                                          static_cast<int64_t>(ls[1]) * face.normal[1] +
                                          static_cast<int64_t>(ls[2]) * face.normal[2]) >>
                                         14) +
                    face.plane_dist;
            const int32_t d1 =
                    static_cast<int32_t>((static_cast<int64_t>(le[0]) * face.normal[0] +
                                          static_cast<int64_t>(le[1]) * face.normal[1] +
                                          static_cast<int64_t>(le[2]) * face.normal[2]) >>
                                         14) +
                    face.plane_dist;
            if (d0 > 0 ? d1 > 0 : d1 <= 0) continue; // both on one side [orig: @ 0x4e5101]
            // Direction rule [orig: @ 0x4e5115 — flag 1 always; the double-sided
            // 0x800 branch rides the witnessed nonzero stack-residue arg; else
            // enter-front only].
            if ((face.flags & 1u) == 0) {
                if ((face.flags & 0x800u) == 0 && !(d0 > 0 && d1 <= 0)) continue;
            }
            const int32_t a0 = abs32(d0);
            const int32_t total = a0 + abs32(d1);
            int32_t dist;
            if (a0 <= total)
                dist = static_cast<int32_t>(static_cast<int64_t>(seg_len) * a0 / total);
            else
                dist = 0x40000000; // [orig: the "Rounds Divide Error" clamp @ 0x4e5194]
            if (dist > best) continue; // [orig: <= rayState[29] accept @ 0x4e51c1]
            int32_t hp[3];
            for (int i = 0; i < 3; ++i)
                hp[i] = ls[i] + static_cast<int32_t>(
                                        (static_cast<int64_t>(ldir[i]) * dist + 0x8000) >> 16);
            if (!point_in_triangle_2d(hp, face, verts)) continue;
            best = dist; // [orig: the rayState[21/22/28..33] stamp @ 0x4e5254]
            out.dist = dist;
            out.face_flags = face.flags;
            out.material = face.material;
            out.section = static_cast<int32_t>(si);
            out.face = fi;
            hit = true;
        }
    }
    return hit;
}

bool collision_raycast_person_sections(const CollisionTargetView &target,
                                       const int32_t start[3], const int32_t end[3],
                                       int32_t extra_radius, uint32_t section_mask,
                                       PersonSectionHit &out) {
    out = PersonSectionHit{};
    if (target.model == nullptr || target.matrices == nullptr) return false;
    const CollisionModel &model = *target.model;
    if (model.sections.empty()) return false;

    // Build the caller's fixed16 unit direction and fixed16 segment length.
    // [orig: the ray-state initialization @ 0x4ea0fc]
    int32_t delta[3];
    for (int axis = 0; axis < 3; ++axis) delta[axis] = end[axis] - start[axis];
    const double length_f =
            std::sqrt(static_cast<double>(delta[0]) * delta[0] +
                      static_cast<double>(delta[1]) * delta[1] +
                      static_cast<double>(delta[2]) * delta[2]);
    if (length_f <= 0.0) return false;
    const int32_t length = static_cast<int32_t>(length_f);
    int32_t dir[3];
    const double normalize = 65536.0 / length_f;
    for (int axis = 0; axis < 3; ++axis)
        dir[axis] = static_cast<int32_t>(delta[axis] * normalize);

    // COBJ and callback matrices are paired by ordinal. Retail walks in
    // reverse, retaining the first/highest overlap as the reaction/death bone
    // while updating the normal-infantry damage zone for every overlap.
    // [orig: Physics_RaycastAgainstBoneSections @ 0x4e4670]
    for (int32_t si = static_cast<int32_t>(model.sections.size()) - 1; si >= 0; --si) {
        const uint32_t bit = 1u << (static_cast<uint32_t>(si) & 31u);
        if ((section_mask & bit) != 0) continue;
        const CollisionSection &section = model.sections[si];
        if (section.radius <= 0) continue;

        int32_t center[3];
        target.matrices[si].transform_point(section.center, center);

        // Projection is deliberately unclamped and unrounded. Endpoints are
        // included; only projections outside the finite segment are rejected.
        const int64_t projection_sum =
                static_cast<int64_t>(dir[0]) * (center[0] - start[0]) +
                static_cast<int64_t>(dir[1]) * (center[1] - start[1]) +
                static_cast<int64_t>(dir[2]) * (center[2] - start[2]);
        const int32_t projection = static_cast<int32_t>(
                static_cast<uint32_t>(projection_sum >> 16));
        if (projection < 0 || projection > length) continue;

        int32_t offset[3];
        for (int axis = 0; axis < 3; ++axis) {
            const int32_t closest =
                    start[axis] + static_cast<int32_t>(
                                          (static_cast<int64_t>(dir[axis]) * projection +
                                           0x8000) >>
                                          16);
            offset[axis] = abs32(closest - center[axis]);
        }
        const int32_t distance =
                sqrt_ftol(static_cast<double>(offset[0]) * offset[0] +
                          static_cast<double>(offset[1]) * offset[1] +
                          static_cast<double>(offset[2]) * offset[2]);

        const int32_t effective_radius =
                person_effective_radius(si, section.radius, extra_radius);
        if (distance > effective_radius) continue;

        if (out.primary_section < 0) {
            out.dist = projection - (section.radius >> 1);
            out.projected_dist = projection;
            out.primary_section = si;
        }
        out.secondary_section = si;
    }
    return out.primary_section >= 0;
}

CollisionWorld::FaceRaycast CollisionWorld::raycast_entity_faces(
        World &world, EntityHandle h, const int32_t start[3], const int32_t end[3],
        uint32_t ammo_flags, RayFaceHit &out) {
    CollisionTargetView scratch;
    std::vector<CollisionMatrix> mats;
    const CollisionTargetView *view = target_view(world, h, scratch, mats);
    if (view == nullptr || view->model == nullptr || view->model->faces.empty())
        return FaceRaycast::kNoFaceMesh;
    return collision_raycast_faces(*view, start, end, ammo_flags, out) ? FaceRaycast::kHit
                                                                       : FaceRaycast::kMiss;
}

bool CollisionWorld::raycast_person_sections(World &world, EntityHandle h,
                                              const int32_t start[3],
                                              const int32_t end[3],
                                              int32_t extra_radius,
                                              PersonSectionHit &out) {
    CollisionTargetView scratch;
    std::vector<CollisionMatrix> mats;
    const CollisionTargetView *view = target_view(world, h, scratch, mats);
    if (view == nullptr) return false;
    const Entity *entity = world.registry.get(h);
    if (entity == nullptr) return false;
    return collision_raycast_person_sections(*view, start, end, extra_radius,
                                             entity->section_mask, out);
}

// Retail's signed-integer odd/even triangle test. The three vertices are already
// in the exact Q16 form produced by CVRT raw 8.8 values shifted left eight.
// [orig: Math_PointInTriangle2D @ 0x414050]
static bool point_in_collision_triangle(const int32_t point[3],
                                        const CollisionVertex vertices[3],
                                        int16_t dominant_axis) {
    int u_axis = 0;
    int v_axis = 1;
    if ((dominant_axis & 1) != 0) {
        u_axis = 0;
        v_axis = 1;
    } else if ((dominant_axis & 2) != 0) {
        u_axis = 0;
        v_axis = 2;
    } else if ((dominant_axis & 4) != 0) {
        u_axis = 1;
        v_axis = 2;
    } else {
        return false;
    }

    int32_t u[3] = {};
    int32_t v[3] = {};
    for (int i = 0; i < 3; ++i) {
        u[i] = vertices[i].p[u_axis];
        v[i] = vertices[i].p[v_axis];
    }

    const int32_t test_u = point[u_axis];
    const int32_t test_v = point[v_axis];
    int32_t prev_u = u[0];
    int32_t prev_v = v[0];
    bool prev_above = prev_v >= test_v;
    int crossings = 0;
    const int next_indices[3] = {1, 2, 0};
    for (int edge = 0; edge < 3 && crossings < 2; ++edge) {
        const int next = next_indices[edge];
        const int32_t next_u = u[next];
        const int32_t next_v = v[next];
        const bool next_above = next_v >= test_v;
        if (prev_above != next_above) {
            const int quadrant = (prev_u < test_u ? 1 : 0) |
                                 (next_u >= test_u ? 0 : 2);
            if (quadrant != 3) {
                if (quadrant == 0) {
                    ++crossings;
                } else {
                    const int64_t intersection =
                        (static_cast<int64_t>(next_u) - prev_u) *
                            (static_cast<int64_t>(test_v) - prev_v) /
                            (static_cast<int64_t>(next_v) - prev_v) +
                        prev_u;
                    if (intersection >= test_u) ++crossings;
                }
            }
        }
        prev_u = next_u;
        prev_v = next_v;
        prev_above = next_above;
    }
    return crossings == 1;
}

bool collision_raycast_polygons(const CollisionTargetView &target,
                                const CollisionRay &ray,
                                int32_t segment_length_q16,
                                uint32_t ammo_flags,
                                CollisionPolygonHit &out_hit) {
    out_hit = CollisionPolygonHit{};
    if (target.model == nullptr || target.matrices == nullptr ||
        segment_length_q16 <= 0)
        return false;

    const CollisionModel &model = *target.model;
    bool found = false;
    for (size_t si = 0; si < model.sections.size(); ++si) {
        const CollisionSection &sec = model.sections[si];
        const CollisionMatrix &mat = target.matrices[si];
        if (sec.face_count <= 0 || mat.polygon_disabled()) continue;

        CollisionMatrix inv;
        if (target.uniform_scale_q16 != 0) {
            if (!mat.invert_uniform_scale_into(inv, target.uniform_scale_q16)) continue;
        } else {
            mat.invert_into(inv);
        }
        int32_t local_start[3] = {};
        int32_t local_end[3] = {};
        int32_t local_dir[3] = {};
        transform_translate_then_rotate(inv.m, ray.start, local_start);
        transform_translate_then_rotate(inv.m, ray.end, local_end);
        inv.rotate_point(ray.dir, local_dir);

        int32_t segment_min[3] = {};
        int32_t segment_max[3] = {};
        for (int axis = 0; axis < 3; ++axis) {
            segment_min[axis] = std::min(local_start[axis], local_end[axis]);
            segment_max[axis] = std::max(local_start[axis], local_end[axis]);
        }

        for (int32_t local_face = 0; local_face < sec.face_count; ++local_face) {
            const int32_t face_index = sec.face_start + local_face;
            if (face_index < 0 || face_index >= static_cast<int32_t>(model.faces.size()))
                continue;
            const CollisionFace &face = model.faces[face_index];
            if (segment_min[0] > face.max[0] || segment_max[0] < face.min[0] ||
                segment_min[1] > face.max[1] || segment_max[1] < face.min[1] ||
                segment_min[2] > face.max[2] || segment_max[2] < face.min[2])
                continue;
            if ((face.material_flags & 0x100u) != 0) continue;
            if (face.poly_type == 17 && (ammo_flags & 0x04000000u) != 0) continue;

            const int32_t normal_index = sec.normal_start + face.normal_index;
            if (face.normal_index < 0 || normal_index < 0 ||
                normal_index >= static_cast<int32_t>(model.normals.size()))
                continue;
            const CollisionNormal &normal = model.normals[normal_index];
            const int32_t d0 = static_cast<int32_t>(
                                   (static_cast<int64_t>(local_start[0]) * normal.n[0] +
                                    static_cast<int64_t>(local_start[1]) * normal.n[1] +
                                    static_cast<int64_t>(local_start[2]) * normal.n[2]) >> 14) +
                               face.plane_dist;
            const int32_t d1 = static_cast<int32_t>(
                                   (static_cast<int64_t>(local_end[0]) * normal.n[0] +
                                    static_cast<int64_t>(local_end[1]) * normal.n[1] +
                                    static_cast<int64_t>(local_end[2]) * normal.n[2]) >> 14) +
                               face.plane_dist;
            if (d0 > 0) {
                if (d1 > 0) continue;
            } else if (d1 <= 0) {
                continue;
            }

            // Projectile callers pass backfaceArg=1. Only a non-two-sided,
            // 0x800 face rejects the negative-to-positive crossing.
            if ((face.material_flags & 1u) == 0 &&
                (face.material_flags & 0x800u) != 0 &&
                !(d0 > 0 && d1 <= 0))
                continue;

            const int32_t abs_d0 = abs32(d0);
            const int32_t denom = abs_d0 + abs32(d1);
            if (denom <= 0) continue;
            const int32_t distance = static_cast<int32_t>(
                static_cast<int64_t>(abs_d0) * segment_length_q16 / denom);
            if (distance > out_hit.distance_q16) continue;

            int32_t local_hit[3] = {};
            for (int axis = 0; axis < 3; ++axis) {
                local_hit[axis] = local_start[axis] + static_cast<int32_t>(
                    (static_cast<int64_t>(local_dir[axis]) * distance + 0x8000) >> 16);
            }

            CollisionVertex triangle[3];
            bool indices_valid = true;
            for (int vertex = 0; vertex < 3; ++vertex) {
                const int32_t local_index = face.vertex_index[vertex];
                const int32_t vertex_index = sec.vertex_start + local_index;
                if (local_index < 0 || vertex_index < 0 ||
                    vertex_index >= static_cast<int32_t>(model.vertices.size())) {
                    indices_valid = false;
                    break;
                }
                triangle[vertex] = model.vertices[vertex_index];
            }
            if (!indices_valid ||
                !point_in_collision_triangle(local_hit, triangle, normal.dominant_axis))
                continue;

            found = true;
            out_hit.distance_q16 = distance;
            mat.transform_point(local_hit, out_hit.position_q16);
            const int32_t local_normal[3] = {
                static_cast<int32_t>(normal.n[0]) << 2,
                static_cast<int32_t>(normal.n[1]) << 2,
                static_cast<int32_t>(normal.n[2]) << 2,
            };
            mat.rotate_point(local_normal, out_hit.normal_q16);
            out_hit.section_index = static_cast<int32_t>(si);
            out_hit.face_index = face_index;
            out_hit.section_face_index = local_face;
            out_hit.material_flags = face.material_flags;
            out_hit.poly_type = face.poly_type;
        }
    }
    return found;
}

// ----------------------------------------------------------------------------
// Contact force. [orig: Entity_ComputeBoneCollisionForce @ 0x4ae150]
// ----------------------------------------------------------------------------
bool collision_contact_force(const CollisionTargetView &target, const ContactQuery &q,
                             BlinkAccum &blink, LadderContact &ladder, ContactResult &out) {
    out = ContactResult{};
    if (target.model == nullptr || target.matrices == nullptr) return false;
    if ((target.entity_flags & 1u) != 0) return false; // [orig: targetEntity[9] & 1 @ 0x4ae1bd]
    const CollisionModel &model = *target.model;
    if (model.sections.empty() || q.num_points <= 0) return false;

    // Broad phase per point. [orig: @ 0x4ae1d0]
    {
        bool any = false;
        for (int32_t i = 0; i < q.num_points; ++i) {
            const int32_t combined = target.bound_radius + q.radii[i];
            if (abs32(q.points[i].x - target.pos[0]) <= combined &&
                abs32(q.points[i].y - target.pos[1]) <= combined &&
                abs32(q.points[i].z - target.pos[2]) <= combined) {
                any = true;
                break;
            }
        }
        if (!any) return false;
    }

    int32_t any_collision = 0;
    int32_t max_penetration = 0;
    int32_t out_force[3] = {0, 0, 0};
    int32_t blink_volume_counter = -1; // per-section BB ordinal [orig: local at @0x4ae575]

    for (size_t si = 0; si < model.sections.size(); ++si) {
        const CollisionSection &sec = model.sections[si];
        const CollisionMatrix &mat = target.matrices[si];
        if (sec.volume_count == 0 || mat.disabled()) continue;
        // [orig: the building destroyed/animated bone-map skip @ 0x4ae30d — the
        // itemDef+2192/2193 section map is not modeled yet (D-COL-2); sections
        // always collide.]

        CollisionMatrix inv;
        mat.invert_into(inv);
        int32_t local_prev[3];
        transform_translate_then_rotate(inv.m, q.prev_pos, local_prev); // [orig: @ 0x4ae3ac]

        bool has_collision = false;
        int32_t primary[3] = {0, 0, 0};
        int32_t secondary[3] = {0, 0, 0};
        // The type-8 ordinal restarts from the section-entry value per point (the
        // same volume keeps its ordinal across points). [orig: v118 save @ 0x4ae384
        // / per-point restore @ 0x4ae4f6]
        const int32_t section_entry_counter = blink_volume_counter;

        for (int32_t pi = 0; pi < q.num_points; ++pi) {
            const bool prev_has_collision = has_collision;
            const int32_t radius = q.radii[pi];
            int32_t local[3];
            const int32_t pw[3] = {q.points[pi].x, q.points[pi].y, q.points[pi].z};
            transform_translate_then_rotate(inv.m, pw, local);
            if (local[0] - radius > sec.max_x || local[0] + radius < sec.min_x ||
                local[1] - radius > sec.max_y || local[1] + radius < sec.min_y ||
                local[2] - radius > sec.max_z || local[2] + radius < sec.min_z)
                continue; // [orig: @ 0x4ae43b-0x4ae4a6]

            blink_volume_counter = section_entry_counter;
            // Vehicle-collision pass start index. A section with VC/VK starts at
            // that specialized run; a section without one falls back to its
            // ordinary CB/default solids. [orig: @ 0x4ae4b8-0x4ae4df]
            const bool vehicle_pass = (q.mask & 8) != 0;
            int32_t vi = 0;
            bool vehicle_scoped = false;
            if (vehicle_pass && sec.vehicle_volume_start != -1) {
                vi = sec.vehicle_volume_start;
                vehicle_scoped = true;
            }

            for (; vi < sec.volume_count; ++vi) {
                const CollisionVolume &vol = model.volumes[sec.volume_start + vi];
                const int32_t type = vol.type;
                if (vehicle_scoped && type != 7 && type != 12) continue; // [orig: @ 0x4ae52f]
                if (type == 19) {
                    if ((q.mask & 2) == 0) continue; // CP: players, not AI [orig: @ 0x4ae543]
                } else if (type == 7) {
                    if (!vehicle_pass) continue; // [orig: @ 0x4ae558]
                } else if (type == 12) {
                    if ((q.mask & 0x10) == 0) continue; // [orig: @ 0x4ae568]
                } else if (type == 8) {
                    ++blink_volume_counter; // [orig: @ 0x4ae575]
                }

                const bool ladder_recontact_mode = (q.mask & 1) != 0;
                int32_t min_pen = INT32_MIN / 2;    // [orig: -1073741824]
                int32_t second_pen = INT32_MIN / 2;
                int32_t best_plane = 0, second_plane = 0;
                int32_t plane_idx = 0;
                bool reached_inside = false;

                if (ladder_recontact_mode && type == 4) {
                    // CL ladder recontact: inflated AABB + current-point plane test.
                    // [orig: @ 0x4ae611-0x4ae6cf — margin 0x8000, dot>>9 vs
                    // 32*(dist - r - 0x8000)]
                    if (local[0] - radius - 0x8000 > vol.max_x ||
                        local[0] + radius + 0x8000 < vol.min_x ||
                        local[1] - radius - 0x8000 > vol.max_y ||
                        local[1] + radius + 0x8000 < vol.min_y ||
                        local[2] - radius - 0x8000 > vol.max_z ||
                        local[2] + radius + 0x8000 < vol.min_z)
                        continue;
                    reached_inside = true;
                    for (plane_idx = 0; plane_idx < vol.plane_count; ++plane_idx) {
                        const CollisionPlane &pl = model.planes[vol.plane_start + plane_idx];
                        const int32_t dot = static_cast<int32_t>(
                            (static_cast<int64_t>(local[1]) * pl.ny +
                             static_cast<int64_t>(local[0]) * pl.nx +
                             static_cast<int64_t>(local[2]) * pl.nz) >> 9);
                        const int32_t v = dot + 32 * (pl.dist - radius - 0x8000);
                        if (v >= 0) { reached_inside = false; break; }
                        if (v > min_pen) { min_pen = v; best_plane = plane_idx; }
                    }
                    if (!reached_inside) continue;
                } else {
                    // Standard volume: AABB + prev-position-gated two-sided plane test.
                    // [orig: @ 0x4ae734-0x4ae863]
                    if (local[0] - radius > vol.max_x || local[0] + radius < vol.min_x ||
                        local[1] - radius > vol.max_y || local[1] + radius < vol.min_y ||
                        local[2] - radius > vol.max_z || local[2] + radius < vol.min_z)
                        continue;
                    reached_inside = true;
                    for (plane_idx = 0; plane_idx < vol.plane_count; ++plane_idx) {
                        const CollisionPlane &pl = model.planes[vol.plane_start + plane_idx];
                        const int32_t dprev = static_cast<int32_t>(
                            (static_cast<int64_t>(local_prev[1]) * pl.ny +
                             static_cast<int64_t>(local_prev[0]) * pl.nx +
                             static_cast<int64_t>(local_prev[2]) * pl.nz) >> 9);
                        // Only planes the previous position was outside of (within +r
                        // margin) are separator candidates. [orig: @ 0x4ae7be]
                        if (dprev + 32 * (radius + pl.dist) >= 0) {
                            const int32_t dcur = static_cast<int32_t>(
                                (static_cast<int64_t>(local[1]) * pl.ny +
                                 static_cast<int64_t>(local[0]) * pl.nx +
                                 static_cast<int64_t>(local[2]) * pl.nz) >> 9);
                            const int32_t v = dcur + 32 * (pl.dist - radius);
                            if (v >= 0) { reached_inside = false; break; } // outside
                            if (v > min_pen) {
                                second_pen = min_pen;
                                second_plane = best_plane;
                                min_pen = v;
                                best_plane = plane_idx;
                            } else if (v > second_pen) {
                                second_pen = v;
                                second_plane = plane_idx;
                            }
                        }
                    }
                    if (!reached_inside) continue;
                }

                // Type dispatch on a contained point. [orig: the switch @ 0x4ae887]
                switch (type) {
                    case 5:
                        has_collision = true; // contact, no force [orig: @ 0x4ae874]
                        break;
                    case 4: { // CL ladder alignment frame [orig: @ 0x4ae894-0x4aea30]
                        out.flags |= 0x1u;
                        // Two rotations through the section matrix: x/y from
                        // (mid, mid, the point's local z), z from (mid, mid,
                        // maxZ - 1.0u). [orig: the paired
                        // Math_TransformPointFixedPoint22 calls @ 0x4ae8f2/0x4ae903]
                        const int32_t mid_x = vol.min_x + ((vol.max_x - vol.min_x) >> 1);
                        const int32_t mid_y = vol.min_y + ((vol.max_y - vol.min_y) >> 1);
                        const int32_t anchor_xy_local[3] = {mid_x, mid_y, local[2]};
                        const int32_t anchor_z_local[3] = {mid_x, mid_y, vol.max_z - 0x10000};
                        int32_t anchor_xy[3], anchor_z[3];
                        mat.rotate_point(anchor_xy_local, anchor_xy);
                        mat.rotate_point(anchor_z_local, anchor_z);
                        ladder.anchor[0] = anchor_xy[0] + target.pos[0];
                        ladder.anchor[1] = anchor_xy[1] + target.pos[1];
                        ladder.anchor[2] = anchor_z[2] + target.pos[2];
                        if (vol.plane_count > 0) {
                            const CollisionPlane &p0 = model.planes[vol.plane_start];
                            // Yaw/pitch are TARGET-RELATIVE: entity Yaw/Pitch minus
                            // atan2 * -BAM (net +). The XY length is ftol'd to int
                            // before the pitch atan2. [orig: @ 0x4ae938-0x4ae9d9,
                            // dbl_7C57B8 = -2^31/pi]
                            ladder.yaw =
                                target.yaw_bam -
                                static_cast<int32_t>(std::atan2(-static_cast<double>(p0.ny),
                                                                -static_cast<double>(p0.nx)) *
                                                     kNegBamPerRadian);
                            const int32_t lxy_int = sqrt_ftol(
                                static_cast<double>(p0.nx) * p0.nx +
                                static_cast<double>(p0.ny) * p0.ny);
                            ladder.pitch =
                                target.pitch_bam -
                                static_cast<int32_t>(std::atan2(static_cast<double>(p0.nz),
                                                                static_cast<double>(lxy_int)) *
                                                     kNegBamPerRadian);
                        } else {
                            // The original reads plane[0] unguarded even for a
                            // 0-plane volume (adjacent-memory read); a bounds
                            // guard is required here, defaults target-relative.
                            ladder.yaw = target.yaw_bam;
                            ladder.pitch = target.pitch_bam;
                        }
                        // Pull the anchor 0.375u back along the ladder-facing yaw — REAL
                        // sin/cos of the BAM angle scaled 2^22 and truncated, not the
                        // quantized table. [orig: @ 0x4ae9df-0x4aea30 — fsin/fcos of
                        // yaw * dbl_7C3608, * dbl_7C3600]
                        const double yaw_rad = static_cast<double>(ladder.yaw) * kRadianPerBam;
                        const int32_t s22 = static_cast<int32_t>(std::sin(yaw_rad) * 4194304.0);
                        const int32_t c22 = static_cast<int32_t>(std::cos(yaw_rad) * 4194304.0);
                        ladder.anchor[0] -= static_cast<int32_t>((24576LL * c22) >> 22);
                        ladder.anchor[1] -= static_cast<int32_t>((24576LL * s22) >> 22);
                        ladder.valid = true;
                        break;
                    }
                    case 6: // "CA" touch volume [orig: @ 0x4aea45]
                        if (pi < 2) out.flags |= 0x4u;
                        break;
                    case 8: // blink box [orig: @ 0x4aea68-0x4aeae8]
                        if (pi < 2) {
                            out.flags |= 0x10u;
                            if (target.is_building) {
                                blink.flags |= vol.flags ^ 6u;
                                if (blink.hit_count != -1 && blink.hit_count < 4 &&
                                    blink_volume_counter < 16)
                                    blink.add_hit(static_cast<int32_t>(si), target.pool_index);
                            }
                        }
                        break;
                    case 9: // CD: door touch [orig: @ 0x4aeb0f-0x4aeb22]
                        out.flags |= 0x20u;
                        out.door_sections |= 1u << (si & 31);
                        break;
                    case 16: out.flags |= 0x100u; break; // DH damage high [orig: @ 0x4aeb39]
                    case 17: out.flags |= 0x80u; break;  // DM damage medium [orig: @ 0x4aeb50]
                    case 18: out.flags |= 0x40u; break;  // DL damage low [orig: @ 0x4aeb67]
                    case 10: out.flags |= 0x200u; break; // CT: change team [orig: @ 0x4aeb7b]
                    case 11: out.flags |= 0x400u; break; // [orig: @ 0x4aeb92]
                    case 13: // CF: flag/special function, grounded touch [orig: @ 0x4aebb3]
                        if (target.is_ground_of_source) out.flags |= 0x800u;
                        break;
                    case 7:  // VC: vehicle-collision solid (reachable only on mask 0x8)
                    case 12: // optional extension of the same vehicle pass
                    default: {
                        // Solid: accumulate the SAT push-out. [orig: @ 0x4aebdd-0x4aed0c]
                        if (prev_has_collision && (q.mask & 1) != 0) break;
                        if (second_pen + 32 * radius > 0 && (q.mask & 1) == 0) {
                            const CollisionPlane &p2 = model.planes[vol.plane_start + second_plane];
                            secondary[0] += static_cast<int32_t>(
                                (static_cast<int64_t>(p2.nx) * min_pen + 0x8000) >> 16);
                            secondary[1] += static_cast<int32_t>(
                                (static_cast<int64_t>(p2.ny) * min_pen + 0x8000) >> 16);
                            secondary[2] += static_cast<int32_t>(
                                (static_cast<int64_t>(p2.nz) * min_pen + 0x8000) >> 16);
                        }
                        const CollisionPlane &pb = model.planes[vol.plane_start + best_plane];
                        has_collision = true;
                        primary[0] += static_cast<int32_t>(
                            (static_cast<int64_t>(pb.nx) * min_pen + 0x8000) >> 16);
                        primary[1] += static_cast<int32_t>(
                            (static_cast<int64_t>(pb.ny) * min_pen + 0x8000) >> 16);
                        primary[2] += static_cast<int32_t>(
                            (static_cast<int64_t>(pb.nz) * min_pen + 0x8000) >> 16);
                        if (-min_pen > max_penetration) max_penetration = -min_pen;
                        break;
                    }
                }
            }
        }

        if (has_collision) {
            // Rotate the accumulated force into world axes; secondary X/Y are
            // halved, while Z is added at full strength, when they grow the
            // component. [orig: @ 0x4aed5b-0x4aee23]
            int32_t world_primary[3];
            mat.rotate_point(primary, world_primary);
            out_force[0] += world_primary[0];
            out_force[1] += world_primary[1];
            out_force[2] += world_primary[2];
            if (secondary[0] != 0 || secondary[1] != 0 || secondary[2] != 0) {
                int32_t world_secondary[3];
                mat.rotate_point(secondary, world_secondary);
                for (int i = 0; i < 2; ++i) {
                    const int32_t grown = out_force[i] + (world_secondary[i] >> 1);
                    if (abs32(out_force[i]) < abs32(grown)) out_force[i] = grown;
                }
                const int32_t grown_z = out_force[2] + world_secondary[2];
                if (abs32(out_force[2]) < abs32(grown_z)) out_force[2] = grown_z;
            }
            any_collision = 1;
        }
    }

    // Clamp |force| to the source bound radius << 7, then scale >> 5.
    // [orig: @ 0x4aee69-0x4aef76]
    const int32_t force_len = vec_len_ftol(out_force[0], out_force[1], out_force[2]);
    int32_t clamp = q.source_bound_radius << 7;
    int32_t pen = max_penetration;
    if (pen > clamp) pen = clamp;
    if (force_len > pen && force_len != 0) {
        const int32_t scale =
            static_cast<int32_t>((static_cast<int64_t>(pen) << 16) / force_len);
        for (int i = 0; i < 3; ++i)
            out_force[i] = static_cast<int32_t>(
                (static_cast<int64_t>(scale) * out_force[i] + 0x8000) >> 16);
    }
    out.force[0] = (out_force[0] + 16) >> 5;
    out.force[1] = (out_force[1] + 16) >> 5;
    out.force[2] = (out_force[2] + 16) >> 5;
    return any_collision != 0;
}

// ----------------------------------------------------------------------------
// CollisionWorld
// ----------------------------------------------------------------------------

int32_t CollisionWorld::add_model(CollisionModel model) {
    model.finalize_sections();
    models_.push_back(std::move(model));
    return static_cast<int32_t>(models_.size()) - 1;
}

const CollisionModel *CollisionWorld::model(int32_t id) const {
    if (id < 0 || id >= static_cast<int32_t>(models_.size())) return nullptr;
    return &models_[id];
}

void CollisionWorld::assign_entity(EntityHandle h, int32_t model_id,
                                   uint64_t registry_spawn_id) {
    if (!h.valid() || model_id < 0 || model_id >= static_cast<int32_t>(models_.size())) return;
    int32_t husk = -1;
    const auto existing = instances_.find(h.packed);
    if (existing != instances_.end() &&
        (registry_spawn_id == 0 ||
         existing->second.registry_spawn_id == registry_spawn_id)) {
        husk = existing->second.husk_model_id;
    }
    instances_[h.packed] = Instance{model_id, husk, registry_spawn_id};
}

void CollisionWorld::remove_entity_instance(EntityHandle h) {
    if (h.valid()) instances_.erase(h.packed);
}

void CollisionWorld::assign_entity_husk(EntityHandle h, int32_t husk_model_id) {
    if (!h.valid() || husk_model_id < 0 ||
        husk_model_id >= static_cast<int32_t>(models_.size()))
        return;
    auto it = instances_.find(h.packed);
    if (it == instances_.end()) return; // husk stages ride an existing instance
    it->second.husk_model_id = husk_model_id;
}

const CollisionWorld::Instance *CollisionWorld::live_instance(
        const World &world, EntityHandle h) const {
    const auto it = instances_.find(h.packed);
    if (it == instances_.end()) return nullptr;
    const Entity *entity = world.registry.get(h);
    if (entity == nullptr) return nullptr;
    if (it->second.registry_spawn_id != 0 &&
        it->second.registry_spawn_id != entity->registry_spawn_id)
        return nullptr;
    return &it->second;
}

bool CollisionWorld::has_instance(const World &world, EntityHandle h) const {
    return live_instance(world, h) != nullptr;
}

bool CollisionWorld::publish_entity_section_matrices(
    EntityHandle h, std::vector<CollisionMatrix> matrices) {
    auto it = instances_.find(h.packed);
    if (it == instances_.end()) return false;
    const CollisionModel *m = model(it->second.model_id);
    if (m == nullptr || matrices.size() != m->sections.size()) return false;
    it->second.section_matrices = std::move(matrices);
    return true;
}

void CollisionWorld::clear_entity_section_matrices(EntityHandle h) {
    auto it = instances_.find(h.packed);
    if (it != instances_.end()) it->second.section_matrices.clear();
}

bool CollisionWorld::has_instance(EntityHandle h) const {
    return instances_.count(h.packed) != 0;
}

int32_t CollisionWorld::candidate_count(EntityHandle h) const {
    auto it = candidates_.find(h.packed);
    return it == candidates_.end() ? 0 : it->second.count;
}

CollisionWorld::StaticSlotView CollisionWorld::static_slot(int32_t i) const {
    StaticSlotView v;
    if (i < 0 || i >= static_count_) return v;
    const StaticSlot &s = statics_[i];
    v.x = s.x;
    v.y = s.y;
    v.z = s.z;
    v.radius = s.radius;
    v.h = s.h;
    return v;
}

const CollisionModel *CollisionWorld::model_for(
        const World &world, EntityHandle h) const {
    const Instance *instance = live_instance(world, h);
    return instance != nullptr ? model(instance->model_id) : nullptr;
}

const CollisionModel *CollisionWorld::model_for(EntityHandle h) const {
    const auto it = instances_.find(h.packed);
    return it != instances_.end() ? model(it->second.model_id) : nullptr;
}

bool CollisionWorld::ensure_entity_instance(World &world, EntityHandle h) {
    auto existing = instances_.find(h.packed);
    if (existing != instances_.end()) {
        const Entity *entity = world.registry.get(h);
        if (entity != nullptr &&
            (existing->second.registry_spawn_id == 0 ||
             entity->registry_spawn_id ==
                     existing->second.registry_spawn_id)) {
            return true;
        }
        // The packed slot was despawned/reused. Never let its old intact or
        // husk model satisfy a query for the new registry lifetime.
        instances_.erase(existing);
    }
    if (section_matrix_provider_ == nullptr) return false;
    if (!section_matrix_provider_->ensure_collision_instance(world, h)) return false;
    return has_instance(world, h);
}

namespace {

// Entity position in 16.16 engine units (registry entities store float mission units).
void entity_pos_fixed(const Entity &e, int32_t out[3]) {
    out[0] = to_fixed(e.position.x);
    out[1] = to_fixed(e.position.y);
    out[2] = to_fixed(e.position.z);
}

// Collision-model fallback for hosts/tests that have not stamped entity+0.
// The real proximity-table radius is the model-header bound on Entity; deriving
// max |collision AABB corner| is only the best available fallback.
int32_t entity_bound_radius(const CollisionWorld &cw, const CollisionModel *model) {
    (void)cw;
    if (model == nullptr) return 0x10000;
    int32_t r = 0x10000;
    for (const CollisionSection &s : model->sections) {
        const int32_t corners[6] = {abs32(s.min_x), abs32(s.max_x), abs32(s.min_y),
                                    abs32(s.max_y), abs32(s.min_z), abs32(s.max_z)};
        for (int i = 0; i < 6; ++i)
            if (corners[i] > r) r = corners[i];
    }
    return r;
}

int32_t entity_proximity_radius(const CollisionWorld &cw, const Entity &e,
                                const CollisionModel *fallback_model) {
    if (e.bound_radius > 0.0f) return to_fixed(e.bound_radius);
    return entity_bound_radius(cw, fallback_model);
}

} // namespace

void CollisionWorld::build_tick_tables(World &world) {
    // Packed pool/slot handles are reused. Remove every binding whose recorded
    // lifetime no longer names the registry occupant before any proximity,
    // contact, or occlusion consumer can observe its old model or husk.
    for (auto it = instances_.begin(); it != instances_.end();) {
        const EntityHandle h{it->first};
        const Entity *entity = world.registry.get(h);
        if (entity == nullptr ||
            (it->second.registry_spawn_id != 0 &&
             it->second.registry_spawn_id != entity->registry_spawn_id)) {
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }

    // --- pool-2 statics: buildings first, then the rest. [orig: 0x4b9430] ---
    statics_.clear();
    static_building_count_ = 0;
    auto push_static = [&](const Entity &e) {
        // The original's count saturates at 1199 — the 1200th slot is written
        // but never counted, so 1199 is the effective cap. [orig: the
        // `count < 1199` post-increment gate @ 0x4b94cb / 0x4b955f]
        if (statics_.size() >= 1199) return;
        auto it = instances_.find(e.handle.packed);
        const CollisionModel *attached =
            it != instances_.end() ? model(it->second.model_id) : nullptr;
        // Projectile proximity also retains the bounded compatibility entry for
        // an item whose graphic could not resolve a collision instance. Other
        // collision consumers still reject it later at target_view().
        if (attached == nullptr && e.bound_radius <= 0.0f) return;
        StaticSlot s;
        int32_t p[3];
        entity_pos_fixed(e, p);
        const int32_t radius =
            entity_proximity_radius(*this, e, attached) + 111876;
        s.x = static_cast<uint16_t>((static_cast<uint32_t>(p[0]) + 0x8000u) >> 16);
        s.y = static_cast<uint16_t>((static_cast<uint32_t>(p[1]) + 0x8000u) >> 16);
        s.z = static_cast<uint16_t>((static_cast<uint32_t>(p[2]) + 0x8000u) >> 16);
        s.radius = static_cast<uint16_t>(static_cast<uint32_t>(radius) >> 16);
        s.h = e.handle;
        statics_.push_back(s);
    };
    // [orig: pass 1 = itemDef type == Building; pass 2 = everything else with a
    // def — our instance map plays the "has a collision model" role.]
    world.registry.for_each([&](const Entity &e) {
        if (e.handle.pool() == 2 && e.kind == EntityKind::Building) push_static(e);
    });
    static_building_count_ = static_cast<int32_t>(statics_.size());
    world.registry.for_each([&](const Entity &e) {
        if (e.handle.pool() != 2 || e.kind == EntityKind::Building) return;
        push_static(e);
    });
    static_count_ = static_cast<int32_t>(statics_.size());

    // --- pool-0 persons + pool-1 dynamics. [orig: 0x4b9340] ---
    persons_.clear();
    dynamics_.clear();
    world.registry.for_each([&](const Entity &e) {
        if (e.kind != EntityKind::Organic || (e.flags & 1u) != 0) return;
        PersonSlot p;
        int32_t pf[3];
        entity_pos_fixed(e, pf);
        p.x = pf[0]; p.y = pf[1]; p.z = pf[2];
        p.radius = 0x10000; // [orig: entity boundRadius; person capsule ~1u] (D-COL-3)
        p.h = e.handle;
        persons_.push_back(p);
    });
    // Dynamics = mounted-object pool entities with instances that are NOT buildings
    // (vehicles ride here once vehicle collision instances land).
    world.registry.for_each([&](const Entity &e) {
        if (e.kind != EntityKind::Item || (e.flags & 1u) != 0) return;
        auto it = instances_.find(e.handle.packed);
        const CollisionModel *attached =
            it != instances_.end() ? model(it->second.model_id) : nullptr;
        if (attached == nullptr && e.bound_radius <= 0.0f) return;
        DynSlot d;
        int32_t pf[3];
        entity_pos_fixed(e, pf);
        d.x = pf[0]; d.y = pf[1]; d.z = pf[2];
        d.radius = entity_proximity_radius(*this, e, attached);
        d.h = e.handle;
        dynamics_.push_back(d);
    });

    // --- per-entity candidate slices, every 17th tick. [orig: 0x4b8eb0 — pool 0
    // radius +4.0u, pool 1 +6.0u, shared 3000-entry arena; the call is gated on
    // dword_B57C84 >= 0x10 @ 0x4c240f (incremented per tick, zeroed inside the
    // builder), so slices are up to 16 ticks stale by design. Pool-1 SOURCE
    // slices (dynamics, +6.0u, the foliage-attrib parent gate) ride the vehicle
    // pass — only organics run our resolver today.] ---
    if (slice_refresh_counter_ < 16) {
        ++slice_refresh_counter_;
        return; // keep the previous slices/arena
    }
    slice_refresh_counter_ = 0;
    arena_.clear();
    candidates_.clear();
    auto build_for = [&](const Entity &e, int32_t pad) {
        int32_t p[3];
        entity_pos_fixed(e, p);
        const int32_t source_radius =
                e.bound_radius > 0.0f ? to_fixed(e.bound_radius) : 0x10000;
        const int32_t range = source_radius + pad;
        CandidateSlice slice;
        slice.start = static_cast<int32_t>(arena_.size());
        slice.count = 0;
        for (const DynSlot &d : dynamics_) {
            if (d.h == e.handle) continue;
            const int32_t total = range + d.radius;
            if (abs32(d.x - p[0]) > total || abs32(d.y - p[1]) > total ||
                abs32(d.z - p[2]) > total)
                continue;
            if (vec_len_ftol(d.x - p[0], d.y - p[1], d.z - p[2]) > total) continue;
            if (arena_.size() >= 3000) break;
            arena_.push_back(d.h);
            ++slice.count;
        }
        for (int32_t i = 0; i < static_cast<int32_t>(statics_.size()); ++i) {
            const StaticSlot &s = statics_[i];
            if (s.h == e.handle) continue;
            const int32_t sx = static_cast<int32_t>(s.x) << 16;
            const int32_t sy = static_cast<int32_t>(s.y) << 16;
            const int32_t sz = static_cast<int32_t>(s.z) << 16;
            // No quantization slack: the original accepts the +-0.5u table error
            // as-is (the +111876 radius pad at table build absorbs it).
            // [orig: total = range + (radius << 16) @ 0x4b902f]
            const int32_t total = range + (static_cast<int32_t>(s.radius) << 16);
            if (abs32(sx - p[0]) > total || abs32(sy - p[1]) > total || abs32(sz - p[2]) > total)
                continue;
            if (vec_len_ftol(sx - p[0], sy - p[1], sz - p[2]) > total) continue;
            if (arena_.size() >= 3000) break;
            arena_.push_back(s.h);
            ++slice.count;
        }
        candidates_[e.handle.packed] = slice;
    };
    world.registry.for_each([&](const Entity &e) {
        if (e.kind == EntityKind::Organic && (e.flags & 1u) == 0)
            build_for(e, 0x40000); // [orig: pool-0 +4.0u]
    });
    // Pool-1 SOURCE slices for motor-driven vehicles (the hull contact query's
    // candidate set). [orig: Entity_BuildProximityListsFromPools @ 0x4b8eb0 — the
    // pool-1 leg, radius +6.0u @ 0x4b902f]
    world.registry.for_each([&](const Entity &e) {
        if (e.handle.pool() != 1 || (e.flags & 1u) != 0) return;
        const VehicleTraits *vt = world.vehicle_traits.get(e.item_id);
        if (vt == nullptr || vt->physics == 0) return;
        build_for(e, 0x60000);
    });
}

const CollisionTargetView *CollisionWorld::target_view(const World &world, EntityHandle h,
                                                       CollisionTargetView &scratch,
                                                       std::vector<CollisionMatrix> &mats) const {
    const Instance *instance = live_instance(world, h);
    if (instance == nullptr) return nullptr;
    const Entity *e = world.registry.get(h);
    if (e == nullptr) return nullptr;
    // The husk collision swap: a destroyed entity (Flags & 4) collides with its
    // husk-stage model when one is attached; no husk -> the intact model keeps
    // serving, the witnessed fallback [orig: Flags & 4 && huskModel picks
    // entity+52 in Entity_RaycastCollisionModel @ 0x413086 and the pool walk
    // @ 0x538720; the contact pick @ 0x4ae233].
    const bool using_husk =
            (e->engine_flags & 0x4u) != 0 && instance->husk_model_id >= 0;
    int32_t model_id =
            using_husk ? instance->husk_model_id : instance->model_id;
    const CollisionModel *m = model(model_id);
    if (m == nullptr || !m->valid()) return nullptr;

    int32_t p[3];
    entity_pos_fixed(*e, p);
    const int32_t heading = bam_heading_from_mission_yaw_deg(static_cast<double>(e->yaw));
    // Statics authored with pitch/roll (rocks seated on slopes) take the full
    // euler matrix so the shell leans with the visual [orig: the entity
    // orientation matrix @ 0x613f40 serves every collision query]; pure-yaw
    // placements keep the quantized-table heading path bit-for-bit.
    CollisionMatrix world_mat =
            (e->pitch != 0 || e->roll != 0)
                    ? collision_matrix_from_euler(
                              heading,
                              bam_from_degrees_wrapped(static_cast<double>(e->pitch)),
                              bam_from_degrees_wrapped(static_cast<double>(e->roll)), p)
                    : collision_matrix_from_heading(heading, p);
    // Entity/item scale is already present in retail's placement/pose matrices;
    // preserve it before either publication path so the matching scaled inverse
    // can be selected by the polygon walker.
    if (e->uniform_scale_q16 != 0) {
        constexpr int rotation_indices[] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
        for (int index : rotation_indices) {
            world_mat.m[index] = static_cast<int32_t>(
                (static_cast<int64_t>(world_mat.m[index]) * e->uniform_scale_q16) >> 16);
        }
    }

    // Explicitly published matrices and the host callback both own the FINAL
    // world-space slot array for animated models. Slots pair with COBJ sections
    // by ordinal. A missing, rejected, or count-mismatched pose falls back to
    // the retail Simple callback: copy the entity placement matrix into every slot.
    // models. Slots pair with COBJ sections by ordinal; the low-level walkers
    // already consume target.matrices[si] that way. A missing, rejected, or
    // count-mismatched callback falls back to the retail Simple callback: copy
    // the entity placement matrix into every slot.
    // [orig: model+168 callback; BoneCallback_Simple @ 0x4e2600;
    // Physics_RaycastAgainstBoneCollision @ 0x4e4cb0 advances matrix+64 and
    // COBJ+108 in lockstep.]
    bool live_pose = instance->section_matrices.size() == m->sections.size();
    if (live_pose) {
        mats = instance->section_matrices;
    } else if (section_matrix_provider_ != nullptr) {
        mats.clear();
        live_pose = section_matrix_provider_->build_section_matrices(
                const_cast<World &>(world), h, model_id, world_mat, *m, mats) &&
            mats.size() == m->sections.size();
    }
    if (!live_pose)
        mats.assign(m->sections.size(), world_mat);
    if (using_husk && e->spawned_piece_mask != 0) {
        // Sections that launched as death pieces no longer belong to the wreck.
        // The retail piece mask wraps section indices at 32, and collision's
        // existing matrix+60 low-bit gate removes the section from every walk.
        for (size_t si = 0; si < mats.size(); ++si) {
            const uint32_t bit = 1u << (static_cast<uint32_t>(si) & 31u);
            if ((e->spawned_piece_mask & bit) != 0) mats[si].m[15] |= 1;
        }
    }
    scratch.model = m;
    scratch.matrices = mats.data();
    scratch.pos[0] = p[0];
    scratch.pos[1] = p[1];
    scratch.pos[2] = p[2];
    scratch.yaw_bam = heading;
    // CL/type-4 ladder contact keeps a separate target-relative pitch even
    // though the authored angle is also baked into the section matrix.
    // [orig: targetEntity+20 Pitch read @ 0x4ae9a7]
    scratch.pitch_bam =
            bam_from_degrees_wrapped(static_cast<double>(e->pitch));
    scratch.entity_flags = e->flags;
    scratch.bound_radius = entity_bound_radius(*this, m);
    scratch.is_building = (e->kind == EntityKind::Building);
    scratch.pool_index = h.slot();
    scratch.is_ground_of_source = false;
    scratch.uniform_scale_q16 = e->uniform_scale_q16;
    scratch.live_section_pose = live_pose;
    return &scratch;
}

ProjectileHit CollisionWorld::trace_projectile(const World &world,
                                               const ProjectileTrace &trace) const {
    // [orig: Projectile_UpdatePhysics @0x4e9d70] Candidate passes are ordered
    // terrain, water, static CFAC, dynamic CFAC, then person bone proxies.
    // A later pass replaces only when strictly closer.
    ProjectileHit best;
    best.position_q16 = trace.end;

    const int32_t delta[3] = {
        trace.end.x - trace.start.x,
        trace.end.y - trace.start.y,
        trace.end.z - trace.start.z,
    };
    if (delta[0] == 0 && delta[1] == 0 && delta[2] == 0) return best;
    const int32_t segment_length = vec_len_ftol(delta[0], delta[1], delta[2]);
    if (segment_length <= 0) return best;
    int32_t best_distance = 0x7FFFFFFF;

    auto point_at = [&](int32_t t_q16) {
        FixedVec3 p;
        p.x = trace.start.x + static_cast<int32_t>(
            (static_cast<int64_t>(delta[0]) * t_q16) >> 16);
        p.y = trace.start.y + static_cast<int32_t>(
            (static_cast<int64_t>(delta[1]) * t_q16) >> 16);
        p.z = trace.start.z + static_cast<int32_t>(
            (static_cast<int64_t>(delta[2]) * t_q16) >> 16);
        return p;
    };
    auto t_for_distance = [&](int32_t distance) {
        int64_t t = (static_cast<int64_t>(distance) << 16) / segment_length;
        if (t < 0) t = 0;
        if (t > 0x10000) t = 0x10000;
        return static_cast<int32_t>(t);
    };
    auto consider = [&](ProjectileHit candidate, int32_t distance) {
        if (!candidate.hit()) return;
        if (!best.hit() || distance < best_distance) {
            best = candidate;
            best_distance = distance;
        }
    };

    // Ammo flag 0x80 is the recovered NoCollide terrain bypass.
    if ((trace.ammo_flags & 0x80u) == 0 && terrain != nullptr && terrain->valid()) {
        const int32_t start[3] = {trace.start.x, trace.start.y, trace.start.z};
        const int32_t end[3] = {trace.end.x, trace.end.y, trace.end.z};
        int32_t hit[3] = {};
        if (terrain_clip_segment(*terrain, start, end, hit)) {
            const int32_t distance = vec_len_ftol(hit[0] - start[0], hit[1] - start[1],
                                                  hit[2] - start[2]);
            ProjectileHit th;
            th.hit_class = ProjectileHitClass::Terrain;
            th.t_q16 = t_for_distance(distance);
            th.position_q16 = FixedVec3{hit[0], hit[1], hit[2]};

            // Local bilinear gradient in engine axes. This is presentation
            // metadata; the fixed refined hit point remains authoritative.
            const float wx = static_cast<float>(hit[0]) / 65536.0f;
            const float wy = static_cast<float>(hit[1]) / 65536.0f;
            const float hx0 = terrain::height_field_height_world_bilinear(*terrain, wx - 1.0f, -wy);
            const float hx1 = terrain::height_field_height_world_bilinear(*terrain, wx + 1.0f, -wy);
            const float hy0 = terrain::height_field_height_world_bilinear(*terrain, wx, -(wy - 1.0f));
            const float hy1 = terrain::height_field_height_world_bilinear(*terrain, wx, -(wy + 1.0f));
            double nx = -0.5 * static_cast<double>(hx1 - hx0);
            double ny = -0.5 * static_cast<double>(hy1 - hy0);
            double nz = 1.0;
            const double nl = std::sqrt(nx * nx + ny * ny + nz * nz);
            th.normal_q16 = FixedVec3{
                static_cast<int32_t>(nx * 65536.0 / nl),
                static_cast<int32_t>(ny * 65536.0 / nl),
                static_cast<int32_t>(nz * 65536.0 / nl),
            };
            consider(th, distance);
        }
    }

    // Water is pass two regardless of a ClipWater-style ammo flag. Retail
    // requires a strict straddle, so an endpoint on the plane is not a hit.
    const int32_t water_z = world.env.water_z;
    if (water_z != 0 &&
        ((trace.start.z > water_z && trace.end.z < water_z) ||
         (trace.start.z < water_z && trace.end.z > water_z))) {
        const int64_t t =
            (static_cast<int64_t>(water_z - trace.start.z) << 16) / delta[2];
        if (t > 0 && t < 0x10000) {
            ProjectileHit wh;
            wh.hit_class = ProjectileHitClass::Water;
            wh.t_q16 = static_cast<int32_t>(t);
            wh.position_q16 = point_at(wh.t_q16);
            wh.position_q16.z = water_z;
            wh.normal_q16 = FixedVec3{0, 0, 0x10000};
            const int32_t distance = static_cast<int32_t>(
                (static_cast<int64_t>(segment_length) * wh.t_q16) >> 16);
            consider(wh, distance);
        }
    }

    const Entity *owner = world.registry.get(trace.owner);
    EntityHandle ignored_mount;
    EntityHandle ignored_mount_parent;
    if (owner != nullptr && owner->mounted) {
        if (owner->mount_type == SeatType::Controller ||
            owner->mount_type == SeatType::Gunner ||
            owner->mount_type == SeatType::Driver) {
            ignored_mount = owner->mount_target;
        }
        if (owner->mount_type == SeatType::Gunner) {
            if (const Entity *mount = world.registry.get(owner->mount_target))
                ignored_mount_parent = mount->ground_target;
        }
    }
    auto ignored = [&](EntityHandle h) {
        if (!h.valid()) return true;
        if (trace.extra_ignore.valid() && h == trace.extra_ignore) return true;
        if ((trace.ammo_flags & 4u) == 0 && h == trace.owner) return true;
        return h == ignored_mount || h == ignored_mount_parent;
    };

    CollisionRay ray;
    ray.start[0] = trace.start.x;
    ray.start[1] = trace.start.y;
    ray.start[2] = trace.start.z;
    ray.end[0] = trace.end.x;
    ray.end[1] = trace.end.y;
    ray.end[2] = trace.end.z;
    ray.refresh();

    // Compatibility sphere for an item whose graphic has no resolved collision
    // instance. An assigned BVOL-only model is deliberately NOT substituted:
    // gameplay volumes (CB/CL/CA/VC/BB/etc.) are not projectile CFAC geometry.
    auto unresolved_sphere_distance = [&](const Entity &entity, int32_t &out_distance) {
        const int32_t radius = entity.bound_radius > 0.0f
            ? to_fixed(entity.bound_radius)
            : 0;
        if (radius <= 0) return false;
        const long double fx = static_cast<long double>(trace.start.x) -
                               static_cast<long double>(to_fixed(entity.position.x));
        const long double fy = static_cast<long double>(trace.start.y) -
                               static_cast<long double>(to_fixed(entity.position.y));
        const long double fz = static_cast<long double>(trace.start.z) -
                               static_cast<long double>(to_fixed(entity.position.z));
        const long double dx = delta[0], dy = delta[1], dz = delta[2];
        const long double a = dx * dx + dy * dy + dz * dz;
        const long double c = fx * fx + fy * fy + fz * fz -
                              static_cast<long double>(radius) * radius;
        if (c <= 0.0L) {
            out_distance = 0;
            return true;
        }
        const long double b = fx * dx + fy * dy + fz * dz;
        const long double discriminant = b * b - a * c;
        if (a <= 0.0L || discriminant < 0.0L) return false;
        const long double t = (-b - std::sqrt(discriminant)) / a;
        if (t < 0.0L || t > 1.0L) return false;
        out_distance = static_cast<int32_t>(
            t * static_cast<long double>(segment_length));
        return true;
    };

    // Each table query uses <= internally: a later equal face, section, or
    // entity overwrites the earlier result. Cross-pass arbitration stays strict.
    auto trace_polygon_table = [&](const auto &slots, ProjectileHitClass hit_class) {
        CollisionPolygonHit table_hit;
        EntityHandle table_entity;
        bool table_found = false;
        for (const auto &slot : slots) {
            const EntityHandle h = slot.h;
            const Entity *entity = world.registry.get(h);
            if (ignored(h) || entity == nullptr || entity->hidden ||
                (entity->engine_flags & 0x02000001u) != 0)
                continue;
            CollisionTargetView view;
            std::vector<CollisionMatrix> matrices;
            CollisionPolygonHit model_hit;
            const CollisionTargetView *target = target_view(world, h, view, matrices);
            if (target == nullptr) {
                if (!unresolved_sphere_distance(*entity, model_hit.distance_q16)) continue;
                const int32_t t = t_for_distance(model_hit.distance_q16);
                const FixedVec3 p = point_at(t);
                model_hit.position_q16[0] = p.x;
                model_hit.position_q16[1] = p.y;
                model_hit.position_q16[2] = p.z;
            } else {
                const CollisionModel &model_ref = *target->model;
                const bool indexed_mesh = !model_ref.faces.empty() &&
                                          !model_ref.vertices.empty() &&
                                          !model_ref.normals.empty();
                const bool q8_mesh = !model_ref.faces.empty() &&
                                     !model_ref.face_vertices.empty();
                if (indexed_mesh) {
                    if (!collision_raycast_polygons(*target, ray, segment_length,
                                                    trace.ammo_flags, model_hit))
                        continue;
                } else if (q8_mesh) {
                    RayFaceHit face_hit;
                    if (!collision_raycast_faces(*target, ray.start, ray.end,
                                                 trace.ammo_flags, face_hit))
                        continue;
                    model_hit.distance_q16 = face_hit.dist;
                    const int32_t t = t_for_distance(face_hit.dist);
                    const FixedVec3 p = point_at(t);
                    model_hit.position_q16[0] = p.x;
                    model_hit.position_q16[1] = p.y;
                    model_hit.position_q16[2] = p.z;
                    model_hit.section_index = face_hit.section;
                    model_hit.section_face_index = face_hit.face;
                    model_hit.poly_type = face_hit.material;
                    model_hit.material_flags = face_hit.face_flags;
                    if (face_hit.section >= 0 &&
                        face_hit.section < static_cast<int32_t>(model_ref.sections.size())) {
                        const CollisionSection &section =
                            model_ref.sections[face_hit.section];
                        const int32_t global_face = section.face_start + face_hit.face;
                        model_hit.face_index = global_face;
                        if (global_face >= 0 &&
                            global_face < static_cast<int32_t>(model_ref.faces.size())) {
                            const CollisionFace &face = model_ref.faces[global_face];
                            const int32_t local_normal[3] = {
                                static_cast<int32_t>(face.normal[0]) << 2,
                                static_cast<int32_t>(face.normal[1]) << 2,
                                static_cast<int32_t>(face.normal[2]) << 2,
                            };
                            target->matrices[face_hit.section].rotate_point(
                                local_normal, model_hit.normal_q16);
                        }
                    }
                } else {
                    // A resolved model without CFAC is BVOL-only and does not
                    // stop ordinary bullets, regardless of its gameplay volume types.
                    continue;
                }
            }
            if (!table_found || model_hit.distance_q16 <= table_hit.distance_q16) {
                table_found = true;
                table_hit = model_hit;
                table_entity = h;
            }
        }
        if (!table_found) return;
        ProjectileHit eh;
        eh.hit_class = hit_class;
        eh.geometry_entity = table_entity;
        eh.t_q16 = t_for_distance(table_hit.distance_q16);
        eh.position_q16 = FixedVec3{table_hit.position_q16[0], table_hit.position_q16[1],
                                    table_hit.position_q16[2]};
        eh.normal_q16 = FixedVec3{table_hit.normal_q16[0], table_hit.normal_q16[1],
                                  table_hit.normal_q16[2]};
        eh.section_index = table_hit.section_index;
        eh.bone_index = table_hit.section_index;
        eh.face_index = table_hit.section_face_index;
        eh.surface_type = table_hit.poly_type;
        eh.material_flags = table_hit.material_flags;
        consider(eh, table_hit.distance_q16);
    };
    trace_polygon_table(statics_, ProjectileHitClass::StaticEntity);
    trace_polygon_table(dynamics_, ProjectileHitClass::DynamicEntity);

    // Consume the pose owner's COBJ matrices when available. The bounded torso
    // fallback is only for entities whose production pose has not been
    // published; both paths preserve retail's first-qualifying-person table
    // behavior rather than choosing the globally nearest person.
    constexpr int32_t kOrganicCenterZQ16 = 58982;
    int32_t effective_radius = std::max(trace.radius_q16, 0);
    const bool authority_fat_bullet =
        world.mp_session && world.projectile_authority && world.fat_bullets &&
        owner != nullptr && (owner->flags & 0x100u) != 0 &&
        trace.owner != world.cached.local_player;
    if (authority_fat_bullet)
        effective_radius = std::max(effective_radius, kProjectileAuthorityMinRadiusQ16);
    for (const PersonSlot &slot : persons_) {
        if (ignored(slot.h)) continue;
        const Entity *e = world.registry.get(slot.h);
        if (e == nullptr || e->kind != EntityKind::Organic || e->hidden ||
            (e->engine_flags & 0x02000001u) != 0)
            continue;
        ProjectileHit eh;
        int32_t hit_distance = 0;
        bool person_hit = false;
        bool live_pose_available = false;

        // With a published live pose, this is the recovered bone-section
        // routine: bones descend, the first qualifying (highest-index) bone is
        // primary, and its COBJ radius controls both tolerance and hit distance.
        if (!has_instance(world, slot.h)) {
            const_cast<CollisionWorld *>(this)->ensure_entity_instance(
                const_cast<World &>(world), slot.h);
        }
        CollisionTargetView person_view;
        std::vector<CollisionMatrix> person_matrices;
        const CollisionTargetView *posed =
            target_view(world, slot.h, person_view, person_matrices);
        if (posed != nullptr && posed->live_section_pose) {
            live_pose_available = true;
            const CollisionModel &person_model = *posed->model;
            const int32_t base_radius = effective_radius + 3276; // +0.05u
            for (int32_t bone = static_cast<int32_t>(person_model.sections.size()) - 1;
                 bone >= 0; --bone) {
                const uint32_t bit = 1u << (static_cast<uint32_t>(bone) & 31u);
                if ((e->section_mask & bit) != 0) continue;
                const CollisionSection &sec = person_model.sections[bone];
                if (sec.radius <= 0) continue;
                int32_t center[3] = {};
                posed->matrices[bone].transform_point(sec.center, center);
                const int32_t projection = static_cast<int32_t>(
                    (static_cast<int64_t>(ray.dir[0]) * (center[0] - ray.start[0]) +
                     static_cast<int64_t>(ray.dir[1]) * (center[1] - ray.start[1]) +
                     static_cast<int64_t>(ray.dir[2]) * (center[2] - ray.start[2])) >> 16);
                if (projection < 0 || projection > segment_length) continue;
                int32_t closest[3] = {};
                for (int axis = 0; axis < 3; ++axis) {
                    closest[axis] = ray.start[axis] + static_cast<int32_t>(
                        (static_cast<int64_t>(ray.dir[axis]) * projection + 0x8000) >> 16);
                }
                const int32_t center_distance =
                    vec_len_ftol(closest[0] - center[0], closest[1] - center[1],
                                 closest[2] - center[2]);
                int32_t hit_radius = base_radius +
                    (bone == 14 ? 65 * sec.radius / 100 : 45 * sec.radius / 100);
                if ((bone == 15 || bone == 16) && hit_radius > 0x3000)
                    hit_radius = 0x3000;
                if (center_distance > hit_radius) continue;

                if (!person_hit) {
                    hit_distance = projection - (sec.radius >> 1);
                    eh.section_index = bone;
                    eh.bone_index = bone;
                    if (sec.face_count > 0 && sec.face_start >= 0 &&
                        sec.face_start < static_cast<int32_t>(person_model.faces.size())) {
                        const CollisionFace &face = person_model.faces[sec.face_start];
                        eh.material_flags = face.normal_index >= 0
                            ? face.material_flags
                            : face.flags;
                    }
                }
                // The reverse walk retains the first/highest overlap in ray[31]
                // for reactions, but ray[32] is overwritten through the final/
                // lowest overlap and drives normal-infantry damage.
                eh.hit_zone = bone;
                person_hit = true;
            }
        }

        if (live_pose_available && !person_hit) continue;
        if (!live_pose_available) {
            // Bounded fallback until the pose owner publishes organic bone
            // matrices. It follows the same projection/radius arithmetic using
            // one characterized torso COBJ rather than a geometric sphere entry.
            constexpr int32_t kFallbackAuthoredRadiusQ16 = 78642; // 1.2u
            const int32_t center[3] = {
                to_fixed(e->position.x),
                to_fixed(e->position.y),
                to_fixed(e->position.z) + kOrganicCenterZQ16,
            };
            const int32_t projection = static_cast<int32_t>(
                (static_cast<int64_t>(ray.dir[0]) * (center[0] - ray.start[0]) +
                 static_cast<int64_t>(ray.dir[1]) * (center[1] - ray.start[1]) +
                 static_cast<int64_t>(ray.dir[2]) * (center[2] - ray.start[2])) >> 16);
            if (projection < 0 || projection > segment_length) continue;
            int32_t closest[3] = {};
            for (int axis = 0; axis < 3; ++axis) {
                closest[axis] = ray.start[axis] + static_cast<int32_t>(
                    (static_cast<int64_t>(ray.dir[axis]) * projection + 0x8000) >> 16);
            }
            const int32_t center_distance =
                vec_len_ftol(closest[0] - center[0], closest[1] - center[1],
                             closest[2] - center[2]);
            const int32_t hit_radius = effective_radius + 3276 +
                                       45 * kFallbackAuthoredRadiusQ16 / 100;
            if (center_distance > hit_radius) continue;
            hit_distance = projection - (kFallbackAuthoredRadiusQ16 >> 1);
            // No authored section identity exists. RoundSim maps this sentinel
            // to synthetic torso 1 for presentation/reactions while preserving
            // a neutral damage multiplier.
            eh.section_index = -1;
            eh.bone_index = -1;
            eh.hit_zone = -1;
        }

        eh.hit_class = ProjectileHitClass::Person;
        eh.geometry_entity = e->handle;
        eh.t_q16 = t_for_distance(hit_distance);
        const int32_t impact[3] = {
            ray.start[0] + static_cast<int32_t>(
                (static_cast<int64_t>(ray.dir[0]) * hit_distance + 0x8000) >> 16),
            ray.start[1] + static_cast<int32_t>(
                (static_cast<int64_t>(ray.dir[1]) * hit_distance + 0x8000) >> 16),
            ray.start[2] + static_cast<int32_t>(
                (static_cast<int64_t>(ray.dir[2]) * hit_distance + 0x8000) >> 16),
        };
        eh.position_q16 = FixedVec3{impact[0], impact[1], impact[2]};
        eh.surface_type = 19;
        consider(eh, hit_distance);
        break;
    }

    return best;
}

void CollisionWorld::refresh_blink(World &world, Entity &ent) {
    // [orig: Entity_BuildProximityList @ 0x4b3dc0]
    BlinkAccum accum;
    ent.flags &= ~kEntityFlagIndoors;
    const bool is_local = ent.handle == local_player && local_player.valid();
    if (is_local) local_player_blink_flags = 0;

    CollisionPoint pt;
    pt.x = to_fixed(ent.position.x);
    pt.y = to_fixed(ent.position.y);
    pt.z = to_fixed(ent.position.z);
    const int32_t radius = 0x8000; // [orig: searchRadius_fp = 0x8000]

    if (ent.kind == EntityKind::Organic) {
        // Persons (and vehicles, once they get slices) walk their own candidate
        // list testing building-kind candidates — no distance prefilter. [orig:
        // the def type 1/3 branch @ 0x4b3e5f-0x4b3f93]
        auto it = candidates_.find(ent.handle.packed);
        if (it != candidates_.end()) {
            const CandidateSlice slice = it->second;
            for (int32_t i = 0; i < slice.count; ++i) {
                const EntityHandle ch = arena_[slice.start + i];
                if (ch == ent.handle) continue; // [orig: @ 0x4b3f73]
                const Entity *ce = world.registry.get(ch);
                if (ce == nullptr || ce->kind != EntityKind::Building) continue;
                CollisionTargetView view;
                std::vector<CollisionMatrix> mats;
                if (const CollisionTargetView *tv = target_view(world, ch, view, mats))
                    collision_test_blink(*tv, &pt, &radius, 1, accum);
            }
        }
    } else if (ent.kind != EntityKind::Building) {
        // Everything else non-building tests the building prefix of the static
        // table. [orig: the def-null / other-type loops @ 0x4b3e6b / 0x4b3fa0,
        // reject radius = building radius + the 0.5u query radius]
        for (int32_t i = 0; i < static_building_count_; ++i) {
            const StaticSlot &s = statics_[i];
            if (s.h == ent.handle) continue; // [orig: the self check @ 0x4b3f13]
            const int32_t sx = static_cast<int32_t>(s.x) << 16;
            const int32_t sy = static_cast<int32_t>(s.y) << 16;
            const int32_t sz = static_cast<int32_t>(s.z) << 16;
            const int32_t range = (static_cast<int32_t>(s.radius) << 16) + 0x8000;
            if (abs32(sx - pt.x) > range || abs32(sy - pt.y) > range || abs32(sz - pt.z) > range)
                continue;
            if (vec_len_ftol(sx - pt.x, sy - pt.y, sz - pt.z) > range) continue;
            CollisionTargetView view;
            std::vector<CollisionMatrix> mats;
            if (const CollisionTargetView *tv = target_view(world, s.h, view, mats))
                collision_test_blink(*tv, &pt, &radius, 1, accum);
        }
    }

    ent.blink_hits[0] = accum.hits[0];
    ent.blink_hits[1] = accum.hits[1];
    ent.blink_hits[2] = accum.hits[2];
    ent.blink_hits[3] = accum.hits[3];
    if ((accum.flags & kBlinkIndoorsBit) != 0) ent.flags |= kEntityFlagIndoors;
    if (is_local) local_player_blink_flags |= accum.flags;
}

void CollisionWorld::query_blink_boxes_at_point(World &world, const int32_t pos[3],
                                                BlinkAccum &accum) {
    // [orig: Entity_QueryBlinkBoxesAtPoint @ 0x4af350 — clears the blink globals,
    // walks the building prefix (per-axis then euclid broad phase at
    // buildingRadius + 0x8000), runs the point query with one point of radius
    // 0x8000. No self-exclusion: the query point is not an entity.]
    accum.reset();
    CollisionPoint pt;
    pt.x = pos[0];
    pt.y = pos[1];
    pt.z = pos[2];
    const int32_t radius = 0x8000; // [orig: searchRadius = 0x8000 @ 0x4af376]
    for (int32_t i = 0; i < static_building_count_; ++i) {
        const StaticSlot &s = statics_[i];
        const int32_t sx = static_cast<int32_t>(s.x) << 16;
        const int32_t sy = static_cast<int32_t>(s.y) << 16;
        const int32_t sz = static_cast<int32_t>(s.z) << 16;
        const int32_t range = (static_cast<int32_t>(s.radius) << 16) + 0x8000;
        if (abs32(sx - pt.x) > range || abs32(sy - pt.y) > range || abs32(sz - pt.z) > range)
            continue;
        if (vec_len_ftol(sx - pt.x, sy - pt.y, sz - pt.z) > range) continue;
        CollisionTargetView view;
        std::vector<CollisionMatrix> mats;
        if (const CollisionTargetView *tv = target_view(world, s.h, view, mats))
            collision_test_blink(*tv, &pt, &radius, 1, accum);
    }
}

int32_t CollisionWorld::raycast_ground(World &world, EntityHandle source, const int32_t pos[3],
                                       int32_t dx, int32_t dy, int32_t z_up, int32_t z_drop,
                                       EntityHandle *out_hit_entity) {
    // [orig: Entity_RaycastGroundHeight(AndObject) @ 0x4142c0/0x414320 ->
    // raycast_entity_collision @ 0x413760]
    CollisionRay ray;
    ray.start[0] = pos[0] + dx;
    ray.start[1] = pos[1] + dy;
    ray.start[2] = pos[2] + z_up;
    ray.end[0] = ray.start[0];
    ray.end[1] = ray.start[1];
    ray.end[2] = ray.start[2] - z_drop;
    if (out_hit_entity) *out_hit_entity = EntityHandle{};

    // Terrain clamp — skipped for an indoors source. [orig: the Flags & 0x800000
    // gate @ 0x413785; vertical column == the hi-res down-raycast result —
    // bilinear column height equals the @ 0x60e710 march for vertical rays,
    // docs/world/world-wac-ai-re.md (D-COL-7)]
    bool indoors = false;
    if (source.valid()) {
        if (const Entity *se = world.registry.get(source))
            indoors = (se->flags & kEntityFlagIndoors) != 0;
    }
    if (terrain != nullptr && terrain->valid() && !indoors) {
        const float wx = static_cast<float>(ray.start[0]) / 65536.0f;
        const float wz = -static_cast<float>(ray.start[1]) / 65536.0f; // engine Y -> sampler z
        const float h = terrain::height_field_height_world_bilinear(*terrain, wx, wz);
        const int32_t ground = static_cast<int32_t>(h * 65536.0f);
        if (ground > ray.end[2] && ground <= ray.start[2]) ray.end[2] = ground;
    }
    ray.refresh();

    // Candidate narrow phase. [orig: the entity loop @ 0x4138c1-0x413abc]
    auto it = candidates_.find(source.packed);
    if (it != candidates_.end()) {
        const CandidateSlice slice = it->second;
        for (int32_t i = 0; i < slice.count; ++i) {
            const EntityHandle ch = arena_[slice.start + i];
            const Entity *ce = world.registry.get(ch);
            if (ce == nullptr) continue;
            if ((ce->flags & 0x2000001u) != 0) continue; // [orig: @ 0x413a66]
            // Skip candidates standing on the source (3-level chain). [orig: @ 0x413a88]
            EntityHandle g = ce->ground_target;
            bool standing_on_source = false;
            for (int depth = 0; depth < 3 && g.valid(); ++depth) {
                if (g == source) { standing_on_source = true; break; }
                const Entity *ge = world.registry.get(g);
                g = ge ? ge->ground_target : EntityHandle{};
            }
            if (standing_on_source) continue;

            CollisionTargetView view;
            std::vector<CollisionMatrix> mats;
            const CollisionTargetView *tv = target_view(world, ch, view, mats);
            if (tv == nullptr) continue;
            // Broad phase vs the ray box + line distance. [orig: @ 0x413948-0x413a66]
            if (abs32(ray.mid[1] - tv->pos[1]) >
                    tv->bound_radius + static_cast<int32_t>(ray.half[1]) ||
                abs32(ray.mid[0] - tv->pos[0]) >
                    tv->bound_radius + static_cast<int32_t>(ray.half[0]) ||
                abs32(ray.mid[2] - tv->pos[2]) >
                    tv->bound_radius + static_cast<int32_t>(ray.half[2]))
                continue;
            if (ray_line_distance(ray.start, ray.dir, tv->pos) > tv->bound_radius) continue;
            if (collision_raycast_model(*tv, ray) && out_hit_entity) *out_hit_entity = ch;
        }
    }
    return ray.end[2];
}

// The LOS terrain leg [orig: Terrain_RaycastHeightmapHiRes @ 0x60c760 — now the
// faithful LOS-variant port terrain_raycast_los_clear; this helper shipped on
// the AI slice with the 0x60e710 sibling as a boolean stand-in, closed here].
// Engine (X,Y) -> field (x, -y), the grounding convention every world-side
// sampler call uses; every sample kHeight (the runtime height field keeps
// retail's clamp-to-edge "terrain forever" semantics). The march's point
// callback is the NEAREST texel sample (retail reads its render tile cache's
// 0.5u-quantized byte there — the note in terrain/terrain_raycast.h); the
// endpoint prechecks use the bilinear column [orig: Terrain_GetHeightAtPosition
// @ 0x606720].
namespace {

terrain::TerrainRaycastSample los_field_sample(const terrain::TerrainHeightField &f,
                                               int32_t x, int32_t y, bool bilinear) {
    const float wx = static_cast<float>(x) / 65536.0f;
    const float wz = -static_cast<float>(y) / 65536.0f; // engine Y -> sampler z
    int32_t height_1616 = 0;
    if (bilinear) {
        const float h = terrain::height_field_height_world_bilinear(f, wx, wz);
        height_1616 = static_cast<int32_t>(h * 65536.0f);
    } else {
        // This LOS variant reads the nearest render-cache texel and expands its
        // half-unit byte (raw16 >> 7, then sample << 15), not the generic
        // floor-sampled full-precision height-field point contract.
        // [orig: Terrain_RaycastHeightmapHiRes @ 0x60c760 point callback]
        const terrain::CoordsResult<float> r = terrain::coords_world_to_source<float>(
            f.layout, wx, wz, terrain::coords_runtime_options());
        if (r.valid && f.valid()) {
            const int mask = f.dim - 1;
            const int hx = static_cast<int>(std::floor(r.source_x + 0.5f)) & mask;
            const int hz = static_cast<int>(std::floor(r.source_z + 0.5f)) & mask;
            const uint8_t cache_height =
                static_cast<uint8_t>(f.heightmap[hz * f.dim + hx] >> 7);
            height_1616 = static_cast<int32_t>(cache_height) << 15;
        }
    }
    terrain::TerrainRaycastSample s;
    s.kind = terrain::TerrainRaycastSample::kHeight;
    s.height_1616 = height_1616;
    return s;
}

terrain::TerrainRaycastSample los_field_point_cb(void *ctx, int32_t x, int32_t y) {
    return los_field_sample(*static_cast<const terrain::TerrainHeightField *>(ctx), x, y, false);
}

terrain::TerrainRaycastSample los_field_bilinear_cb(void *ctx, int32_t x, int32_t y) {
    return los_field_sample(*static_cast<const terrain::TerrainHeightField *>(ctx), x, y, true);
}

} // namespace

bool los_terrain_blocked(const terrain::TerrainHeightField &field, const int32_t a[3],
                         const int32_t b[3]) {
    terrain::TerrainRaycastSampler sampler;
    sampler.point = &los_field_point_cb;
    sampler.bilinear = &los_field_bilinear_cb;
    sampler.ctx = const_cast<terrain::TerrainHeightField *>(&field);
    return !terrain::terrain_raycast_los_clear(sampler, a, b);
}

bool terrain_clip_segment(const terrain::TerrainHeightField &field, const int32_t a[3],
                          const int32_t b[3], int32_t out_hit[3]) {
    // [orig: raycast_entity_collision @ 0x413760 -> Terrain_RaycastHeightmapHiRes_0
    // @ 0x60e710, called (start, end, end) so the ray end clips in place]
    terrain::TerrainRaycastSampler sampler;
    sampler.point = &los_field_point_cb;
    sampler.bilinear = &los_field_bilinear_cb;
    sampler.ctx = const_cast<terrain::TerrainHeightField *>(&field);
    return terrain::terrain_raycast_refined(sampler, a, b, out_hit);
}

namespace {

// The radiused segment clip, boolean-only: does any TYPE-1 solid volume of
// `target` contain a span of [ray.start, ray.end] under the per-plane radius
// term? [orig: raycast_against_entity_pool @ 0x538720 — plane eval
// (dot >> 14) + dist - radius with strict > 0 = outside (0 counts inside,
// unlike the 0x413060 clip's >= 0); both-outside-a-plane = miss @ 0x538e37;
// a straddle splits at |d0u| / (|d0u| + |d1u|) computed on the UNRADIUSED
// distances with a truncating divide, moving the start when d0u >= 0 else
// the end @ 0x538e3d-0x538f06.] The LOS callers pass the height offset as
// the radius (the frameless-callee arg-slot reuse): ray 1 radius 0, ray 2
// radius -0x8000 — planes read 0.5u THINNER, the mechanism that lets ray 2
// clear near-miss walls the unshifted segment grazes. Every plane takes the
// raw radius here; the original's per-plane flag-byte branch (flagged planes
// use the max(radius, 0) clamp instead) rides D-SND-9 until plane flags are
// plumbed through the collision feed.
bool sound_segment_blocked(const CollisionTargetView &target, const CollisionRay &ray,
                           int32_t radius) {
    if (target.model == nullptr || target.matrices == nullptr) return false;
    const CollisionModel &model = *target.model;
    const int32_t broad_r = radius > 0 ? radius : 0; // [orig: @ 0x538752 clamp]
    for (size_t si = 0; si < model.sections.size(); ++si) {
        const CollisionSection &sec = model.sections[si];
        const CollisionMatrix &mat = target.matrices[si];
        if (sec.volume_count == 0 || mat.disabled()) continue; // [orig: @ 0x538aa8]

        CollisionMatrix inv;
        mat.invert_into(inv);
        int32_t ls[3], le[3], ld[3];
        transform_translate_then_rotate(inv.m, ray.start, ls); // [orig: @ 0x538acd]
        transform_translate_then_rotate(inv.m, ray.end, le);   // [orig: @ 0x538ae6]
        inv.rotate_point(ray.dir, ld);
        // Section broad phase (the original's local AABB-with-margin endpoint
        // test @ 0x538af2-0x538bb2, stood in by the conservative
        // bound-sphere-vs-line reject the 0x413060 clip uses).
        if (ray_line_distance(ls, ld, sec.center) > sec.radius + broad_r) continue;

        for (int32_t vi = 0; vi < sec.volume_count; ++vi) {
            const CollisionVolume &vol = model.volumes[sec.volume_start + vi];
            if (vol.type != 1) continue; // [orig: @ 0x538bd8 — solid type-1 only]

            int32_t cs[3] = {ls[0], ls[1], ls[2]};
            int32_t ce[3] = {le[0], le[1], le[2]};
            bool miss = false;
            for (int32_t k = 0; k < vol.plane_count && !miss; ++k) {
                const CollisionPlane &pl = model.planes[vol.plane_start + k];
                const int32_t d0u = static_cast<int32_t>(
                                        (static_cast<int64_t>(cs[1]) * pl.ny +
                                         static_cast<int64_t>(cs[0]) * pl.nx +
                                         static_cast<int64_t>(cs[2]) * pl.nz) >> 14) +
                                    pl.dist;
                const int32_t d1u = static_cast<int32_t>(
                                        (static_cast<int64_t>(ce[1]) * pl.ny +
                                         static_cast<int64_t>(ce[0]) * pl.nx +
                                         static_cast<int64_t>(ce[2]) * pl.nz) >> 14) +
                                    pl.dist;
                const int32_t d0 = d0u - radius;
                const int32_t d1 = d1u - radius;
                if (d0 > 0 && d1 > 0) { miss = true; break; } // [orig: @ 0x538e29/0x538e37]
                if (d0 > 0 || d1 > 0) {
                    // Straddle split on the unradiused distances. [orig: @ 0x538e3d-0x538f06]
                    const int32_t num = abs32(d0u);
                    int32_t den = num + abs32(d1u);
                    if (den == 0) den = 1; // [orig: @ 0x538e64]
                    if (d0u >= 0) {
                        for (int i = 0; i < 3; ++i)
                            cs[i] += static_cast<int32_t>(
                                static_cast<int64_t>(num) * (ce[i] - cs[i]) / den);
                    } else {
                        for (int i = 0; i < 3; ++i)
                            ce[i] = cs[i] + static_cast<int32_t>(
                                                static_cast<int64_t>(num) * (ce[i] - cs[i]) / den);
                    }
                }
            }
            if (!miss) return true; // a surviving span = blocked [orig: hit_found @ 0x538f25]
        }
    }
    return false;
}

} // namespace

bool CollisionWorld::raycast_clear(World &world, const int32_t a[3], const int32_t b[3],
                                   EntityHandle exclude_a, EntityHandle exclude_b) {
    // [orig: Physics_RaycastTerrainAndSectors @ 0x539910, TRUE = clear; the LOS
    // callers pass ray radius 0, so the witnessed thick-ray Z-drop (@ 0x53994e)
    // and volume inflation are no-ops and are folded out here.]
    const Entity *ea = world.registry.get(exclude_a);
    const Entity *eb = world.registry.get(exclude_b);

    // Terrain leg — skipped when BOTH entities are INDOORS (the heightmap has no
    // interiors) [orig: the Flags & 0x800000 pair gate @ 0x53994c]. The original's
    // null-entity buried-endpoint variant (@ 0x53999a) has no caller on the LOS
    // chain and is not modeled.
    const bool both_indoors = ea != nullptr && eb != nullptr &&
                              (ea->flags & kEntityFlagIndoors) != 0 &&
                              (eb->flags & kEntityFlagIndoors) != 0;
    if (!both_indoors && terrain != nullptr && terrain->valid() &&
        los_terrain_blocked(*terrain, a, b))
        return false; // [orig: heightmap hit -> return 0 @ 0x539968]

    // Sector leg [orig: raycast_against_entity_pool @ 0x538720, pool 2 then pool 1
    // @ 0x539a3a]. Degenerate segments (< 1 u) skip the walk entirely
    // [orig: Physics_RaycastIntContext @ 0x5385e0 returns 1 -> clear on len < 16].
    const int64_t sdx = static_cast<int64_t>(b[0]) - a[0];
    const int64_t sdy = static_cast<int64_t>(b[1]) - a[1];
    const int64_t sdz = static_cast<int64_t>(b[2]) - a[2];
    const double seg_len = std::sqrt(static_cast<double>(sdx) * sdx +
                                     static_cast<double>(sdy) * sdy +
                                     static_cast<double>(sdz) * sdz);
    if (static_cast<int64_t>(seg_len) < 16) return true; // < 16 raw 16.16 (1/4096 u)

    CollisionRay ray;
    ray.start[0] = a[0];
    ray.start[1] = a[1];
    ray.start[2] = a[2];
    ray.end[0] = b[0];
    ray.end[1] = b[1];
    ray.end[2] = b[2];
    ray.refresh();

    // Per candidate [orig: the @ 0x538720 walk]: in-use with a collision model,
    // skip flags & 1 (@ 0x538792), skip engine_flags & 0x8000000 (@ 0x5387b4),
    // skip the excluded entities and anything standing on them (the +0x28
    // owner-link pair test @ 0x538836-0x538877), bound-sphere broad phase (the
    // segment box + line-distance fold of @ 0x5387c4-0x5389a4), then the TYPE-1
    // volume convex clip (the shared @ 0x413060 core). A hit blocks — the
    // original keeps walking to clip the nearest point; the boolean result is
    // identical (@ 0x5390e6 miss_result = 0).
    auto blocked_by = [&](const Entity &e) -> bool {
        if ((e.flags & 1u) != 0) return false;
        if ((e.engine_flags & 0x8000000u) != 0) return false;
        if (e.handle == exclude_a || e.handle == exclude_b) return false;
        if (e.ground_target.valid() &&
            (e.ground_target == exclude_a || e.ground_target == exclude_b))
            return false;
        CollisionTargetView view;
        std::vector<CollisionMatrix> mats;
        const CollisionTargetView *tv = target_view(world, e.handle, view, mats);
        if (tv == nullptr) return false;
        if (abs32(ray.mid[0] - tv->pos[0]) >
                    tv->bound_radius + static_cast<int32_t>(ray.half[0]) ||
            abs32(ray.mid[1] - tv->pos[1]) >
                    tv->bound_radius + static_cast<int32_t>(ray.half[1]) ||
            abs32(ray.mid[2] - tv->pos[2]) >
                    tv->bound_radius + static_cast<int32_t>(ray.half[2]))
            return false;
        if (ray_line_distance(ray.start, ray.dir, tv->pos) > tv->bound_radius) return false;
        CollisionRay probe = ray; // model clip clips end in place; keep the walk ray whole
        return collision_raycast_model(*tv, probe);
    };

    bool blocked = false;
    world.registry.for_each([&](const Entity &e) { // pass 1: pool-2 statics
        if (blocked || e.handle.pool() != 2) return;
        if (blocked_by(e)) blocked = true;
    });
    if (blocked) return false;
    world.registry.for_each([&](const Entity &e) { // pass 2: dynamics (Item kind off pool 2)
        if (blocked || e.handle.pool() == 2 || e.kind != EntityKind::Item) return;
        if (blocked_by(e)) blocked = true;
    });
    return !blocked;
}

bool CollisionWorld::sound_los_clear(World &world, EntityHandle listener, EntityHandle source,
                                     const int32_t start_in[3], const int32_t end_in[3],
                                     int32_t height_offset) {
    const Entity *le = listener.valid() ? world.registry.get(listener) : nullptr;
    const Entity *se = source.valid() ? world.registry.get(source) : nullptr;

    // --- Terrain leg. [orig: Physics_CheckTerrainLineOfSight @ 0x53b080] ---
    bool terrain_clear = false;
    if (le != nullptr && se != nullptr && (le->flags & kEntityFlagIndoors) != 0 &&
        (se->flags & kEntityFlagIndoors) != 0) {
        terrain_clear = true; // both indoors: no heightfield test [orig: @ 0x53b0a0]
    }
    if (!terrain_clear && (terrain == nullptr || !terrain->valid())) {
        terrain_clear = true; // hostless terrain: nothing to block (host seam)
    }
    if (!terrain_clear) {
        if (le == nullptr || se == nullptr) {
            // No-entity path: both UNSHIFTED endpoints must sit above the
            // bilinear surface for the ray to run at all — an under-surface
            // endpoint reads as terrain-clear. [orig: @ 0x53b0f1-0x53b100]
            const terrain::TerrainRaycastSample hs =
                    los_field_sample(*terrain, start_in[0], start_in[1], true);
            const terrain::TerrainRaycastSample he =
                    los_field_sample(*terrain, end_in[0], end_in[1], true);
            if (hs.height_1616 > start_in[2] || he.height_1616 > end_in[2]) {
                terrain_clear = true;
            }
        }
        if (!terrain_clear) {
            // z -= heightOffset on both endpoints (ray 2's -0x8000 raises the
            // segment 0.5u). [orig: @ 0x53b0b2-0x53b0c4 / @ 0x53b106-0x53b118]
            const int32_t s[3] = {start_in[0], start_in[1], start_in[2] - height_offset};
            const int32_t e[3] = {end_in[0], end_in[1], end_in[2] - height_offset};
            if (los_terrain_blocked(*terrain, s, e)) return false;
        }
    }

    // --- Entity leg: building-kind candidates from the LISTENER's slice.
    // [orig: raycast_find_collision_entity @ 0x539a70 with allowAllTypes = 0 —
    // the def-type-5 filter @ 0x539baf; walker raycast_against_entity_pool
    // @ 0x538720.] The segment is the UNSHIFTED one (the z shift was
    // terrain-leg-internal); the height offset instead reaches the walker as
    // its clip RADIUS (the arg-slot reuse witnessed at the @ 0x53b166 push),
    // so ray 2 clips 0.5u thin — see sound_segment_blocked.
    CollisionRay ray;
    ray.start[0] = start_in[0];
    ray.start[1] = start_in[1];
    ray.start[2] = start_in[2];
    ray.end[0] = end_in[0];
    ray.end[1] = end_in[1];
    ray.end[2] = end_in[2];
    ray.refresh();
    const int32_t clip_radius = height_offset; // [orig: the arg-5 reuse @ 0x53b167]
    const int32_t broad_r = clip_radius > 0 ? clip_radius : 0;

    auto it = candidates_.find(listener.packed);
    if (it != candidates_.end()) {
        const CandidateSlice slice = it->second;
        for (int32_t i = 0; i < slice.count; ++i) {
            const EntityHandle ch = arena_[slice.start + i];
            if (ch == listener || (source.valid() && ch == source)) continue;
            const Entity *ce = world.registry.get(ch);
            if (ce == nullptr) continue;
            if ((ce->flags & 1u) != 0) continue; // [orig: @ 0x538792]
            // Candidates standing on the listener/source are excluded (one
            // level). [orig: the +0x28 groundEntity checks @ 0x538843-0x538877]
            if (ce->ground_target == listener) continue;
            if (source.valid() && ce->ground_target == source) continue;

            CollisionTargetView view;
            std::vector<CollisionMatrix> mats;
            const CollisionTargetView *tv = target_view(world, ch, view, mats);
            if (tv == nullptr) continue;
            if (!tv->is_building) continue; // [orig: the itemDef+92 == 5 gate @ 0x539baf]
            // Broad phase vs the ray box + line distance, radius-padded.
            // [orig: @ 0x5387c4-0x5389a4]
            if (abs32(ray.mid[1] - tv->pos[1]) >
                    tv->bound_radius + broad_r + static_cast<int32_t>(ray.half[1]) ||
                abs32(ray.mid[0] - tv->pos[0]) >
                    tv->bound_radius + broad_r + static_cast<int32_t>(ray.half[0]) ||
                abs32(ray.mid[2] - tv->pos[2]) >
                    tv->bound_radius + broad_r + static_cast<int32_t>(ray.half[2]))
                continue;
            if (ray_line_distance(ray.start, ray.dir, tv->pos) > tv->bound_radius + broad_r)
                continue;
            if (sound_segment_blocked(*tv, ray, clip_radius)) return false; // blocked
        }
    }
    return true;
}

bool CollisionWorld::segment_hits_static(World &world, const int32_t a[3], const int32_t b[3],
                                         int32_t radius) {
    // [orig: raycast_find_collision_entity @ 0x539a70 with allowAllTypes = 1
    // (the iris sun-ray caller @ 0x5c7784) -> raycast_against_entity_pool
    // @ 0x538720 — the pool-2 statics walk with the same broad phase and
    // radiused segment clip the sound leg ports; no building-kind gate.]
    CollisionRay ray;
    ray.start[0] = a[0];
    ray.start[1] = a[1];
    ray.start[2] = a[2];
    ray.end[0] = b[0];
    ray.end[1] = b[1];
    ray.end[2] = b[2];
    ray.refresh();
    const int32_t broad_r = radius > 0 ? radius : 0; // [orig: @ 0x538752 clamp]

    for (int32_t i = 0; i < static_count_; ++i) {
        const StaticSlot &s = statics_[i];
        CollisionTargetView view;
        std::vector<CollisionMatrix> mats;
        const CollisionTargetView *tv = target_view(world, s.h, view, mats);
        if (tv == nullptr) continue;
        // Broad phase vs the ray box + line distance, radius-padded.
        // [orig: @ 0x5387c4-0x5389a4]
        if (abs32(ray.mid[1] - tv->pos[1]) >
                tv->bound_radius + broad_r + static_cast<int32_t>(ray.half[1]) ||
            abs32(ray.mid[0] - tv->pos[0]) >
                tv->bound_radius + broad_r + static_cast<int32_t>(ray.half[0]) ||
            abs32(ray.mid[2] - tv->pos[2]) >
                tv->bound_radius + broad_r + static_cast<int32_t>(ray.half[2]))
            continue;
        if (ray_line_distance(ray.start, ray.dir, tv->pos) > tv->bound_radius + broad_r)
            continue;
        if (sound_segment_blocked(*tv, ray, radius)) return true;
    }
    return false;
}

int32_t CollisionWorld::sound_occlusion_inflate(World &world, EntityHandle listener,
                                                EntityHandle source,
                                                const int32_t listener_pos[3],
                                                const int32_t source_pos[3],
                                                int32_t distance_q16) {
    // [orig: Sound_ApplyOcclusionDistance @ 0x529970]
    int32_t base = static_cast<int32_t>(static_cast<uint32_t>(distance_q16) >> 3);
    if (base > 0xA0000) base = 0xA0000; // min(d/8, 10u) [orig: @ 0x529982]
    // Source z lifted +0x2000 for BOTH rays. [orig: @ 0x52998d / restore @ 0x5299d7]
    const int32_t end[3] = {source_pos[0], source_pos[1], source_pos[2] + 0x2000};
    if (!sound_los_clear(world, listener, source, listener_pos, end, 0))
        base = 2 * base + 0x50000; // ray 1 blocked compounds [orig: @ 0x5299b6]
    const bool ray2_clear = sound_los_clear(world, listener, source, listener_pos, end, -0x8000);
    // The single final add. [orig: @ 0x5299e6-0x5299f3]
    return distance_q16 + (ray2_clear ? base : 2 * base + 0x50000);
}

// See collision.h — the vehicle hull contact. [orig: Entity_CheckCollisionState
// @ 0x462a30, the entity-collision half; the per-wheel terrain half rides the
// motor's terrain column (D-NET-161).]
int32_t CollisionWorld::resolve_vehicle_hull(World &world, EntityHandle source,
                                             const int32_t pos[3], const int32_t prev_pos[3],
                                             int32_t out_force[2]) {
    out_force[0] = 0;
    out_force[1] = 0;
    auto it = candidates_.find(source.packed);
    if (it == candidates_.end()) return 0;
    const CandidateSlice slice = it->second;
    if (slice.count <= 0) return 0;

    const Entity *ent = world.registry.get(source);

    // The hull-center test point: +1.5 u lift (mid-hull, so a wall's bottom face
    // is never the cheapest SAT exit), radius 1.5 u — the wheel-point array and
    // per-wheel radii ride the unported wheel solver (D-NET-161).
    CollisionPoint point{pos[0], pos[1], pos[2] + 0x18000, 0};
    int32_t radius = 0x18000;

    ContactQuery q;
    q.points = &point;
    q.radii = &radius;
    q.num_points = 1;
    q.prev_pos[0] = prev_pos[0];
    q.prev_pos[1] = prev_pos[1];
    q.prev_pos[2] = prev_pos[2] + 0x18000;
    q.source_bound_radius = radius;
    // The vehicle contact mask is 8: use the VC/type-7 run when present, otherwise
    // fall back to ordinary CB/default solids. It is 24 when the def
    // attrib2 low byte has bit 7 set, adding type-12 volumes [orig: @ 0x462a91-
    // 0x462a9f — collisionMask = 8; attrib2 sign byte -> 24]. attrib2 is not
    // fed to the sim yet, so the 24 leg is a tracked residual (D-NET-161).
    q.mask = 8;
    q.query_is_player = false;

    BlinkAccum blink;       // vehicles accumulate no blink state
    LadderContact ladder;   // nor ladder contact frames
    int32_t severity = 0;

    for (int32_t i = 0; i < slice.count; ++i) {
        const EntityHandle ch = arena_[slice.start + i];
        if (ch == source) continue;
        // Skip the carrier chain like the original's groundEntity walk
        // [orig: @ 0x462e3d-0x462e4f].
        if (ent != nullptr && ent->ground_target == ch) continue;
        CollisionTargetView view;
        std::vector<CollisionMatrix> mats;
        const CollisionTargetView *tv = target_view(world, ch, view, mats);
        if (tv == nullptr) continue;
        ContactResult res;
        if (!collision_contact_force(*tv, q, blink, ladder, res)) continue;
        // Verticality split [orig: @ 0x462fc2-0x462fcb — |fz|<<22 / |force| vs the
        // caller's slope thresholds]: a wall-like (horizontal-dominant) push lands
        // in FULL at severity 3 [orig: @ 0x46322d-0x463240]; vertical-dominant
        // force is the ground's — dropped here, the terrain column owns it (the
        // graded ¼/⅛ bands ride the wheel solver, D-NET-161).
        if (abs32(res.force[0]) + abs32(res.force[1]) < abs32(res.force[2])) continue;
        out_force[0] -= res.force[0];
        out_force[1] -= res.force[1];
        severity = 3;
    }
    return severity;
}

int32_t CollisionWorld::resolve_entity(World &world, EntityHandle source, ResolveState &state,
                                       int32_t pos[3], int32_t vel_xy[2], int32_t &vel_z,
                                       int32_t capsule_bottom, int32_t capsule_top,
                                       int32_t heading, int32_t body_pitch, bool is_player,
                                       bool is_authority, uint32_t tick, int32_t anim_state_id,
                                       uint32_t anim_state_flags, int16_t &health) {
    // [orig: movement collision resolver @ 0x4b2bd0]
    (void)heading;    // consumed by retail's on-ladder 2-point variant (D-COL-5)
    (void)body_pitch;
    Entity *ent = world.registry.get(source);

    if (!state.prev_valid) {
        state.prev_pos[0] = pos[0];
        state.prev_pos[1] = pos[1];
        state.prev_pos[2] = pos[2];
        state.prev_valid = true;
    }

    // Idle skip-throttle. [orig: @ 0x4b2c3d-0x4b2cba — full update when the anim
    // state's table bit 0 is set, moving, sliding, displaced > 200, swimming
    // (Flags 0x2000), or every 64th tick; otherwise counter 0..10 full, 11..20
    // skip (revert the caller's gravity integration + zero vel_z).]
    bool full_update = false;
    if ((anim_state_flags & 1u) != 0) full_update = true; // [orig: @ 0x4b2c1e]
    if (vel_xy[0] != 0 || vel_xy[1] != 0) full_update = true;
    if (vel_z > 0 || vel_z < -420) full_update = true;
    if (abs32(pos[0] - state.prev_pos[0]) > 200 || abs32(pos[1] - state.prev_pos[1]) > 200)
        full_update = true;
    if (ent != nullptr && (ent->flags & 0x2000u) != 0) full_update = true; // [orig: @ 0x4b2ca6]
    if ((tick & 0x3Fu) == 0) full_update = true;
    if (!full_update) {
        if (state.skip_counter <= 10) {
            ++state.skip_counter;
        } else {
            if (++state.skip_counter > 20) state.skip_counter = 10;
            // Revert the caller's gravity displacement so a skipped tick nets
            // zero — PER MOTOR, matching each caller's integrate: x1 for the
            // player body (org2 `pos += vel`, -208/tick) and x2 for the NPC
            // (org1 `pos += 2*vel`, -416/tick). [orig: @ 0x4b2cd9-0x4b2ce9 —
            // `test ecx,100h` skips the doubling for Flags&0x100 PLAYER bodies;
            // the resolver's old "0x100=mounted" gloss was a kong misnomer (the
            // kill router @0x4fd160 and the AI target filters key players on
            // 0x100 — world-wac-ai-re.md §16/§20).] While the reimpl player ran
            // the pre-§22 2-tick `pos += 2*vel` cadence this was deliberately
            // x2-for-both; the §22 per-tick -208 + `pos += vel` port restores
            // the witnessed split — a mismatched undo here leaks per gravity
            // tick through the skip band (the standing rise-and-snap sawtooth).
            pos[2] -= is_player ? vel_z : 2 * vel_z;
            vel_z = 0;
            return 0; // [orig: skip path returns 0 @ 0x4b2cec]
        }
    } else {
        state.skip_counter = 0;
    }

    // Per-resolve blink/query state. [orig: the g_Blink* clears @ 0x4b2d54-0x4b2d7d]
    // Not modeled: the mounted/carried source gate (savedPosY force suppression),
    // the +0x2c aux latches, and the resolver's kill/sound/callback side effects —
    // docs/world/world-wac-ai-re.md (D-COL-8, D-COL-9); on-foot organics only today.
    BlinkAccum blink;
    const bool is_local = ent != nullptr && local_player.valid() && source == local_player;
    if (is_local) local_player_blink_flags = 0;
    if (ent != nullptr)
        ent->flags &= ~(kEntityFlagIndoors | kEntityFlagLadderContact | kEntityFlagArmoryZone |
                        kEntityFlagVehicleLoadoutZone);

    // Capsule test points. [orig: the not-on-ladder branch @ 0x4b2edb-0x4b2f2a —
    // 3 points: head (z + collisionRadius - halfRadius + 0.0625), eye (pos +
    // CameraOffset -> our head stand-in), feet; radii {collisionRadius, 0.3125,
    // outerRadius}. CameraOffset is not modeled: the eye point reuses the head
    // column (D-COL-4).]
    const int32_t half_radius = capsule_bottom >> 4;
    int32_t collision_radius =
        (capsule_bottom >> 4) + abs32(capsule_top - capsule_bottom) / 2;
    int32_t min_radius = 57344 - 2 * collision_radius;
    int32_t outer_radius = capsule_bottom >> 1;
    if (min_radius < 4096) min_radius = 4096;
    if (collision_radius < min_radius) {
        collision_radius = min_radius;
        outer_radius -= 4096;
    }
    if (outer_radius < 6144) outer_radius = 6144;
    if (collision_radius < 6144) collision_radius = 6144;

    CollisionPoint points[3];
    int32_t radii[3];
    const int32_t head_lift = collision_radius - half_radius + 4096;
    points[0] = {pos[0], pos[1], pos[2] + head_lift, 0};
    points[1] = {pos[0], pos[1], pos[2] + head_lift, 0}; // eye stand-in (D-COL-4)
    points[2] = {pos[0], pos[1], pos[2], 0};
    radii[0] = collision_radius;
    radii[1] = 20480;
    radii[2] = outer_radius;
    const int32_t num_points = 3;

    // Debug capture (local player, full resolves only): the untouched test
    // points — the pass-2 relaxation shifts `points` in place below.
    if (is_local) {
        local_resolve_debug.valid = true;
        for (int32_t i = 0; i < 3; ++i) {
            local_resolve_debug.points[i][0] = points[i].x;
            local_resolve_debug.points[i][1] = points[i].y;
            local_resolve_debug.points[i][2] = points[i].z;
            local_resolve_debug.radii[i] = radii[i];
        }
        local_resolve_debug.capsule_bottom = capsule_bottom;
        local_resolve_debug.capsule_top = capsule_top;
    }

    ContactQuery q;
    q.points = points;
    q.radii = radii;
    q.num_points = num_points;
    q.prev_pos[0] = state.prev_pos[0];
    q.prev_pos[1] = state.prev_pos[1];
    q.prev_pos[2] = state.prev_pos[2];
    q.source_bound_radius = 0x10000; // [orig: entity boundRadius] (D-COL-3)
    q.mask = static_cast<uint8_t>((is_player ? 2 : 0));
    q.query_is_player = is_player;

    int32_t total_force[3] = {0, 0, 0};
    LadderContact ladder;
    EntityHandle ladder_entity;

    auto it = candidates_.find(source.packed);
    if (it != candidates_.end()) {
        const CandidateSlice slice = it->second;
        for (int pass = 0; pass < 2; ++pass) {
            // [orig: first pass @ 0x4b2f54, second relaxation pass at shifted points
            // adds half the force @ 0x4b3549-0x4b36ec]
            int32_t pass_force[3] = {0, 0, 0};
            bool pass_contact = false;
            for (int32_t i = 0; i < slice.count; ++i) {
                const EntityHandle ch = arena_[slice.start + i];
                CollisionTargetView view;
                std::vector<CollisionMatrix> mats;
                const CollisionTargetView *tv = target_view(world, ch, view, mats);
                if (tv == nullptr) continue;
                if (ent != nullptr) {
                    CollisionTargetView &mut = view;
                    mut.is_ground_of_source = (ent->ground_target == ch);
                }
                ContactResult res;
                const bool contact = collision_contact_force(*tv, q, blink, ladder, res);
                if (pass == 0) {
                    // The contact-flag dispatch runs whether or not the query
                    // produced force — a pure ladder/zone touch still latches.
                    // [orig: the goto LABEL_67 on a zero return @ 0x4b2fa5]
                    if ((res.flags & 0x1u) != 0 && ladder.valid) ladder_entity = ch;
                    apply_touch_flags(ent, res.flags, health, is_authority);
                }
                if (!contact) continue;
                if (pass == 0) {
                    // Down-force suppression: a mostly-vertical negative force is
                    // dropped (standing pressure, not a wall). [orig: @ 0x4b3010]
                    int32_t f[3] = {res.force[0], res.force[1], res.force[2]};
                    if (f[2] < 0) {
                        if (abs32(f[0]) + abs32(f[1]) < abs32(f[2])) {
                            f[0] = 0;
                            f[1] = 0;
                        }
                        f[2] = 0;
                    }
                    pass_force[0] -= f[0];
                    pass_force[1] -= f[1];
                    pass_force[2] -= f[2];
                    pass_contact = true;
                } else {
                    int32_t f[3] = {res.force[0], res.force[1], res.force[2]};
                    if (f[2] < 0) {
                        if (abs32(f[0]) + abs32(f[1]) < abs32(f[2])) {
                            f[0] = 0;
                            f[1] = 0;
                        }
                        f[2] = 0;
                    }
                    pass_force[0] -= f[0];
                    pass_force[1] -= f[1];
                    pass_contact = true;
                }
            }
            if (pass == 0) {
                total_force[0] += pass_force[0];
                total_force[1] += pass_force[1];
                total_force[2] += pass_force[2];
                if (!pass_contact || (total_force[0] == 0 && total_force[1] == 0 &&
                                      total_force[2] == 0))
                    break;
                // Shift the test points by the accumulated force for the second pass.
                for (int32_t pi = 0; pi < num_points; ++pi) {
                    points[pi].x += total_force[0];
                    points[pi].y += total_force[1];
                }
            } else if (pass_contact && !ladder_entity.valid()) {
                // [orig: @ 0x4b36da — second-pass half force only without a CL contact]
                total_force[0] += pass_force[0] >> 1; // [orig: @ 0x4b36e2]
                total_force[1] += pass_force[1] >> 1;
            }
        }
    }

    // Apply the push-out; a net push resets the idle skip counter. [orig:
    // @ 0x4b3746-0x4b375a; pad_370[3] = 0 @ 0x4b3773]
    if (total_force[0] != 0 || total_force[1] != 0 || total_force[2] != 0)
        state.skip_counter = 0;
    pos[0] += total_force[0];
    pos[1] += total_force[1];
    pos[2] += total_force[2];

    // Low-level CL bookkeeping only. Retail's full climb state transitions,
    // alignment chase, root motion, and top exit remain D-COL-5.
    // [orig: @ 0x4b3291-0x4b3297 — Flags |= 0x100000 + groundEntity = ladder]
    if (ladder_entity.valid() && ent != nullptr) {
        ent->flags |= kEntityFlagLadderContact;
        ent->ground_target = ladder_entity;
    }

    // Blink apply. [orig: @ 0x4b34c2-0x4b3502 — bit 2 -> Flags 0x800000; local
    // player accumulates the flags word.]
    if (ent != nullptr) {
        ent->blink_hits[0] = blink.hits[0];
        ent->blink_hits[1] = blink.hits[1];
        ent->blink_hits[2] = blink.hits[2];
        ent->blink_hits[3] = blink.hits[3];
        if (is_local) local_player_blink_flags |= blink.flags;
        if ((blink.flags & kBlinkIndoorsBit) != 0) ent->flags |= kEntityFlagIndoors;
    }

    // Inter-entity sphere repulsion (no model contact only). [orig: @ 0x4b3a5c-0x4b3c52 —
    // threshold 30% of summed radii, push (thr - dist)/4 along the atan2 direction
    // via the quantized table with the (0x200000 - bam) index.]
    const bool had_model_contact =
        total_force[0] != 0 || total_force[1] != 0 || total_force[2] != 0;
    // [orig: @ 0x4b3a77-0x4b3aa1 — the dragger/carry anim states skip repulsion]
    const bool repulse_exempt_state = anim_state_id == 27 || anim_state_id == 137 ||
                                      anim_state_id == 138 || anim_state_id == 139;
    // [orig: @ 0x4b3aac — Flags 0x43 (dead/hidden/carried) skips repulsion;
    // @ 0x4b3aba — Flags 0x20 widens the radius by 2.0u]
    const bool repulse_exempt_flags = ent != nullptr && (ent->flags & 0x43u) != 0;
    if (!had_model_contact && !repulse_exempt_state && !repulse_exempt_flags) {
        int32_t my_radius = 0x10000; // [orig: entity boundRadius] (D-COL-3)
        if (ent != nullptr && (ent->flags & 0x20u) != 0) my_radius += 0x20000;
        for (const PersonSlot &p : persons_) {
            if (p.h == source) continue;
            const int32_t threshold = 30 * (my_radius + p.radius) / 100;
            // Snapshot positions for the coarse reject... [orig: the g_PersonProx*
            // table reads @ 0x4b3b07-0x4b3b74]
            const int32_t ddx = p.x - pos[0];
            const int32_t ddy = p.y - pos[1];
            const int32_t ddz = p.z - pos[2];
            if (abs32(ddx) > threshold || abs32(ddy) > threshold || abs32(ddz) > threshold)
                continue;
            if (vec_len_ftol(ddx, ddy, 0) > threshold) continue;
            // ...then the LIVE entity for the second distance and the push, and
            // the dead/hidden peer skip. [orig: g_PersonProxEntity re-read
            // @ 0x4b3b7a-0x4b3bca, the +36 & 2 skip @ 0x4b3b8d]
            const Entity *peer = world.registry.get(p.h);
            if (peer == nullptr || (peer->flags & 2u) != 0) continue;
            int32_t live[3];
            entity_pos_fixed(*peer, live);
            const int32_t dist = vec_len_ftol(pos[0] - live[0], pos[1] - live[1], 0);
            if (dist > threshold) continue;
            const int32_t amount = (threshold - dist) >> 2;
            const int32_t ang = static_cast<int32_t>(
                std::atan2(static_cast<double>(pos[1] - live[1]),
                           static_cast<double>(pos[0] - live[0])) *
                kBamPerRadian);
            const uint32_t idx = (0x200000u - static_cast<uint32_t>(ang)) >> 22;
            const DirTable &t = dir_table();
            const int32_t sn = t.sin22[idx & 1023];
            const int32_t cs = t.sin22[(idx & 1023) + 256];
            pos[0] += static_cast<int32_t>((static_cast<int64_t>(amount) * cs) >> 22);
            pos[1] += static_cast<int32_t>((static_cast<int64_t>(amount) * sn) >> 22);
        }
    }

    // Ground settle tail. [orig: @ 0x4b3d6e-0x4b3da9 — quantize Z up to the 6144
    // grid, probe down 2.0u through terrain + candidates, restore Z, return feet
    // clearance; the probe stores the hit entity into groundEntity.]
    const int32_t saved_z = pos[2];
    const int32_t feet_z = pos[2] - capsule_bottom;
    pos[2] = (pos[2] + 6143) & ~0x17FF;
    EntityHandle ground_hit;
    const int32_t ground =
        raycast_ground(world, source, pos, 0, 0, 0, 0x20000, &ground_hit);
    pos[2] = saved_z;
    // The probe's hit ALWAYS lands in groundEntity — null on a miss, overwriting
    // even a same-resolve CL latch. Generic ground is still resolved only by
    // terrain or a type-1 CB solid.
    // [orig: the unconditional +0x28 store in
    // Entity_RaycastGroundHeightAndObject @ 0x414370]
    if (ent != nullptr) ent->ground_target = ground_hit;

    state.prev_pos[0] = pos[0];
    state.prev_pos[1] = pos[1];
    state.prev_pos[2] = pos[2];
    if (is_local) {
        local_resolve_debug.pos[0] = pos[0];
        local_resolve_debug.pos[1] = pos[1];
        local_resolve_debug.pos[2] = pos[2];
        local_resolve_debug.foot_clearance = feet_z - ground;
    }
    return feet_z - ground;
}

std::vector<CollisionWorld::DebugInstance> CollisionWorld::debug_instances(
    World &world, const int32_t anchor[3], int32_t range, int32_t max_instances) const {
    std::vector<DebugInstance> out;
    for (const auto &kv : instances_) {
        if (max_instances > 0 && static_cast<int32_t>(out.size()) >= max_instances) break;
        EntityHandle h;
        h.packed = kv.first;
        CollisionTargetView view;
        std::vector<CollisionMatrix> mats;
        // The SAME per-instance view every query goes through: entity pose ->
        // collision_matrix_from_heading (quantized dir table) per section.
        const CollisionTargetView *tv = target_view(world, h, view, mats);
        if (tv == nullptr) continue;
        if (range > 0 && anchor != nullptr &&
            (abs32(tv->pos[0] - anchor[0]) > range || abs32(tv->pos[1] - anchor[1]) > range ||
             abs32(tv->pos[2] - anchor[2]) > range))
            continue;

        DebugInstance inst;
        inst.handle = h;
        inst.pos[0] = tv->pos[0];
        inst.pos[1] = tv->pos[1];
        inst.pos[2] = tv->pos[2];
        if (const Entity *e = world.registry.get(h))
            inst.heading_bam = bam_heading_from_mission_yaw_deg(static_cast<double>(e->yaw));

        const CollisionModel &m = *tv->model;
        for (size_t si = 0; si < m.sections.size(); ++si) {
            const CollisionSection &sec = m.sections[si];
            const CollisionMatrix &mat = tv->matrices[si];
            if (sec.volume_count == 0 || mat.disabled()) continue; // mirrors the query skip
            for (int32_t vi = 0; vi < sec.volume_count; ++vi) {
                const CollisionVolume &vol = m.volumes[sec.volume_start + vi];
                DebugVolume dv;
                dv.type = vol.type;
                dv.flags = vol.flags;
                dv.min[0] = vol.min_x; dv.max[0] = vol.max_x;
                dv.min[1] = vol.min_y; dv.max[1] = vol.max_y;
                dv.min[2] = vol.min_z; dv.max[2] = vol.max_z;
                for (int c = 0; c < 8; ++c) {
                    const int32_t local[3] = {(c & 1) ? vol.max_x : vol.min_x,
                                              (c & 2) ? vol.max_y : vol.min_y,
                                              (c & 4) ? vol.max_z : vol.min_z};
                    mat.transform_point(local, dv.corners[c]);
                }
                inst.volumes.push_back(dv);
            }
        }
        if (!inst.volumes.empty()) out.push_back(std::move(inst));
    }
    return out;
}

std::vector<CollisionWorld::DebugHitboxEntity> CollisionWorld::debug_hitboxes(
    World &world, const int32_t anchor[3], int32_t range, int32_t max_entities,
    int32_t max_faces) const {
    std::vector<DebugHitboxEntity> out;
    int32_t face_budget = max_faces > 0 ? max_faces : INT32_MAX;
    for (const auto &kv : instances_) {
        if (max_entities > 0 && static_cast<int32_t>(out.size()) >= max_entities) break;
        EntityHandle h;
        h.packed = kv.first;
        // Persons have their own posed sphere query. Range-reject every other
        // instance from the same fixed entity position target_view uses before
        // asking the animation provider for section matrices.
        const Entity *world_entity = world.registry.get(h);
        if (world_entity == nullptr || world_entity->kind == EntityKind::Organic)
            continue;
        if (range > 0 && anchor != nullptr) {
            int32_t entity_pos[3];
            entity_pos_fixed(*world_entity, entity_pos);
            if (abs32(entity_pos[0] - anchor[0]) > range ||
                abs32(entity_pos[1] - anchor[1]) > range ||
                abs32(entity_pos[2] - anchor[2]) > range)
                continue;
        }
        CollisionTargetView view;
        std::vector<CollisionMatrix> mats;
        // The SAME husk-aware view + full-euler placement matrices the
        // projectile raycast walks — the drawn mesh IS the tested mesh.
        const CollisionTargetView *tv = target_view(world, h, view, mats);
        if (tv == nullptr) continue;

        DebugHitboxEntity ent;
        ent.handle = h;
        ent.pos[0] = tv->pos[0];
        ent.pos[1] = tv->pos[1];
        ent.pos[2] = tv->pos[2];
        ent.bound_radius = tv->bound_radius;
        if (const Entity *e = world.registry.get(h)) {
            ent.husk = (e->engine_flags & 0x4u) != 0 &&
                       instances_.at(h.packed).husk_model_id >= 0;
            if (e->bound_radius > 0.0f) ent.bound_radius = to_fixed(e->bound_radius);
        }

        const CollisionModel &m = *tv->model;
        ent.has_faces = !m.faces.empty();
        for (size_t si = 0; si < m.sections.size() && face_budget > 0; ++si) {
            const CollisionSection &sec = m.sections[si];
            if (sec.face_count <= 0) continue;
            const CollisionMatrix &mat = tv->matrices[si];
            if (mat.disabled()) continue; // mirrors the raycast skip
            const CollisionFaceVertex *verts =
                m.face_vertices.data() + sec.face_vertex_start;
            ent.face_total += sec.face_count;
            for (int32_t fi = 0; fi < sec.face_count && face_budget > 0; ++fi) {
                const CollisionFace &face = m.faces[sec.face_start + fi];
                DebugHitboxFace df;
                df.material = face.material;
                df.flags = face.flags;
                for (int k = 0; k < 3; ++k) {
                    const CollisionFaceVertex &vt = verts[face.v[k]];
                    // Q8 int16 -> 16.16 (<< 8), then the section world matrix —
                    // the raycast's own vertex scale and transform.
                    const int32_t local[3] = {static_cast<int32_t>(vt.x) << 8,
                                              static_cast<int32_t>(vt.y) << 8,
                                              static_cast<int32_t>(vt.z) << 8};
                    mat.transform_point(local, df.v[k]);
                }
                ent.faces.push_back(df);
                --face_budget;
            }
        }
        out.push_back(std::move(ent));
    }
    return out;
}

std::vector<CollisionWorld::DebugPersonSection>
CollisionWorld::debug_person_sections(World &world, const int32_t anchor[3],
                                      int32_t range, int32_t max_entities) {
    std::vector<DebugPersonSection> out;
    if (max_entities <= 0) return out;
    int32_t entity_count = 0;
    world.registry.for_each([&](const Entity &entity) {
        if (entity_count >= max_entities || entity.kind != EntityKind::Organic ||
            (entity.engine_flags & 0x02000001u) != 0)
            return;
        int32_t entity_pos[3];
        entity_pos_fixed(entity, entity_pos);
        if (range >= 0 &&
            (abs32(entity_pos[0] - anchor[0]) > range ||
             abs32(entity_pos[1] - anchor[1]) > range ||
             abs32(entity_pos[2] - anchor[2]) > range))
            return;

        // Late-spawned players enter pool 0 after the mission-start graphic
        // sweep. Resolve them through the same host hook RoundSim uses before
        // deciding whether an authored person model exists.
        ensure_entity_instance(world, entity.handle);

        CollisionTargetView view;
        std::vector<CollisionMatrix> matrices;
        const CollisionTargetView *target =
                target_view(world, entity.handle, view, matrices);
        if (target == nullptr || target->model == nullptr) return;
        bool any = false;
        for (int32_t si = 0;
             si < static_cast<int32_t>(target->model->sections.size()); ++si) {
            const CollisionSection &section = target->model->sections[si];
            if (section.radius <= 0) continue;
            DebugPersonSection debug;
            debug.handle = entity.handle;
            debug.section = si;
            debug.authored_radius = section.radius;
            debug.radius = person_effective_radius(si, section.radius, 0);
            debug.masked =
                    (entity.section_mask &
                     (1u << (static_cast<uint32_t>(si) & 31u))) != 0;
            target->matrices[si].transform_point(section.center, debug.center);
            out.push_back(debug);
            any = true;
        }
        if (any) ++entity_count;
    });
    return out;
}

// Contact-flag side effects shared by both passes. [orig: the flag dispatch inside
// the resolver loop @ 0x4b30b7-0x4b351e]
void CollisionWorld::apply_touch_flags(Entity *ent, uint32_t flags, int16_t &health,
                                       bool is_authority) {
    if (ent == nullptr || flags == 0) return;
    // DH/DM/DL contact damage is authority-only AND gated off for
    // EngineFlags 0x4000000 entities.
    // [orig: the is_authority + (Flags & 0x4000000) == 0 wrap @ 0x4b3139-0x4b3148]
    if (is_authority && (ent->engine_flags & 0x4000000u) == 0) {
        // Damage low/medium/high. [orig: @ 0x4b317b-0x4b31d7 — -1 / -6 / -50 HP]
        if ((flags & 0x40u) != 0 && health > 0) health = static_cast<int16_t>(health - 1);
        if ((flags & 0x80u) != 0 && health > 0) health = static_cast<int16_t>(health - 6);
        if ((flags & 0x100u) != 0 && health > 0) health = static_cast<int16_t>(health - 50);
        // CT/change-team touch (0x200) feeds the retail capture/team-change request
        // callback (`Server_OnPlayerTouchCaptureZone @ 0x500ba0`). Ours still rides
        // the zone system's independent proximity path (D-COL-6).
    }
    if ((flags & 0x4u) != 0) ent->flags |= kEntityFlagArmoryZone; // type 6 [orig: @ 0x4b34a0]
    if ((flags & 0x400u) != 0)
        ent->flags |= kEntityFlagVehicleLoadoutZone; // type 11 [orig: @ 0x4b34ae]
}

} // namespace opennova::world
