#include <runtime/world/collision.h>
#include <runtime/world/pose_provider.h>
#include <base/io/fixed.h>

// Split out of collision.cpp (quality campaign W3-2). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// The projectile trace: target views, the broad phase, trace_projectile itself, and
// the blink refresh that rides the same view cache.

#include <runtime/world/angle.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <runtime/terrain_query/height_field.h>

#include "collision_detail.h"

#include <runtime/world/world.h>

namespace opennova::world {

using namespace detail; // the shared fixed-point helpers, unqualified as before

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
            (e->engine_flags & kEntityFlagHusk) != 0 && instance->husk_model_id >= 0;
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
    } else if (pose_provider_ != nullptr) {
        mats.clear();
        live_pose = pose_provider_->build_section_matrices(
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
    scratch.bound_radius = entity_proximity_radius(*this, *e, m);
    scratch.is_building = (e->kind == EntityKind::Building);
    scratch.pool_index = h.slot();
    scratch.is_ground_of_source = false;
    scratch.uniform_scale_q16 = e->uniform_scale_q16;
    scratch.live_section_pose = live_pose;
    scratch.door_passable_sections = world.doors.passable_sections(*e);
    scratch.door_first_bone = e->door_first_bone;
    return &scratch;
}

bool CollisionWorld::entity_section_matrix(const World &world, EntityHandle h, int section,
                                           CollisionMatrix &out) const {
    CollisionTargetView scratch;
    std::vector<CollisionMatrix> matrices;
    const CollisionTargetView *target = target_view(world, h, scratch, matrices);
    if (target == nullptr || target->model == nullptr || target->matrices == nullptr ||
        section < 0 || section >= static_cast<int>(target->model->sections.size()))
        return false;
    out = target->matrices[section];
    return true;
}

std::vector<SectionDebrisSample> CollisionWorld::sample_section_debris(
        const World &world, EntityHandle h, const Vec3 &blast_center) const {
    std::vector<SectionDebrisSample> out;
    CollisionTargetView scratch;
    std::vector<CollisionMatrix> matrices;
    const CollisionTargetView *target = target_view(world, h, scratch, matrices);
    if (target == nullptr || target->model == nullptr ||
        target->matrices == nullptr || target->model->faces.empty())
        return out;

    const CollisionModel &model = *target->model;
    const int64_t total_faces = static_cast<int64_t>(model.faces.size());
    // model+0x60 is the total CFAC count. Its 8.8 step is recomputed for
    // every section and the accumulator begins at zero each time.
    // [orig: @0x43f5a6..0x43f5d9]
    const int64_t stride = (total_faces * 256) / 150;
    if (stride <= 0) return out;

    const int32_t blast_fixed[3] = {
            to_fixed(blast_center.x), to_fixed(blast_center.y),
            to_fixed(blast_center.z)};
    const bool has_blast_center =
            blast_fixed[0] != 0 || blast_fixed[1] != 0 || blast_fixed[2] != 0;
    constexpr double kBamPerRad = kBamFullTurn /
            6.283185307179586476925286766559;
    constexpr int32_t kRadialFallbackPitchBam = 754974675;
    auto direction_from_angles = [](double heading, double pitch) {
        const double cp = std::cos(pitch);
        return Vec3{static_cast<float>(cp * std::cos(heading)),
                    static_cast<float>(cp * std::sin(heading)),
                    static_cast<float>(std::sin(pitch))};
    };

    // The retail function asks the callback for the matrix array once and
    // passes its base to every per-section invocation; it does not add the
    // section ordinal before Math_FixedPointTransformPoint22.
    // [orig: callback @0x43f5a4; transform @0x43f6b8]
    const CollisionMatrix &matrix = target->matrices[0];
    for (const CollisionSection &section : model.sections) {
        if (section.face_count <= 0) continue;
        int64_t accumulator = 0;
        do {
            const int64_t local_face = accumulator >> 8;
            if (local_face < 0 || local_face >= section.face_count) break;
            const int64_t face_index =
                    static_cast<int64_t>(section.face_start) + local_face;
            if (face_index < 0 || face_index >= total_faces) break;
            const CollisionFace &face =
                    model.faces[static_cast<size_t>(face_index)];
            int32_t centroid[3] = {};
            bool valid = true;
            for (int axis = 0; axis < 3; ++axis) {
                int32_t sum = 0;
                for (int corner = 0; corner < 3; ++corner) {
                    const int64_t vertex_index =
                            static_cast<int64_t>(section.face_vertex_start) +
                            face.vertex_index[corner];
                    if (vertex_index < 0 ||
                        vertex_index >= static_cast<int64_t>(model.face_vertices.size())) {
                        valid = false;
                        break;
                    }
                    const CollisionFaceVertex &vertex =
                            model.face_vertices[static_cast<size_t>(vertex_index)];
                    sum += axis == 0 ? vertex.x : axis == 1 ? vertex.y : vertex.z;
                }
                if (!valid) break;
                // C++ signed division matches the x86 magic-divide sequence:
                // truncate toward zero, then promote Q8 to 16.16.
                centroid[axis] = (sum / 3) * 256;
            }
            if (valid) {
                int32_t world_point[3];
                matrix.transform_point(centroid, world_point);
                int64_t dx = 0, dy = 0, dz = 0;
                double pitch = 0.0;
                if (has_blast_center) {
                    dx = static_cast<int64_t>(world_point[0]) - blast_fixed[0];
                    dy = static_cast<int64_t>(world_point[1]) - blast_fixed[1];
                    dz = static_cast<int64_t>(world_point[2]) - blast_fixed[2];
                    const int64_t ax = dx < 0 ? -dx : dx;
                    const int64_t ay = dy < 0 ? -dy : dy;
                    const int64_t lesser = ax < ay ? ax : ay;
                    const int64_t greater = ax < ay ? ay : ax;
                    const int64_t approx_distance = greater + 5 * (lesser / 16);
                    pitch = std::atan2(static_cast<double>(dz),
                                       static_cast<double>(approx_distance));
                } else {
                    dx = static_cast<int64_t>(world_point[0]) - target->pos[0];
                    dy = static_cast<int64_t>(world_point[1]) - target->pos[1];
                    pitch = static_cast<double>(kRadialFallbackPitchBam) /
                            kBamPerRad;
                }
                const double heading = std::atan2(
                        static_cast<double>(dy), static_cast<double>(dx));
                out.push_back(SectionDebrisSample{
                        Vec3{world_point[0] * io::kInvFp16One,
                             world_point[1] * io::kInvFp16One,
                             world_point[2] * io::kInvFp16One},
                        direction_from_angles(heading, pitch), face.material});
            }
            accumulator += stride;
        } while ((accumulator >> 8) < section.face_count);
    }
    return out;
}

// The witnessed projectile broad phase over the pools-2/1 proximity slots
// [orig: Projectile_RaycastProximitySlots @ 0x4e53d4-0x4e554a]: per-axis
// |slotCenter - segCenter| <= radius + halfExtent, then the UNCLAMPED
// perpendicular line distance <= radius — a segment entirely inside the bound
// passes trivially (the D-ITEM-13d lesson: inside-sphere segments are the
// common case near buildings). Restored from #266's round_broad_phase after
// the #276 trace_projectile restructure dropped it, leaving every round to
// resolve a target view (and its per-section matrix vector) for every table
// entity each tick.
static bool round_broad_phase(const float p0[3], const float p1[3], const float c[3],
                              float r) {
    const float hx = std::fabs(p1[0] - p0[0]) * 0.5f;
    const float hy = std::fabs(p1[1] - p0[1]) * 0.5f;
    const float hz = std::fabs(p1[2] - p0[2]) * 0.5f;
    if (std::fabs((p0[0] + p1[0]) * 0.5f - c[0]) > r + hx) return false;
    if (std::fabs((p0[1] + p1[1]) * 0.5f - c[1]) > r + hy) return false;
    if (std::fabs((p0[2] + p1[2]) * 0.5f - c[2]) > r + hz) return false;
    const float dx = p1[0] - p0[0], dy = p1[1] - p0[1], dz = p1[2] - p0[2];
    const float len2 = dx * dx + dy * dy + dz * dz;
    if (len2 <= 0.0f)
        return true; // zero-length segment inside the AABB gate: the axis test decided
    const float t = ((c[0] - p0[0]) * dx + (c[1] - p0[1]) * dy + (c[2] - p0[2]) * dz) / len2;
    const float px = p0[0] + dx * t - c[0];
    const float py = p0[1] + dy * t - c[1];
    const float pz = p0[2] + dz * t - c[2];
    return px * px + py * py + pz * pz <= r * r;
}

bool CollisionWorld::target_bound(const World &world, EntityHandle h, int32_t pos_out[3],
                                  int32_t &radius_out, bool solid_only) const {
    // target_view's model selection (live identity, the husk swap, validity)
    // WITHOUT the section-matrix build — bound-gate metadata only, so callers
    // can run the witnessed cheap gate first and build the full view for gate
    // survivors alone. Returns false exactly when target_view would return
    // null; `solid_only` additionally rejects models without a solid volume
    // (the LOS / stable-candidate callers).
    const Instance *instance = live_instance(world, h);
    if (instance == nullptr) return false;
    const Entity *e = world.registry.get(h);
    if (e == nullptr) return false;
    const bool using_husk =
            (e->engine_flags & kEntityFlagHusk) != 0 && instance->husk_model_id >= 0;
    const CollisionModel *m =
            model(using_husk ? instance->husk_model_id : instance->model_id);
    if (m == nullptr || !m->valid()) return false;
    if (solid_only && !m->has_solid_volume) return false;
    entity_pos_fixed(*e, pos_out);
    radius_out = entity_proximity_radius(*this, *e, m);
    return true;
}

// The per-tick projectile view: build once per (entity, tick), reuse for
// every round tracing it this tick. A hit revalidates the husk bit — retail
// picks the model per query [orig: Flags & 4 pick @ 0x413086], and a round
// earlier in the same tick can husk the target.
const CollisionTargetView *CollisionWorld::trace_target_view(const World &world,
                                                             EntityHandle h) const {
    const Entity *e = world.registry.get(h);
    const bool husk_now = e != nullptr && (e->engine_flags & kEntityFlagHusk) != 0;
    const uint32_t mask_now = e != nullptr ? e->spawned_piece_mask : 0;
    const uint64_t spawn_id_now =
            e != nullptr ? e->registry_spawn_id : uint64_t{0};
    const Instance *instance = live_instance(world, h);
    int32_t model_id_now = -1;
    if (instance != nullptr) {
        model_id_now = husk_now && instance->husk_model_id >= 0
                ? instance->husk_model_id
                : instance->model_id;
    }
    const auto cached = trace_view_cache_.find(h.packed);
    if (cached != trace_view_cache_.end()) {
        const TraceViewCacheEntry &entry = cached->second;
        if (husk_now == entry.husk_bit &&
            mask_now == entry.piece_mask &&
            spawn_id_now == entry.registry_spawn_id &&
            model_id_now == entry.effective_model_id)
            return entry.valid ? &entry.view : nullptr;
    }

    // Build outside the map: the host matrix callback is an external seam.
    // Its contract is read-only, but keeping no map reference across it also
    // prevents accidental callback invalidation from becoming use-after-free.
    TraceViewCacheEntry rebuilt;
    rebuilt.valid =
            target_view(world, h, rebuilt.view, rebuilt.matrices) != nullptr;
    rebuilt.husk_bit = husk_now;
    rebuilt.piece_mask = mask_now;
    rebuilt.registry_spawn_id = spawn_id_now;
    rebuilt.effective_model_id = model_id_now;
    auto inserted = trace_view_cache_.insert_or_assign(
            h.packed, std::move(rebuilt));
    TraceViewCacheEntry &entry = inserted.first->second;
    entry.view.matrices =
            entry.matrices.empty() ? nullptr : entry.matrices.data();
    return entry.valid ? &entry.view : nullptr;
}

namespace {

// The contact-capture kind for a resolved trace: terrain and water flash as
// point markers (no box to light), everything else flashes the hit body.
CollisionWorld::ContactDebugKind contact_kind_for_trace(ProjectileHitClass hit_class,
                                                        bool knife) {
    if (hit_class == ProjectileHitClass::Terrain)
        return CollisionWorld::ContactDebugKind::kTerrainHit;
    if (hit_class == ProjectileHitClass::Water)
        return CollisionWorld::ContactDebugKind::kWaterHit;
    return knife ? CollisionWorld::ContactDebugKind::kKnifeHit
                 : CollisionWorld::ContactDebugKind::kProjectileHit;
}

}  // namespace

ProjectileHit CollisionWorld::trace_projectile(const World &world,
                                               const ProjectileTrace &trace) const {
    const ProjectileHit hit = trace_projectile_impl(world, trace, false);
    if (ray_debug_enabled_) {
        const int32_t a[3] = {trace.start.x, trace.start.y, trace.start.z};
        const int32_t b[3] = {trace.end.x, trace.end.y, trace.end.z};
        const int32_t h[3] = {hit.position_q16.x, hit.position_q16.y,
                              hit.position_q16.z};
        ray_debug_record(RayDebugCategory::kProjectile, world.logic_tick, a, b,
                         hit.hit() ? h : nullptr,
                         hit.hit() ? kRayDebugHit : kRayDebugClear);
    }
    if (contact_debug_enabled_ && hit.hit()) {
        const int32_t h[3] = {hit.position_q16.x, hit.position_q16.y,
                              hit.position_q16.z};
        contact_debug_record(contact_kind_for_trace(hit.hit_class, false),
                             world.logic_tick, hit.geometry_entity, h,
                             static_cast<uint8_t>(hit.hit_class));
    }
    return hit;
}

ProjectileHit CollisionWorld::trace_knife_impact(
        const World &world, const ProjectileTrace &trace) const {
    const ProjectileHit hit = trace_projectile_impl(world, trace, true);
    if (ray_debug_enabled_) {
        const int32_t a[3] = {trace.start.x, trace.start.y, trace.start.z};
        const int32_t b[3] = {trace.end.x, trace.end.y, trace.end.z};
        const int32_t h[3] = {hit.position_q16.x, hit.position_q16.y,
                              hit.position_q16.z};
        ray_debug_record(RayDebugCategory::kKnife, world.logic_tick, a, b,
                         hit.hit() ? h : nullptr,
                         hit.hit() ? kRayDebugHit : kRayDebugClear);
    }
    if (contact_debug_enabled_ && hit.hit()) {
        const int32_t h[3] = {hit.position_q16.x, hit.position_q16.y,
                              hit.position_q16.z};
        contact_debug_record(contact_kind_for_trace(hit.hit_class, true),
                             world.logic_tick, hit.geometry_entity, h,
                             static_cast<uint8_t>(hit.hit_class));
    }
    return hit;
}

// [orig: compute_clamped_displacement @ 0x4AD6A0]
int32_t CollisionWorld::minefield_ground(const World &world, EntityHandle source,
        FixedVec3 position, bool indoors) const {
    ProjectileTrace trace;
    trace.start = position;
    trace.end = {position.x, position.y, 0};
    trace.extra_ignore = source;
    trace.mount_ignore = source;
    trace.walk_terrain = trace.walk_water = trace.walk_persons = false;
    trace.include_wire_proxies = world.rules.mp_session && !world.rules.projectile_authority;
    trace.shooter_carrier_wire_handle = source.packed;
    // The CFAC-only traversal (no unresolved-model sphere substitute).
    const ProjectileHit hit = trace_projectile_impl(world, trace, true);
    const bool object_ground = hit.hit() && hit.distance_q16 < position.z;
    int32_t height = object_ground ? position.z - hit.distance_q16 : position.z;
    if (!indoors) {
        const int32_t floor = terrain ? to_fixed(terrain::height_field_height_world_bilinear(
                *terrain, position.x * io::kInvFp16One, -position.y * io::kInvFp16One)) : 0;
        if (!object_ground || height < floor) height = floor;
    }
    return height;
}

ProjectileHit CollisionWorld::trace_projectile_impl(
        const World &world, const ProjectileTrace &trace,
        bool person_faces_only) const {
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

    // Trace attribution is deliberately cold unless an F3/probe consumer opts
    // in: ordinary projectile traces do not touch the clock or counters.
    const bool profile_trace = trace_profile_enabled_;
    const auto prof_now = []() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
    };
    int64_t prof_t = 0;
    if (profile_trace) {
        trace_profile_.calls++;
        prof_t = prof_now();
    }

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
    // Cross-pass arbitration is strict (a later pass replaces only when
    // closer); `tie_wins` is the one witnessed exception, the Knife
    // presenter's person leg (below).
    auto consider = [&](ProjectileHit candidate, int32_t distance,
                        bool tie_wins = false) {
        if (!candidate.hit()) return;
        if (!best.hit() || distance < best_distance ||
            (tie_wins && distance == best_distance)) {
            candidate.distance_q16 = distance;
            best = candidate;
            best_distance = distance;
        }
    };

    // Ammo flag 0x80 is the recovered NoCollide terrain bypass.
    if (trace.walk_terrain && (trace.ammo_flags & 0x80u) == 0 &&
        terrain != nullptr && terrain->valid()) {
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
            const float wx = static_cast<float>(hit[0]) / io::kFp16One;
            const float wy = static_cast<float>(hit[1]) / io::kFp16One;
            const float hx0 = terrain::height_field_height_world_bilinear(*terrain, wx - 1.0f, -wy);
            const float hx1 = terrain::height_field_height_world_bilinear(*terrain, wx + 1.0f, -wy);
            const float hy0 = terrain::height_field_height_world_bilinear(*terrain, wx, -(wy - 1.0f));
            const float hy1 = terrain::height_field_height_world_bilinear(*terrain, wx, -(wy + 1.0f));
            double nx = -0.5 * static_cast<double>(hx1 - hx0);
            double ny = -0.5 * static_cast<double>(hy1 - hy0);
            double nz = 1.0;
            const double nl = std::sqrt(nx * nx + ny * ny + nz * nz);
            th.normal_q16 = FixedVec3{
                static_cast<int32_t>(nx * io::kFp16OneD / nl),
                static_cast<int32_t>(ny * io::kFp16OneD / nl),
                static_cast<int32_t>(nz * io::kFp16OneD / nl),
            };
            consider(th, distance);
        }
    }

    if (profile_trace) {
        const int64_t prof_n = prof_now();
        trace_profile_.terrain_us += prof_n - prof_t;
        prof_t = prof_n;
    }

    // Water is pass two regardless of a ClipWater-style ammo flag. Retail
    // requires a strict straddle, so an endpoint on the plane is not a hit.
    const int32_t water_z = world.env.water_z;
    if (trace.walk_water && water_z != 0 &&
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
    EntityHandle ignored_mount = trace.mount_ignore;
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
    // The mission-authored refNum group (BMS byte 153 -> entity+533) carries
    // self-site immunity through two DIFFERENT reference channels: the ITEM
    // face narrow phase compares the candidate against the ray[18] MOUNT
    // exclusion's refNum, null-guarded [orig: @ 0x4e4d40 in
    // Physics_RaycastAgainstBoneCollision @ 0x4e4cb0], while the PERSON leg
    // compares against the ray[17] SHOOTER's refNum [orig: @ 0x4e4688-0x4e46a3
    // in Physics_RaycastAgainstBoneSections @ 0x4e4670 — retail derefs a null
    // ray[17] unguarded there; a live owner is required here instead].
    const Entity *mount_entity = world.registry.get(ignored_mount);
    const uint8_t mount_ref_num =
        mount_entity != nullptr ? mount_entity->ref_num : 0;
    const uint8_t owner_ref_num = owner != nullptr ? owner->ref_num : 0;
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
    auto sphere_distance = [&](const int32_t center[3], int32_t radius,
                               int32_t &out_distance) {
        if (radius <= 0) return false;
        const long double fx = static_cast<long double>(trace.start.x) -
                               static_cast<long double>(center[0]);
        const long double fy = static_cast<long double>(trace.start.y) -
                               static_cast<long double>(center[1]);
        const long double fz = static_cast<long double>(trace.start.z) -
                               static_cast<long double>(center[2]);
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

    // Segment endpoints in slot-table units for the witnessed broad phase.
    const float p0f[3] = {
        static_cast<float>(trace.start.x) / io::kFp16One,
        static_cast<float>(trace.start.y) / io::kFp16One,
        static_cast<float>(trace.start.z) / io::kFp16One,
    };
    const float p1f[3] = {
        static_cast<float>(trace.end.x) / io::kFp16One,
        static_cast<float>(trace.end.y) / io::kFp16One,
        static_cast<float>(trace.end.z) / io::kFp16One,
    };

    // The per-candidate CFAC narrow phase shared by the local slot tables and
    // the decoded wire projection. False = this candidate does not stop the
    // round (no CFAC mesh, or the segment misses it).
    auto narrow_phase = [&](const CollisionTargetView &target,
                            CollisionPolygonHit &model_hit) {
        const CollisionModel &model_ref = *target.model;
        const bool indexed_mesh = !model_ref.faces.empty() &&
                                  !model_ref.vertices.empty() &&
                                  !model_ref.normals.empty();
        const bool q8_mesh = !model_ref.faces.empty() &&
                             !model_ref.face_vertices.empty();
        if (indexed_mesh) {
            return collision_raycast_polygons(target, ray, segment_length,
                                              trace.ammo_flags, model_hit);
        }
        if (q8_mesh) {
            RayFaceHit face_hit;
            if (!collision_raycast_faces(target, ray.start, ray.end,
                                         trace.ammo_flags, face_hit))
                return false;
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
                    target.matrices[face_hit.section].rotate_point(
                        local_normal, model_hit.normal_q16);
                }
            }
            return true;
        }
        // A resolved model without CFAC is BVOL-only and does not stop
        // ordinary bullets, regardless of its gameplay volume types.
        return false;
    };

    // Each table query uses <= internally: a later equal face, section, or
    // entity overwrites the earlier result. Cross-pass arbitration stays strict.
    // `slot_to_units` scales the table's stored coordinates to world units:
    // static slots are signed whole-unit coordinates carried in u16 storage
    // (their +1.707u radius pad absorbs the rounding [orig: the u16 tables
    // built @ 0x4b94cb]); dynamic slots store signed full 16.16.
    auto trace_polygon_table = [&](const auto &slots, ProjectileHitClass hit_class,
                                   float slot_to_units, bool signed_word_coords,
                                   bool tie_wins = false) {
        CollisionPolygonHit table_hit;
        EntityHandle table_entity;
        bool table_found = false;
        for (const auto &slot : slots) {
            const EntityHandle h = slot.h;
            if (ignored(h)) continue;
            // The slot gate runs on table fields alone; the entity resolves
            // only for gate survivors [orig: @ 0x4e53d4 reads the slot x/y/z/
            // radius words before any entity deref].
            const auto coord_to_units = [&](auto value) {
                if (signed_word_coords)
                    return static_cast<float>(
                            static_slot_coord_units(static_cast<uint16_t>(value)));
                int32_t raw = static_cast<int32_t>(value);
                return static_cast<float>(raw) * slot_to_units;
            };
            const float sc[3] = {coord_to_units(slot.x), coord_to_units(slot.y),
                                 coord_to_units(slot.z)};
            if (!round_broad_phase(p0f, p1f, sc,
                                   static_cast<float>(slot.radius) * slot_to_units))
                continue;
            if (profile_trace) {
                if (hit_class == ProjectileHitClass::StaticEntity)
                    trace_profile_.static_survivors++;
                else if (hit_class == ProjectileHitClass::DynamicEntity)
                    trace_profile_.dynamic_survivors++;
                else
                    trace_profile_.person_survivors++;
            }
            const Entity *entity = world.registry.get(h);
            if (entity == nullptr || entity->hidden ||
                (entity->engine_flags & 0x02000001u) != 0)
                continue;
            // Item self-site immunity: candidate refNum vs the mount
            // exclusion's refNum [orig: @ 0x4e4d40].
            if (entity->ref_num != 0 && mount_ref_num != 0 &&
                entity->ref_num == mount_ref_num)
                continue;
            CollisionPolygonHit model_hit;
            const CollisionTargetView *target = trace_target_view(world, h);
            if (target == nullptr &&
                hit_class == ProjectileHitClass::DynamicEntity &&
                entity->kind == EntityKind::Item) {
                // Runtime-placed satchels/mines are ordinary pool-1 item clones:
                // their model initialization precedes the same CFAC projectile
                // walk as mission-start items. Give the retained host asset
                // provider one idempotent chance to attach that late allocation
                // before the polygon walk. Failure is an impossible clone/model
                // state, not alternate collision geometry: surface it instead of
                // silently widening the item to its proximity sphere.
                // [orig: Entity_CloneFromTemplateByType @0x4398A0 ->
                // Entity_InitFromModel @0x40DC30; Projectile_RaycastProximitySlots
                // @0x4E53D4 -> Physics_RaycastAgainstBoneCollision @0x4E4CB0]
                const_cast<CollisionWorld *>(this)->ensure_entity_instance(
                    const_cast<World &>(world), h);
                target = trace_target_view(world, h);
                if (target == nullptr && !person_faces_only) {
                    throw std::logic_error(
                        "CollisionWorld::trace_projectile: pool-1 item has no live collision model");
                }
            }
            if (profile_trace && target != nullptr && target->model != nullptr) {
                const int64_t faces =
                    static_cast<int64_t>(target->model->faces.size());
                if (hit_class == ProjectileHitClass::StaticEntity)
                    trace_profile_.static_faces += faces;
                else if (hit_class == ProjectileHitClass::DynamicEntity)
                    trace_profile_.dynamic_faces += faces;
            }
            if (target == nullptr) {
                // Weapon_RaycastAndSpawnImpact feeds every pool through the
                // CFAC walker. A model-less person/item is a miss here, never
                // the bounded sphere compatibility used by ordinary bullets.
                // [orig: Weapon_RaycastAndSpawnImpact @0x4e8460;
                // Physics_RaycastAgainstBoneCollision @0x4e4cb0]
                if (person_faces_only) continue;
                const int32_t center[3] = {to_fixed(entity->position.x),
                                           to_fixed(entity->position.y),
                                           to_fixed(entity->position.z)};
                const int32_t radius = entity->bound_radius > 0.0f
                    ? to_fixed(entity->bound_radius)
                    : 0;
                if (!sphere_distance(center, radius, model_hit.distance_q16)) continue;
                const int32_t t = t_for_distance(model_hit.distance_q16);
                const FixedVec3 p = point_at(t);
                model_hit.position_q16[0] = p.x;
                model_hit.position_q16[1] = p.y;
                model_hit.position_q16[2] = p.z;
            } else if (!narrow_phase(*target, model_hit)) {
                continue;
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
        consider(eh, table_hit.distance_q16, tie_wins);
    };
    // A visual-only MP client (mp_session && !projectile_authority) uses decoded
    // wire projections for pose-bearing projectile contacts. Pools 1-3 are
    // materialized at their exact streamed native handles, but the pool-1
    // dynamics pass below samples ClientState's live pose instead of walking a
    // second native representation. Pool-2 statics come from exact S2C 0x10
    // materialization. Remote pool-0 organics remain ClientState proxies; the
    // local player L is the one live native person.
    const bool wire_projected = world.rules.mp_session && !world.rules.projectile_authority;

    if (profile_trace) {
        const int64_t prof_n = prof_now();
        prof_t = prof_n; // owner/exclusion setup charged to neither pass
    }
    // The Knife presenter's entity legs run in retail's own order: the PERSON
    // prox table FIRST (Projectile_RaycastProximitySlots' default slot type),
    // replacing a terrain/water hit on `candidateDist <= closestDist` — the one
    // non-strict compare in the family; the building (2) and item (1) legs that
    // follow are strict, exactly like an ordinary round.
    // [orig: Weapon_RaycastAndSpawnImpact @0x4e86ad (person, <=), @0x4e873f
    //  (building, <), @0x4e87cb (item, <)]
    if (person_faces_only && trace.walk_persons) {
        trace_polygon_table(persons_, ProjectileHitClass::Person,
                            io::kInvFp16One, false, /*tie_wins=*/true);
    }
    trace_polygon_table(statics_, ProjectileHitClass::StaticEntity, 1.0f, true);
    if (profile_trace) {
        const int64_t prof_n = prof_now();
        trace_profile_.static_us += prof_n - prof_t;
        prof_t = prof_n;
    }
    if (!wire_projected)
        trace_polygon_table(dynamics_, ProjectileHitClass::DynamicEntity,
                            io::kInvFp16One, false);

    // The decoded pool-1 wire projection: authored CFAC geometry posed from the
    // decoded wire state, walked in wire-handle (= host pool slot) order as the
    // visual client's dyn-table stand-in, in the same pass position (after
    // statics, before persons). On a retail client this pass IS the ordinary
    // dyn-table walk over its wire-built pool-1 entities
    // [orig: Projectile_RaycastProximitySlots @ 0x4e5340]; the wire-keyed
    // proxy layer is the documented visual-only-client reimpl model
    // (docs/net/novaworld-net-re.md §5.60). Wire identities never become
    // registry handles: geometry_entity stays invalid on a proxy hit.
    if (trace.include_wire_proxies && !wire_dynamic_proxies_.empty()) {
        CollisionPolygonHit table_hit;
        bool table_found = false;
        for (const WireDynamicCollisionProxy &proxy : wire_dynamic_proxies_) {
            // Self-site immunity: the shooter's own wire slot (a decoded
            // vehicle/item shooter never clips itself) and the mounted
            // shooter's own carrier, resolved from the decoded carrier_handle
            // — the ray[17..20] exclusion-set analog [orig: the four-slot
            // compare before Physics_RaycastAgainstBoneCollision @ 0x4e5572].
            if (proxy.wire_handle == trace.shooter_wire_handle) continue;
            if (proxy.wire_handle == trace.shooter_carrier_wire_handle) continue;
            const float sc[3] = {
                static_cast<float>(proxy.position_q16.x) / io::kFp16One,
                static_cast<float>(proxy.position_q16.y) / io::kFp16One,
                static_cast<float>(proxy.position_q16.z) / io::kFp16One,
            };
            const int32_t broad_radius = proxy.bound_radius_q16 > 0
                ? proxy.bound_radius_q16
                : 0x10000;
            if (!round_broad_phase(p0f, p1f, sc,
                                   static_cast<float>(broad_radius) / io::kFp16One))
                continue;
            if (profile_trace) trace_profile_.dynamic_survivors++;
            CollisionPolygonHit model_hit;
            const CollisionModel *proxy_model = model(proxy.model_id);
            if (proxy_model != nullptr && proxy_model->valid()) {
                if (profile_trace)
                    trace_profile_.dynamic_faces +=
                        static_cast<int64_t>(proxy_model->faces.size());
                const int32_t pos[3] = {proxy.position_q16.x, proxy.position_q16.y,
                                        proxy.position_q16.z};
                // The decoded pose mirrors the retail client entity fields the
                // placement matrix reads: live coarse heading (entity+16 high
                // byte) plus the retained spawn/dead pitch/roll samples
                // (entity+20/+24) [orig: the placement matrix @ 0x613f40].
                CollisionMatrix world_mat =
                    (proxy.pitch_bam != 0 || proxy.roll_bam != 0)
                        ? collision_matrix_from_euler(proxy.heading_bam,
                                                      proxy.pitch_bam,
                                                      proxy.roll_bam, pos)
                        : collision_matrix_from_heading(proxy.heading_bam, pos);
                if (proxy.uniform_scale_q16 != 0) {
                    constexpr int rotation_indices[] = {
                        0, 1, 2, 4, 5, 6, 8, 9, 10};
                    for (int index : rotation_indices) {
                        world_mat.m[index] = static_cast<int32_t>(
                            (static_cast<int64_t>(world_mat.m[index]) *
                             proxy.uniform_scale_q16) >> 16);
                    }
                }
                std::vector<CollisionMatrix> proxy_matrices(
                    proxy_model->sections.size(), world_mat);
                CollisionTargetView view;
                view.model = proxy_model;
                view.matrices = proxy_matrices.data();
                view.pos[0] = pos[0];
                view.pos[1] = pos[1];
                view.pos[2] = pos[2];
                view.yaw_bam = proxy.heading_bam;
                view.pitch_bam = proxy.pitch_bam;
                view.bound_radius = broad_radius;
                view.pool_index = proxy.wire_handle & 0xFFF;
                view.uniform_scale_q16 = proxy.uniform_scale_q16;
                view.live_section_pose = false;
                if (!narrow_phase(view, model_hit)) continue;
            } else {
                // Retail's pool-1 projectile pass walks client entities only
                // after model initialization has supplied their authored CFAC.
                // An unresolved wire projection is therefore a broken binding
                // invariant, never alternate sphere collision.
                // [orig: Entity_InitFromModel @0x40DC30;
                // Projectile_RaycastProximitySlots @0x4E5340 ->
                // Physics_RaycastAgainstBoneCollision @0x4E4CB0]
                throw std::logic_error(
                    "CollisionWorld::trace_projectile: pool-1 wire proxy has no live collision model");
            }
            if (!table_found || model_hit.distance_q16 <= table_hit.distance_q16) {
                table_found = true;
                table_hit = model_hit;
            }
        }
        if (table_found) {
            ProjectileHit eh;
            eh.hit_class = ProjectileHitClass::DynamicEntity;
            eh.t_q16 = t_for_distance(table_hit.distance_q16);
            eh.position_q16 = FixedVec3{table_hit.position_q16[0],
                                        table_hit.position_q16[1],
                                        table_hit.position_q16[2]};
            eh.normal_q16 = FixedVec3{table_hit.normal_q16[0],
                                      table_hit.normal_q16[1],
                                      table_hit.normal_q16[2]};
            eh.section_index = table_hit.section_index;
            eh.bone_index = table_hit.section_index;
            eh.face_index = table_hit.section_face_index;
            eh.surface_type = table_hit.poly_type;
            eh.material_flags = table_hit.material_flags;
            consider(eh, table_hit.distance_q16);
        }
    }

    if (profile_trace) {
        const int64_t prof_n = prof_now();
        trace_profile_.dynamic_us += prof_n - prof_t;
        prof_t = prof_n;
    }

    // Consume the pose owner's COBJ matrices when available. The bounded torso
    // fallback is only for entities whose production pose has not been
    // published; both paths preserve retail's first-qualifying-person table
    // behavior rather than choosing the globally nearest person. Visual decoded
    // players join this bounded presentation walk without entering the registry
    // or any consequence-producing table. The client's own L retains local
    // geometry/ignore identity but uses its supplied server H only as an ordering
    // key; because that key is unrelated to L's registry slot, the merged walk
    // order is materialized and sorted below rather than read off persons_.
    constexpr int32_t kOrganicCenterZQ16 = 58982;
    int32_t effective_radius = std::max(trace.radius_q16, 0);
    const bool authority_fat_bullet =
        world.rules.mp_session && world.rules.projectile_authority && world.rules.fat_bullets &&
        owner != nullptr && (owner->flags & kEntityFlagPlayer) != 0 &&
        trace.owner != world.cached.local_player;
    if (authority_fat_bullet)
        effective_radius = std::max(effective_radius, kProjectileAuthorityMinRadiusQ16);
    constexpr int32_t kFallbackAuthoredRadiusQ16 = 78642; // 1.2u
    auto trace_torso_fallback = [&](const FixedVec3 &feet, int32_t uniform_scale_q16,
                                    ProjectileHit &eh,
                                    int32_t &hit_distance) {
        const int32_t center_lift = uniform_scale_q16 != 0
            ? retail_q16_mul_rhu(kOrganicCenterZQ16, uniform_scale_q16)
            : kOrganicCenterZQ16;
        const int64_t scale_magnitude = uniform_scale_q16 < 0
            ? -static_cast<int64_t>(uniform_scale_q16)
            : static_cast<int64_t>(uniform_scale_q16);
        const int32_t radius_scale = uniform_scale_q16 == 0
            ? 0x10000
            : static_cast<int32_t>(
                  std::min<int64_t>(scale_magnitude, 0x7fffffffLL));
        const int32_t fallback_radius = retail_q16_mul_rhu(
            kFallbackAuthoredRadiusQ16, radius_scale);
        const int32_t center[3] = {
            feet.x, feet.y, feet.z + center_lift,
        };
        const int32_t projection = static_cast<int32_t>(
            (static_cast<int64_t>(ray.dir[0]) * (center[0] - ray.start[0]) +
             static_cast<int64_t>(ray.dir[1]) * (center[1] - ray.start[1]) +
             static_cast<int64_t>(ray.dir[2]) * (center[2] - ray.start[2])) >> 16);
        if (projection < 0 || projection > segment_length) return false;
        int32_t closest[3] = {};
        for (int axis = 0; axis < 3; ++axis) {
            closest[axis] = ray.start[axis] + static_cast<int32_t>(
                (static_cast<int64_t>(ray.dir[axis]) * projection + 0x8000) >> 16);
        }
        const int32_t center_distance =
            vec_len_ftol(closest[0] - center[0], closest[1] - center[1],
                         closest[2] - center[2]);
        const int32_t hit_radius = effective_radius + 3276 +
                static_cast<int32_t>((45LL * fallback_radius) / 100);
        if (center_distance > hit_radius) return false;
        hit_distance = projection - (fallback_radius >> 1);
        // No authored section identity exists. RoundSim maps this sentinel to
        // synthetic torso 1 for presentation/reactions while preserving a
        // neutral damage multiplier.
        eh.section_index = -1;
        eh.bone_index = -1;
        eh.hit_zone = -1;
        return true;
    };
    auto finish_person_hit = [&](ProjectileHit &eh, int32_t hit_distance) {
        eh.hit_class = ProjectileHitClass::Person;
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
    };

    // The motor sweep's pools-2/1-only contract: no person leg at all, local
    // or proxied [orig: Projectile_RaycastProximitySlots(2/1) only
    // @ 0x444619..0x444667].
    struct PersonWalkEntry {
        uint16_t key = 0;
        bool is_proxy = false;
        uint32_t index = 0;
    };
    std::vector<PersonWalkEntry> person_walk;
    if (trace.walk_persons && !person_faces_only) {
        const bool merge_proxies =
            trace.include_wire_proxies && !wire_person_proxies_.empty();
        person_walk.reserve(persons_.size() +
                            (merge_proxies ? wire_person_proxies_.size()
                                           : size_t{0}));
        for (size_t i = 0; i < persons_.size(); ++i) {
            const PersonSlot &slot = persons_[i];
            const uint16_t key =
                (wire_local_player_handle_ != EntityHandle::kInvalid &&
                 slot.h == world.cached.local_player)
                    ? wire_local_player_handle_
                    : slot.h.packed;
            person_walk.push_back({key, false, static_cast<uint32_t>(i)});
        }
        if (merge_proxies) {
            for (size_t i = 0; i < wire_person_proxies_.size(); ++i) {
                person_walk.push_back(
                    {wire_person_proxies_[i].wire_handle, true,
                     static_cast<uint32_t>(i)});
            }
            // persons_ arrives in registry pool/slot order, which is the wire
            // order only while no key is substituted — L's server H can sort
            // anywhere, so a two-pointer merge over the raw inputs would visit
            // out of order. Sorting the materialized plan keeps the walk in
            // ascending wire order; equal keys keep the person ahead of the
            // proxy (stable, persons appended first).
            std::stable_sort(person_walk.begin(), person_walk.end(),
                             [](const PersonWalkEntry &a, const PersonWalkEntry &b) {
                                 return a.key < b.key;
                             });
        }
    }
    for (const PersonWalkEntry &entry : person_walk) {
        if (entry.is_proxy) {
            const WirePersonCollisionProxy &proxy =
                wire_person_proxies_[entry.index];
            // The wire identity is used only for ordered projection and the
            // self gate. In particular it is never assigned to geometry_entity.
            if ((trace.ammo_flags & 4u) == 0 &&
                proxy.wire_handle == trace.shooter_wire_handle)
                continue;
            {
                const float sc[3] = {
                    static_cast<float>(proxy.position_q16.x) / io::kFp16One,
                    static_cast<float>(proxy.position_q16.y) / io::kFp16One,
                    static_cast<float>(proxy.position_q16.z) / io::kFp16One};
                const int32_t proxy_bound = std::max(proxy.bound_radius_q16, 0);
                const float r = static_cast<float>(proxy_bound + 0x18000 +
                                                   effective_radius) /
                                io::kFp16One;
                if (!round_broad_phase(p0f, p1f, sc, r)) continue;
            }
            if (profile_trace) trace_profile_.person_survivors++;
            ProjectileHit eh;
            int32_t hit_distance = 0;
            if (!trace_torso_fallback(proxy.position_q16,
                                      proxy.uniform_scale_q16, eh, hit_distance))
                continue;
            finish_person_hit(eh, hit_distance); // geometry_entity stays invalid
            break;
        }

        const PersonSlot &slot = persons_[entry.index];
        // The witnessed person-slot gate [orig:
        // Physics_RaycastAgainstProximityList @ 0x4e4a30]. This walk had lost
        // it, so every organic in the mission built a full pose view per
        // ROUND per TICK (~0.9 ms each with rounds in flight - the sustained
        // full-auto collapse). The pad covers the bone spread beyond the
        // capsule center plus the ray's effective radius.
        {
            // Gate around the capsule MIDDLE (feet + 0.9u, the torso
            // fallback's own center) so head bones ~1.8u above the feet stay
            // inside; radius = capsule + bone spread + the ray's effective
            // radius. Generous by design: a false pass only costs the narrow
            // test it always ran before.
            const float sc[3] = {
                static_cast<float>(slot.x) / io::kFp16One,
                static_cast<float>(slot.y) / io::kFp16One,
                static_cast<float>(slot.z + kOrganicCenterZQ16) / io::kFp16One};
            const float r = static_cast<float>(slot.radius + 0x18000 +
                                               effective_radius) /
                            io::kFp16One;
            if (!round_broad_phase(p0f, p1f, sc, r)) continue;
        }
        if (profile_trace) trace_profile_.person_survivors++;
        // A header-only visual client keeps remote pool-0 organics in ClientState;
        // their decoded person proxies (folded into this same ordered walk) serve
        // instead. Suppress any non-local native organic from debug/complete-BMS
        // construction so it cannot duplicate that proxy.
        if (wire_projected && slot.h != world.cached.local_player) continue;
        if (ignored(slot.h)) continue;
        const Entity *e = world.registry.get(slot.h);
        if (e == nullptr || e->kind != EntityKind::Organic || e->hidden ||
            (e->engine_flags & 0x02000001u) != 0)
            continue;
        // Person self-group immunity: candidate refNum vs the SHOOTER's
        // refNum [orig: @ 0x4e4688-0x4e46a3].
        if (e->ref_num != 0 && owner_ref_num != 0 && e->ref_num == owner_ref_num)
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
        const CollisionTargetView *posed = trace_target_view(world, slot.h);
        if (posed != nullptr && posed->live_section_pose) {
            live_pose_available = true;
            const CollisionModel &person_model = *posed->model;
            for (int32_t bone = static_cast<int32_t>(person_model.sections.size()) - 1;
                 bone >= 0; --bone) {
                const uint32_t bit = 1u << (static_cast<uint32_t>(bone) & 31u);
                if ((e->section_mask & bit) != 0) continue;
                const CollisionSection &sec = person_model.sections[bone];
                if (sec.radius < 0) continue; // host/test sentinel; authored zero is valid
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
                const int32_t hit_radius =
                    person_effective_radius(bone, sec.radius, effective_radius);
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
        if (!live_pose_available &&
            !trace_torso_fallback(
                FixedVec3{to_fixed(e->position.x), to_fixed(e->position.y),
                          to_fixed(e->position.z)},
                e->uniform_scale_q16,
                eh, hit_distance))
            continue;

        eh.geometry_entity = e->handle;
        finish_person_hit(eh, hit_distance);
        break;
    }

    if (profile_trace) trace_profile_.person_us += prof_now() - prof_t;
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
        // Persons walk their own candidate list testing building-kind entries
        // with no distance prefilter. [orig: the def type 1/3 branch
        // @ 0x4b3e5f-0x4b3f93]
        const int32_t point[3] = {pt.x, pt.y, pt.z};
        query_candidate_blink_boxes_at_point(world, ent.handle, point, accum);
    } else if (ent.kind != EntityKind::Building) {
        // Everything else non-building tests the building prefix of the static
        // table. [orig: the def-null / other-type loops @ 0x4b3e6b / 0x4b3fa0,
        // reject radius = building radius + the 0.5u query radius]
        for (int32_t i = 0; i < static_building_count_; ++i) {
            const StaticSlot &s = statics_[i];
            if (s.h == ent.handle) continue; // [orig: the self check @ 0x4b3f13]
            const int32_t sx = static_slot_coord_q16(s.x);
            const int32_t sy = static_slot_coord_q16(s.y);
            const int32_t sz = static_slot_coord_q16(s.z);
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
        const int32_t sx = static_slot_coord_q16(s.x);
        const int32_t sy = static_slot_coord_q16(s.y);
        const int32_t sz = static_slot_coord_q16(s.z);
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

void CollisionWorld::query_candidate_blink_boxes_at_point(
        World &world, EntityHandle source, const int32_t pos[3],
        BlinkAccum &accum) {
    // [orig: terrain_sector_compute_lighting @ 0x5c7550 — its first argument
    // is g_local_player_entity, and +0x1BC/+0x1C0 name that entity's fixed
    // candidate slice. The same walk appears in Entity_BuildProximityList's
    // person branch @ 0x4b3e5f-0x4b3f93.]
    accum.reset();
    CollisionPoint pt;
    pt.x = pos[0];
    pt.y = pos[1];
    pt.z = pos[2];
    const int32_t radius = 0x8000;

    int32_t count = 0;
    const EntityHandle *slice = candidate_slice(source, count);
    if (slice == nullptr) return;
    for (int32_t i = 0; i < count; ++i) {
        const EntityHandle candidate = slice[i];
        if (candidate == source) continue;
        const Entity *entity = world.registry.get(candidate);
        if (entity == nullptr || entity->kind != EntityKind::Building) continue;
        CollisionTargetView view;
        std::vector<CollisionMatrix> mats;
        if (const CollisionTargetView *target =
                    target_view(world, candidate, view, mats))
            collision_test_blink(*target, &pt, &radius, 1, accum);
    }
}

} // namespace opennova::world
