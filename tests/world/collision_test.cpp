// World-object collision unit tests [orig: the Jointops.exe query/resolver set —
// Entity_TestCollisionSections @0x4aef90, Entity_RaycastCollisionModel @0x413060,
// Entity_ComputeBoneCollisionForce @0x4ae150, Entity_ProcessCollisionAndPlatformPhysics
// @0x4b2bd0, the proximity builders @0x4b9430/0x4b9340/0x4b8eb0] over hand-built
// volumes (docs/world/world-wac-ai-re.md §15):
//   * fixed matrix build/invert roundtrip (Q22 rows, translate-then-rotate inverse),
//   * blink point query: type-8-only filter, flags ^ 6 accumulation, the packed hit
//     encoding, indoors only on accum bit 2 (the V-letter-cleared authored bit),
//   * segment clip: entry-point exactness on an axis ray into a box volume,
//   * ground probe through a candidate model (standing on a box roof) + terrain,
//   * resolver wall push-out with prev-position gating, and the idle skip throttle,
//   * the read-only debug seams (debug_instances corner transform + range cap,
//     the local player's LocalResolveDebug capture).
#include <cstdint>
#include <cstdio>
#include <vector>

#include "terrain/height_field.h"
#include "world/collision.h"
#include "world/world.h"

using namespace opennova::world;
using opennova::terrain::TerrainHeightField;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr int32_t fx(double units) { return static_cast<int32_t>(units * 65536.0); }

// Flat 512x512 height field at a constant raw16 column (raw = units * 256).
struct Field {
    static constexpr int kDim = 512;
    std::vector<uint16_t> heightmap;
    std::vector<int> sector_grid;
    TerrainHeightField field;

    explicit Field(uint16_t raw16) : heightmap(kDim * kDim, raw16), sector_grid(256, 1) {
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
        field.layout.origin_x = 0;
        field.layout.origin_y = 0;
    }
};

// An axis-aligned box volume [(-hx,-hy,0) .. (hx,hy,height)] in section-local
// space: 6 outward Q14 planes, dist = -face offset (inside = dot>>14 + dist < 0).
CollisionModel box_model(int32_t type, uint32_t flags, double hx, double hy, double height) {
    CollisionModel m;
    auto plane = [&](int nx, int ny, int nz, double d) {
        CollisionPlane p;
        p.nx = static_cast<int16_t>(nx);
        p.ny = static_cast<int16_t>(ny);
        p.nz = static_cast<int16_t>(nz);
        p.dist = fx(d);
        m.planes.push_back(p);
    };
    plane(16384, 0, 0, -hx);
    plane(-16384, 0, 0, -hx);
    plane(0, 16384, 0, -hy);
    plane(0, -16384, 0, -hy);
    plane(0, 0, 16384, -height);
    plane(0, 0, -16384, 0.0);

    CollisionVolume v;
    v.type = type;
    v.flags = flags;
    v.min_x = fx(-hx);
    v.max_x = fx(hx);
    v.min_y = fx(-hy);
    v.max_y = fx(hy);
    v.min_z = 0;
    v.max_z = fx(height);
    v.plane_start = 0;
    v.plane_count = 6;
    m.volumes.push_back(v);

    CollisionSection s;
    s.volume_start = 0;
    s.volume_count = 1;
    m.sections.push_back(s);
    return m;
}

struct Rig {
    World world;
    CollisionWorld cw;
    Field field{0}; // ground plane at 0
    EntityHandle building;
    EntityHandle soldier;

    explicit Rig(CollisionModel model, double bx = 10.0, double by = 10.0) {
        world.registry.configure_pool(0, 8);
        world.registry.configure_pool(2, 8);
        cw.terrain = &field.field;

        // Occupy pool-2 slot 0 so the building's packed blink hit is non-zero
        // (slot 0 packs to 0 — the original shares that ambiguity).
        Entity filler;
        filler.kind = EntityKind::Marker;
        filler.net_id = 99;
        world.registry.spawn(2, filler);

        Entity b;
        b.kind = EntityKind::Building;
        b.net_id = 100;
        b.position = {static_cast<float>(bx), static_cast<float>(by), 0.0f};
        b.yaw = 90; // mission yaw 90 -> engine heading 0 (identity rotation)
        b.alive = true;
        building = world.registry.spawn(2, b);

        Entity s;
        s.kind = EntityKind::Organic;
        s.net_id = 1;
        s.position = {0.0f, 0.0f, 0.0f};
        s.alive = true;
        soldier = world.registry.spawn(0, s);

        const int32_t mid = cw.add_model(std::move(model));
        cw.assign_entity(building, mid);
        rebuild();
    }

    // Candidate slices only rebuild every 17th tick [orig: dword_B57C84 vs 0x10
    // @ 0x4c240f], so a test step ticks the tables 17 times to force one build.
    void rebuild() {
        for (int i = 0; i < 17; ++i) cw.build_tick_tables(world);
    }

    void move_soldier(double x, double y, double z) {
        Entity *s = world.registry.get(soldier);
        s->position = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
        rebuild();
    }
};

// ---------------------------------------------------------------------------
void test_matrix_roundtrip() {
    const int32_t pos[3] = {fx(5.0), fx(7.0), fx(2.0)};
    // Heading 1/4 turn (BAM 0x40000000): local +X -> world +Y.
    const CollisionMatrix m = collision_matrix_from_heading(0x40000000, pos);
    const int32_t local[3] = {fx(1.0), 0, 0};
    int32_t w[3];
    m.transform_point(local, w);
    CHECK(std::abs(w[0] - pos[0]) <= 2);
    CHECK(std::abs(w[1] - (pos[1] + fx(1.0))) <= 2);
    CHECK(std::abs(w[2] - pos[2]) <= 2);

    CollisionMatrix inv;
    m.invert_into(inv);
    int32_t back[3];
    // Inverse application = translate-then-rotate [orig: Math_TransformPointWithTranslation22]
    const int32_t tin[3] = {w[0] + inv.m[3], w[1] + inv.m[7], w[2] + inv.m[11]};
    (void)tin;
    // Round-trip via the same helpers the queries use:
    int32_t lp[3];
    {
        // world -> local
        const int32_t p[3] = {w[0], w[1], w[2]};
        int32_t tmp[3] = {p[0] + inv.m[3], p[1] + inv.m[7], p[2] + inv.m[11]};
        inv.rotate_point(tmp, lp);
    }
    CHECK(std::abs(lp[0] - local[0]) <= 4);
    CHECK(std::abs(lp[1] - local[1]) <= 4);
    CHECK(std::abs(lp[2] - local[2]) <= 4);
    (void)back;
}

// ---------------------------------------------------------------------------
void test_blink_query_and_refresh() {
    // Authored blink flags 0x3C = init 0x3E with the bit-1 letter cleared -> runtime
    // accum (0x3C ^ 6) = 0x3A carries bit 2 -> INDOORS. The 0x3E default does not.
    Rig indoors(box_model(8, 0x3C, 4.0, 4.0, 3.0));
    Entity *s = indoors.world.registry.get(indoors.soldier);

    indoors.move_soldier(10.0, 10.0, 0.5); // inside the blink box
    indoors.cw.refresh_blink(indoors.world, *s);
    CHECK((s->flags & kEntityFlagIndoors) != 0);
    CHECK(s->blink_hits[0] != 0);
    // Packed hit: ((section 0 & 0x1F) + (pool_index << 8)) << 12.
    const uint32_t expected =
        static_cast<uint32_t>(((0) + (indoors.building.slot() << 8)) << 12);
    CHECK(s->blink_hits[0] == expected);

    indoors.move_soldier(30.0, 30.0, 0.5); // far away
    indoors.cw.refresh_blink(indoors.world, *s);
    CHECK((s->flags & kEntityFlagIndoors) == 0);
    CHECK(s->blink_hits[0] == 0);

    // Default-authored flags 0x3E: hit recorded, but NOT indoors (bit 2 clear after ^6).
    Rig plain(box_model(8, 0x3E, 4.0, 4.0, 3.0));
    Entity *s2 = plain.world.registry.get(plain.soldier);
    plain.move_soldier(10.0, 10.0, 0.5);
    plain.cw.refresh_blink(plain.world, *s2);
    CHECK(s2->blink_hits[0] != 0);
    CHECK((s2->flags & kEntityFlagIndoors) == 0);

    // Non-building targets never blink. [orig: itemDef type != 5 gate @0x4aefb2]
    Rig item(box_model(8, 0x3C, 4.0, 4.0, 3.0));
    Entity *b = item.world.registry.get(item.building);
    b->kind = EntityKind::Item;
    item.cw.build_tick_tables(item.world);
    Entity *s3 = item.world.registry.get(item.soldier);
    item.move_soldier(10.0, 10.0, 0.5);
    item.cw.refresh_blink(item.world, *s3);
    CHECK((s3->flags & kEntityFlagIndoors) == 0);
}

// ---------------------------------------------------------------------------
void test_ray_clip() {
    CollisionModel model = box_model(1, 0, 2.0, 2.0, 2.0);
    model.finalize_sections();
    const int32_t pos[3] = {fx(10.0), fx(10.0), 0};
    const CollisionMatrix mat = collision_matrix_from_heading(0, pos);
    std::vector<CollisionMatrix> mats(1, mat);
    CollisionTargetView view;
    view.model = &model;
    view.matrices = mats.data();
    view.pos[0] = pos[0];
    view.pos[1] = pos[1];
    view.pos[2] = pos[2];
    view.bound_radius = fx(4.0);
    view.is_building = true;

    // Vertical ray from above the roof straight down: entry = the roof plane z=2.
    CollisionRay ray;
    ray.start[0] = fx(10.0);
    ray.start[1] = fx(10.0);
    ray.start[2] = fx(6.0);
    ray.end[0] = fx(10.0);
    ray.end[1] = fx(10.0);
    ray.end[2] = fx(-1.0);
    ray.refresh();
    CHECK(collision_raycast_model(view, ray));
    CHECK(std::abs(ray.end[2] - fx(2.0)) < fx(0.02));
    CHECK(std::abs(ray.end[0] - fx(10.0)) < fx(0.02));

    // A ray beside the box misses.
    CollisionRay miss;
    miss.start[0] = fx(20.0);
    miss.start[1] = fx(10.0);
    miss.start[2] = fx(6.0);
    miss.end[0] = fx(20.0);
    miss.end[1] = fx(10.0);
    miss.end[2] = fx(-1.0);
    miss.refresh();
    CHECK(!collision_raycast_model(view, miss));
    CHECK(miss.end[2] == fx(-1.0));

    // Type-8 volumes are invisible to rays. [orig: the *subobj == 1 gate @0x413298]
    CollisionModel blink = box_model(8, 0x3C, 2.0, 2.0, 2.0);
    blink.finalize_sections();
    view.model = &blink;
    CollisionRay through;
    through = ray; // ray.end already clipped -> rebuild
    through.start[2] = fx(6.0);
    through.end[2] = fx(-1.0);
    through.refresh();
    CHECK(!collision_raycast_model(view, through));
}

// ---------------------------------------------------------------------------
void test_ground_probe_roof() {
    Rig rig(box_model(1, 0, 3.0, 3.0, 2.0));
    // Soldier above the roof column.
    rig.move_soldier(10.0, 10.0, 5.0);
    const int32_t pos[3] = {fx(10.0), fx(10.0), fx(5.0)};
    EntityHandle hit;
    const int32_t ground =
        rig.cw.raycast_ground(rig.world, rig.soldier, pos, 0, 0, 0, fx(10.0), &hit);
    CHECK(std::abs(ground - fx(2.0)) < fx(0.02)); // the roof, not the terrain at 0
    CHECK(hit == rig.building);

    // Off the building the terrain column (0) answers.
    rig.move_soldier(30.0, 30.0, 5.0);
    const int32_t pos2[3] = {fx(30.0), fx(30.0), fx(5.0)};
    EntityHandle hit2;
    const int32_t ground2 =
        rig.cw.raycast_ground(rig.world, rig.soldier, pos2, 0, 0, 0, fx(10.0), &hit2);
    CHECK(std::abs(ground2 - 0) < fx(0.02));
    CHECK(!hit2.valid());
}

// ---------------------------------------------------------------------------
void test_resolver_wall_pushout() {
    Rig rig(box_model(1, 0, 2.0, 2.0, 3.0));
    // Approach outside the +X wall (wall plane at x = 12), then step inside.
    rig.move_soldier(12.8, 10.0, 0.0);
    int32_t pos[3] = {fx(12.8), fx(10.0), 0};
    int32_t vel[3] = {0, 0, 0};
    int16_t health = 100;
    CollisionWorld::ResolveState state;
    rig.cw.resolve_entity(rig.world, rig.soldier, state, pos, vel, vel[2], 0, fx(1.8), 0, 0,
                          /*is_player=*/false, /*is_authority=*/true, /*tick=*/0,
                          /*anim=*/43, 0u, health);

    // Step into the wall: prev pos (12.8) was outside the +X plane -> it separates.
    pos[0] = fx(11.6);
    Entity *s = rig.world.registry.get(rig.soldier);
    s->position.x = 11.6f;
    rig.cw.build_tick_tables(rig.world);
    const int32_t before = pos[0];
    rig.cw.resolve_entity(rig.world, rig.soldier, state, pos, vel, vel[2], 0, fx(1.8), 0, 0,
                          false, true, 0, 43, 0u, health);
    CHECK(pos[0] > before); // pushed back toward +X (out of the wall)
    CHECK(health == 100);   // solid volumes never hurt
}

// ---------------------------------------------------------------------------
void test_secondary_vertical_force_is_full_strength() {
    CollisionModel model = box_model(1, 0, 2.0, 2.0, 2.0);
    model.finalize_sections();
    const int32_t target_pos[3] = {0, 0, 0};
    const CollisionMatrix matrix = collision_matrix_from_heading(0, target_pos);

    CollisionTargetView target;
    target.model = &model;
    target.matrices = &matrix;
    target.bound_radius = fx(4.0);

    // Enter the expanded +X/+Z corner with +X as the primary separator and
    // +Z as the secondary. Retail adds the secondary Z component at full
    // strength while halving only X/Y. [orig: @ 0x4aedd1-0x4aee20]
    const CollisionPoint point{fx(2.9), 0, fx(2.8), 0};
    const int32_t radius = fx(1.0);
    ContactQuery query;
    query.points = &point;
    query.radii = &radius;
    query.num_points = 1;
    query.prev_pos[0] = fx(3.5);
    query.prev_pos[2] = fx(3.5);
    query.source_bound_radius = fx(4.0);

    BlinkAccum blink;
    PlatformContact platform;
    ContactResult result;
    CHECK(collision_contact_force(target, query, blink, platform, result));
    CHECK(result.force[0] < 0);
    CHECK(result.force[2] < 0);
    CHECK(std::abs(result.force[2] - result.force[0]) <= 1);
}

// ---------------------------------------------------------------------------
void test_resolver_hurt_and_zones() {
    // A hurt volume (type 18: -1 HP) overlapping the soldier.
    Rig rig(box_model(18, 0, 3.0, 3.0, 3.0));
    rig.move_soldier(10.0, 10.0, 0.5);
    int32_t pos[3] = {fx(10.0), fx(10.0), fx(0.5)};
    int32_t vel[3] = {0, 0, 0};
    int16_t health = 100;
    CollisionWorld::ResolveState state;
    rig.cw.resolve_entity(rig.world, rig.soldier, state, pos, vel, vel[2], 0, fx(1.8), 0, 0,
                          false, true, 0, 43, 0u, health);
    CHECK(health == 99); // [orig: flag 0x40 -> health - 1 @0x4b318c]

    // A vehicle-loadout volume (type 11) sets the zone flag; an armory volume
    // (type 6) sets 0x400000 [orig: the input-218 gates @0x49b848/@0x49b858].
    Rig lrig(box_model(11, 0, 3.0, 3.0, 3.0));
    lrig.move_soldier(10.0, 10.0, 0.5);
    int32_t lpos[3] = {fx(10.0), fx(10.0), fx(0.5)};
    int32_t lvel[3] = {0, 0, 0};
    int16_t lhealth = 100;
    CollisionWorld::ResolveState lstate;
    lrig.cw.resolve_entity(lrig.world, lrig.soldier, lstate, lpos, lvel, lvel[2], 0, fx(1.8),
                           0, 0, false, true, 0, 43, 0u, lhealth);
    Entity *s = lrig.world.registry.get(lrig.soldier);
    CHECK((s->flags & kEntityFlagVehicleLoadoutZone) != 0);
    CHECK(lhealth == 100);

    Rig arig(box_model(6, 0, 3.0, 3.0, 3.0));
    arig.move_soldier(10.0, 10.0, 0.5);
    int32_t apos[3] = {fx(10.0), fx(10.0), fx(0.5)};
    int32_t avel[3] = {0, 0, 0};
    int16_t ahealth = 100;
    CollisionWorld::ResolveState astate;
    arig.cw.resolve_entity(arig.world, arig.soldier, astate, apos, avel, avel[2], 0, fx(1.8),
                           0, 0, false, true, 0, 43, 0u, ahealth);
    Entity *sa = arig.world.registry.get(arig.soldier);
    CHECK((sa->flags & kEntityFlagArmoryZone) != 0);
}

// ---------------------------------------------------------------------------
void test_idle_skip_throttle() {
    Rig rig(box_model(1, 0, 2.0, 2.0, 3.0));
    rig.move_soldier(30.0, 30.0, 0.0);
    int32_t pos[3] = {fx(30.0), fx(30.0), 0};
    int32_t vel[3] = {0, 0, 0};
    int16_t health = 100;
    CollisionWorld::ResolveState state;
    // Ticks 1..11 (tick & 0x3F != 0, no motion): counter ramps to the skip band.
    for (uint32_t t = 1; t <= 11; ++t)
        rig.cw.resolve_entity(rig.world, rig.soldier, state, pos, vel, vel[2], 0, fx(1.8), 0,
                              0, false, true, t, 43, 0u, health);
    // Tick 12: skip path — the caller's gravity displacement is reverted and vel zeroed.
    vel[2] = -400;
    pos[2] -= 400 * 2; // what the caller's gravity integration just did
    const int32_t sunk = pos[2];
    const int32_t ret = rig.cw.resolve_entity(rig.world, rig.soldier, state, pos, vel, vel[2],
                                              0, fx(1.8), 0, 0, false, true, 12, 43, 0u, health);
    CHECK(ret == 0);
    CHECK(vel[2] == 0);
    CHECK(pos[2] == sunk + 2 * 400); // [orig: pos.Z -= 2*slideDecay on the skip path]

    // The PLAYER skip band nets ZERO drift under the org2 motor's witnessed
    // per-tick gravity (`vel -= 208; pos += vel` — infantry.cpp, D-INF-10/§22):
    // the skip undo is x1 for the player, matching the original's Flags&0x100
    // select [orig: @ 0x4b2cd9-0x4b2ce9]. The x2-for-both undo (tuned to the
    // pre-§22 2-tick reimpl cadence) leaked +208 per skip tick — the standing
    // rise-and-snap sawtooth this pins against.
    {
        Rig prig(box_model(1, 0, 2.0, 2.0, 3.0));
        prig.move_soldier(30.0, 30.0, 1.0);
        int32_t ppos[3] = {fx(30.0), fx(30.0), fx(1.0)};
        int32_t pvel[3] = {0, 0, 0};
        int16_t phealth = 100;
        CollisionWorld::ResolveState pstate;
        for (uint32_t t = 1; t <= 11; ++t) // ramp the counter to the skip band
            prig.cw.resolve_entity(prig.world, prig.soldier, pstate, ppos, pvel, pvel[2], 0,
                                   fx(1.8), 0, 0, /*is_player=*/true, true, t, 43, 0u, phealth);
        const int32_t before_z = ppos[2];
        for (uint32_t t = 12; t <= 21; ++t) { // the 10-tick skip band
            pvel[2] -= 208;      // the org2 per-tick gravity...
            ppos[2] += pvel[2];  // ...and the x1 integrate [orig: @0x4b7acf/@0x4b7cef]
            const int32_t r = prig.cw.resolve_entity(prig.world, prig.soldier, pstate, ppos,
                                                     pvel, pvel[2], 0, fx(1.8), 0, 0, true,
                                                     true, t, 43, 0u, phealth);
            CHECK(r == 0);       // every band tick skips
            CHECK(pvel[2] == 0); // the skip zeroes vel_z each tick
        }
        CHECK(ppos[2] == before_z); // net zero — no idle sawtooth
    }
}

// ---------------------------------------------------------------------------
void test_debug_seams() {
    // debug_instances: the world-space wireframe snapshot transforms the volume
    // AABB through the SAME matrix path the queries use.
    Rig rig(box_model(1, 0, 2.0, 2.0, 3.0));
    const int32_t anchor[3] = {0, 0, 0};
    const auto insts = rig.cw.debug_instances(rig.world, anchor, /*range=*/-1, /*max=*/16);
    CHECK(insts.size() == 1);
    if (!insts.empty()) {
        const auto &inst = insts[0];
        CHECK(inst.handle == rig.building);
        CHECK(inst.pos[0] == fx(10.0) && inst.pos[1] == fx(10.0));
        CHECK(inst.volumes.size() == 1);
        const auto &v = inst.volumes[0];
        CHECK(v.type == 1);
        // Building yaw 90 -> engine heading 0 (identity rotation): corner 0 is
        // pos + local min, corner 7 pos + local max (corner index bit0 = max x,
        // bit1 = max y, bit2 = max z; quantized-table rounding tolerance).
        CHECK(std::abs(v.corners[0][0] - fx(8.0)) <= 4);
        CHECK(std::abs(v.corners[0][1] - fx(8.0)) <= 4);
        CHECK(std::abs(v.corners[0][2] - 0) <= 4);
        CHECK(std::abs(v.corners[7][0] - fx(12.0)) <= 4);
        CHECK(std::abs(v.corners[7][1] - fx(12.0)) <= 4);
        CHECK(std::abs(v.corners[7][2] - fx(3.0)) <= 4);
    }

    // Range cap: an anchor farther than `range` on any axis excludes the instance.
    const int32_t far_anchor[3] = {fx(400.0), fx(400.0), 0};
    CHECK(rig.cw.debug_instances(rig.world, far_anchor, fx(150.0), 16).empty());

    // LocalResolveDebug: the local player's full resolve captures the capsule
    // test points/extents and the returned foot clearance; a non-local resolve
    // never writes it (still invalid before local_player is set).
    CHECK(!rig.cw.local_resolve_debug.valid);
    rig.cw.local_player = rig.soldier;
    rig.move_soldier(30.0, 30.0, 1.0);
    int32_t pos[3] = {fx(30.0), fx(30.0), fx(1.0)};
    int32_t vel[3] = {0, 0, 0};
    int16_t health = 100;
    CollisionWorld::ResolveState state;
    const int32_t ret = rig.cw.resolve_entity(rig.world, rig.soldier, state, pos, vel, vel[2],
                                              fx(0.5), fx(1.8), 0, 0, /*is_player=*/true,
                                              /*is_authority=*/true, /*tick=*/0, 43, 0u, health);
    const CollisionWorld::LocalResolveDebug &lrd = rig.cw.local_resolve_debug;
    CHECK(lrd.valid);
    CHECK(lrd.capsule_bottom == fx(0.5));
    CHECK(lrd.capsule_top == fx(1.8));
    CHECK(lrd.foot_clearance == ret);
    CHECK(lrd.pos[0] == pos[0] && lrd.pos[1] == pos[1] && lrd.pos[2] == pos[2]);
    CHECK(lrd.points[2][0] == fx(30.0)); // the feet point rides the entity column
    CHECK(lrd.radii[1] == 20480);        // the fixed eye-point radius [orig: 0.3125u]
}

// ---------------------------------------------------------------------------
void test_slice_cadence_and_invuln() {
    // Candidate slices build on the 17th table tick, not the first — retail's
    // BSS-zero counter means mission starts run 16 sliceless ticks. [orig:
    // dword_B57C84 vs 0x10 @ 0x4c240f, zeroed inside 0x4b8eb0]
    World world;
    CollisionWorld cw;
    Field field{0};
    cw.terrain = &field.field;
    world.registry.configure_pool(0, 8);
    world.registry.configure_pool(2, 8);
    Entity b;
    b.kind = EntityKind::Building;
    b.net_id = 100;
    b.position = {10.0f, 10.0f, 0.0f};
    b.yaw = 90;
    b.alive = true;
    const EntityHandle building = world.registry.spawn(2, b);
    Entity s;
    s.kind = EntityKind::Organic;
    s.net_id = 1;
    s.position = {10.0f, 10.0f, 0.0f};
    s.alive = true;
    const EntityHandle soldier = world.registry.spawn(0, s);
    const int32_t mid = cw.add_model(box_model(1, 0, 2.0, 2.0, 3.0));
    cw.assign_entity(building, mid);
    for (int i = 0; i < 16; ++i) {
        cw.build_tick_tables(world);
        CHECK(cw.candidate_count(soldier) == 0);
    }
    cw.build_tick_tables(world); // the 17th call builds
    CHECK(cw.candidate_count(soldier) == 1);

    // Hurt volumes never damage an EngineFlags-0x4000000 entity. [orig: the
    // (Flags & 0x4000000) == 0 wrap @ 0x4b3148]
    Rig rig(box_model(18, 0, 3.0, 3.0, 3.0));
    Entity *inv = rig.world.registry.get(rig.soldier);
    inv->engine_flags |= 0x4000000u;
    rig.move_soldier(10.0, 10.0, 0.5);
    int32_t pos[3] = {fx(10.0), fx(10.0), fx(0.5)};
    int32_t vel[3] = {0, 0, 0};
    int16_t health = 100;
    CollisionWorld::ResolveState state;
    rig.cw.resolve_entity(rig.world, rig.soldier, state, pos, vel, vel[2], 0, fx(1.8), 0, 0,
                          false, true, 0, 43, 0u, health);
    CHECK(health == 100);

    // The similarly numbered runtime Flags bit is unrelated: only EngineFlags
    // carries item-definition indestructibility.
    Rig runtime_flags(box_model(18, 0, 3.0, 3.0, 3.0));
    Entity *not_inv = runtime_flags.world.registry.get(runtime_flags.soldier);
    not_inv->flags |= 0x4000000u;
    runtime_flags.move_soldier(10.0, 10.0, 0.5);
    int32_t pos2[3] = {fx(10.0), fx(10.0), fx(0.5)};
    int32_t vel2[3] = {0, 0, 0};
    int16_t health2 = 100;
    CollisionWorld::ResolveState state2;
    runtime_flags.cw.resolve_entity(runtime_flags.world, runtime_flags.soldier, state2, pos2,
                                    vel2, vel2[2], 0, fx(1.8), 0, 0, false, true, 0, 43, 0u,
                                    health2);
    CHECK(health2 == 99);
}

// ---------------------------------------------------------------------------
void test_pool1_item_is_one_collision_candidate() {
    World world;
    CollisionWorld cw;
    world.registry.configure_pool(0, 8);
    world.registry.configure_pool(1, 8);

    Entity item;
    item.kind = EntityKind::Item;
    item.position = {10.0f, 10.0f, 0.0f};
    item.alive = true;
    const EntityHandle item_handle = world.registry.spawn(1, item);

    Entity organic;
    organic.kind = EntityKind::Organic;
    organic.position = {10.0f, 10.0f, 0.0f};
    organic.alive = true;
    const EntityHandle source = world.registry.spawn(0, organic);

    const int32_t model_id = cw.add_model(box_model(1, 0, 2.0, 2.0, 3.0));
    cw.assign_entity(item_handle, model_id);
    for (int i = 0; i < 17; ++i) cw.build_tick_tables(world);

    CHECK(cw.static_count() == 0);
    CHECK(cw.candidate_count(source) == 1);
}

// ---------------------------------------------------------------------------
void test_platform_latch() {
    // A type-4 seat volume latches the platform state EVEN WITH ZERO FORCE —
    // the flag dispatch runs on a zero contact return [orig: the goto LABEL_67
    // @ 0x4b2fa5; the latch @ 0x4b3291-0x4b3297]. The authored shape pairs the
    // seat with a type-1 deck below it; the ground-settle tail probe re-hits
    // the deck (type 4 is ray-invisible), so groundEntity lands on the
    // platform via the probe's unconditional +0x28 store [orig: @ 0x414370].
    // The anchor/carry legs stay D-COL-5.
    CollisionModel model = box_model(1, 0, 3.0, 3.0, 1.0); // the solid deck z 0..1
    {
        // The seat volume z 1.0..1.2 above the deck, its own plane run.
        auto plane = [&](int nx, int ny, int nz, double d) {
            CollisionPlane p;
            p.nx = static_cast<int16_t>(nx);
            p.ny = static_cast<int16_t>(ny);
            p.nz = static_cast<int16_t>(nz);
            p.dist = fx(d);
            model.planes.push_back(p);
        };
        plane(16384, 0, 0, -3.0);
        plane(-16384, 0, 0, -3.0);
        plane(0, 16384, 0, -3.0);
        plane(0, -16384, 0, -3.0);
        plane(0, 0, 16384, -1.2);
        plane(0, 0, -16384, 1.0);
        CollisionVolume seat;
        seat.type = 4;
        seat.min_x = fx(-3.0);
        seat.max_x = fx(3.0);
        seat.min_y = fx(-3.0);
        seat.max_y = fx(3.0);
        seat.min_z = fx(1.0);
        seat.max_z = fx(1.2);
        seat.plane_start = 6;
        seat.plane_count = 6;
        model.volumes.push_back(seat);
        model.sections[0].volume_count = 2;
    }
    Rig rig(std::move(model));
    rig.move_soldier(10.0, 10.0, 1.05); // standing on the deck, inside the seat
    int32_t pos[3] = {fx(10.0), fx(10.0), fx(1.05)};
    int32_t vel[3] = {0, 0, 0};
    int16_t health = 100;
    CollisionWorld::ResolveState state;
    rig.cw.resolve_entity(rig.world, rig.soldier, state, pos, vel, vel[2], 0, fx(1.8), 0, 0,
                          false, true, 0, 43, 0u, health);
    Entity *s = rig.world.registry.get(rig.soldier);
    CHECK((s->flags & kEntityFlagOnPlatform) != 0);
    CHECK(s->ground_target == rig.building);
}

} // namespace

// ---- raycast_clear: the LOS segment query, TRUE = clear. [orig:
// Physics_RaycastTerrainAndSectors @0x539910 — terrain leg + pool-2/pool-1 solid
// clip; the D-AI-7 leg.] A pool-2 solid across the segment blocks; the excluded
// pair never blocks; a segment crossing the terrain surface blocks.
void test_raycast_clear_los() {
    Rig rig(box_model(1, 0, 2.0, 2.0, 3.0), 10.0, 0.0); // 4x4x3 wall at (10, 0)

    // Chest-high segment straight through the wall -> blocked.
    const int32_t a[3] = {fx(0.0), fx(0.0), fx(1.5)};
    const int32_t b[3] = {fx(20.0), fx(0.0), fx(1.5)};
    CHECK(!rig.cw.raycast_clear(rig.world, a, b, rig.soldier, EntityHandle{}));

    // Over the wall (z 4.5 > height 3) -> clear.
    const int32_t a_hi[3] = {fx(0.0), fx(0.0), fx(4.5)};
    const int32_t b_hi[3] = {fx(20.0), fx(0.0), fx(4.5)};
    CHECK(rig.cw.raycast_clear(rig.world, a_hi, b_hi, rig.soldier, EntityHandle{}));

    // The wall itself excluded (the sighting pair never blocks its own ray)
    // [orig: ctx[17]/[18] @0x538836].
    CHECK(rig.cw.raycast_clear(rig.world, a, b, rig.soldier, rig.building));

    // Terrain leg: a segment dipping below the ground plane hits the heightmap
    // march -> blocked [orig: the @0x539958 leg].
    const int32_t buried[3] = {fx(30.0), fx(0.0), fx(-1.0)};
    CHECK(!rig.cw.raycast_clear(rig.world, a_hi, buried, rig.soldier, EntityHandle{}));
}

int main() {
    test_matrix_roundtrip();
    test_blink_query_and_refresh();
    test_ray_clip();
    test_ground_probe_roof();
    test_resolver_wall_pushout();
    test_secondary_vertical_force_is_full_strength();
    test_resolver_hurt_and_zones();
    test_idle_skip_throttle();
    test_slice_cadence_and_invuln();
    test_pool1_item_is_one_collision_candidate();
    test_platform_latch();
    test_debug_seams();
    test_raycast_clear_los();
    if (failures == 0) std::printf("collision_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
