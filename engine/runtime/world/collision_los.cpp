#include <runtime/world/collision.h>

// Line of sight and ground casts: the terrain raycast legs, raycast_clear, the
// sound LOS/occlusion queries and the static-segment test.

#include <cmath>
#include <runtime/terrain_query/height_field.h>
#include <runtime/terrain_query/terrain_raycast.h>

#include "collision_detail.h"
#include <runtime/world/geom.h>

#include <runtime/world/world.h>

namespace opennova::world {

using namespace detail; // the shared fixed-point helpers, unqualified as before

EntityHandle CollisionWorld::clip_segment_to_nearest_collision(
        World &world, EntityHandle source, const int32_t start[3],
        int32_t inout_end[3]) {
    // [orig: Entity_RaycastCollision @ 0x413760]
    EntityHandle nearest;
    const int32_t requested_end[3] = {inout_end[0], inout_end[1], inout_end[2]};

    // Terrain clips the endpoint before the collision ray is built. An indoors
    // source skips the heightfield because it has no representation of room
    // floors. The invalid source key is reserved by resolve_replica; its staged
    // Flags mirror drives the same gate.
    bool indoors = false;
    if (source.valid()) {
        if (const Entity *se = world.registry.get(source))
            indoors = (se->flags & kEntityFlagIndoors) != 0;
    } else if (replica_flags_ != nullptr) {
        indoors = (*replica_flags_ & kEntityFlagIndoors) != 0;
    }
    if (terrain != nullptr && terrain->valid() && !indoors) {
        // Retail aliases rayEnd and hitPoint. Besides clipping a crossing ray,
        // that preserves the column shortcut's unconditional terrain-height
        // write when a vertical probe begins below the surface.
        (void)terrain_clip_segment(*terrain, start, inout_end, inout_end);
    }

    CollisionRay ray;
    for (int axis = 0; axis < 3; ++axis) {
        ray.start[axis] = start[axis];
        ray.end[axis] = inout_end[axis];
    }
    ray.refresh();

    // Candidate narrow phase. [orig: the entity loop @ 0x4138c1-0x413abc]
    auto it = candidates_.find(source.packed);
    if (it != candidates_.end()) {
        const CandidateSlice slice = it->second;
        CollisionTargetView view;
        std::vector<CollisionMatrix> mats;
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

            // Broad phase vs the ray box + line distance. [orig: @ 0x413948-0x413a66]
            // The same entity/model metadata backs target_view below, but does
            // not require the live section-matrix provider.
            int32_t bound_pos[3];
            int32_t bound_radius = 0;
            if (!target_bound(world, ch, bound_pos, bound_radius, /*solid_only=*/true)) continue;
            if (abs32(ray.mid[1] - bound_pos[1]) >
                    bound_radius + static_cast<int32_t>(ray.half[1]) ||
                abs32(ray.mid[0] - bound_pos[0]) >
                    bound_radius + static_cast<int32_t>(ray.half[0]) ||
                abs32(ray.mid[2] - bound_pos[2]) >
                    bound_radius + static_cast<int32_t>(ray.half[2]))
                continue;
            if (ray_line_distance(ray.start, ray.dir, bound_pos) > bound_radius) continue;

            const CollisionTargetView *tv = target_view(world, ch, view, mats);
            if (tv == nullptr) continue;
            if (collision_raycast_model(*tv, ray)) {
                nearest = ch;
            }
        }
    }

    for (int axis = 0; axis < 3; ++axis) inout_end[axis] = ray.end[axis];
    if (ray_debug_enabled_) {
        const bool clipped = nearest.valid() ||
                             inout_end[0] != requested_end[0] ||
                             inout_end[1] != requested_end[1] ||
                             inout_end[2] != requested_end[2];
        ray_debug_record(RayDebugCategory::kUncategorized, world.logic_tick,
                         start, requested_end, clipped ? inout_end : nullptr,
                         clipped ? kRayDebugHit : kRayDebugClear);
    }
    return nearest;
}

int32_t CollisionWorld::raycast_ground(World &world, EntityHandle source, const int32_t pos[3],
                                       int32_t dx, int32_t dy, int32_t z_up, int32_t z_drop,
                                       EntityHandle *out_hit_entity) {
    // [orig: Entity_RaycastGroundHeight(AndObject) @ 0x4142c0/0x414320 ->
    // Entity_RaycastCollision @ 0x413760]
    const int32_t start[3] = {pos[0] + dx, pos[1] + dy, pos[2] + z_up};
    int32_t end[3] = {start[0], start[1], start[2] - z_drop};
    const RayDebugScope ray_scope(*this, RayDebugCategory::kGroundProbe);
    const EntityHandle hit =
            clip_segment_to_nearest_collision(world, source, start, end);
    if (out_hit_entity != nullptr) *out_hit_entity = hit;
    return end[2];
}

// The LOS terrain leg [orig: Terrain_RaycastHeightmapHiRes @ 0x60c760 — now the
// faithful LOS-variant port terrain_raycast_los_clear; this helper shipped on
// the AI slice with the 0x60e710 sibling as a boolean stand-in, closed here].
// Engine (X,Y) -> field (x, -y), the grounding convention every world-side
// sampler call uses; every sample kHeight (the runtime height field keeps
// retail's clamp-to-edge "terrain forever" semantics). The march's point
// callback is the NEAREST texel sample (retail reads its render tile cache's
// 0.5u-quantized byte there — the note in terrain_query/terrain_raycast.h); the
// endpoint prechecks use the bilinear column [orig: Terrain_GetHeightAtPosition
// @ 0x606720].
namespace {


// The LOS march calls the point sampler once per 4-unit texel step — up to
// ~250 times per ray, three rays per re-probed entity per frame (§3.4) — so
// the per-sample work must stay retail-cheap (the original's inner loop is a
// flat heightmap index). The sector half of the world->source transform is
// invariant across the ~128 steps a ray spends inside one 512 u sector, so
// the context memoizes coords_resolve_sector and per sample only derives the
// sector-local offset. The sector index comes from the EXACT fixed-point
// coordinate (arithmetic >>25 == floor(world/512)); the local offset remains
// Q16 so nearest-texel selection is one exact integer add/shift per axis.
struct LosSamplerCtx {
    const terrain::TerrainHeightField *field = nullptr;
    int32_t cached_sx = INT32_MIN;
    int32_t cached_sz = INT32_MIN;
    bool sector_valid = false;
    int32_t base_x = 0; // integer quadrant offset (source/atlas units)
    int32_t base_z = 0;
};

terrain::TerrainRaycastSample los_field_point_cb(void *vctx, int32_t x, int32_t y) {
    LosSamplerCtx &ctx = *static_cast<LosSamplerCtx *>(vctx);
    const terrain::TerrainHeightField &f = *ctx.field;
    terrain::TerrainRaycastSample s;
    s.kind = terrain::TerrainRaycastSample::kHeight;
    s.height_1616 = 0;
    // world = (x, -y) / 65536; sector = floor(world / 512). -y stays clear of
    // overflow: march coordinates are bounded by mission extent + the budgeted
    // overshoot, far under 2^31.
    const int32_t fx = x;
    const int32_t fy = -y;
    const int32_t sx = fx >> 25;
    const int32_t sz = fy >> 25;
    if (sx != ctx.cached_sx || sz != ctx.cached_sz) {
        ctx.cached_sx = sx;
        ctx.cached_sz = sz;
        const terrain::CoordsSectorResolve sector = terrain::coords_resolve_sector(
            f.layout, sx, sz, terrain::coords_runtime_options());
        ctx.sector_valid = sector.valid && f.valid();
        ctx.base_x = sector.quadrant_x;
        ctx.base_z = sector.quadrant_z;
    }
    if (!ctx.sector_valid) return s; // height 0, matching the invalid-coords branch
    // This LOS variant reads the nearest 1 u render-cache texel and expands
    // its half-unit byte (raw16 >> 7, then sample << 15), not the generic
    // floor-sampled full-precision height-field point contract.
    // [orig: Terrain_RaycastHeightmapHiRes @ 0x60c760 point callback — retail
    //  samples its 128x128 4 u cache tile by truncation ((local >> 18) & 0x7F)
    //  with a +1 u start bias (@ 0x60c760 tile index); this port's nearest
    //  1 u texel of the runtime height field is the D-AI-7 open delta]
    // quadrant_* is integral and the sector-local Q16 coordinate is always
    // nonnegative, so floor(base + local + 0.5) is exactly the integer
    // nearest-sample fold below. Besides avoiding two float conversions and
    // floor calls per four-unit march step, the int64 sector product avoids a
    // signed-left-shift edge for negative sector coordinates.
    const int32_t local_x_q16 = static_cast<int32_t>(
            static_cast<int64_t>(fx) - static_cast<int64_t>(sx) * (int64_t{1} << 25));
    const int32_t local_z_q16 = static_cast<int32_t>(
            static_cast<int64_t>(fy) - static_cast<int64_t>(sz) * (int64_t{1} << 25));
    const int mask = f.dim - 1;
    const int hx = (ctx.base_x + ((local_x_q16 + 0x8000) >> 16)) & mask;
    const int hz = (ctx.base_z + ((local_z_q16 + 0x8000) >> 16)) & mask;
    const uint8_t cache_height = static_cast<uint8_t>(f.heightmap[hz * f.dim + hx] >> 7);
    s.height_1616 = static_cast<int32_t>(cache_height) << 15;
    return s;
}

terrain::TerrainRaycastSample los_field_bilinear_sample(const terrain::TerrainHeightField &f,
                                                        int32_t x, int32_t y) {
    const float wx = static_cast<float>(x) / 65536.0f;
    const float wz = -static_cast<float>(y) / 65536.0f; // engine Y -> sampler z
    const float h = terrain::height_field_height_world_bilinear(f, wx, wz);
    terrain::TerrainRaycastSample s;
    s.kind = terrain::TerrainRaycastSample::kHeight;
    s.height_1616 = static_cast<int32_t>(h * 65536.0f);
    return s;
}

terrain::TerrainRaycastSample los_field_bilinear_cb(void *vctx, int32_t x, int32_t y) {
    const LosSamplerCtx &ctx = *static_cast<const LosSamplerCtx *>(vctx);
    return los_field_bilinear_sample(*ctx.field, x, y);
}

} // namespace

bool los_terrain_blocked(const terrain::TerrainHeightField &field, const int32_t a[3],
                         const int32_t b[3]) {
    LosSamplerCtx ctx;
    ctx.field = &field;
    terrain::TerrainRaycastSampler sampler;
    sampler.point = &los_field_point_cb;
    sampler.bilinear = &los_field_bilinear_cb;
    sampler.ctx = &ctx;
    return !terrain::terrain_raycast_los_clear(sampler, a, b);
}

bool terrain_clip_segment(const terrain::TerrainHeightField &field, const int32_t a[3],
                          const int32_t b[3], int32_t out_hit[3]) {
    // [orig: Entity_RaycastCollision @ 0x413760 -> Terrain_RaycastHeightmapHiRes_0
    // @ 0x60e710, called (start, end, end) so the ray end clips in place]
    LosSamplerCtx ctx;
    ctx.field = &field;
    terrain::TerrainRaycastSampler sampler;
    sampler.point = &los_field_point_cb;
    sampler.bilinear = &los_field_bilinear_cb;
    sampler.ctx = &ctx;
    return terrain::terrain_raycast_refined(sampler, a, b, out_hit);
}

bool terrain_column_height(const terrain::TerrainHeightField *field, int32_t x, int32_t y,
                           int32_t &height) {
    if (field == nullptr) return false;
    const int32_t start[3] = {x, y, 0x7FFF0000};
    int32_t end[3] = {x, y, -0x7FFF0000};
    const int32_t before = end[2];
    terrain_clip_segment(*field, start, end, end);
    if (end[2] == before) return false;
    height = end[2];
    return true;
}

int32_t terrain_settle_clearance(const terrain::TerrainHeightField *field, const int32_t pos[3],
                                 int32_t capsule_bottom, bool indoors) {
    // [orig: Entity_MovementCollisionResolver @0x4B3D6E..0x4B3DA9; the probe
    // Entity_RaycastGroundHeightAndObject @0x4B3D95 over no candidates]
    const int32_t feet_z = pos[2] - capsule_bottom;
    const int32_t start[3] = {pos[0], pos[1], ground_probe_origin_z(pos[2])};
    int32_t end[3] = {start[0], start[1], start[2] - 0x20000};
    if (field != nullptr && field->valid() && !indoors) (void)terrain_clip_segment(*field, start, end, end);
    return feet_z - end[2];
}

namespace {

// The radiused segment clip, boolean-only: does any TYPE-1 solid volume of
// `target` contain a span of [ray.start, ray.end] under the per-plane radius
// term? [orig: Physics_RaycastAgainstEntityPool @ 0x538720 — plane eval
// (dot >> 14) + dist - radius with strict > 0 = outside (0 counts inside,
// unlike the 0x413060 clip's >= 0); both-outside-a-plane = miss @ 0x538e37;
// a straddle splits at |d0u| / (|d0u| + |d1u|) computed on the UNRADIUSED
// distances with a truncating divide, moving the start when d0u >= 0 else
// the end @ 0x538e3d-0x538f06.] The LOS callers pass the height offset as
// the radius (the frameless-callee arg-slot reuse): ray 1 radius 0, ray 2
// radius -0x8000 — planes read 0.5u THINNER, the mechanism that lets ray 2
// clear near-miss walls the unshifted segment grazes. Per plane, a nonzero
// BPLN flags BYTE (the +0 word's low byte) substitutes the max(radius, 0)
// clamp for the raw radius, so only flag-0 planes read thin on ray 2
// [orig: the flag-byte branch @ 0x538d00; flagged arm subtracts the clamped
// register, flag-0 arm the raw arg @ 0x538d4b/0x538dd6] (the D-SND-9 closure).
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
                const int32_t plane_r =
                        (pl.flags & 0xFF) != 0 ? (radius > 0 ? radius : 0) : radius;
                const int32_t d0 = d0u - plane_r;
                const int32_t d1 = d1u - plane_r;
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

EntityHandle los_walker_parent(const Entity *e, bool parent_cleared) {
    if (e == nullptr) return EntityHandle{};
    if (parent_cleared) return e->mounted_child;
    return e->mount_target.valid() ? e->mount_target : e->mounted_child;
}

bool CollisionWorld::raycast_clear(World &world, const int32_t a[3], const int32_t b[3],
                                   EntityHandle exclude_a, EntityHandle exclude_b,
                                   EntityHandle parent_a, EntityHandle parent_b) {
    const bool clear =
            raycast_clear_impl(world, a, b, exclude_a, exclude_b, parent_a, parent_b, false);
    if (ray_debug_enabled_) {
        ray_debug_record(RayDebugCategory::kUncategorized, world.logic_tick, a, b,
                         nullptr, clear ? kRayDebugClear : kRayDebugBlocked);
    }
    return clear;
}

bool CollisionWorld::raycast_clear_cached(World &world, const int32_t a[3],
                                          const int32_t b[3], EntityHandle exclude_a,
                                          EntityHandle exclude_b, EntityHandle parent_a,
                                          EntityHandle parent_b) {
    const bool clear =
            raycast_clear_impl(world, a, b, exclude_a, exclude_b, parent_a, parent_b, true);
    if (ray_debug_enabled_) {
        ray_debug_record(RayDebugCategory::kUncategorized, world.logic_tick, a, b,
                         nullptr, clear ? kRayDebugClear : kRayDebugBlocked);
    }
    return clear;
}

bool CollisionWorld::raycast_clear_impl(World &world, const int32_t a[3],
                                        const int32_t b[3], EntityHandle exclude_a,
                                        EntityHandle exclude_b, EntityHandle parent_a,
                                        EntityHandle parent_b,
                                        bool cache_target_views) {
    // [orig: Physics_RaycastTerrainAndSectors @ 0x539910, TRUE = clear; the LOS
    // callers pass ray radius 0, so the witnessed thick-ray Z-drop (@ 0x53994e)
    // and volume inflation are no-ops and are folded out here.]
    // The terrain/sector split is attributed only for the replication fan's
    // cached epoch (the one caller that ever profiled it); the AI's own rays
    // stay inside their combat span.
    devtools::ProfileLap lap(cache_target_views ? world.profile : nullptr);
    last_los_sector_candidates_ = 0;
    const Entity *ea = world.registry.get(exclude_a);
    const Entity *eb = world.registry.get(exclude_b);

    // Terrain leg — skipped when BOTH entities are INDOORS (the heightmap has no
    // interiors) [orig: the Flags & 0x800000 pair gate @ 0x53994c]. The original's
    // null-entity buried-endpoint variant (@ 0x53999a) has no caller on the LOS
    // chain and is not modeled.
    const bool both_indoors = ea != nullptr && eb != nullptr &&
                              (ea->flags & kEntityFlagIndoors) != 0 &&
                              (eb->flags & kEntityFlagIndoors) != 0;
    const bool terrain_hit = !both_indoors && terrain != nullptr && terrain->valid() &&
                             los_terrain_blocked(*terrain, a, b);
    lap.mark(devtools::Slot::SIM_REPLICATION_ENTITY_LOS_TERRAIN);
    if (terrain_hit) return false; // [orig: heightmap hit -> return 0 @ 0x539968]

    const devtools::ProfileScope sector_scope(
            cache_target_views ? world.profile : nullptr,
            devtools::Slot::SIM_REPLICATION_ENTITY_LOS_SECTOR);

    // Sector leg [orig: Physics_RaycastAgainstEntityPool @ 0x538720, pool 2 then pool 1
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
    // skip the four excluded entities A, B, parentA, parentB and anything
    // standing on A or B (+0x28) [orig: @0x53882F..0x538877], bound-sphere broad
    // phase (the segment box + line-distance fold of @ 0x5387c4-0x5389a4), then
    // the TYPE-1 volume convex clip (the shared @ 0x413060 core). A hit blocks —
    // the original keeps walking to clip the nearest point; the boolean result is
    // identical (@ 0x5390e6 miss_result = 0).
    auto blocked_by_bound = [&](const Entity &e, const int32_t bp[3],
                                int32_t br) -> bool {
        if ((e.flags & 1u) != 0) return false;
        if ((e.engine_flags & 0x8000000u) != 0) return false;
        if (e.handle == exclude_a || e.handle == exclude_b) return false;
        if ((parent_a.valid() && e.handle == parent_a) ||
            (parent_b.valid() && e.handle == parent_b))
            return false;
        if (e.ground_target.valid() &&
            (e.ground_target == exclude_a || e.ground_target == exclude_b))
            return false;
        // The bound gate runs on target_bound's cheap metadata BEFORE the
        // view/matrix build — the witnessed order [orig: the bound-sphere fold
        // @ 0x5387c4-0x5389a4 gates first; the @ 0x413060 clip core only runs
        // for gate survivors]. Same pos/radius inputs as the old post-view
        // gate, so the reject set is unchanged.
        if (abs32(ray.mid[0] - bp[0]) > br + static_cast<int32_t>(ray.half[0]) ||
            abs32(ray.mid[1] - bp[1]) > br + static_cast<int32_t>(ray.half[1]) ||
            abs32(ray.mid[2] - bp[2]) > br + static_cast<int32_t>(ray.half[2]))
            return false;
        if (ray_line_distance(ray.start, ray.dir, bp) > br) return false;
        CollisionTargetView view;
        std::vector<CollisionMatrix> mats;
        const CollisionTargetView *tv = cache_target_views
                ? trace_target_view(world, e.handle)
                : target_view(world, e.handle, view, mats);
        if (tv == nullptr) return false;
        CollisionRay probe = ray; // model clip clips end in place; keep the walk ray whole
        return collision_raycast_model(*tv, probe);
    };

    auto blocked_by = [&](const Entity &e) -> bool {
        ++last_los_sector_candidates_;
        int32_t bp[3];
        int32_t br = 0;
        if (!target_bound(world, e.handle, bp, br, /*solid_only=*/true)) return false;
        return blocked_by_bound(e, bp, br);
    };

    // Replication declares a stable post-movement epoch and prepares an exact
    // spatial index once. Candidate bounds were captured in that epoch; this
    // changes only the broad-phase search, then runs the same flags,
    // exclusions, sphere gate, live matrix view, and type-1 convex clip.
    if (cache_target_views && stable_los_index_ready_) {
        const std::vector<uint32_t> &indices = stable_los_candidates_for_ray(ray);
        for (uint32_t index : indices) {
            ++last_los_sector_candidates_;
            const StableLosCandidate &candidate = stable_los_candidates_[index];
            const Entity *e = world.registry.get(candidate.h);
            if (e != nullptr &&
                blocked_by_bound(*e, candidate.pos, candidate.radius))
                return false;
        }
        return true;
    }

    // The pool passes in the witnessed order: every pool-2 entry, then every
    // pool-1 entry once pool 2 came back clear. Each walk reads the pool's own
    // used count, not the per-tick proximity tables (whose pool-2 count also
    // stops at 1199), so an entry spawned after this tick's table build
    // blocks at once [orig: Physics_RaycastTerrainAndSectors pool 2
    // @0x539A16..0x539A30, `jz loc_53996A` @0x539A3A, pool 1
    // @0x539A40..0x539A5A; the table's `count < 1199` gate @0x4B94CB].
    if (tick_tables_ready()) {
        bool blocked = false;
        const auto walk = [&](const Entity &e) {
            if (!blocked && blocked_by(e)) blocked = true;
        };
        world.registry.for_each_in_pool(2, walk);
        if (blocked) return false;
        world.registry.for_each_in_pool(1, walk);
        return !blocked;
    }
    // Un-ticked worlds (headless callers that never ran the per-tick table
    // build) keep the registry sweep so LOS still sees their entities.
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

bool CollisionWorld::entity_los_clear(World &world, EntityHandle listener, EntityHandle source,
		const int32_t start_in[3], const int32_t end_in[3], int32_t height_offset, bool all_types,
		bool query_parent_cleared) {
    const Entity *se = source.valid() ? world.registry.get(source) : nullptr;
    // The walker's two parent slots: per endpoint entity, parentEntity (+0x16C,
    // the seat mount) wins over mountedChild (+0x268) [orig:
    // Physics_RaycastFindCollisionEntity @0x539ab8..0x539b10 -> ctx[19]/ctx[20]].
    return entity_los_clear_impl(world, listener, source, se != nullptr,
            se != nullptr && (se->flags & kEntityFlagIndoors) != 0, los_walker_parent(se),
            start_in, end_in, height_offset, all_types, query_parent_cleared);
}

bool CollisionWorld::wire_person_los_clear(World &world, EntityHandle listener,
		const LosWireEndpoint &endpoint, const int32_t start_in[3], const int32_t end_in[3],
		int32_t height_offset, bool all_types) {
	// No registry entity names the endpoint, so the walker's endpoint and
	// standing-on exclusions have nothing to match; its parent slot is the
	// row's own.
	return entity_los_clear_impl(world, listener, EntityHandle{}, true, endpoint.indoors,
			endpoint.parent, start_in, end_in, height_offset, all_types, false);
}

bool CollisionWorld::entity_los_clear_impl(World &world, EntityHandle listener,
		EntityHandle source, bool endpoint_present, bool endpoint_indoors, EntityHandle parent_b,
		const int32_t start_in[3], const int32_t end_in[3], int32_t height_offset, bool all_types,
		bool query_parent_cleared) {
	const Entity *le = listener.valid() ? world.registry.get(listener) : nullptr;
    // The blast sweep nulls the query entity's parentEntity for the call
    // (`mov [edi+16Ch], 0` @0x4eb158, restored @0x4eb16c), so its slot holds
    // only the mountedChild there.
    const EntityHandle parent_a = los_walker_parent(le, query_parent_cleared);

    // --- Terrain leg. [orig: Physics_CheckTerrainLineOfSight @ 0x53b080] ---
    bool terrain_clear = false;
    if (le != nullptr && endpoint_present && (le->flags & kEntityFlagIndoors) != 0 &&
        endpoint_indoors) {
        terrain_clear = true; // both indoors: no heightfield test [orig: @ 0x53b0a0]
    }
    if (!terrain_clear && (terrain == nullptr || !terrain->valid())) {
        terrain_clear = true; // unwired terrain: nothing to block (embedder seam)
    }
    if (!terrain_clear) {
        if (le == nullptr || !endpoint_present) {
            // No-entity path: both UNSHIFTED endpoints must sit above the
            // bilinear surface for the ray to run at all — an under-surface
            // endpoint reads as terrain-clear. [orig: @ 0x53b0f1-0x53b100]
            const terrain::TerrainRaycastSample hs =
                    los_field_bilinear_sample(*terrain, start_in[0], start_in[1]);
            const terrain::TerrainRaycastSample he =
                    los_field_bilinear_sample(*terrain, end_in[0], end_in[1]);
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

	// --- Entity leg: listener candidates; sound filters to buildings, USE admits all types.
	// [orig: Physics_RaycastFindCollisionEntity @ 0x539a70 with allowAllTypes = 0 —
	// the def-type-5 filter @ 0x539baf; walker Physics_RaycastAgainstEntityPool
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
			if (ch == listener)
				continue;
			const Entity *ce = world.registry.get(ch);
            if (ce == nullptr) continue;
			// The walker's exclusion set is ctx[17..20]: the query entity, the
			// endpoint entity and both parent slots. The endpoint skip is
			// unconditional: a USE ray is never blocked by the hull of the
			// vehicle it targets [orig: Physics_RaycastAgainstEntityPool
			// @0x538832..0x538859]. The EWeap clause @0x539B85..0x539B98 gates
			// a candidate whose groundEntity (+0x28) IS the endpoint (the gun
			// standing on it), which the ground skips below already cover.
			if (source.valid() && ch == source) continue;
			if (parent_a.valid() && ch == parent_a) continue;
			if (parent_b.valid() && ch == parent_b) continue;
			if ((ce->flags & 1u) != 0) continue; // [orig: @ 0x538792]
			// The 0x8000000 skip is sound-only: the walker's includeFlagged arg
			// is the caller's allTypes [orig: push ebp @0x539ba4 -> @0x5387ac..0x5387b4].
            if (!all_types && (ce->engine_flags & 0x8000000u) != 0) continue;
			if (!all_types && ce->kind != EntityKind::Building)
				continue; // [orig: itemDef+92 == 5]
			// Candidates standing on the listener/source are excluded (one
            // level). [orig: the +0x28 groundEntity checks @ 0x538843-0x538877]
            if (ce->ground_target == listener) continue;
            if (source.valid() && ce->ground_target == source) continue;

            if (entity_blocks_segment(world, ch, ray, clip_radius, broad_r))
                return false; // blocked
        }
    }
    return true;
}

bool CollisionWorld::candidate_segment_hits_solid(
        World &world, EntityHandle source, const int32_t a[3],
        const int32_t b[3], int32_t radius) {
    // [orig: Physics_RaycastFindCollisionEntity @ 0x539a70 with allowAllTypes = 1
    // -> Physics_RaycastAgainstEntityPool @ 0x538720. entity_a and entity_b are both
    // the local/query entity, and the walker consumes entity_a's fixed
    // +0x1BC/+0x1C0 slice in stored order.]
    int32_t slice_count = 0;
    const EntityHandle *slice = candidate_slice(source, slice_count);
    const bool hits = candidate_slice_segment_hits_solid(
            world, slice, slice_count, source, a, b, radius);
    if (ray_debug_enabled_) {
        ray_debug_record(RayDebugCategory::kUncategorized, world.logic_tick, a, b,
                         nullptr, hits ? kRayDebugBlocked : kRayDebugClear);
    }
    return hits;
}

// The candidate-walk skip gate shared by the sound and sun ray walkers:
// the query entity itself, dead rows, flag-27 rows, and (because
// Physics_RaycastFindCollisionEntity passes source as both exclusion entities) a
// direct child standing on the source are never blockers.
static bool segment_candidate_skipped(World &world, EntityHandle candidate,
                                      EntityHandle source_registry_twin) {
    if (source_registry_twin.valid() && candidate == source_registry_twin) return true;
    const Entity *entity = world.registry.get(candidate);
    if (entity == nullptr || (entity->flags & 1u) != 0) return true;
    if ((entity->engine_flags & 0x8000000u) != 0) return true;
    return source_registry_twin.valid() && entity->ground_target == source_registry_twin;
}

// The bound-sphere AABB reject both walkers run on cheap entity metadata
// before any section matrix resolves: the ray's midpoint against the
// candidate bound inflated by the broad radius, Y first as witnessed.
static bool segment_bound_aabb_rejects(const CollisionRay &ray, const int32_t bound_pos[3],
                                       int32_t reach) {
    return abs32(ray.mid[1] - bound_pos[1]) > reach + static_cast<int32_t>(ray.half[1]) ||
           abs32(ray.mid[0] - bound_pos[0]) > reach + static_cast<int32_t>(ray.half[0]) ||
           abs32(ray.mid[2] - bound_pos[2]) > reach + static_cast<int32_t>(ray.half[2]);
}

bool CollisionWorld::candidate_slice_segment_hits_solid(
        World &world, const EntityHandle *slice, int32_t slice_count,
        EntityHandle source_registry_twin, const int32_t a[3],
        const int32_t b[3], int32_t radius) {
    if (slice == nullptr || slice_count <= 0) return false;
    CollisionRay ray;
    ray.start[0] = a[0];
    ray.start[1] = a[1];
    ray.start[2] = a[2];
    ray.end[0] = b[0];
    ray.end[1] = b[1];
    ray.end[2] = b[2];
    ray.refresh();
    const int32_t broad_r = radius > 0 ? radius : 0; // [orig: @ 0x538752 clamp]

    for (int32_t i = 0; i < slice_count; ++i) {
        const EntityHandle candidate = slice[i];
        if (segment_candidate_skipped(world, candidate, source_registry_twin)) continue;
        if (entity_blocks_segment(world, candidate, ray, radius, broad_r)) return true;
    }
    return false;
}

bool CollisionWorld::entity_blocks_segment(World &world, EntityHandle candidate,
                                           const CollisionRay &ray,
                                           int32_t radius, int32_t broad_r) {
    // The iris path can cast nine sun rays per rendered frame. Preserve the
    // witnessed walker order here too: reject on cheap entity metadata
    // before resolving live section matrices for the few gate survivors.
    int32_t bound_pos[3];
    int32_t bound_radius = 0;
    if (!target_bound(world, candidate, bound_pos, bound_radius, /*solid_only=*/false))
        return false;
    const int32_t reach = bound_radius + broad_r;
    if (segment_bound_aabb_rejects(ray, bound_pos, reach)) return false;
    if (ray_line_distance(ray.start, ray.dir, bound_pos) > reach) return false;

    CollisionTargetView view;
    std::vector<CollisionMatrix> mats;
    const CollisionTargetView *tv = target_view(world, candidate, view, mats);
    if (tv == nullptr) return false;
    return sound_segment_blocked(*tv, ray, radius);
}

// [orig: Entity_ComputeSunVisibility @0x5c6800 — three rays toward the light over the entity's
//  candidate slice, (4 - blocked) * 0.25; slice source Terrain_RenderSectorEntitiesBySide @0x5c7fa5]
int CollisionWorld::candidate_slice_sun_blocked_rays(
        World &world, const EntityHandle *slice, int32_t slice_count,
        EntityHandle source_registry_twin, const int32_t a[3],
        const int32_t b[3]) {
    if (slice == nullptr || slice_count <= 0) return 0;

    CollisionRay ray;
    ray.start[0] = a[0];
    ray.start[1] = a[1];
    ray.start[2] = a[2];
    ray.end[0] = b[0];
    ray.end[1] = b[1];
    ray.end[2] = b[2];
    ray.refresh();

    // The three retail casts share endpoints and candidate order. Walk that
    // slice once and build each surviving target view once, while retaining a
    // separate exact clip verdict for every witnessed radius. This changes no
    // visibility result; it only removes three identical broad-phase walks and
    // repeated section-matrix construction from the render-frame hot path.
    bool blocked[3] = {false, false, false};
    int blocked_count = 0;
    CollisionTargetView view;
    std::vector<CollisionMatrix> mats;
    for (int32_t i = 0; i < slice_count && blocked_count < 3; ++i) {
        const EntityHandle candidate = slice[i];
        if (segment_candidate_skipped(world, candidate, source_registry_twin)) continue;

        int32_t bound_pos[3];
        int32_t bound_radius = 0;
        if (!target_bound(world, candidate, bound_pos, bound_radius, /*solid_only=*/false))
            continue;
        const int32_t line_distance =
                ray_line_distance(ray.start, ray.dir, bound_pos);
        bool radius_survives[3] = {false, false, false};
        bool any_survives = false;
        for (int r = 0; r < 3; ++r) {
            if (blocked[r]) continue;
            const int32_t radius = kSunOcclusionClipRadii[r];
            const int32_t broad_r = radius > 0 ? radius : 0;
            // The same reject entity_blocks_segment runs, once per witnessed
            // radius over the one line distance computed above.
            const int32_t reach = bound_radius + broad_r;
            if (segment_bound_aabb_rejects(ray, bound_pos, reach) || line_distance > reach)
                continue;
            radius_survives[r] = true;
            any_survives = true;
        }
        if (!any_survives) continue;

        const CollisionTargetView *tv = target_view(world, candidate, view, mats);
        if (tv == nullptr) continue;
        for (int r = 0; r < 3; ++r) {
            if (!radius_survives[r]) continue;
            if (sound_segment_blocked(*tv, ray, kSunOcclusionClipRadii[r])) {
                blocked[r] = true;
                ++blocked_count;
            }
        }
    }
    return blocked_count;
}

int CollisionWorld::sun_visibility_blocked_rays(World &world, const Entity &e,
                                                const int32_t sun_step_q16[3]) {
    // The casts walk the query entity's OWN proximity-candidate slice, not the
    // whole statics table: retail's ray walker iterates entity_a's +0x1BC arena
    // slice bounded by the +0x1C0 count, so only candidates whose inflated
    // bounding sphere overlaps the entity's bubble (the slice build's
    // radius + 4u/6u pad) can ever block its sun — a solid shading the
    // entity from far across the map leaves it in full sun. An entity with no
    // slice (statics, ineligible pool-1 rows, or the 16 sliceless
    // mission-start ticks) blocks nothing and keeps quality 4, which is exactly
    // retail's +0x1C0 == 0 skip. [orig: Entity_ComputeSunVisibility @ 0x5c6800
    // — the +0x1C0 gate @ 0x5c6808; Physics_RaycastFindCollisionEntity @ 0x539a70 —
    // the entity_a[+0x1BC]/[+0x1C0] entry walk]
    int32_t slice_count = 0;
    const EntityHandle *slice = candidate_slice(e.handle, slice_count);
    if (slice == nullptr || slice_count <= 0) return 0;

    // Origin = position + the RAW (unrotated) collision-bbox midpoint — the
    // same raw add the cat-2 trigger LOS endpoints take; zero when unstamped,
    // leaving the ray at the entity origin exactly like retail's zeroed pool
    // memory. [orig: Entity_ComputeSunVisibility @ 0x5c6800 — start block
    // @ 0x5c681f..0x5c6847]
    int32_t origin[3];
    entity_pos_fixed(e, origin);
    origin[0] += to_fixed(e.bbox_center.x);
    origin[1] += to_fixed(e.bbox_center.y);
    origin[2] += to_fixed(e.bbox_center.z);
    // End = origin + 200 u along the active light direction (the caller scales
    // the direction; sub-1.0 verticals were already clamped by the light
    // getter's consumers). [orig: end = start + dir*0xC8 @ 0x5c6850..0x5c6876]
    const int32_t end[3] = {origin[0] + sun_step_q16[0],
                            origin[1] + sun_step_q16[1],
                            origin[2] + sun_step_q16[2]};

    const int blocked = candidate_slice_sun_blocked_rays(
            world, slice, slice_count, e.handle, origin, end);
    if (ray_debug_enabled_) {
        // One event for the retail ray triple: the three casts share the
        // segment and differ only in clip radius.
        ray_debug_record(RayDebugCategory::kSunVisibility, world.logic_tick,
                         origin, end, nullptr,
                         blocked > 0 ? kRayDebugBlocked : kRayDebugClear);
    }
    return blocked;
}

int CollisionWorld::wire_sun_visibility_blocked_rays(
        World &world, uint16_t wire_handle, const FixedVec3 &position_q16,
        const FixedVec3 &bbox_center_q16,
        const int32_t sun_step_q16[3]) {
    int32_t slice_count = 0;
    const EntityHandle *slice = wire_candidate_slice(wire_handle, slice_count);
    if (slice == nullptr || slice_count <= 0) return 0;

    const int32_t origin[3] = {
        position_q16.x + bbox_center_q16.x,
        position_q16.y + bbox_center_q16.y,
        position_q16.z + bbox_center_q16.z};
    const int32_t end[3] = {origin[0] + sun_step_q16[0],
                            origin[1] + sun_step_q16[1],
                            origin[2] + sun_step_q16[2]};

    // A wire row may have an explicitly verified pool-1 registry twin. The
    // packed number itself is insufficient evidence because H and L are
    // independent domains (H=0 and L=0 are both valid at once).
    EntityHandle registry_twin;
    for (const WireDynamicCollisionProxy &dynamic : wire_dynamic_proxies_) {
        if (dynamic.wire_handle == wire_handle) {
            registry_twin = dynamic.registry_twin;
            break;
        }
        if (dynamic.wire_handle > wire_handle) break;
    }

    const int blocked = candidate_slice_sun_blocked_rays(
            world, slice, slice_count, registry_twin, origin, end);
    if (ray_debug_enabled_) {
        ray_debug_record(RayDebugCategory::kSunVisibility, world.logic_tick,
                         origin, end, nullptr,
                         blocked > 0 ? kRayDebugBlocked : kRayDebugClear);
    }
    return blocked;
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
    const bool ray1_clear = entity_los_clear(world, listener, source, listener_pos, end, 0);
    if (!ray1_clear)
        base = 2 * base + 0x50000; // ray 1 blocked compounds [orig: @ 0x5299b6]
    const bool ray2_clear = entity_los_clear(world, listener, source, listener_pos, end, -0x8000);
    if (ray_debug_enabled_) {
        ray_debug_record(RayDebugCategory::kSoundOcclusion, world.logic_tick,
                         listener_pos, end, nullptr,
                         ray1_clear ? kRayDebugClear : kRayDebugBlocked);
        ray_debug_record(RayDebugCategory::kSoundOcclusion, world.logic_tick,
                         listener_pos, end, nullptr,
                         ray2_clear ? kRayDebugClear : kRayDebugBlocked);
    }
    // The single final add. [orig: @ 0x5299e6-0x5299f3]
    return distance_q16 + (ray2_clear ? base : 2 * base + 0x50000);
}

} // namespace opennova::world
