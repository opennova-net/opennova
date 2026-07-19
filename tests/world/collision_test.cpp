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
#include "world/angle.h"
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

    // Retail composes Rz(heading) * Ry(-pitch) * Rx(roll). Positive authored
    // pitch therefore turns local +X toward world +Z. RckS05 at 00TRg entity
    // 650 authors -173 degrees; using +pitch instead displaces corresponding
    // render/CFAC vertices by up to 0.62u.
    const int32_t origin[3] = {0, 0, 0};
    const CollisionMatrix pitched =
        collision_matrix_from_euler(0, 0x40000000, 0, origin);
    int32_t pitched_x[3];
    pitched.rotate_point(local, pitched_x);
    CHECK(std::abs(pitched_x[0]) <= 4);
    CHECK(std::abs(pitched_x[1]) <= 4);
    CHECK(std::abs(pitched_x[2] - fx(1.0)) <= 4);
}

void test_retail_render_pose_matrix_roundtrip_and_order() {
    const float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    const int32_t entity_pos[3] = {fx(10.0), fx(20.0), fx(3.0)};
    const CollisionMatrix entity =
        collision_matrix_from_heading(0x40000000, entity_pos);
    CollisionMatrix roundtrip;
    CHECK(collision_matrix_apply_render_pose(entity, identity, roundtrip));
    for (int i = 0; i < 12; ++i) CHECK(roundtrip.m[i] == entity.m[i]);
    CHECK(!roundtrip.disabled()); // affine m[15] == 1 never disables a section.

    // Render-float translation (2,3,4) maps back to mission (4,-2,3).
    float translated[16];
    std::copy(std::begin(identity), std::end(identity), translated);
    translated[12] = 2.0f;
    translated[13] = 3.0f;
    translated[14] = 4.0f;
    const int32_t origin[3] = {};
    const CollisionMatrix at_origin = collision_matrix_from_heading(0, origin);
    CollisionMatrix translated_world;
    CHECK(collision_matrix_apply_render_pose(at_origin, translated, translated_world));
    CHECK(translated_world.m[3] == fx(4.0));
    CHECK(translated_world.m[7] == fx(-2.0));
    CHECK(translated_world.m[11] == fx(3.0));

    // Non-commuting order: local render +X is mission -Y. Pose*entity rotates
    // that by the +90-degree entity heading to mission +X. Reversing the float
    // multiply would leave it on -Y instead.
    float local_x[16];
    std::copy(std::begin(identity), std::end(identity), local_x);
    local_x[12] = 2.0f;
    CollisionMatrix posed_world;
    CHECK(collision_matrix_apply_render_pose(entity, local_x, posed_world));
    CHECK(posed_world.m[3] == fx(12.0));
    CHECK(posed_world.m[7] == fx(20.0));
    CHECK(posed_world.m[11] == fx(3.0));

    // Reachable values that distinguish retail x87 PC53 accumulation from a
    // binary32 matrix loop. The converted fixed matrix is bit-exact.
    const int32_t x87_pos[3] = {655360, 1310720, 196608};
    const CollisionMatrix x87_entity =
        collision_matrix_from_heading(0x62c00000, x87_pos);
    const float x87_pose[16] = {
        0.156207114f, 0.0f, -0.987724304f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.987724304f, 0.0f, 0.156207114f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    const int32_t x87_expected[16] = {
        2231699, -3551295, 0, 655360,
        3551295, 2231699, 0, 1310720,
        0, 0, 4194304, 196608,
        0, 0, 0, 0,
    };
    CollisionMatrix x87_world;
    CHECK(collision_matrix_apply_render_pose(x87_entity, x87_pose, x87_world));
    for (int i = 0; i < 16; ++i)
        CHECK(x87_world.m[i] == x87_expected[i]);
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
void test_platform_contact_uses_positive_authored_pitch() {
    CollisionModel model = box_model(4, 0, 2.0, 2.0, 2.0);
    model.finalize_sections();
    const int32_t target_pos[3] = {0, 0, 0};
    const CollisionMatrix matrix = collision_matrix_from_heading(0, target_pos);

    CollisionTargetView target;
    target.model = &model;
    target.matrices = &matrix;
    target.bound_radius = fx(4.0);
    target.pitch_bam = bam_from_degrees_wrapped(30.0);

    const CollisionPoint point{0, 0, fx(1.0), 0};
    const int32_t radius = 0;
    ContactQuery query;
    query.points = &point;
    query.radii = &radius;
    query.num_points = 1;
    query.source_bound_radius = fx(1.0);
    query.mask = 0x1;

    BlinkAccum blink;
    PlatformContact platform;
    ContactResult result;
    CHECK(!collision_contact_force(target, query, blink, platform, result));
    CHECK((result.flags & 0x1u) != 0);
    CHECK(platform.valid);
    // Plane 0 has no vertical normal component, so retail's
    // target.pitch - normal.pitch leg must preserve the raw positive value.
    CHECK(platform.pitch == bam_from_degrees_wrapped(30.0));
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
void test_proximity_tables_use_host_bound_radius() {
    // Both placed objects carry a tiny intact collision shell, but the host has
    // stamped entity+0 with the max intact/husk model bound. At 12u from the
    // organic source, retail's +4u source range admits each 8u hull; deriving
    // the table radius from the 1u intact shell would drop both candidates.
    World world;
    CollisionWorld cw;
    world.registry.configure_pool(0, 4);
    world.registry.configure_pool(1, 4);
    world.registry.configure_pool(2, 4);

    Entity source_seed;
    source_seed.kind = EntityKind::Organic;
    source_seed.alive = true;
    const EntityHandle source = world.registry.spawn(0, source_seed);

    Entity dynamic_seed;
    dynamic_seed.kind = EntityKind::Item;
    dynamic_seed.position = {0.0f, 12.0f, 0.0f};
    dynamic_seed.bound_radius = 8.0f;
    dynamic_seed.alive = true;
    const EntityHandle dynamic = world.registry.spawn(1, dynamic_seed);

    Entity static_seed = dynamic_seed;
    static_seed.kind = EntityKind::Building;
    static_seed.position = {12.0f, 0.0f, 0.0f};
    const EntityHandle statik = world.registry.spawn(2, static_seed);
    CHECK(source.valid() && dynamic.valid() && statik.valid());

    const int32_t tiny_model = cw.add_model(box_model(1, 0, 1.0, 1.0, 1.0));
    cw.assign_entity(dynamic, tiny_model);
    cw.assign_entity(statik, tiny_model);
    for (int i = 0; i < 17; ++i) cw.build_tick_tables(world);

    CHECK(cw.candidate_count(source) == 2);

    // Pool-1 vehicle sources use their own host-stamped bound too. With the
    // witnessed +6u vehicle pad, an 8u hull reaches this 1u static at 15u;
    // the old hard-coded 1u source radius incorrectly dropped it.
    Entity vehicle_seed;
    vehicle_seed.kind = EntityKind::Item;
    vehicle_seed.item_id = 900;
    vehicle_seed.position = {40.0f, 40.0f, 0.0f};
    vehicle_seed.bound_radius = 8.0f;
    vehicle_seed.alive = true;
    const EntityHandle vehicle = world.registry.spawn(1, vehicle_seed);

    Entity near_static_seed;
    near_static_seed.kind = EntityKind::Building;
    near_static_seed.position = {55.0f, 40.0f, 0.0f};
    near_static_seed.bound_radius = 1.0f;
    near_static_seed.alive = true;
    const EntityHandle near_static = world.registry.spawn(2, near_static_seed);
    CHECK(vehicle.valid() && near_static.valid());

    VehicleTraits vehicle_traits;
    vehicle_traits.physics = 1;
    world.vehicle_traits.set(vehicle_seed.item_id, vehicle_traits);
    cw.assign_entity(vehicle, tiny_model);
    cw.assign_entity(near_static, tiny_model);
    for (int i = 0; i < 17; ++i) cw.build_tick_tables(world);

    CHECK(cw.candidate_count(vehicle) == 1);
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

// ---------------------------------------------------------------------------
void test_sound_occlusion() {
    // Sound-occlusion distance inflation [orig: Sound_ApplyOcclusionDistance
    // @ 0x529970]: base = min(d/8, 10u); ray-1 block compounds 2*base + 5u;
    // one final add of base (ray 2 clear) or 2*base + 5u (ray 2 blocked).
    // A tall solid (type-1) wall at (6, 0) blocks both rays between the
    // listener at the origin and a source at (12, 0).
    {
        Rig rig(box_model(1, 0, 2.0, 2.0, 8.0), 6.0, 0.0);
        rig.move_soldier(0.0, 0.0, 1.0);
        const int32_t lpos[3] = {fx(0.0), fx(0.0), fx(1.5)};
        const int32_t spos[3] = {fx(12.0), fx(0.0), fx(1.5)};
        const int32_t d = fx(12.0); // base = 1.5u
        const int32_t got = rig.cw.sound_occlusion_inflate(rig.world, rig.soldier,
                                                           EntityHandle{}, lpos, spos, d);
        // ray 1 blocked: base = 2*1.5 + 5 = 8u; ray 2 blocked: d += 2*8 + 5 = 21u.
        CHECK(got == d + fx(21.0));

        // A source with clear LOS (no wall on the +Y leg) inflates by base only.
        const int32_t spos_clear[3] = {fx(0.0), fx(12.0), fx(1.5)};
        const int32_t got_clear = rig.cw.sound_occlusion_inflate(
            rig.world, rig.soldier, EntityHandle{}, lpos, spos_clear, d);
        CHECK(got_clear == d + fx(1.5));

        // The base clamp: min(d/8, 10u) caps at 10u past 80u. [orig: @ 0x529982]
        const int32_t spos_far[3] = {fx(0.0), fx(200.0), fx(1.5)};
        const int32_t got_far = rig.cw.sound_occlusion_inflate(
            rig.world, rig.soldier, EntityHandle{}, lpos, spos_far, fx(200.0));
        CHECK(got_far == fx(200.0) + fx(10.0));
    }
    // A 1.9u wall blocks only ray 1 (source z lifted +0x2000; ray 2's segment
    // runs 0.5u higher and clears): the compounded base adds once.
    // [orig: the -0x8000 offset @ 0x5299c5 -> Physics_CheckTerrainLineOfSight
    // z -= offset]
    {
        Rig rig(box_model(1, 0, 2.0, 2.0, 1.9), 6.0, 0.0);
        rig.move_soldier(0.0, 0.0, 1.0);
        const int32_t lpos[3] = {fx(0.0), fx(0.0), fx(1.5)};
        const int32_t spos[3] = {fx(12.0), fx(0.0), fx(1.5)};
        const int32_t d = fx(12.0); // base = 1.5u
        const int32_t got = rig.cw.sound_occlusion_inflate(rig.world, rig.soldier,
                                                           EntityHandle{}, lpos, spos, d);
        // ray 1 blocked -> base = 8u; ray 2 clear -> d += 8u.
        CHECK(got == d + fx(8.0));
    }
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

// The 0x60c760 LOS marcher point callback rounds to the nearest render-cache
// texel. A floor sample would miss the isolated x=1 column at the first step.
void test_los_point_bias() {
    Field f(0);
    f.heightmap[1] = 2 * 256;
    const int32_t a[3] = {fx(0.75), 0, fx(1.0)};
    const int32_t b[3] = {fx(8.75), 0, fx(1.0)};
    CHECK(los_terrain_blocked(f.field, a, b));

    // That cache stores floor(raw16 / 128) as a byte and expands it in
    // half-unit steps. A 1.25u source column therefore reads as 1.0u and does
    // not block a 1.125u ray; returning the full-precision height would block.
    f.heightmap[1] = 320;
    const int32_t q_a[3] = {fx(0.75), 0, fx(1.125)};
    const int32_t q_b[3] = {fx(8.75), 0, fx(1.125)};
    CHECK(!los_terrain_blocked(f.field, q_a, q_b));
}

// ----------------------------------------------------------------------------
// The projectile face raycast [orig: Physics_RaycastAgainstBoneCollision
// @ 0x4e4cb0 + Math_PointInTriangle2D @ 0x414050].
// ----------------------------------------------------------------------------

// A one-section face mesh: a 2x2 u quad (two triangles) at section-local z = 1,
// normal +z (Q14), projection plane XY (axis 1).
CollisionModel face_quad_model(uint8_t material, uint32_t flags) {
    CollisionModel m;
    m.face_vertices = {{-256, -256, 256}, {256, -256, 256}, {256, 256, 256},
                       {-256, 256, 256}}; // Q8: (+-1, +-1, 1)
    auto face = [&](int a, int b, int c) {
        CollisionFace f;
        f.v[0] = static_cast<int16_t>(a);
        f.v[1] = static_cast<int16_t>(b);
        f.v[2] = static_cast<int16_t>(c);
        f.normal[0] = 0;
        f.normal[1] = 0;
        f.normal[2] = 16384;
        f.axis = 1;
        // side = (v.n >> 14) + dist: on-plane at z=1 -> dist = -fx(1).
        f.plane_dist = -fx(1.0);
        f.min[0] = fx(-1.0); f.max[0] = fx(1.0);
        f.min[1] = fx(-1.0); f.max[1] = fx(1.0);
        f.min[2] = fx(1.0);  f.max[2] = fx(1.0);
        f.flags = flags;
        f.material = material;
        m.faces.push_back(f);
    };
    face(0, 1, 2);
    face(0, 2, 3);
    m.sections.assign(1, {});
    m.sections[0].face_start = 0;
    m.sections[0].face_count = 2;
    m.sections[0].face_vertex_start = 0;
    m.sections[0].face_vertex_count = 4;
    return m;
}

// Two spatially separate husk sections: the root quad is centered at local
// x=-3, while section 1 is centered at x=+3. Each face's vertex indices are
// relative to its section's own vertex run, matching the retail face walker.
CollisionModel two_section_face_model(uint8_t root_material, uint8_t piece_material) {
    CollisionModel m;
    auto append_quad = [&](int center_x, uint8_t material) {
        const int32_t vertex_start = static_cast<int32_t>(m.face_vertices.size());
        const int32_t face_start = static_cast<int32_t>(m.faces.size());
        const int16_t x0 = static_cast<int16_t>((center_x - 1) * 256);
        const int16_t x1 = static_cast<int16_t>((center_x + 1) * 256);
        m.face_vertices.push_back({x0, -256, 256});
        m.face_vertices.push_back({x1, -256, 256});
        m.face_vertices.push_back({x1, 256, 256});
        m.face_vertices.push_back({x0, 256, 256});

        auto append_face = [&](int a, int b, int c) {
            CollisionFace f;
            f.v[0] = static_cast<int16_t>(a);
            f.v[1] = static_cast<int16_t>(b);
            f.v[2] = static_cast<int16_t>(c);
            f.normal[2] = 16384;
            f.axis = 1;
            f.plane_dist = -fx(1.0);
            f.min[0] = fx(center_x - 1.0);
            f.max[0] = fx(center_x + 1.0);
            f.min[1] = fx(-1.0); f.max[1] = fx(1.0);
            f.min[2] = fx(1.0);  f.max[2] = fx(1.0);
            f.material = material;
            m.faces.push_back(f);
        };
        append_face(0, 1, 2);
        append_face(0, 2, 3);

        CollisionSection section;
        section.face_start = face_start;
        section.face_count = 2;
        section.face_vertex_start = vertex_start;
        section.face_vertex_count = 4;
        m.sections.push_back(section);
    };
    append_quad(-3, root_material);
    append_quad(3, piece_material);
    return m;
}

struct TwoSectionMatrixProvider final : ICollisionSectionMatrixProvider {
    bool build_section_matrices(World &, EntityHandle, int32_t,
                                const CollisionMatrix &,
                                const CollisionModel &model,
                                std::vector<CollisionMatrix> &out) override {
        if (model.sections.size() != 2) return false;
        const int32_t root_pos[3] = {fx(10.0), fx(10.0), 0};
        const int32_t moved_pos[3] = {fx(14.0), fx(10.0), 0};
        out = {collision_matrix_from_heading(0, root_pos),
               collision_matrix_from_heading(0, moved_pos)};
        return true;
    }
};

CollisionModel person_section_model() {
    CollisionModel model;
    model.sections.resize(19);
    // CharModel.3di COBJ 14 (head), preserved here as exact 16.16 authored data.
    CollisionSection &head = model.sections[14];
    head.center[0] = 3578;
    head.center[1] = 65;
    head.center[2] = 54371;
    head.radius = 10345;
    return model;
}

struct PosedHeadMatrixProvider final : ICollisionSectionMatrixProvider {
    bool build_section_matrices(World &, EntityHandle, int32_t,
                                const CollisionMatrix &entity_world,
                                const CollisionModel &model,
                                std::vector<CollisionMatrix> &out) override {
        out.assign(model.sections.size(), entity_world);
        // Move only COBJ/bone 14 one unit in +X. The narrow phase must consume
        // callback slot 14, not the entity matrix or COBJ parent metadata.
        out[14].m[3] += fx(1.0);
        return true;
    }
};

struct CountingMatrixProvider final : ICollisionSectionMatrixProvider {
    std::vector<EntityHandle> build_handles;

    bool build_section_matrices(World &, EntityHandle entity, int32_t,
                                const CollisionMatrix &entity_world,
                                const CollisionModel &model,
                                std::vector<CollisionMatrix> &out) override {
        build_handles.push_back(entity);
        out.assign(model.sections.size(), entity_world);
        return true;
    }

    int calls_for(EntityHandle entity) const {
        int count = 0;
        for (const EntityHandle built : build_handles) {
            if (built == entity) ++count;
        }
        return count;
    }
};

struct DemandPersonProvider final : ICollisionSectionMatrixProvider {
    CollisionWorld *collision = nullptr;
    int32_t model_id = -1;
    EntityHandle expected;
    int ensure_calls = 0;

    bool ensure_collision_instance(World &, EntityHandle entity) override {
        ++ensure_calls;
        if (collision == nullptr || entity != expected || model_id < 0) return false;
        collision->assign_entity(entity, model_id);
        return true;
    }

    bool build_section_matrices(World &, EntityHandle, int32_t,
                                const CollisionMatrix &entity_world,
                                const CollisionModel &model,
                                std::vector<CollisionMatrix> &out) override {
        out.assign(model.sections.size(), entity_world);
        return true;
    }
};

struct ReusedSlotPersonProvider final : ICollisionSectionMatrixProvider {
    CollisionWorld *collision = nullptr;
    int32_t replacement_model_id = -1;
    uint64_t expected_spawn_id = 0;
    int ensure_calls = 0;

    bool ensure_collision_instance(World &world, EntityHandle entity) override {
        ++ensure_calls;
        const Entity *current = world.registry.get(entity);
        if (collision == nullptr || current == nullptr ||
            current->registry_spawn_id != expected_spawn_id)
            return false;
        collision->assign_entity(entity, replacement_model_id,
                                 current->registry_spawn_id);
        return true;
    }

    bool build_section_matrices(World &, EntityHandle, int32_t,
                                const CollisionMatrix &entity_world,
                                const CollisionModel &model,
                                std::vector<CollisionMatrix> &out) override {
        out.assign(model.sections.size(), entity_world);
        return true;
    }
};

void test_person_section_raycast_uses_posed_bone_matrix() {
    Rig rig(person_section_model());
    rig.cw.assign_entity(rig.soldier, 0);
    rig.world.registry.get(rig.soldier)->yaw = 90;
    PosedHeadMatrixProvider provider;
    rig.cw.set_section_matrix_provider(&provider);

    // The soldier is at the origin and its posed head center is near
    // (1.055, 0.001, 0.830). A Y-axis segment through that moved center must
    // hit bone 14; the same segment through the stale rest center must miss.
    PersonSectionHit hit;
    const int32_t moved_start[3] = {fx(1.0545959473), fx(-1.0), fx(0.8296356201)};
    const int32_t moved_end[3] = {fx(1.0545959473), fx(1.0), fx(0.8296356201)};
    CHECK(rig.cw.raycast_person_sections(rig.world, rig.soldier,
                                         moved_start, moved_end, 0, hit));
    CHECK(hit.primary_section == 14);
    CHECK(hit.secondary_section == 14);
    CHECK(hit.material == 19);

    const int32_t stale_start[3] = {fx(0.0545959473), fx(-1.0), fx(0.8296356201)};
    const int32_t stale_end[3] = {fx(0.0545959473), fx(1.0), fx(0.8296356201)};
    CHECK(!rig.cw.raycast_person_sections(rig.world, rig.soldier,
                                          stale_start, stale_end, 0, hit));
}

void test_f3_debug_prefilters_before_building_section_matrices() {
    World world;
    world.registry.configure_pool(0, 4);
    world.registry.configure_pool(2, 4);

    auto spawn = [&](int pool, EntityKind kind, double x) {
        Entity seed;
        seed.kind = kind;
        seed.alive = true;
        seed.position = Vec3{static_cast<float>(x), 0.0f, 0.0f};
        seed.yaw = 90; // engine heading 0: section center stays axis-aligned
        return world.registry.spawn(pool, seed);
    };
    const EntityHandle near_person = spawn(0, EntityKind::Organic, 1.0);
    const EntityHandle far_person = spawn(0, EntityKind::Organic, 100.0);
    const EntityHandle near_static = spawn(2, EntityKind::Building, 2.0);
    const EntityHandle far_static = spawn(2, EntityKind::Building, 100.0);
    CHECK(near_person.valid() && far_person.valid() &&
          near_static.valid() && far_static.valid());

    CollisionModel model;
    model.sections.resize(1);
    model.sections[0].center[0] = fx(0.25);
    model.sections[0].center[2] = fx(1.0);
    model.sections[0].radius = fx(0.5);
    CollisionWorld collision;
    const int32_t model_id = collision.add_model(std::move(model));
    collision.assign_entity(near_person, model_id);
    collision.assign_entity(far_person, model_id);
    collision.assign_entity(near_static, model_id);
    collision.assign_entity(far_static, model_id);
    CountingMatrixProvider provider;
    collision.set_section_matrix_provider(&provider);

    const int32_t anchor[3] = {};
    const auto statics = collision.debug_hitboxes(
            world, anchor, fx(10.0), 8, 100);
    CHECK(statics.size() == 1);
    if (!statics.empty()) CHECK(statics[0].handle == near_static);
    CHECK(provider.calls_for(near_static) == 1);
    CHECK(provider.calls_for(near_person) == 0);
    CHECK(provider.calls_for(far_person) == 0);
    CHECK(provider.calls_for(far_static) == 0);

    provider.build_handles.clear();
    const auto people = collision.debug_person_sections(
            world, anchor, fx(10.0), 8);
    CHECK(people.size() == 1);
    if (!people.empty()) {
        CHECK(people[0].handle == near_person);
        CHECK(people[0].section == 0);
        CHECK(people[0].center[0] == fx(1.25));
        CHECK(people[0].center[1] == 0);
        CHECK(people[0].center[2] == fx(1.0));
        CHECK(people[0].authored_radius == fx(0.5));
    }
    CHECK(provider.calls_for(near_person) == 1);
    CHECK(provider.calls_for(far_person) == 0);
    CHECK(provider.calls_for(near_static) == 0);
    CHECK(provider.calls_for(far_static) == 0);
}

void test_late_person_instance_is_demand_resolved_for_rounds_and_debug() {
    World world;
    world.registry.configure_pool(0, 4);
    Entity seed;
    seed.kind = EntityKind::Organic;
    seed.health = 100;
    seed.alive = true;
    seed.position = Vec3{5.0f, 0.0f, 0.0f};
    const EntityHandle victim = world.registry.spawn(0, seed);
    CHECK(victim.valid());

    CollisionModel person;
    person.sections.resize(15);
    person.sections[14].radius = fx(1.0);
    CollisionWorld collision;
    const int32_t model_id = collision.add_model(std::move(person));
    DemandPersonProvider provider;
    provider.collision = &collision;
    provider.model_id = model_id;
    provider.expected = victim;
    collision.set_section_matrix_provider(&provider);

    // No instance was assigned by a mission-start sweep. RoundSim must invoke
    // the provider before choosing the torso sphere; this z=0 ray hits authored
    // head section 14 while missing the fallback centered at z=0.9.
    LiveRound &round = world.round_sim.rounds[0];
    round.active = true;
    world.round_sim.active_count = 1;
    round.pos = Vec3{0.0f, 0.0f, 0.0f};
    round.vel = Vec3{10.0f, 0.0f, 0.0f};
    round.max_age_ticks = 100;
    world.round_sim.tick(world, nullptr, &collision);
    CHECK(provider.ensure_calls == 1);
    CHECK(collision.has_instance(world, victim));
    CHECK(world.round_sim.debug_trail_count == 1);
    if (world.round_sim.debug_trail_count == 1)
        CHECK(world.round_sim.debug_trail[0].section == 14);

    // F3's posed-section snapshot shares the same ensure seam and does not
    // re-enter it once an instance exists.
    const int32_t anchor[3] = {};
    const auto rows = collision.debug_person_sections(world, anchor, -1, 4);
    CHECK(rows.size() == 1);
    if (!rows.empty()) CHECK(rows[0].section == 14);
    CHECK(provider.ensure_calls == 1);
}

void test_reused_registry_slot_rejects_old_collision_identity() {
    World world;
    world.registry.configure_pool(0, 2);
    Entity seed;
    seed.kind = EntityKind::Organic;
    seed.health = 100;
    seed.position = Vec3{5.0f, 0.0f, 0.0f};
    const EntityHandle first = world.registry.spawn(0, seed);
    CHECK(first.valid());
    const uint64_t first_spawn_id =
            world.registry.get(first)->registry_spawn_id;

    CollisionWorld collision;
    CollisionModel old_main;
    old_main.sections.resize(4);
    old_main.sections[3].radius = fx(1.0);
    const int32_t old_main_id = collision.add_model(std::move(old_main));
    CollisionModel old_husk;
    old_husk.sections.resize(3);
    old_husk.sections[2].radius = fx(1.0);
    const int32_t old_husk_id = collision.add_model(std::move(old_husk));
    collision.assign_entity(first, old_main_id, first_spawn_id);
    collision.assign_entity_husk(first, old_husk_id);

    world.registry.despawn(first);
    seed.engine_flags = 0x4u; // would select the old husk if it leaked
    const EntityHandle reused = world.registry.spawn(0, seed);
    CHECK(reused == first);
    const uint64_t reused_spawn_id =
            world.registry.get(reused)->registry_spawn_id;
    CHECK(reused_spawn_id != first_spawn_id);
    CHECK(!collision.has_instance(world, reused));
    CHECK(collision.model_for(world, reused) == nullptr);
    collision.build_tick_tables(world);
    CHECK(collision.instance_count() == 0);

    CollisionModel replacement;
    replacement.sections.resize(15);
    replacement.sections[14].radius = fx(1.0);
    const int32_t replacement_id = collision.add_model(std::move(replacement));
    ReusedSlotPersonProvider provider;
    provider.collision = &collision;
    provider.replacement_model_id = replacement_id;
    provider.expected_spawn_id = reused_spawn_id;
    collision.set_section_matrix_provider(&provider);

    const int32_t start[3] = {fx(0.0), 0, 0};
    const int32_t end[3] = {fx(10.0), 0, 0};
    PersonSectionHit hit;
    CHECK(!collision.raycast_person_sections(
            world, reused, start, end, 0, hit));
    CHECK(collision.ensure_entity_instance(world, reused));
    CHECK(provider.ensure_calls == 1);
    CHECK(collision.raycast_person_sections(
            world, reused, start, end, 0, hit));
    CHECK(hit.primary_section == 14);
    CHECK(collision.ensure_entity_instance(world, reused));
    CHECK(provider.ensure_calls == 1);
}

struct PersonSectionsFixture {
    CollisionModel model;
    std::vector<CollisionMatrix> matrices;
    CollisionTargetView target;

    explicit PersonSectionsFixture(int section_count) {
        model.sections.resize(static_cast<size_t>(section_count));
        const int32_t origin[3] = {0, 0, 0};
        matrices.assign(static_cast<size_t>(section_count),
                        collision_matrix_from_heading(0, origin));
        target.model = &model;
        target.matrices = matrices.data();
    }

    void set_section(int section, double x, double y, double z, double radius) {
        CollisionSection &s = model.sections[static_cast<size_t>(section)];
        s.center[0] = fx(x);
        s.center[1] = fx(y);
        s.center[2] = fx(z);
        s.radius = fx(radius);
    }
};

void test_person_section_reverse_scan_and_mask() {
    PersonSectionsFixture rig(6);
    rig.set_section(1, 5.0, 0.0, 0.0, 1.0);
    rig.set_section(5, 5.0, 0.0, 0.0, 1.0);
    const int32_t start[3] = {0, 0, 0};
    const int32_t end[3] = {fx(10.0), 0, 0};

    PersonSectionHit hit;
    CHECK(collision_raycast_person_sections(rig.target, start, end, 0, 0, hit));
    CHECK(hit.primary_section == 5);   // first accepted in the high-to-low walk
    CHECK(hit.secondary_section == 1); // final accepted overlap
    CHECK(hit.projected_dist == fx(5.0));
    CHECK(hit.dist == fx(4.5));
    CHECK(hit.material == 19);

    // entity+0x134 removes a section before the overlap test. Masking the high
    // section promotes the lower overlap to both retail ray outputs.
    CHECK(collision_raycast_person_sections(
            rig.target, start, end, 0, 1u << 5, hit));
    CHECK(hit.primary_section == 1);
    CHECK(hit.secondary_section == 1);
    CHECK(!collision_raycast_person_sections(
            rig.target, start, end, 0, (1u << 5) | (1u << 1), hit));
    CHECK(hit.primary_section == -1);
    CHECK(hit.secondary_section == -1);
}

void test_person_section_retail_radius_rules() {
    const int32_t start[3] = {0, 0, 0};
    const int32_t end[3] = {fx(10.0), 0, 0};
    PersonSectionHit hit;

    // With authored radius 1u, the normal 45% rule plus 0xCCC padding reaches
    // only ~0.50u. Head section 14 uses 65% and reaches ~0.70u.
    PersonSectionsFixture head(15);
    head.set_section(13, 5.0, 0.6, 0.0, 1.0);
    head.set_section(14, 5.0, 0.6, 0.0, 1.0);
    CHECK(collision_raycast_person_sections(head.target, start, end, 0, 0, hit));
    CHECK(hit.primary_section == 14);
    CHECK(hit.secondary_section == 14);
    CHECK(!collision_raycast_person_sections(
            head.target, start, end, 0, 1u << 14, hit));

    // Hand sections 15 and 16 cap the FINAL effective radius at 0x3000
    // (0.1875u), even though a 2u authored radius would otherwise be ~0.95u.
    PersonSectionsFixture inside_cap(17);
    inside_cap.set_section(15, 5.0, 0.18, 0.0, 2.0);
    inside_cap.set_section(16, 5.0, 0.18, 0.0, 2.0);
    CHECK(collision_raycast_person_sections(
            inside_cap.target, start, end, 0, 0, hit));
    CHECK(hit.primary_section == 16);
    CHECK(hit.secondary_section == 15);

    PersonSectionsFixture outside_cap(17);
    outside_cap.set_section(15, 5.0, 0.19, 0.0, 2.0);
    outside_cap.set_section(16, 5.0, 0.19, 0.0, 2.0);
    CHECK(!collision_raycast_person_sections(
            outside_cap.target, start, end, 0, 0, hit));
}

void test_face_raycast() {
    // The quad sits at entity (10, 10, 0) -> world plane z = 1.
    Rig rig(face_quad_model(14, 0)); // material 14 -> impact tag 18 'metal'
    Entity *b = rig.world.registry.get(rig.building);
    b->bound_radius = 3.0f;

    // Straight down through the quad center: hit at half the segment.
    const int32_t s_hit[3] = {fx(10.0), fx(10.0), fx(3.0)};
    const int32_t e_hit[3] = {fx(10.0), fx(10.0), fx(-1.0)};
    RayFaceHit fh;
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, s_hit, e_hit, 0, fh) ==
          CollisionWorld::FaceRaycast::kHit);
    CHECK(fh.material == 14);
    CHECK(fh.section == 0);
    // dist = |d0| * len / (|d0|+|d1|) = 2.0 u of the 4.0 u segment.
    CHECK(fh.dist > fx(1.95) && fh.dist < fx(2.05));

    // THE angle case: inside the bound sphere, past the quad edge — the round
    // must fly on (a sphere test alone would stop it midair).
    const int32_t s_graze[3] = {fx(12.5), fx(10.0), fx(3.0)};
    const int32_t e_graze[3] = {fx(12.5), fx(10.0), fx(-1.0)};
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, s_graze, e_graze, 0, fh) ==
          CollisionWorld::FaceRaycast::kMiss);

    // Enter-front only: from below, the +z-facing quad is a backface -> miss.
    const int32_t s_up[3] = {fx(10.0), fx(10.0), fx(-1.0)};
    const int32_t e_up[3] = {fx(10.0), fx(10.0), fx(3.0)};
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, s_up, e_up, 0, fh) ==
          CollisionWorld::FaceRaycast::kMiss);

    // No instance on the soldier -> the sphere stand-in verdict.
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.soldier, s_hit, e_hit, 0, fh) ==
          CollisionWorld::FaceRaycast::kNoFaceMesh);
}

void test_face_raycast_flags_and_materials() {
    RayFaceHit fh;
    const int32_t s_dn[3] = {fx(10.0), fx(10.0), fx(3.0)};
    const int32_t e_dn[3] = {fx(10.0), fx(10.0), fx(-1.0)};
    const int32_t s_up[3] = {fx(10.0), fx(10.0), fx(-1.0)};
    const int32_t e_up[3] = {fx(10.0), fx(10.0), fx(3.0)};

    {
        // flags 0x100 = never hit.
        Rig rig(face_quad_model(14, 0x100));
        CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, s_dn, e_dn, 0, fh) ==
              CollisionWorld::FaceRaycast::kMiss);
    }
    {
        // Material 17 foliage vs the ammo 0x4000000 flag; hits without it.
        Rig rig(face_quad_model(17, 0));
        CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, s_dn, e_dn,
                                          0x4000000u, fh) ==
              CollisionWorld::FaceRaycast::kMiss);
        CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, s_dn, e_dn, 0, fh) ==
              CollisionWorld::FaceRaycast::kHit);
        CHECK(fh.material == 17);
    }
    {
        // 0x800 double-sided accepts the from-below ray; flag 1 does too.
        Rig rig(face_quad_model(14, 0x800));
        CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, s_up, e_up, 0, fh) ==
              CollisionWorld::FaceRaycast::kHit);
    }
    {
        Rig rig(face_quad_model(14, 1));
        CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, s_up, e_up, 0, fh) ==
              CollisionWorld::FaceRaycast::kHit);
    }
}

void test_face_raycast_husk_swap() {
    // Intact model material 14; husk model material 5. Flags & 4 must swap the
    // face set the ray walks [orig: the (Flags & 4) huskModel pick @ 0x4e4d68].
    Rig rig(face_quad_model(14, 0));
    const int32_t husk_id = rig.cw.add_model(face_quad_model(5, 0));
    rig.cw.assign_entity_husk(rig.building, husk_id);
    const int32_t s_dn[3] = {fx(10.0), fx(10.0), fx(3.0)};
    const int32_t e_dn[3] = {fx(10.0), fx(10.0), fx(-1.0)};
    RayFaceHit fh;
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, s_dn, e_dn, 0, fh) ==
          CollisionWorld::FaceRaycast::kHit);
    CHECK(fh.material == 14);
    rig.world.registry.get(rig.building)->engine_flags |= 0x4u;
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, s_dn, e_dn, 0, fh) ==
          CollisionWorld::FaceRaycast::kHit);
    CHECK(fh.material == 5);
}

void test_face_raycast_husk_omits_spawned_piece_sections() {
    // Death-piece bits describe sections that left the wreck. Once bit 1 is
    // stamped, rays must pass through that part's old location while the hull
    // section (bit 0 never sets in retail) remains solid.
    Rig rig(face_quad_model(14, 0));
    const int32_t husk_id = rig.cw.add_model(two_section_face_model(5, 6));
    rig.cw.assign_entity_husk(rig.building, husk_id);
    Entity *building = rig.world.registry.get(rig.building);
    building->engine_flags |= 0x4u;
    building->bound_radius = 8.0f;

    const int32_t root_start[3] = {fx(7.0), fx(10.0), fx(3.0)};
    const int32_t root_end[3] = {fx(7.0), fx(10.0), fx(-1.0)};
    const int32_t piece_start[3] = {fx(13.0), fx(10.0), fx(3.0)};
    const int32_t piece_end[3] = {fx(13.0), fx(10.0), fx(-1.0)};
    RayFaceHit fh;

    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, piece_start, piece_end, 0, fh) ==
          CollisionWorld::FaceRaycast::kHit);
    CHECK(fh.section == 1 && fh.material == 6);

    building->spawned_piece_mask = 1u << 1;
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, piece_start, piece_end, 0, fh) ==
          CollisionWorld::FaceRaycast::kMiss);
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, root_start, root_end, 0, fh) ==
          CollisionWorld::FaceRaycast::kHit);
    CHECK(fh.section == 0 && fh.material == 5);
}

void test_face_raycast_uses_callback_matrix_per_section() {
    // The retail model callback returns one FINAL world matrix per COBJ and the
    // projectile walker pairs both arrays by ordinal. Section 1 is posed +4u on
    // X while section 0 stays at its entity placement. The old shared-matrix
    // target_view hits x=13 and misses x=17; the callback-correct path reverses
    // those verdicts without moving the root section.
    Rig rig(two_section_face_model(5, 6));
    // JetSki's two retail COBJ rows both name parent 0. Pin that hierarchy
    // metadata cannot collapse the ordinal matrix pairing onto slot 0.
    CollisionModel *model = const_cast<CollisionModel *>(rig.cw.model(0));
    CHECK(model != nullptr && model->sections.size() == 2);
    if (model != nullptr && model->sections.size() == 2) {
        model->sections[0].parent_part_index = 0;
        model->sections[1].parent_part_index = 0;
    }
    rig.world.registry.get(rig.building)->bound_radius = 12.0f;
    TwoSectionMatrixProvider provider;
    rig.cw.set_section_matrix_provider(&provider);

    RayFaceHit fh;
    const int32_t old_piece_start[3] = {fx(13.0), fx(10.0), fx(3.0)};
    const int32_t old_piece_end[3] = {fx(13.0), fx(10.0), fx(-1.0)};
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building,
                                      old_piece_start, old_piece_end, 0, fh) ==
          CollisionWorld::FaceRaycast::kMiss);

    const int32_t moved_piece_start[3] = {fx(17.0), fx(10.0), fx(3.0)};
    const int32_t moved_piece_end[3] = {fx(17.0), fx(10.0), fx(-1.0)};
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building,
                                      moved_piece_start, moved_piece_end, 0, fh) ==
          CollisionWorld::FaceRaycast::kHit);
    CHECK(fh.section == 1 && fh.material == 6);

    const int32_t root_start[3] = {fx(7.0), fx(10.0), fx(3.0)};
    const int32_t root_end[3] = {fx(7.0), fx(10.0), fx(-1.0)};
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building,
                                      root_start, root_end, 0, fh) ==
          CollisionWorld::FaceRaycast::kHit);
    CHECK(fh.section == 0 && fh.material == 5);
}

void test_face_raycast_rolled_entity() {
    // A static authored with roll must lean its collision shell WITH the
    // visual [orig: the spawn orientation matrix Rz(90-yaw)*Ry(-pitch)*Rx(roll)
    // @ 0x613f40 serves every collision query] — the yaw-only stand-in left
    // tilted rocks' shells upright, and rays threaded past them where the
    // model still looked solid (the 00TRg RckS07 through-shot).
    Rig rig(face_quad_model(14, 0));
    Entity *b = rig.world.registry.get(rig.building);
    b->bound_radius = 3.0f;

    // Control: yaw-only (roll 0), the straight-down ray hits the z=1 quad.
    const int32_t s_dn[3] = {fx(10.0), fx(10.0), fx(3.0)};
    const int32_t e_dn[3] = {fx(10.0), fx(10.0), fx(-1.0)};
    RayFaceHit fh;
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, s_dn, e_dn, 0, fh) ==
          CollisionWorld::FaceRaycast::kHit);

    // Roll the entity 90 deg: the quad plane stands up at world y = 9 facing -y.
    b->roll = 90;
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, s_dn, e_dn, 0, fh) ==
          CollisionWorld::FaceRaycast::kMiss); // the old plane position is empty air now
    const int32_t s_fw[3] = {fx(10.0), fx(7.0), fx(0.0)};
    const int32_t e_fw[3] = {fx(10.0), fx(11.0), fx(0.0)};
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, s_fw, e_fw, 0, fh) ==
          CollisionWorld::FaceRaycast::kHit); // the rolled plane catches the level ray
    CHECK(fh.material == 14);
    // dist = 2.0 of the 4.0 u segment (plane at world y = 9).
    CHECK(fh.dist > fx(1.9) && fh.dist < fx(2.1));
    b->roll = 0;
}

void test_face_raycast_pitched_entity() {
    // Exercise the production target_view scratch path, not just the Euler
    // helper. At heading 0, +90 authored pitch applies Ry(-90), rotating the
    // local z=1 quad onto world x=9 with its front normal facing -x.
    Rig rig(face_quad_model(14, 0));
    Entity *b = rig.world.registry.get(rig.building);
    b->bound_radius = 3.0f;

    const int32_t s_dn[3] = {fx(10.0), fx(10.0), fx(3.0)};
    const int32_t e_dn[3] = {fx(10.0), fx(10.0), fx(-1.0)};
    RayFaceHit fh;
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, s_dn, e_dn, 0, fh) ==
          CollisionWorld::FaceRaycast::kHit);

    b->pitch = 90;
    CHECK(rig.cw.raycast_entity_faces(rig.world, rig.building, s_dn, e_dn, 0, fh) ==
          CollisionWorld::FaceRaycast::kMiss);
    const int32_t s_across[3] = {fx(8.0), fx(10.0), fx(0.0)};
    const int32_t e_across[3] = {fx(10.0), fx(10.0), fx(0.0)};
    CHECK(rig.cw.raycast_entity_faces(
                  rig.world, rig.building, s_across, e_across, 0, fh) ==
          CollisionWorld::FaceRaycast::kHit);
    CHECK(fh.material == 14);
    CHECK(fh.dist > fx(0.95) && fh.dist < fx(1.05));
    b->pitch = 0;
}

void test_round_inside_bound_sphere_hits_wall() {
    // The shoot-through-walls regression: a tick segment lying entirely INSIDE
    // a big bound sphere must still reach the face walk — the witnessed broad
    // phase is an UNCLAMPED perpendicular line-distance gate [orig:
    // Projectile_RaycastProximitySlots @ 0x4e5492], with no boundary-crossing
    // requirement. The old segment-vs-sphere gate returned no-hit for
    // inside-sphere segments, so standing inside a building's bound sphere let
    // rounds cross its walls untested.
    Rig rig(face_quad_model(14, 0));
    Entity *b = rig.world.registry.get(rig.building);
    b->bound_radius = 3.0f;

    RoundSim &rs = rig.world.round_sim;
    LiveRound &r = rs.rounds[0];
    r.active = true;
    rs.active_count = 1;
    r.owner = EntityHandle{};              // no shooter entity in this rig
    r.ammo_index = -1;                     // no ammo table: flight/stop only
    r.pos = Vec3{10.0f, 10.0f, 2.5f};      // 2.5 u from center: inside the sphere
    r.vel = Vec3{0.0f, 0.0f, -4.0f};       // ends at z -1.5: still inside
    r.max_age_ticks = 100;
    rs.tick(rig.world, nullptr, &rig.cw);
    CHECK(!r.active); // the round stopped on the quad instead of flying through
    CHECK(rs.impacts.size() == 1);
    CHECK(rs.impacts[0].effect_tag == 4 + 14); // the face material tag
    CHECK(rs.impacts[0].position.z > 0.95f && rs.impacts[0].position.z < 1.05f);

    // Inside the sphere but past the quad's edge: the face walk misses and the
    // round flies on — the fixed gate must not reintroduce midair sphere stops.
    LiveRound &r2 = rs.rounds[1];
    r2.active = true;
    rs.active_count = 1;
    r2.owner = EntityHandle{};
    r2.ammo_index = -1;
    r2.pos = Vec3{11.5f, 10.0f, 2.0f};     // dist 2.5 < 3: inside; x past the quad
    r2.vel = Vec3{0.0f, 0.0f, -4.0f};
    r2.max_age_ticks = 100;
    rs.tick(rig.world, nullptr, &rig.cw);
    CHECK(r2.active);              // flew on
    CHECK(rs.impacts.size() == 1); // no new impact
}

// Equal-distance entity hits keep the first retail dispatch winner: pool 2
// statics before pool 1 dynamics, before the later organic list.
void test_round_equal_distance_uses_retail_pool_order() {
    World world;
    world.registry.configure_pool(0, 4);
    world.registry.configure_pool(1, 4);
    world.registry.configure_pool(2, 4);

    Entity seed;
    seed.kind = EntityKind::Item;
    seed.health = 100;
    seed.position = Vec3{5.0f, 0.0f, 0.9f};
    seed.bound_radius = kOrganicStandInRadius;
    const EntityHandle dynamic = world.registry.spawn(1, seed);
    const EntityHandle statik = world.registry.spawn(2, seed);
    Entity organic_seed = seed;
    organic_seed.kind = EntityKind::Organic;
    organic_seed.position.z = 0.0f; // torso center is +0.9, identical to the items
    const EntityHandle organic = world.registry.spawn(0, organic_seed);
    CHECK(dynamic.valid() && statik.valid() && organic.valid());

    LiveRound &round = world.round_sim.rounds[0];
    round.active = true;
    world.round_sim.active_count = 1;
    round.pos = Vec3{0.0f, 0.0f, 0.9f};
    round.vel = Vec3{10.0f, 0.0f, 0.0f};
    round.max_age_ticks = 100;
    world.round_sim.tick(world, nullptr, nullptr);

    CHECK(world.round_sim.debug_trail_count == 1);
    CHECK(world.round_sim.debug_trail[0].entity == statik.packed);
}

// Projectile_RaycastProximitySlots excludes the complete witnessed
// Flags&0x02000001 mask. A bit-25 target must not stop the round; the eligible
// item behind it becomes the winner.
void test_round_item_skip_mask() {
    World world;
    world.registry.configure_pool(1, 4);

    Entity skipped_seed;
    skipped_seed.kind = EntityKind::Item;
    skipped_seed.health = 100;
    skipped_seed.position = Vec3{4.0f, 0.0f, 0.0f};
    skipped_seed.bound_radius = 1.0f;
    skipped_seed.engine_flags = 0x02000000u;
    const EntityHandle skipped = world.registry.spawn(1, skipped_seed);

    Entity hit_seed = skipped_seed;
    hit_seed.position = Vec3{8.0f, 0.0f, 0.0f};
    hit_seed.engine_flags = 0;
    const EntityHandle eligible = world.registry.spawn(1, hit_seed);
    CHECK(skipped.valid() && eligible.valid());

    LiveRound &round = world.round_sim.rounds[0];
    round.active = true;
    world.round_sim.active_count = 1;
    round.pos = Vec3{};
    round.vel = Vec3{12.0f, 0.0f, 0.0f};
    round.max_age_ticks = 100;
    world.round_sim.tick(world, nullptr, nullptr);

    CHECK(world.round_sim.debug_trail_count == 1);
    CHECK(world.round_sim.debug_trail[0].entity == eligible.packed);
}

// Indestructible is a damage-zeroing gate, not a collision filter. The front
// organic still stops the round and protects a vulnerable target behind it.
void test_round_indestructible_organic_still_collides() {
    World world;
    world.registry.configure_pool(0, 4);

    Entity front_seed;
    front_seed.kind = EntityKind::Organic;
    front_seed.health = 100;
    front_seed.position = Vec3{4.0f, 0.0f, 0.0f};
    front_seed.engine_flags = kEntityFlagIndestructible;
    const EntityHandle front = world.registry.spawn(0, front_seed);

    Entity rear_seed = front_seed;
    rear_seed.position.x = 8.0f;
    rear_seed.engine_flags = 0;
    const EntityHandle rear = world.registry.spawn(0, rear_seed);
    CHECK(front.valid() && rear.valid());

    AmmoTableEntry ammo;
    ammo.valid = true;
    ammo.weight_in_grains = 875;
    world.ammo.entries.push_back(ammo);

    LiveRound &round = world.round_sim.rounds[0];
    round.active = true;
    world.round_sim.active_count = 1;
    round.pos = Vec3{0.0f, 0.0f, kOrganicStandInCenterZ};
    round.vel = Vec3{12.0f, 0.0f, 0.0f};
    round.ammo_index = 0;
    round.max_age_ticks = 100;
    world.round_sim.tick(world, nullptr, nullptr);

    CHECK(world.round_sim.debug_trail_count == 1);
    CHECK(world.round_sim.debug_trail[0].entity == front.packed);
    CHECK(world.round_sim.debug_trail[0].organic_fallback);
    CHECK(world.round_sim.debug_trail[0].section == 1);
    CHECK(world.round_sim.debug_trail[0].secondary_section == 1);
    CHECK(world.registry.get(front)->health == 100);
    CHECK(world.registry.get(rear)->health == 100);
}

void test_round_person_sections_drive_hit_and_death_animation() {
    World world;
    world.registry.configure_pool(0, 4);

    Entity victim_seed;
    victim_seed.kind = EntityKind::Organic;
    victim_seed.health = 775;
    victim_seed.alive = true;
    victim_seed.position = Vec3{5.0f, 0.0f, 0.0f};
    victim_seed.yaw = 90; // engine heading 0: +X round is the back quadrant
    const EntityHandle victim = world.registry.spawn(0, victim_seed);
    CHECK(victim.valid());

    // Both spheres overlap. Retail's reverse walk publishes head 14 as the
    // primary reaction/death bone and section 2 as the secondary damage zone.
    CollisionModel model;
    model.sections.resize(15);
    model.sections[2].radius = fx(1.0);
    model.sections[14].radius = fx(1.0);
    CollisionWorld collision;
    const int32_t model_id = collision.add_model(std::move(model));
    collision.assign_entity(victim, model_id);

    AmmoTableEntry ammo;
    ammo.valid = true;
    ammo.weight_in_grains = 875;
    world.ammo.entries.push_back(ammo);

    LiveRound &round = world.round_sim.rounds[0];
    round.active = true;
    world.round_sim.active_count = 1;
    round.pos = Vec3{0.0f, 0.0f, 0.0f};
    round.vel = Vec3{10.0f, 0.0f, 0.0f};
    round.ammo_index = 0;
    round.max_age_ticks = 100;
    world.round_sim.tick(world, nullptr, &collision);

    CHECK(!round.active);
    CHECK(world.registry.get(victim)->health == 0);
    CHECK(world.round_sim.hits.size() == 1);
    if (!world.round_sim.hits.empty()) {
        CHECK(world.round_sim.hits[0].victim == victim);
        // speed 10 * 62, grains 875 => base 620; damage reads ray[32]=2,
        // whose retail multiplier is 1.25. Using primary head 14 would be 1860.
        CHECK(world.round_sim.hits[0].damage == 775);
        CHECK(world.round_sim.hits[0].primary_section == 14);
        CHECK(world.round_sim.hits[0].secondary_section == 2);
    }
    // Head group (base + 8) plus back quadrant 2. The former torso stand-in
    // would select 186, so this exact 190 assertion catches lost bone routing.
    CHECK(world.registry.get(victim)->death_anim_state == 190);

    CHECK(world.round_sim.debug_trail_count == 1);
    if (world.round_sim.debug_trail_count == 1) {
        const RoundDebugEvent &event = world.round_sim.debug_trail[0];
        CHECK(event.kind == RoundDebugEvent::kOrganic);
        CHECK(event.entity == victim.packed);
        CHECK(event.material == 19);
        CHECK(event.section == 14);
        CHECK(event.secondary_section == 2);
        CHECK(!event.organic_fallback);
        CHECK(event.hit.x > 4.46f && event.hit.x < 4.48f);
    }
}

int main() {
    test_matrix_roundtrip();
    test_retail_render_pose_matrix_roundtrip_and_order();
    test_blink_query_and_refresh();
    test_sound_occlusion();
    test_ray_clip();
    test_ground_probe_roof();
    test_resolver_wall_pushout();
    test_secondary_vertical_force_is_full_strength();
    test_platform_contact_uses_positive_authored_pitch();
    test_resolver_hurt_and_zones();
    test_idle_skip_throttle();
    test_slice_cadence_and_invuln();
    test_pool1_item_is_one_collision_candidate();
    test_platform_latch();
    test_debug_seams();
    test_raycast_clear_los();
    test_los_point_bias();
    test_proximity_tables_use_host_bound_radius();
    test_face_raycast();
    test_face_raycast_flags_and_materials();
    test_face_raycast_husk_swap();
    test_face_raycast_husk_omits_spawned_piece_sections();
    test_face_raycast_uses_callback_matrix_per_section();
    test_person_section_raycast_uses_posed_bone_matrix();
    test_f3_debug_prefilters_before_building_section_matrices();
    test_late_person_instance_is_demand_resolved_for_rounds_and_debug();
    test_reused_registry_slot_rejects_old_collision_identity();
    test_person_section_reverse_scan_and_mask();
    test_person_section_retail_radius_rules();
    test_face_raycast_rolled_entity();
    test_face_raycast_pitched_entity();
    test_round_inside_bound_sphere_hits_wall();
    test_round_equal_distance_uses_retail_pool_order();
    test_round_item_skip_mask();
    test_round_indestructible_organic_still_collides();
    test_round_person_sections_drive_hit_and_death_animation();
    if (failures == 0) std::printf("collision_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
