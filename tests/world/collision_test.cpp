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
//   * resolver wall push-out with prev-position gating, and the idle skip throttle.
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
        cw.build_tick_tables(world);
    }

    void move_soldier(double x, double y, double z) {
        Entity *s = world.registry.get(soldier);
        s->position = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
        cw.build_tick_tables(world);
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
                          /*anim=*/43, health);

    // Step into the wall: prev pos (12.8) was outside the +X plane -> it separates.
    pos[0] = fx(11.6);
    Entity *s = rig.world.registry.get(rig.soldier);
    s->position.x = 11.6f;
    rig.cw.build_tick_tables(rig.world);
    const int32_t before = pos[0];
    rig.cw.resolve_entity(rig.world, rig.soldier, state, pos, vel, vel[2], 0, fx(1.8), 0, 0,
                          false, true, 0, 43, health);
    CHECK(pos[0] > before); // pushed back toward +X (out of the wall)
    CHECK(health == 100);   // solid volumes never hurt
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
                          false, true, 0, 43, health);
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
                           0, 0, false, true, 0, 43, lhealth);
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
                           0, 0, false, true, 0, 43, ahealth);
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
                              0, false, true, t, 43, health);
    // Tick 12: skip path — the caller's gravity displacement is reverted and vel zeroed.
    vel[2] = -400;
    pos[2] -= 400 * 2; // what the caller's gravity integration just did
    const int32_t sunk = pos[2];
    const int32_t ret = rig.cw.resolve_entity(rig.world, rig.soldier, state, pos, vel, vel[2],
                                              0, fx(1.8), 0, 0, false, true, 12, 43, health);
    CHECK(ret == 0);
    CHECK(vel[2] == 0);
    CHECK(pos[2] == sunk + 2 * 400); // [orig: pos.Z -= 2*slideDecay on the skip path]
}

} // namespace

int main() {
    test_matrix_roundtrip();
    test_blink_query_and_refresh();
    test_ray_clip();
    test_ground_probe_roof();
    test_resolver_wall_pushout();
    test_resolver_hurt_and_zones();
    test_idle_skip_throttle();
    if (failures == 0) std::printf("collision_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
