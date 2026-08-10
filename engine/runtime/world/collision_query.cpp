#include "world/collision.h"

// Split out of collision.cpp (quality campaign W3-2). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// The stateless target queries: point-vs-blink, the segment-vs-solid clip, the
// projectile face and polygon raycasts, and the contact-force accumulation. Each
// takes a CollisionTargetView and touches no world state.

#include "world/ammo_table.h" // kAmmoFlagIgnorFoilage
#include "world/angle.h"
#include <algorithm>
#include <cmath>

#include "collision_detail.h"

#include "world/world.h"

namespace opennova::world {

using namespace detail; // the shared fixed-point helpers, unqualified as before

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
            if ((face.flags & kFaceFlagNeverHit) != 0) continue;
            if (face.material == 17 && (ammo_flags & kAmmoFlagIgnorFoilage) != 0) continue;
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
                if ((face.flags & kFaceFlagDoubleSided) == 0 && !(d0 > 0 && d1 <= 0)) continue;
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
        if (section.radius < 0) continue; // host/test sentinel; authored zero is valid

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
            if (face.poly_type == 17 && (ammo_flags & kAmmoFlagIgnorFoilage) != 0) continue;

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
namespace detail { // was anonymous; contact_query_overlaps_bound is declared in collision_detail.h

// The target entity's placement bound is available without resolving its live
// section matrices. Keep this predicate identical to the contact walk's first
// gate so proximity candidates that cannot touch any query point never invoke
// the host pose callback.
bool contact_query_overlaps_bound(const int32_t target_pos[3], int32_t target_bound_radius,
                                  const ContactQuery &q) {
    for (int32_t i = 0; i < q.num_points; ++i) {
        const int32_t combined = target_bound_radius + q.radii[i];
        if (abs32(q.points[i].x - target_pos[0]) <= combined &&
            abs32(q.points[i].y - target_pos[1]) <= combined &&
            abs32(q.points[i].z - target_pos[2]) <= combined)
            return true;
    }
    return false;
}

} // namespace detail

bool collision_contact_force(const CollisionTargetView &target, const ContactQuery &q,
                             BlinkAccum &blink, LadderContact &ladder, ContactResult &out) {
    out = ContactResult{};
    if (target.model == nullptr || target.matrices == nullptr) return false;
    if ((target.entity_flags & 1u) != 0) return false; // [orig: targetEntity[9] & 1 @ 0x4ae1bd]
    const CollisionModel &model = *target.model;
    if (model.sections.empty() || q.num_points <= 0) return false;

    // Broad phase per point. [orig: @ 0x4ae1d0]
    if (!contact_query_overlaps_bound(target.pos, target.bound_radius, q)) return false;

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
                    case bvol_type::kContactMarker:
                        has_collision = true; // contact, no force [orig: @ 0x4ae874]
                        break;
                    case bvol_type::kLadderCL: { // ladder alignment frame [orig: @ 0x4ae894-0x4aea30]
                        out.flags |= kTouchLadder;
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
                    case bvol_type::kArmoryCA: // touch volume [orig: @ 0x4aea45]
                        if (pi < 2) out.flags |= kTouchArmory;
                        break;
                    case bvol_type::kBlinkBB: // blink box [orig: @ 0x4aea68-0x4aeae8]
                        if (pi < 2) {
                            out.flags |= kTouchBlink;
                            if (target.is_building) {
                                blink.flags |= vol.flags ^ 6u;
                                if (blink.hit_count != -1 && blink.hit_count < 4 &&
                                    blink_volume_counter < 16)
                                    blink.add_hit(static_cast<int32_t>(si), target.pool_index);
                            }
                        }
                        break;
                    case bvol_type::kDoorCD: // door touch [orig: @ 0x4aeb0f-0x4aeb22]
                        out.flags |= kTouchDoor;
                        out.door_sections |= 1u << (si & 31);
                        break;
                    case bvol_type::kDamageHighDH: out.flags |= kTouchDamageHigh; break; // [orig: @ 0x4aeb39]
                    case bvol_type::kDamageMediumDM: out.flags |= kTouchDamageMedium; break; // [orig: @ 0x4aeb50]
                    case bvol_type::kDamageLowDL: out.flags |= kTouchDamageLow; break; // [orig: @ 0x4aeb67]
                    case bvol_type::kChangeTeamCT: out.flags |= kTouchChangeTeam; break; // [orig: @ 0x4aeb7b]
                    case bvol_type::kVehicleLoadout: out.flags |= kTouchVehicleLoadout; break; // [orig: @ 0x4aeb92]
                    case bvol_type::kFlagCF: // grounded touch [orig: @ 0x4aebb3]
                        if (target.is_ground_of_source) out.flags |= kTouchFlagGrounded;
                        break;
                    case bvol_type::kVehicleVC:  // vehicle-collision solid (reachable only on mask 0x8)
                    case bvol_type::kVehicleExt: // optional extension of the same vehicle pass
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

} // namespace opennova::world
