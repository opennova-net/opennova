#include "world/collision.h"

// Split out of collision.cpp (quality campaign W3-2). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// Line of sight and ground casts: the terrain raycast legs, raycast_clear, the
// sound LOS/occlusion queries and the static-segment test.

#include <cmath>
#include <terrain/height_field.h>
#include <terrain/terrain_raycast.h>

#include "collision_detail.h"

#include "world/world.h"

namespace opennova::world {

using namespace detail; // the shared fixed-point helpers, unqualified as before

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
            if (!target_bound(world, ch, bound_pos, bound_radius)) continue;
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
                if (out_hit_entity) *out_hit_entity = ch;
            }
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

// The LOS march calls the point sampler once per 4-unit texel step — up to
// ~250 times per ray, three rays per re-probed entity per frame (§3.4) — so
// the per-sample work must stay retail-cheap (the original's inner loop is a
// flat heightmap index). The sector half of the world->source transform is
// invariant across the ~128 steps a ray spends inside one 512 u sector, so
// the context memoizes coords_resolve_sector and per sample only derives the
// sector-local offset. The sector index comes from the EXACT fixed-point
// coordinate (arithmetic >>25 == floor(world/512)); the local offset is a
// <2^25 integer whose float form is exact to ~1e-5 u — at least as precise as
// the previous whole-coordinate /65536.0f float path it replaces.
struct LosSamplerCtx {
    const terrain::TerrainHeightField *field = nullptr;
    int32_t cached_sx = INT32_MIN;
    int32_t cached_sz = INT32_MIN;
    bool sector_valid = false;
    float base_x = 0.0f; // quadrant offset (source/atlas units)
    float base_z = 0.0f;
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
        ctx.base_x = static_cast<float>(sector.quadrant_x);
        ctx.base_z = static_cast<float>(sector.quadrant_z);
    }
    if (!ctx.sector_valid) return s; // height 0, matching the invalid-coords branch
    // This LOS variant reads the nearest render-cache texel and expands its
    // half-unit byte (raw16 >> 7, then sample << 15), not the generic
    // floor-sampled full-precision height-field point contract.
    // [orig: Terrain_RaycastHeightmapHiRes @ 0x60c760 point callback]
    const float local_x = static_cast<float>(fx - (sx << 25)) / 65536.0f;
    const float local_z = static_cast<float>(fy - (sz << 25)) / 65536.0f;
    const int mask = f.dim - 1;
    const int hx = static_cast<int>(std::floor(ctx.base_x + local_x + 0.5f)) & mask;
    const int hz = static_cast<int>(std::floor(ctx.base_z + local_z + 0.5f)) & mask;
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
    // [orig: raycast_entity_collision @ 0x413760 -> Terrain_RaycastHeightmapHiRes_0
    // @ 0x60e710, called (start, end, end) so the ray end clips in place]
    LosSamplerCtx ctx;
    ctx.field = &field;
    terrain::TerrainRaycastSampler sampler;
    sampler.point = &los_field_point_cb;
    sampler.bilinear = &los_field_bilinear_cb;
    sampler.ctx = &ctx;
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
        // The bound gate runs on target_bound's cheap metadata BEFORE the
        // view/matrix build — the witnessed order [orig: the bound-sphere fold
        // @ 0x5387c4-0x5389a4 gates first; the @ 0x413060 clip core only runs
        // for gate survivors]. Same pos/radius inputs as the old post-view
        // gate, so the reject set is unchanged.
        int32_t bp[3];
        int32_t br = 0;
        if (!target_bound(world, e.handle, bp, br)) return false;
        if (abs32(ray.mid[0] - bp[0]) > br + static_cast<int32_t>(ray.half[0]) ||
            abs32(ray.mid[1] - bp[1]) > br + static_cast<int32_t>(ray.half[1]) ||
            abs32(ray.mid[2] - bp[2]) > br + static_cast<int32_t>(ray.half[2]))
            return false;
        if (ray_line_distance(ray.start, ray.dir, bp) > br) return false;
        CollisionTargetView view;
        std::vector<CollisionMatrix> mats;
        const CollisionTargetView *tv = target_view(world, e.handle, view, mats);
        if (tv == nullptr) return false;
        CollisionRay probe = ray; // model clip clips end in place; keep the walk ray whole
        return collision_raycast_model(*tv, probe);
    };

    // Pool passes over the per-tick tables — the same membership the witnessed
    // pool walk covers (@ 0x539a3a pool 2 then pool 1), without a whole-registry
    // sweep per ray. Entities absent from the tables (no model AND no bound)
    // could never block: target_bound returns false for them. Entries spawned
    // after this tick's table build are missed for at most one 62 Hz tick.
    if (tick_tables_ready()) {
        for (const StaticSlot &s : statics_) { // pass 1: pool-2 statics
            const Entity *e = world.registry.get(s.h);
            if (e != nullptr && blocked_by(*e)) return false;
        }
        for (const DynSlot &d : dynamics_) { // pass 2: dynamics (Item kind)
            if (d.h.pool() == 2) continue; // statics table already covered pool 2
            const Entity *e = world.registry.get(d.h);
            if (e != nullptr && blocked_by(*e)) return false;
        }
        return true;
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
        terrain_clear = true; // unwired terrain: nothing to block (embedder seam)
    }
    if (!terrain_clear) {
        if (le == nullptr || se == nullptr) {
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
            if (ce->kind != EntityKind::Building) continue; // [orig: itemDef+92 == 5]
            // Candidates standing on the listener/source are excluded (one
            // level). [orig: the +0x28 groundEntity checks @ 0x538843-0x538877]
            if (ce->ground_target == listener) continue;
            if (source.valid() && ce->ground_target == source) continue;

            int32_t bound_pos[3];
            int32_t bound_radius = 0;
            if (!target_bound(world, ch, bound_pos, bound_radius)) continue;
            if (abs32(ray.mid[1] - bound_pos[1]) >
                        bound_radius + broad_r + static_cast<int32_t>(ray.half[1]) ||
                abs32(ray.mid[0] - bound_pos[0]) >
                        bound_radius + broad_r + static_cast<int32_t>(ray.half[0]) ||
                abs32(ray.mid[2] - bound_pos[2]) >
                        bound_radius + broad_r + static_cast<int32_t>(ray.half[2]))
                continue;
            if (ray_line_distance(ray.start, ray.dir, bound_pos) > bound_radius + broad_r)
                continue;

            CollisionTargetView view;
            std::vector<CollisionMatrix> mats;
            const CollisionTargetView *tv = target_view(world, ch, view, mats);
            if (tv == nullptr) continue;
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
        // The iris path can cast nine sun rays per rendered frame. Preserve the
        // witnessed walker order here too: reject on cheap entity metadata
        // before resolving live section matrices for the few gate survivors.
        int32_t bound_pos[3];
        int32_t bound_radius = 0;
        if (!target_bound(world, s.h, bound_pos, bound_radius)) continue;
        if (abs32(ray.mid[1] - bound_pos[1]) >
                    bound_radius + broad_r + static_cast<int32_t>(ray.half[1]) ||
            abs32(ray.mid[0] - bound_pos[0]) >
                    bound_radius + broad_r + static_cast<int32_t>(ray.half[0]) ||
            abs32(ray.mid[2] - bound_pos[2]) >
                    bound_radius + broad_r + static_cast<int32_t>(ray.half[2]))
            continue;
        if (ray_line_distance(ray.start, ray.dir, bound_pos) > bound_radius + broad_r)
            continue;

        CollisionTargetView view;
        std::vector<CollisionMatrix> mats;
        const CollisionTargetView *tv = target_view(world, s.h, view, mats);
        if (tv == nullptr) continue;
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

} // namespace opennova::world
