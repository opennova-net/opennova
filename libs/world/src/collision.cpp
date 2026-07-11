// World-object collision queries + the per-tick proximity/blink machinery.
// Faithful structural translations from Jointops.exe (imagebase 0x400000);
// witness record docs/world/world-wac-ai-re.md §15. See collision.h for the
// per-function [orig] map.

#include "world/collision.h"

#include <cmath>
#include <cstdlib>

#include <io/bam.h>
#include <terrain/height_field.h>

#include "world/angle.h"
#include "world/dir_table.h"
#include "world/world.h"

namespace opennova::world {

namespace {

inline int32_t abs32(int32_t v) { return opennova::io::bam_abs(v); }

// [orig: dbl_7C19D8 = 2^31/pi — BAM per radian]
constexpr double kBamPerRadian = 683565275.5764316;
// [orig: dbl_7C57B8 = -2^31/pi — the NEGATED BAM-per-radian the platform-anchor
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

} // namespace

// ----------------------------------------------------------------------------
// Model finalize: derive the per-section AABB + bound sphere from its volumes.
// [orig: precomputed on the runtime COBJ records by the model loader; the queries
// read them at +68..+104.]
// ----------------------------------------------------------------------------
void CollisionModel::finalize_sections() {
    for (CollisionSection &s : sections) {
        if (s.volume_count <= 0) {
            s.min_x = s.max_x = s.min_y = s.max_y = s.min_z = s.max_z = 0;
            s.center[0] = s.center[1] = s.center[2] = 0;
            s.radius = 0;
            continue;
        }
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
        const int32_t hx = (s.max_x - s.min_x) >> 1;
        const int32_t hy = (s.max_y - s.min_y) >> 1;
        const int32_t hz = (s.max_z - s.min_z) >> 1;
        s.center[0] = s.min_x + hx;
        s.center[1] = s.min_y + hy;
        s.center[2] = s.min_z + hz;
        s.radius = vec_len_ftol(hx, hy, hz);
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
        mat.invert_into(inv);
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

bool collision_raycast_model(const CollisionTargetView &target, CollisionRay &ray) {
    if (target.model == nullptr || target.matrices == nullptr) return false;
    const CollisionModel &model = *target.model;
    bool hit_found = false;

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
        }

        if (clipped_any) {
            mat.transform_point(lend, ray.end); // [orig: @ 0x4136ac]
            hit_found = true;
        }
    }

    if (hit_found) ray.refresh_bounds(); // [orig: @ 0x41370c mid/half recompute — dir untouched]
    return hit_found;
}

// ----------------------------------------------------------------------------
// Contact force. [orig: Entity_ComputeBoneCollisionForce @ 0x4ae150]
// ----------------------------------------------------------------------------
bool collision_contact_force(const CollisionTargetView &target, const ContactQuery &q,
                             BlinkAccum &blink, PlatformContact &platform, ContactResult &out) {
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
    int32_t damage_volume_counter = -1; // [orig: damageVolumeCount — type-8 counter]

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
        const int32_t section_entry_counter = damage_volume_counter;

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

            damage_volume_counter = section_entry_counter;
            // Damage pass start index. [orig: @ 0x4ae4b8-0x4ae4df]
            const bool damage_pass = (q.mask & 8) != 0;
            int32_t vi = 0;
            bool damage_scoped = false;
            if (damage_pass && sec.damage_start != -1) {
                vi = sec.damage_start;
                damage_scoped = true;
            }

            for (; vi < sec.volume_count; ++vi) {
                const CollisionVolume &vol = model.volumes[sec.volume_start + vi];
                const int32_t type = vol.type;
                if (damage_scoped && type != 7 && type != 12) continue; // [orig: @ 0x4ae52f]
                if (type == 19) {
                    if ((q.mask & 2) == 0) continue; // [orig: @ 0x4ae543 player-only]
                } else if (type == 7) {
                    if (!damage_pass) continue; // [orig: @ 0x4ae558]
                } else if (type == 12) {
                    if ((q.mask & 0x10) == 0) continue; // [orig: @ 0x4ae568]
                } else if (type == 8) {
                    ++damage_volume_counter; // [orig: @ 0x4ae575]
                }

                const bool platform_mode = (q.mask & 1) != 0;
                int32_t min_pen = INT32_MIN / 2;    // [orig: -1073741824]
                int32_t second_pen = INT32_MIN / 2;
                int32_t best_plane = 0, second_plane = 0;
                int32_t plane_idx = 0;
                bool reached_inside = false;

                if (platform_mode && type == 4) {
                    // Seat/platform volume: inflated AABB + current-point plane test.
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
                    case 4: { // platform/seat anchor [orig: @ 0x4ae894-0x4aea30]
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
                        platform.anchor[0] = anchor_xy[0] + target.pos[0];
                        platform.anchor[1] = anchor_xy[1] + target.pos[1];
                        platform.anchor[2] = anchor_z[2] + target.pos[2];
                        if (vol.plane_count > 0) {
                            const CollisionPlane &p0 = model.planes[vol.plane_start];
                            // Yaw/pitch are TARGET-RELATIVE: entity Yaw/Pitch minus
                            // atan2 * -BAM (net +). The XY length is ftol'd to int
                            // before the pitch atan2. [orig: @ 0x4ae938-0x4ae9d9,
                            // dbl_7C57B8 = -2^31/pi]
                            platform.yaw =
                                target.yaw_bam -
                                static_cast<int32_t>(std::atan2(-static_cast<double>(p0.ny),
                                                                -static_cast<double>(p0.nx)) *
                                                     kNegBamPerRadian);
                            const int32_t lxy_int = sqrt_ftol(
                                static_cast<double>(p0.nx) * p0.nx +
                                static_cast<double>(p0.ny) * p0.ny);
                            platform.pitch =
                                target.pitch_bam -
                                static_cast<int32_t>(std::atan2(static_cast<double>(p0.nz),
                                                                static_cast<double>(lxy_int)) *
                                                     kNegBamPerRadian);
                        } else {
                            // The original reads plane[0] unguarded even for a
                            // 0-plane volume (adjacent-memory read); a bounds
                            // guard is required here, defaults target-relative.
                            platform.yaw = target.yaw_bam;
                            platform.pitch = target.pitch_bam;
                        }
                        // Pull the anchor 0.375u back along the platform yaw — REAL
                        // sin/cos of the BAM angle scaled 2^22 and truncated, not the
                        // quantized table. [orig: @ 0x4ae9df-0x4aea30 — fsin/fcos of
                        // yaw * dbl_7C3608, * dbl_7C3600]
                        const double yaw_rad = static_cast<double>(platform.yaw) * kRadianPerBam;
                        const int32_t s22 = static_cast<int32_t>(std::sin(yaw_rad) * 4194304.0);
                        const int32_t c22 = static_cast<int32_t>(std::cos(yaw_rad) * 4194304.0);
                        platform.anchor[0] -= static_cast<int32_t>((24576LL * c22) >> 22);
                        platform.anchor[1] -= static_cast<int32_t>((24576LL * s22) >> 22);
                        platform.valid = true;
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
                                    damage_volume_counter < 16)
                                    blink.add_hit(static_cast<int32_t>(si), target.pool_index);
                            }
                        }
                        break;
                    case 9: // destructible-section touch [orig: @ 0x4aeb0f-0x4aeb22]
                        out.flags |= 0x20u;
                        out.touched_sections |= 1u << (si & 31);
                        break;
                    case 16: out.flags |= 0x100u; break; // [orig: @ 0x4aeb39]
                    case 17: out.flags |= 0x80u; break;  // [orig: @ 0x4aeb50]
                    case 18: out.flags |= 0x40u; break;  // [orig: @ 0x4aeb67]
                    case 10: out.flags |= 0x200u; break; // [orig: @ 0x4aeb7b]
                    case 11: out.flags |= 0x400u; break; // [orig: @ 0x4aeb92]
                    case 13: // grounded-only touch [orig: @ 0x4aebb3]
                        if (target.is_ground_of_source) out.flags |= 0x800u;
                        break;
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

void CollisionWorld::assign_entity(EntityHandle h, int32_t model_id) {
    if (!h.valid() || model_id < 0 || model_id >= static_cast<int32_t>(models_.size())) return;
    instances_[h.packed] = Instance{model_id};
}

bool CollisionWorld::has_instance(EntityHandle h) const {
    return instances_.count(h.packed) != 0;
}

int32_t CollisionWorld::candidate_count(EntityHandle h) const {
    auto it = candidates_.find(h.packed);
    return it == candidates_.end() ? 0 : it->second.count;
}

namespace {

// Entity position in 16.16 engine units (registry entities store float mission units).
void entity_pos_fixed(const Entity &e, int32_t out[3]) {
    out[0] = to_fixed(e.position.x);
    out[1] = to_fixed(e.position.y);
    out[2] = to_fixed(e.position.z);
}

// The bound radius the proximity tables use. The item-traits sweep does not carry
// the model bound yet, so derive from the collision model when instanced
// (max |AABB corner|), else a 1-unit default. Tracked deviation (D-COL-3): the
// original reads entity+0 boundRadius stamped at model load.
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

} // namespace

void CollisionWorld::build_tick_tables(World &world) {
    // --- pool-2 statics: buildings first, then the rest. [orig: 0x4b9430] ---
    statics_.clear();
    static_building_count_ = 0;
    auto push_static = [&](const Entity &e) {
        // The original's count saturates at 1199 — the 1200th slot is written
        // but never counted, so 1199 is the effective cap. [orig: the
        // `count < 1199` post-increment gate @ 0x4b94cb / 0x4b955f]
        if (statics_.size() >= 1199) return;
        auto it = instances_.find(e.handle.packed);
        if (it == instances_.end()) return; // no collision model -> not a collider
        StaticSlot s;
        int32_t p[3];
        entity_pos_fixed(e, p);
        const int32_t radius =
            entity_bound_radius(*this, model(it->second.model_id)) + 111876; // [orig: pad]
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
        if (it == instances_.end()) return;
        DynSlot d;
        int32_t pf[3];
        entity_pos_fixed(e, pf);
        d.x = pf[0]; d.y = pf[1]; d.z = pf[2];
        d.radius = entity_bound_radius(*this, model(it->second.model_id));
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
        const int32_t range = 0x10000 + pad; // bound radius + pad (D-COL-3)
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
}

const CollisionTargetView *CollisionWorld::target_view(World &world, EntityHandle h,
                                                       CollisionTargetView &scratch,
                                                       std::vector<CollisionMatrix> &mats) const {
    auto it = instances_.find(h.packed);
    if (it == instances_.end()) return nullptr;
    const Entity *e = world.registry.get(h);
    if (e == nullptr) return nullptr;
    const CollisionModel *m = model(it->second.model_id);
    if (m == nullptr || !m->valid()) return nullptr;

    int32_t p[3];
    entity_pos_fixed(*e, p);
    const int32_t heading = bam_heading_from_mission_yaw_deg(static_cast<double>(e->yaw));
    const CollisionMatrix world_mat = collision_matrix_from_heading(heading, p);
    mats.assign(m->sections.size(), world_mat); // yaw-only, shared per section (D-COL-1)

    scratch.model = m;
    scratch.matrices = mats.data();
    scratch.pos[0] = p[0];
    scratch.pos[1] = p[1];
    scratch.pos[2] = p[2];
    scratch.yaw_bam = heading;
    scratch.pitch_bam = 0; // statics carry no pitch; the vehicle pass fills it (D-COL-5)
    scratch.entity_flags = e->flags;
    scratch.bound_radius = entity_bound_radius(*this, m);
    scratch.is_building = (e->kind == EntityKind::Building);
    scratch.pool_index = h.slot();
    scratch.is_ground_of_source = false;
    return &scratch;
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

int32_t CollisionWorld::resolve_entity(World &world, EntityHandle source, ResolveState &state,
                                       int32_t pos[3], int32_t vel_xy[2], int32_t &vel_z,
                                       int32_t capsule_bottom, int32_t capsule_top,
                                       int32_t heading, int32_t body_pitch, bool is_player,
                                       bool is_authority, uint32_t tick, int32_t anim_state_id,
                                       uint32_t anim_state_flags, int16_t &health) {
    // [orig: Entity_ProcessCollisionAndPlatformPhysics @ 0x4b2bd0]
    (void)heading;    // consumed by the on-platform 2-point variant (D-COL-5)
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
            // zero. The original reverts x1 for players / x2 for NPCs
            // [orig: @ 0x4b2ce5-0x4b2ce9] because ITS player motor integrates
            // slideDecay x1 EVERY tick; OUR motor integrates pos += 2*vel for
            // both (the player on the 2-tick gravity cadence — infantry.cpp,
            // D-INF-10), so the exact undo is x2 for both. A x1 player revert
            // here leaks -vel per gravity tick through the skip band: the
            // idle sink-and-pop sawtooth.
            pos[2] -= 2 * vel_z;
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
        ent->flags &= ~(kEntityFlagIndoors | kEntityFlagOnPlatform | kEntityFlagArmoryZone |
                        kEntityFlagVehicleLoadoutZone);

    // Capsule test points. [orig: the not-on-platform branch @ 0x4b2edb-0x4b2f2a —
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
    PlatformContact platform;
    EntityHandle platform_entity;

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
                const bool contact = collision_contact_force(*tv, q, blink, platform, res);
                if (pass == 0) {
                    // The contact-flag dispatch runs whether or not the query
                    // produced force — a pure seat/zone touch still latches.
                    // [orig: the goto LABEL_67 on a zero return @ 0x4b2fa5]
                    if ((res.flags & 0x1u) != 0 && platform.valid) platform_entity = ch;
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
            } else if (pass_contact && !platform_entity.valid()) {
                // [orig: @ 0x4b36da — second-pass half force only when NOT on a platform]
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

    // Platform standing-on. [orig: @ 0x4b3291-0x4b3297 — Flags |= 0x100000 +
    // groundEntity = platform. The moving-deck carry (yaw chase + anchor follow)
    // rides the vehicle pass (D-COL-5).]
    if (platform_entity.valid() && ent != nullptr) {
        ent->flags |= kEntityFlagOnPlatform;
        ent->ground_target = platform_entity;
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
    // even a same-resolve platform latch (which normally re-hits the platform).
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

// Contact-flag side effects shared by both passes. [orig: the flag dispatch inside
// the resolver loop @ 0x4b30b7-0x4b351e]
void CollisionWorld::apply_touch_flags(Entity *ent, uint32_t flags, int16_t &health,
                                       bool is_authority) {
    if (ent == nullptr || flags == 0) return;
    // Hurt damage is authority-only AND gated off for EngineFlags 0x4000000 entities.
    // [orig: the is_authority + (Flags & 0x4000000) == 0 wrap @ 0x4b3139-0x4b3148]
    if (is_authority && (ent->engine_flags & 0x4000000u) == 0) {
        // Hurt-volume damage tiers. [orig: @ 0x4b317b-0x4b31d7 — -1 / -6 / -50 HP]
        if ((flags & 0x40u) != 0 && health > 0) health = static_cast<int16_t>(health - 1);
        if ((flags & 0x80u) != 0 && health > 0) health = static_cast<int16_t>(health - 6);
        if ((flags & 0x100u) != 0 && health > 0) health = static_cast<int16_t>(health - 50);
        // Capture-zone touch (0x200 -> Server_OnPlayerTouchCaptureZone @ 0x500ba0)
        // rides the zone system's own proximity path for now (D-COL-6).
    }
    if ((flags & 0x4u) != 0) ent->flags |= kEntityFlagArmoryZone; // type 6 [orig: @ 0x4b34a0]
    if ((flags & 0x400u) != 0)
        ent->flags |= kEntityFlagVehicleLoadoutZone; // type 11 [orig: @ 0x4b34ae]
}

} // namespace opennova::world
