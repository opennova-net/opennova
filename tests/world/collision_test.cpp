// World-object collision unit tests [orig: the Jointops.exe query/resolver set —
// Entity_TestCollisionSections @0x4aef90, Entity_RaycastCollisionModel @0x413060,
// Entity_ComputeBoneCollisionForce @0x4ae150, the movement collision resolver
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

// One authored CFAC triangle in the local X=0 plane. A +X ray crosses its
// -X Q14 normal from positive to nonpositive, the retail front-facing case.
CollisionModel wall_triangle_model(uint32_t material_flags = 0,
                                   uint8_t poly_type = 5) {
    CollisionModel m;
    auto vertex = [&](double x, double y, double z) {
        CollisionVertex v;
        v.p[0] = fx(x);
        v.p[1] = fx(y);
        v.p[2] = fx(z);
        m.vertices.push_back(v);
    };
    vertex(0.0, -2.0, 0.0);
    vertex(0.0, 2.0, 0.0);
    vertex(0.0, 0.0, 2.0);

    CollisionNormal n;
    n.n[0] = -16384;
    n.dominant_axis = 4; // YZ projection
    m.normals.push_back(n);

    CollisionFace f;
    f.vertex_index[0] = 0;
    f.vertex_index[1] = 1;
    f.vertex_index[2] = 2;
    f.normal_index = 0;
    f.min[0] = f.max[0] = 0;
    f.min[1] = fx(-2.0);
    f.max[1] = fx(2.0);
    f.min[2] = 0;
    f.max[2] = fx(2.0);
    f.material_flags = material_flags;
    f.poly_type = poly_type;
    m.faces.push_back(f);

    CollisionSection s;
    s.vertex_count = 3;
    s.normal_count = 1;
    s.face_count = 1;
    m.sections.push_back(s);
    return m;
}

// A centered triangle in the plane selected by normal_axis. This pins all
// three CNRM dominant-axis encodings used by Math_PointInTriangle2D.
CollisionModel axis_triangle_model(int normal_axis) {
    CollisionModel m;
    const int u_axis = normal_axis == 0 ? 1 : 0;
    const int v_axis = normal_axis == 2 ? 1 : 2;
    auto vertex = [&](double u, double v) {
        CollisionVertex p;
        p.p[u_axis] = fx(u);
        p.p[v_axis] = fx(v);
        m.vertices.push_back(p);
    };
    vertex(-2.0, -2.0);
    vertex(2.0, -2.0);
    vertex(0.0, 2.0);

    CollisionNormal n;
    n.n[normal_axis] = -16384;
    n.dominant_axis = normal_axis == 0 ? 4 : (normal_axis == 1 ? 2 : 1);
    m.normals.push_back(n);

    CollisionFace f;
    f.vertex_index[0] = 0;
    f.vertex_index[1] = 1;
    f.vertex_index[2] = 2;
    f.normal_index = 0;
    f.min[u_axis] = f.min[v_axis] = fx(-2.0);
    f.max[u_axis] = f.max[v_axis] = fx(2.0);
    m.faces.push_back(f);

    CollisionSection s;
    s.vertex_count = 3;
    s.normal_count = 1;
    s.face_count = 1;
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

    // The special inverse divides a uniformly scaled basis by scale squared.
    CollisionMatrix scaled = m;
    constexpr int rotation_indices[] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
    for (int index : rotation_indices) scaled.m[index] *= 2;
    CollisionMatrix scaled_inverse;
    CHECK(scaled.invert_uniform_scale_into(scaled_inverse, fx(2.0)));
    int32_t scaled_world[3] = {};
    int32_t scaled_local[3] = {};
    scaled.transform_point(local, scaled_world);
    const int32_t scaled_tmp[3] = {
        scaled_world[0] + scaled_inverse.m[3],
        scaled_world[1] + scaled_inverse.m[7],
        scaled_world[2] + scaled_inverse.m[11],
    };
    scaled_inverse.rotate_point(scaled_tmp, scaled_local);
    CHECK(std::abs(scaled_local[0] - local[0]) <= 4);
    CHECK(std::abs(scaled_local[1] - local[1]) <= 4);
    CHECK(std::abs(scaled_local[2] - local[2]) <= 4);
    CHECK(!scaled.invert_uniform_scale_into(scaled_inverse, 1));

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

void test_negative_static_slot_candidates_and_blink() {
    Rig rig(box_model(8, 0x3C, 4.0, 4.0, 3.0), -10.0, -10.0);
    const CollisionWorld::StaticSlotView slot = rig.cw.static_slot(0);
    CHECK(slot.x == 0xFFF6u && slot.y == 0xFFF6u);
    CHECK(static_slot_coord_units(slot.x) == -10);
    CHECK(static_slot_coord_q16(slot.y) == fx(-10.0));

    // The organic slice builder consumes the same signed static-table words.
    rig.move_soldier(-10.0, -10.0, 0.5);
    int32_t candidate_count = 0;
    const EntityHandle *candidates =
            rig.cw.candidate_slice(rig.soldier, candidate_count);
    CHECK(candidate_count == 1);
    CHECK(candidates != nullptr && candidates[0] == rig.building);

    // A non-organic refresh walks the static building prefix directly instead
    // of using a candidate slice; the arbitrary-point query does the same.
    Entity marker;
    marker.kind = EntityKind::Marker;
    marker.position = {-10.0f, -10.0f, 0.5f};
    marker.alive = true;
    const EntityHandle marker_h = rig.world.registry.spawn(0, marker);
    Entity *marker_entity = rig.world.registry.get(marker_h);
    rig.cw.refresh_blink(rig.world, *marker_entity);
    CHECK((marker_entity->flags & kEntityFlagIndoors) != 0);
    CHECK(marker_entity->blink_hits[0] != 0);

    BlinkAccum accum;
    const int32_t point[3] = {fx(-10.0), fx(-10.0), fx(0.5)};
    rig.cw.query_blink_boxes_at_point(rig.world, point, accum);
    CHECK(accum.hit_count == 1);
    CHECK((accum.flags & kBlinkIndoorsBit) != 0);
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
void test_mounted_resolver_keeps_touch_without_parent_pushout() {
    World world;
    world.registry.configure_pool(0, 4);
    world.registry.configure_pool(1, 4);

    Entity gun{};
    gun.kind = EntityKind::Item;
    gun.has_item_def = true;
    gun.health = 100;
    gun.alive = true;
    gun.net_id = 0x20;
    gun.position = Vec3{10.0f, 10.0f, 0.0f};
    gun.yaw = 90;
    Seat seat{};
    seat.type = SeatType::Gunner;
    gun.seats.push_back(seat);
    const EntityHandle gun_h = world.registry.spawn(1, gun);

    Entity rider{};
    rider.kind = EntityKind::Organic;
    rider.health = 100;
    rider.alive = true;
    rider.net_id = 0x10;
    rider.position = Vec3{12.8f, 10.0f, 0.0f};
    const EntityHandle rider_h = world.registry.spawn(0, rider);

    // One parent model carries both an ordinary solid and an armory trigger.
    // The solid proves the contact would push an on-foot source; the trigger
    // proves mounted resolution still dispatches touch state.
    CollisionModel model = box_model(1, 0, 2.0, 2.0, 3.0);
    CollisionModel armory = box_model(6, 0, 2.0, 2.0, 3.0);
    CollisionVolume trigger = armory.volumes.front();
    trigger.plane_start = static_cast<int32_t>(model.planes.size());
    model.planes.insert(model.planes.end(), armory.planes.begin(), armory.planes.end());
    model.volumes.push_back(trigger);
    model.sections.front().volume_count = 2;

    CollisionWorld collision;
    const int32_t model_id = collision.add_model(std::move(model));
    collision.assign_entity(gun_h, model_id);
    CHECK(world.commands.mount(0x10, 0x20));
    for (int i = 0; i < 17; ++i) collision.build_tick_tables(world);

    int32_t pos[3] = {fx(12.8), fx(10.0), 0};
    int32_t vel[3] = {0, 0, 0};
    int16_t health = 100;
    CollisionWorld::ResolveState mounted_state;
    collision.resolve_entity(world, rider_h, mounted_state, pos, vel, vel[2], 0,
                             fx(1.8), 0, 0, false, true, 0, 43, 0u, health);

    Entity *rider_live = world.registry.get(rider_h);
    rider_live->position.x = 11.6f;
    for (int i = 0; i < 17; ++i) collision.build_tick_tables(world);
    pos[0] = fx(11.6);
    const int32_t mounted_before = pos[0];
    collision.resolve_entity(world, rider_h, mounted_state, pos, vel, vel[2], 0,
                             fx(1.8), 0, 0, false, true, 1, 43, 0u, health);

    // Retail's carried-source latch suppresses model force, not the contact walk.
    // [orig: savedPosY @0x4b2be0..0x4b2d3f; force gates
    //  @0x4b3045..0x4b30af/@0x4b3658..0x4b36b9]
    CHECK(pos[0] == mounted_before);
    CHECK((rider_live->flags & kEntityFlagArmoryZone) != 0);

    // The identical model transition pushes once the source is on foot, proving
    // that the mounted equality above observes suppression rather than a miss.
    CHECK(world.commands.dismount(0x10));
    rider_live->position.x = 12.8f;
    for (int i = 0; i < 17; ++i) collision.build_tick_tables(world);
    CollisionWorld::ResolveState on_foot_state;
    pos[0] = fx(12.8);
    collision.resolve_entity(world, rider_h, on_foot_state, pos, vel, vel[2], 0,
                             fx(1.8), 0, 0, false, true, 0, 43, 0u, health);
    rider_live->position.x = 11.6f;
    for (int i = 0; i < 17; ++i) collision.build_tick_tables(world);
    pos[0] = fx(11.6);
    const int32_t on_foot_before = pos[0];
    collision.resolve_entity(world, rider_h, on_foot_state, pos, vel, vel[2], 0,
                             fx(1.8), 0, 0, false, true, 1, 43, 0u, health);
    CHECK(pos[0] > on_foot_before);
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
    LadderContact ladder;
    ContactResult result;
    CHECK(collision_contact_force(target, query, blink, ladder, result));
    CHECK(result.force[0] < 0);
    CHECK(result.force[2] < 0);
    CHECK(std::abs(result.force[2] - result.force[0]) <= 1);
}

// ---------------------------------------------------------------------------
void test_ladder_contact_uses_positive_authored_pitch() {
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
    LadderContact ladder;
    ContactResult result;
    CHECK(!collision_contact_force(target, query, blink, ladder, result));
    CHECK((result.flags & 0x1u) != 0);
    CHECK(ladder.valid);
    // Plane 0 has no vertical normal component, so retail's
    // target.pitch - normal.pitch leg must preserve the raw positive value.
    CHECK(ladder.pitch == bam_from_degrees_wrapped(30.0));
}

// ---------------------------------------------------------------------------
void test_vehicle_collision_volume_selection() {
    const int32_t target_pos[3] = {0, 0, 0};
    const CollisionMatrix matrix = collision_matrix_from_heading(0, target_pos);

    CollisionTargetView target;
    target.matrices = &matrix;
    target.bound_radius = fx(4.0);

    const CollisionPoint point{fx(1.5), 0, fx(5.0), 0};
    const int32_t radius = fx(1.0);
    ContactQuery query;
    query.points = &point;
    query.radii = &radius;
    query.num_points = 1;
    query.prev_pos[0] = fx(3.5);
    query.prev_pos[2] = fx(5.0);
    query.source_bound_radius = fx(4.0);

    auto contact_x = [&](CollisionModel &model, uint32_t mask, int32_t &force_x) {
        model.finalize_sections();
        target.model = &model;
        query.mask = mask;
        BlinkAccum blink;
        LadderContact ladder;
        ContactResult result;
        const bool contact = collision_contact_force(target, query, blink, ladder, result);
        force_x = result.force[0];
        return contact;
    };

    // VC itself is invisible to an ordinary actor pass and solid to mask 0x8.
    CollisionModel vc_only = box_model(7, 0, 2.0, 10.0, 10.0);
    int32_t unused_force = 0;
    CHECK(!contact_x(vc_only, 0, unused_force));
    int32_t vc_force = 0;
    CHECK(contact_x(vc_only, 0x8, vc_force));
    CHECK(vc_force < 0);

    // Retail falls back to CB/default solids when the section has no VC/VK run.
    CollisionModel cb_only = box_model(1, 0, 3.0, 10.0, 10.0);
    int32_t cb_force = 0;
    CHECK(contact_x(cb_only, 0x8, cb_force));
    CHECK(cb_force < 0);

    // Once a VC run exists, the vehicle pass begins there and does not also apply
    // the earlier CB. Different widths let the selected force identify the run.
    CollisionModel mixed = box_model(1, 0, 3.0, 10.0, 10.0);
    CollisionModel vc_part = box_model(7, 0, 2.0, 10.0, 10.0);
    CollisionVolume vc = vc_part.volumes.front();
    vc.plane_start = static_cast<int32_t>(mixed.planes.size());
    mixed.planes.insert(mixed.planes.end(), vc_part.planes.begin(), vc_part.planes.end());
    mixed.volumes.push_back(vc);
    mixed.sections.front().volume_count = 2;

    int32_t mixed_vehicle_force = 0;
    CHECK(contact_x(mixed, 0x8, mixed_vehicle_force));
    CHECK(mixed_vehicle_force == vc_force);
    CHECK(mixed_vehicle_force != cb_force);

    int32_t mixed_actor_force = 0;
    CHECK(contact_x(mixed, 0, mixed_actor_force));
    CHECK(mixed_actor_force == cb_force);
}

// ---------------------------------------------------------------------------
void test_named_gameplay_volume_dispatch() {
    const int32_t target_pos[3] = {0, 0, 0};
    const CollisionMatrix matrix = collision_matrix_from_heading(0, target_pos);
    CollisionTargetView target;
    target.matrices = &matrix;
    target.bound_radius = fx(4.0);

    const CollisionPoint point{fx(1.5), 0, fx(1.0), 0};
    const int32_t radius = fx(1.0);
    ContactQuery query;
    query.points = &point;
    query.radii = &radius;
    query.num_points = 1;
    query.prev_pos[0] = fx(3.5);
    query.prev_pos[2] = fx(1.0);
    query.source_bound_radius = fx(4.0);

    auto run = [&](int type, uint8_t mask, bool grounded, ContactResult &result) {
        CollisionModel model = box_model(type, 0, 2.0, 2.0, 2.0);
        model.finalize_sections();
        target.model = &model;
        target.is_ground_of_source = grounded;
        query.mask = mask;
        BlinkAccum blink;
        LadderContact ladder;
        return collision_contact_force(target, query, blink, ladder, result);
    };

    // CD is a non-solid door activation touch and carries the touched door section.
    ContactResult cd;
    CHECK(!run(9, 0, false, cd));
    CHECK((cd.flags & 0x20u) != 0);
    CHECK(cd.door_sections == 1u);

    // CT is the non-solid change-team box signal. Its downstream request bridge is
    // deliberately tracked separately as D-COL-6.
    ContactResult ct;
    CHECK(!run(10, 0, false, ct));
    CHECK((ct.flags & 0x200u) != 0);

    // CF only activates when the source is already grounded on the target.
    ContactResult cf_airborne;
    CHECK(!run(13, 0, false, cf_airborne));
    CHECK((cf_airborne.flags & 0x800u) == 0);
    ContactResult cf_grounded;
    CHECK(!run(13, 0, true, cf_grounded));
    CHECK((cf_grounded.flags & 0x800u) != 0);

    // DH/DM/DL are non-solid contact-damage signals, high through low.
    ContactResult dh, dm, dl;
    CHECK(!run(16, 0, false, dh));
    CHECK(!run(17, 0, false, dm));
    CHECK(!run(18, 0, false, dl));
    CHECK(dh.flags == 0x100u);
    CHECK(dm.flags == 0x80u);
    CHECK(dl.flags == 0x40u);

    // CP is physical collision for a player query only; an AI query walks through.
    ContactResult cp_ai;
    CHECK(!run(19, 0, false, cp_ai));
    ContactResult cp_player;
    CHECK(run(19, 0x2, false, cp_player));
    CHECK(cp_player.force[0] < 0);
}

// ---------------------------------------------------------------------------
void test_resolver_damage_grades_and_zones() {
    auto health_after_touch = [](int type, bool authority, uint32_t engine_flags) {
        Rig rig(box_model(type, 0, 3.0, 3.0, 3.0));
        Entity *soldier = rig.world.registry.get(rig.soldier);
        soldier->engine_flags |= engine_flags;
        rig.move_soldier(10.0, 10.0, 0.5);
        int32_t pos[3] = {fx(10.0), fx(10.0), fx(0.5)};
        int32_t vel[3] = {0, 0, 0};
        int16_t health = 100;
        CollisionWorld::ResolveState state;
        rig.cw.resolve_entity(rig.world, rig.soldier, state, pos, vel, vel[2], 0,
                              fx(1.8), 0, 0, false, authority, 0, 43, 0u, health);
        return health;
    };

    CHECK(health_after_touch(16, true, 0) == 50);  // DH: -50 HP
    CHECK(health_after_touch(17, true, 0) == 94);  // DM: -6 HP
    CHECK(health_after_touch(18, true, 0) == 99);  // DL: -1 HP
    CHECK(health_after_touch(16, false, 0) == 100); // authority-only
    CHECK(health_after_touch(16, true, 0x4000000u) == 100); // indestructible

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
    // A completed pool-table build is live even when this particular world has
    // no static or dynamic entries. Callers use this distinction to keep the
    // whole-registry compatibility fallback only for worlds that never built
    // collision tables at all.
    {
        World empty_world;
        CollisionWorld empty_cw;
        CHECK(!empty_cw.tick_tables_ready());
        empty_cw.build_tick_tables(empty_world);
        CHECK(empty_cw.tick_tables_ready());
    }

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
    cw.build_initial_tables(world);
    CHECK(cw.tick_tables_ready());
    CHECK(cw.static_count() == 1);
    CHECK(cw.candidate_count(soldier) == 0);
    for (int i = 0; i < 16; ++i) {
        cw.build_tick_tables(world);
        CHECK(cw.candidate_count(soldier) == 0);
    }
    cw.build_tick_tables(world); // the 17th call builds
    CHECK(cw.candidate_count(soldier) == 1);

    // Spawn/restore can invalidate an already-authoritative mission-init pool
    // snapshot before the first candidate epoch. Refresh must publish the new
    // pool membership immediately without spending one of those 16 logic ticks.
    {
        World refresh_world;
        CollisionWorld refresh_cw;
        refresh_world.registry.configure_pool(0, 4);
        refresh_world.registry.configure_pool(2, 4);

        Entity source_seed;
        source_seed.kind = EntityKind::Organic;
        source_seed.alive = true;
        const EntityHandle source = refresh_world.registry.spawn(0, source_seed);
        refresh_cw.build_initial_tables(refresh_world);
        CHECK(refresh_cw.static_count() == 0);

        Entity blocker_seed;
        blocker_seed.kind = EntityKind::Building;
        blocker_seed.position = {5.0f, 0.0f, 0.0f};
        blocker_seed.yaw = 90;
        blocker_seed.alive = true;
        const EntityHandle blocker = refresh_world.registry.spawn(2, blocker_seed);
        const int32_t blocker_model =
                refresh_cw.add_model(box_model(1, 0, 1.0, 1.0, 2.0));
        refresh_cw.assign_entity(blocker, blocker_model);
        refresh_cw.refresh_after_registry_change(refresh_world);
        CHECK(refresh_cw.static_count() == 1);

        const int32_t ray_a[3] = {0, 0, fx(1.0)};
        const int32_t ray_b[3] = {fx(10.0), 0, fx(1.0)};
        CHECK(!refresh_cw.raycast_clear(refresh_world, ray_a, ray_b,
                                        EntityHandle{}, EntityHandle{}));
        CHECK(refresh_cw.candidate_count(source) == 0);
        for (int i = 0; i < 16; ++i) {
            refresh_cw.build_tick_tables(refresh_world);
            CHECK(refresh_cw.candidate_count(source) == 0);
        }
        refresh_cw.build_tick_tables(refresh_world);
        CHECK(refresh_cw.candidate_count(source) == 1);
    }

    // DH/DM/DL volumes never damage an EngineFlags-0x4000000 entity. [orig: the
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
void test_ladder_contact_bookkeeping_is_not_ground() {
    // A CL/type-4 touch records the ladder frame and raw 0x100000 flag even
    // though it produces no push force. This is low-level retail bookkeeping,
    // not a claim that climb locomotion is implemented. A CL is ray-invisible,
    // so the ground-settle tail must not turn it into an ordinary floor.
    Rig rig(box_model(4, 0, 0.25, 2.0, 3.0));
    rig.move_soldier(10.0, 10.0, 1.0);
    int32_t pos[3] = {fx(10.0), fx(10.0), fx(1.0)};
    int32_t vel[3] = {0, 0, 0};
    int16_t health = 100;
    CollisionWorld::ResolveState state;
    rig.cw.resolve_entity(rig.world, rig.soldier, state, pos, vel, vel[2], 0, fx(1.8), 0, 0,
                          false, true, 0, 43, 0u, health);
    Entity *s = rig.world.registry.get(rig.soldier);
    CHECK((s->flags & kEntityFlagLadderContact) != 0);
    CHECK(!s->ground_target.valid());
}

// ---------------------------------------------------------------------------
void test_cb_ground_probe_sets_ground_target() {
    // Ordinary standing-on bookkeeping comes from the post-resolve ground ray,
    // not CL's transient 0x100000 ladder contact.
    Rig rig(box_model(1, 0, 3.0, 3.0, 1.0));
    rig.move_soldier(10.0, 10.0, 1.05);
    int32_t pos[3] = {fx(10.0), fx(10.0), fx(1.05)};
    int32_t vel[3] = {0, 0, 0};
    int16_t health = 100;
    CollisionWorld::ResolveState state;
    rig.cw.resolve_entity(rig.world, rig.soldier, state, pos, vel, vel[2], 0, fx(1.8), 0, 0,
                          false, true, 0, 43, 0u, health);
    const Entity *s = rig.world.registry.get(rig.soldier);
    CHECK((s->flags & kEntityFlagLadderContact) == 0);
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

void test_raycast_clear_table_readiness() {
    const int32_t a[3] = {0, 0, fx(1.0)};
    const int32_t b[3] = {fx(10.0), 0, fx(1.0)};

    auto spawn_blocker = [](World &world, CollisionWorld &cw) {
        world.registry.configure_pool(2, 4);
        Entity seed;
        seed.kind = EntityKind::Building;
        seed.position = {5.0f, 0.0f, 0.0f};
        seed.yaw = 90;
        seed.alive = true;
        const EntityHandle h = world.registry.spawn(2, seed);
        cw.assign_entity(h, cw.add_model(box_model(1, 0, 1.0, 1.0, 2.0)));
        return h;
    };

    // A headless/never-built world keeps the compatibility registry walk.
    {
        World world;
        CollisionWorld cw;
        CHECK(spawn_blocker(world, cw).valid());
        CHECK(!cw.tick_tables_ready());
        CHECK(!cw.raycast_clear(world, a, b, EntityHandle{}, EntityHandle{}));
    }

    // Once an empty table build completed, emptiness is authoritative for that
    // tick. A later spawn appears only when the next table snapshot is built.
    {
        World world;
        CollisionWorld cw;
        world.registry.configure_pool(2, 4);
        cw.build_tick_tables(world);
        CHECK(cw.tick_tables_ready());
        CHECK(spawn_blocker(world, cw).valid());
        CHECK(cw.raycast_clear(world, a, b, EntityHandle{}, EntityHandle{}));
        cw.build_tick_tables(world);
        CHECK(!cw.raycast_clear(world, a, b, EntityHandle{}, EntityHandle{}));
    }
}

void test_raycast_uses_stamped_bound_for_live_section_pose() {
    World world;
    CollisionWorld cw;
    world.registry.configure_pool(2, 4);

    Entity seed;
    seed.kind = EntityKind::Building;
    seed.position = {5.0f, 0.0f, 0.0f};
    seed.yaw = 90;
    seed.alive = true;
    const EntityHandle target = world.registry.spawn(2, seed);
    const int32_t model_id = cw.add_model(box_model(1, 0, 0.5, 0.5, 2.0));
    cw.assign_entity(target, model_id);

    // Animation can place a live collision section away from the entity root.
    // The tiny local-model fallback cannot reach this ray, but the host-stamped
    // effective bound can and must gate the section through to the exact clip.
    const int32_t posed_at[3] = {fx(5.0), fx(5.0), 0};
    CHECK(cw.publish_entity_section_matrices(
            target, {collision_matrix_from_heading(0, posed_at)}));
    const int32_t a[3] = {0, fx(5.0), fx(1.0)};
    const int32_t b[3] = {fx(10.0), fx(5.0), fx(1.0)};
    CHECK(cw.raycast_clear(world, a, b, EntityHandle{}, EntityHandle{}));
    world.registry.get(target)->bound_radius = 8.0f;
    CHECK(!cw.raycast_clear(world, a, b, EntityHandle{}, EntityHandle{}));

    // An unstamped model fallback receives entity scale once. The pool-2 word
    // stores floor((2 * local diagonal + the retail pad) / 1u): five units for
    // this 0.5 x 0.5 x 2 local box, not the ten-unit double-scaled result.
    World scaled_world;
    CollisionWorld scaled_cw;
    scaled_world.registry.configure_pool(2, 4);
    Entity scaled_seed;
    scaled_seed.kind = EntityKind::Building;
    scaled_seed.yaw = 90;
    scaled_seed.uniform_scale_q16 = fx(2.0);
    scaled_seed.alive = true;
    const EntityHandle scaled = scaled_world.registry.spawn(2, scaled_seed);
    scaled_cw.assign_entity(
            scaled, scaled_cw.add_model(box_model(1, 0, 0.5, 0.5, 2.0)));
    scaled_cw.build_initial_tables(scaled_world);
    CHECK(scaled_cw.static_count() == 1);
    CHECK(scaled_cw.static_slot(0).radius == 5u);
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

void mark_unset_person_sections_absent(CollisionModel &model) {
    for (CollisionSection &section : model.sections) section.radius = -1;
}

CollisionModel person_section_model() {
    CollisionModel model;
    model.sections.resize(19);
    mark_unset_person_sections_absent(model);
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

void test_ground_and_resolver_prefilter_stale_candidates_before_section_matrices() {
    Rig rig(box_model(1, 0, 3.0, 3.0, 1.0));
    rig.move_soldier(10.0, 10.0, 1.05);

    // Build a second, initially-near static into the source's 17-tick candidate
    // slice, then move it without refreshing that slice. Both queries must use
    // its live bound to reject it before asking the host for section matrices.
    Entity stale_seed;
    stale_seed.kind = EntityKind::Building;
    stale_seed.position = {10.0f, 10.0f, 0.0f};
    stale_seed.yaw = 90;
    stale_seed.alive = true;
    const EntityHandle stale = rig.world.registry.spawn(2, stale_seed);
    CHECK(stale.valid());
    rig.cw.assign_entity(stale, 0);
    rig.rebuild();
    CHECK(rig.cw.candidate_count(rig.soldier) == 2);
    Entity *stale_live = rig.world.registry.get(stale);
    CHECK(stale_live != nullptr);
    if (stale_live != nullptr) stale_live->position = {100.0f, 100.0f, 0.0f};

    CountingMatrixProvider provider;
    rig.cw.set_section_matrix_provider(&provider);

    const int32_t probe[3] = {fx(10.0), fx(10.0), fx(5.0)};
    EntityHandle hit;
    const int32_t ground =
            rig.cw.raycast_ground(rig.world, rig.soldier, probe, 0, 0, 0, fx(10.0), &hit);
    CHECK(std::abs(ground - fx(1.0)) < fx(0.02));
    CHECK(hit == rig.building);
    CHECK(provider.calls_for(rig.building) == 1);
    CHECK(provider.calls_for(stale) == 0);

    provider.build_handles.clear();
    int32_t pos[3] = {fx(10.0), fx(10.0), fx(1.05)};
    const int32_t before_xy[2] = {pos[0], pos[1]};
    int32_t vel[3] = {0, 0, 0};
    int16_t health = 100;
    CollisionWorld::ResolveState state;
    rig.cw.resolve_entity(rig.world, rig.soldier, state, pos, vel, vel[2], 0, fx(1.8), 0, 0,
                          false, true, 0, 43, 0u, health);
    const Entity *soldier = rig.world.registry.get(rig.soldier);
    CHECK(pos[0] == before_xy[0]);
    CHECK(pos[1] == before_xy[1]);
    CHECK(health == 100);
    CHECK(soldier != nullptr && soldier->ground_target == rig.building);
    CHECK(provider.calls_for(stale) == 0);
    CHECK(provider.calls_for(rig.building) >= 2);
}

void test_vehicle_hull_prefilters_stale_candidates_before_section_matrices() {
    World world;
    world.registry.configure_pool(1, 4);
    world.registry.configure_pool(2, 8);
    CollisionWorld collision;

    Entity vehicle_seed;
    vehicle_seed.kind = EntityKind::Item;
    vehicle_seed.item_id = 900;
    vehicle_seed.position = {13.0f, 10.0f, 0.0f};
    vehicle_seed.bound_radius = 1.0f;
    vehicle_seed.alive = true;
    const EntityHandle vehicle = world.registry.spawn(1, vehicle_seed);
    CHECK(vehicle.valid());
    VehicleTraits traits;
    traits.physics = 1;
    world.vehicle_traits.set(vehicle_seed.item_id, traits);

    auto spawn_static = [&](float y) {
        Entity seed;
        seed.kind = EntityKind::Building;
        seed.position = {10.0f, y, 0.0f};
        seed.yaw = 90;
        seed.alive = true;
        return world.registry.spawn(2, seed);
    };
    const EntityHandle wall = spawn_static(10.0f);
    const EntityHandle stale = spawn_static(12.0f);
    CHECK(wall.valid() && stale.valid());

    const int32_t model_id = collision.add_model(box_model(1, 0, 2.0, 2.0, 3.0));
    collision.assign_entity(wall, model_id);
    collision.assign_entity(stale, model_id);
    for (int i = 0; i < 17; ++i) collision.build_tick_tables(world);
    CHECK(collision.candidate_count(vehicle) == 2);
    Entity *stale_live = world.registry.get(stale);
    CHECK(stale_live != nullptr);
    if (stale_live != nullptr) stale_live->position = {100.0f, 100.0f, 0.0f};

    CountingMatrixProvider provider;
    collision.set_section_matrix_provider(&provider);
    const int32_t moved[3] = {fx(13.0), fx(10.0), 0};
    const int32_t previous[3] = {fx(14.0), fx(10.0), 0};
    int32_t push[2] = {};
    const int32_t severity =
            collision.resolve_vehicle_hull(world, vehicle, moved, previous, push);
    CHECK(severity == 3);
    CHECK(push[0] > 0);
    CHECK(push[1] == 0);
    CHECK(provider.calls_for(wall) == 1);
    CHECK(provider.calls_for(stale) == 0);
    CHECK(provider.build_handles.size() == 1);
}

struct DemandPersonProvider final : ICollisionSectionMatrixProvider {
    CollisionWorld *collision = nullptr;
    int32_t model_id = -1;
    EntityHandle expected;
    int ensure_calls = 0;
    int matrix_calls = 0;

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
        ++matrix_calls;
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

void test_iris_static_rays_prefilter_before_section_matrices() {
    World world;
    world.registry.configure_pool(2, 64);
    CollisionWorld collision;
    const int32_t model_id = collision.add_model(box_model(1, 0, 1.0, 1.0, 2.0));

    std::vector<EntityHandle> far_statics;
    for (int i = 0; i < 32; ++i) {
        Entity seed;
        seed.kind = EntityKind::Building;
        seed.position = Vec3{100.0f + static_cast<float>(i), 0.0f, 0.0f};
        seed.yaw = 90;
        seed.alive = true;
        const EntityHandle h = world.registry.spawn(2, seed);
        CHECK(h.valid());
        collision.assign_entity(h, model_id);
        far_statics.push_back(h);
    }
    Entity near_seed;
    near_seed.kind = EntityKind::Building;
    near_seed.position = Vec3{5.0f, 0.0f, 0.0f};
    near_seed.yaw = 90;
    near_seed.alive = true;
    const EntityHandle near_static = world.registry.spawn(2, near_seed);
    CHECK(near_static.valid());
    collision.assign_entity(near_static, model_id);
    collision.build_initial_tables(world);

    CountingMatrixProvider provider;
    collision.set_section_matrix_provider(&provider);
    const int32_t start[3] = {0, 0, fx(1.0)};
    const int32_t end[3] = {fx(10.0), 0, fx(1.0)};
    CHECK(collision.segment_hits_static(world, start, end, 0));
    CHECK(provider.calls_for(near_static) == 1);
    CHECK(provider.build_handles.size() == 1);
    for (const EntityHandle h : far_statics)
        CHECK(provider.calls_for(h) == 0);
}

void test_sound_los_prefilters_candidates_before_section_matrices() {
    World world;
    world.registry.configure_pool(0, 4);
    world.registry.configure_pool(1, 16);
    world.registry.configure_pool(2, 32);
    CollisionWorld collision;
    const int32_t model_id = collision.add_model(box_model(1, 0, 1.0, 1.0, 2.0));

    Entity listener_seed;
    listener_seed.kind = EntityKind::Organic;
    listener_seed.bound_radius = 1.0f;
    listener_seed.alive = true;
    const EntityHandle listener = world.registry.spawn(0, listener_seed);
    CHECK(listener.valid());

    std::vector<EntityHandle> rejected;
    for (int i = 0; i < 8; ++i) {
        Entity dynamic_seed;
        dynamic_seed.kind = EntityKind::Item;
        dynamic_seed.position = Vec3{1.0f, static_cast<float>(i) * 0.25f, 0.0f};
        dynamic_seed.yaw = 90;
        dynamic_seed.alive = true;
        const EntityHandle h = world.registry.spawn(1, dynamic_seed);
        collision.assign_entity(h, model_id);
        rejected.push_back(h);
    }
    for (int i = 0; i < 16; ++i) {
        Entity far_seed;
        far_seed.kind = EntityKind::Building;
        far_seed.position = Vec3{3.0f, static_cast<float>(i) * 0.1f, 0.0f};
        far_seed.yaw = 90;
        far_seed.alive = true;
        const EntityHandle h = world.registry.spawn(2, far_seed);
        collision.assign_entity(h, model_id);
        rejected.push_back(h);
    }
    Entity blocker_seed;
    blocker_seed.kind = EntityKind::Building;
    blocker_seed.position = Vec3{0.0f, 5.0f, 0.0f};
    blocker_seed.yaw = 90;
    blocker_seed.alive = true;
    const EntityHandle blocker = world.registry.spawn(2, blocker_seed);
    collision.assign_entity(blocker, model_id);
    for (int i = 0; i < 17; ++i) collision.build_tick_tables(world);

    CountingMatrixProvider provider;
    collision.set_section_matrix_provider(&provider);
    const int32_t start[3] = {0, 0, fx(1.0)};
    const int32_t end[3] = {0, fx(10.0), fx(1.0)};
    const int32_t inflated = collision.sound_occlusion_inflate(
            world, listener, EntityHandle{}, start, end, fx(10.0));
    CHECK(inflated > fx(10.0));
    CHECK(provider.calls_for(blocker) == 2);
    CHECK(provider.build_handles.size() == 2);
    for (const EntityHandle h : rejected)
        CHECK(provider.calls_for(h) == 0);
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
    mark_unset_person_sections_absent(person);
    person.sections[14].radius = fx(1.0);
    CollisionWorld collision;
    const int32_t model_id = collision.add_model(std::move(person));
    DemandPersonProvider provider;
    provider.collision = &collision;
    provider.model_id = model_id;
    provider.expected = victim;
    collision.set_section_matrix_provider(&provider);
    // RoundSim consumes the normal per-tick proximity snapshot. The entity may
    // be present before its graphic instance; ensure-on-hit attaches that late.
    collision.build_tick_tables(world);

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

void test_projectile_person_broad_gate_skips_far_provider() {
    World world;
    world.registry.configure_pool(0, 4);

    Entity owner_seed;
    owner_seed.kind = EntityKind::Organic;
    owner_seed.position = Vec3{0.0f, 200.0f, 0.0f};
    const EntityHandle owner = world.registry.spawn(0, owner_seed);
    Entity victim_seed;
    victim_seed.kind = EntityKind::Organic;
    victim_seed.position = Vec3{5.0f, 100.0f, 0.0f};
    const EntityHandle victim = world.registry.spawn(0, victim_seed);
    CHECK(owner.valid() && victim.valid());

    CollisionModel person;
    person.sections.resize(1);
    person.sections[0].radius = fx(1.0);
    CollisionWorld collision;
    const int32_t model_id = collision.add_model(std::move(person));
    DemandPersonProvider provider;
    provider.collision = &collision;
    provider.model_id = model_id;
    provider.expected = victim;
    collision.set_section_matrix_provider(&provider);
    collision.set_trace_profile_enabled(true);
    collision.build_tick_tables(world);

    ProjectileTrace trace;
    trace.owner = owner;
    trace.start = FixedVec3{0, 0, 0};
    trace.end = FixedVec3{fx(10.0), 0, 0};
    CHECK(!collision.trace_projectile(world, trace).hit());
    CHECK(provider.ensure_calls == 0);
    CHECK(collision.trace_profile().person_survivors == 0);

    // A fresh slot snapshot admits the same provider-backed entity once it is
    // near. The optimization gates only the expensive provider/view build.
    world.registry.get(victim)->position = Vec3{5.0f, 0.0f, 0.0f};
    collision.build_tick_tables(world);
    const ProjectileHit hit = collision.trace_projectile(world, trace);
    CHECK(hit.hit_class == ProjectileHitClass::Person);
    CHECK(hit.geometry_entity == victim);
    CHECK(provider.ensure_calls == 1);
    CHECK(collision.trace_profile().person_survivors == 1);
}

void test_projectile_trace_view_cache_is_tick_scoped() {
    World world;
    world.registry.configure_pool(0, 4);

    Entity victim_seed;
    victim_seed.kind = EntityKind::Organic;
    victim_seed.position = Vec3{5.0f, 0.0f, 0.0f};
    const EntityHandle victim = world.registry.spawn(0, victim_seed);
    CHECK(victim.valid());

    CollisionModel person;
    person.sections.resize(1);
    person.sections[0].radius = fx(1.0);
    CollisionWorld collision;
    const int32_t model_id = collision.add_model(std::move(person));
    DemandPersonProvider provider;
    provider.collision = &collision;
    provider.model_id = model_id;
    provider.expected = victim;
    collision.set_section_matrix_provider(&provider);
    collision.build_tick_tables(world);

    ProjectileTrace trace;
    trace.start = FixedVec3{0, 0, 0};
    trace.end = FixedVec3{fx(10.0), 0, 0};
    const auto hits_victim = [&](const ProjectileHit &hit) {
        return hit.hit_class == ProjectileHitClass::Person &&
               hit.geometry_entity == victim;
    };

    CHECK(hits_victim(collision.trace_projectile(world, trace)));
    CHECK(provider.ensure_calls == 1);
    CHECK(provider.matrix_calls == 1);
    CHECK(hits_victim(collision.trace_projectile(world, trace)));
    CHECK(provider.ensure_calls == 1);
    CHECK(provider.matrix_calls == 1);

    // build_tick_tables publishes the next proximity epoch and expires every
    // cached target view. The provider is not re-entered for identity, but its
    // posed section matrices must be copied exactly once for the new tick.
    collision.build_tick_tables(world);
    CHECK(hits_victim(collision.trace_projectile(world, trace)));
    CHECK(provider.ensure_calls == 1);
    CHECK(provider.matrix_calls == 2);

    collision.build_initial_tables(world);
    CHECK(hits_victim(collision.trace_projectile(world, trace)));
    CHECK(provider.matrix_calls == 3);

    collision.refresh_after_registry_change(world);
    CHECK(hits_victim(collision.trace_projectile(world, trace)));
    CHECK(provider.matrix_calls == 4);

    CollisionModel unrelated;
    unrelated.sections.resize(1);
    unrelated.sections[0].radius = fx(1.0);
    collision.add_model(std::move(unrelated));
    CHECK(hits_victim(collision.trace_projectile(world, trace)));
    CHECK(provider.matrix_calls == 5);

    CountingMatrixProvider replacement_provider;
    collision.set_section_matrix_provider(&replacement_provider);
    CHECK(hits_victim(collision.trace_projectile(world, trace)));
    CHECK(replacement_provider.calls_for(victim) == 1);
}

void test_projectile_trace_cache_revalidates_reused_registry_slot() {
    World world;
    world.registry.configure_pool(0, 4);

    Entity owner_seed;
    owner_seed.kind = EntityKind::Organic;
    owner_seed.position = Vec3{0.0f, 100.0f, 0.0f};
    const EntityHandle owner = world.registry.spawn(0, owner_seed);
    Entity victim_seed;
    victim_seed.kind = EntityKind::Organic;
    victim_seed.position = Vec3{5.0f, 0.0f, 0.0f};
    const EntityHandle first = world.registry.spawn(0, victim_seed);
    CHECK(owner.valid() && first.valid());

    CollisionModel old_model;
    old_model.sections.resize(1);
    old_model.sections[0].radius = fx(1.0);
    CollisionModel replacement_model;
    replacement_model.sections.resize(2);
    mark_unset_person_sections_absent(replacement_model);
    replacement_model.sections[1].radius = fx(1.0);

    CollisionWorld collision;
    const int32_t old_model_id = collision.add_model(std::move(old_model));
    const int32_t replacement_model_id =
            collision.add_model(std::move(replacement_model));
    const uint64_t first_spawn_id =
            world.registry.get(first)->registry_spawn_id;
    collision.assign_entity(first, old_model_id, first_spawn_id);
    const int32_t at_victim[3] = {fx(5.0), 0, 0};
    CHECK(collision.publish_entity_section_matrices(
            first, {collision_matrix_from_heading(0, at_victim)}));
    collision.build_tick_tables(world);

    ProjectileTrace trace;
    trace.owner = owner;
    trace.start = FixedVec3{0, 0, 0};
    trace.end = FixedVec3{fx(10.0), 0, 0};
    const ProjectileHit first_hit = collision.trace_projectile(world, trace);
    CHECK(first_hit.hit_class == ProjectileHitClass::Person);
    CHECK(first_hit.geometry_entity == first);
    CHECK(first_hit.bone_index == 0);

    // Recycle the packed handle and attach a structurally different model
    // without publishing a new table epoch. The existing proximity slot is
    // still valid for this handle, but its cached target view is not.
    world.registry.despawn(first);
    const EntityHandle replacement = world.registry.spawn(0, victim_seed);
    CHECK(replacement == first);
    const uint64_t replacement_spawn_id =
            world.registry.get(replacement)->registry_spawn_id;
    CHECK(replacement_spawn_id != first_spawn_id);
    collision.assign_entity(
            replacement, replacement_model_id, replacement_spawn_id);
    const CollisionMatrix replacement_matrix =
            collision_matrix_from_heading(0, at_victim);
    CHECK(collision.publish_entity_section_matrices(
            replacement, {replacement_matrix, replacement_matrix}));

    const ProjectileHit replacement_hit =
            collision.trace_projectile(world, trace);
    CHECK(replacement_hit.hit_class == ProjectileHitClass::Person);
    CHECK(replacement_hit.geometry_entity == replacement);
    CHECK(replacement_hit.bone_index == 1);
}

void test_projectile_trace_cache_observes_published_pose_same_tick() {
    World world;
    world.registry.configure_pool(0, 4);

    Entity owner_seed;
    owner_seed.kind = EntityKind::Organic;
    owner_seed.position = Vec3{0.0f, 100.0f, 0.0f};
    const EntityHandle owner = world.registry.spawn(0, owner_seed);
    Entity victim_seed;
    victim_seed.kind = EntityKind::Organic;
    victim_seed.position = Vec3{5.0f, 0.0f, 0.0f};
    victim_seed.bound_radius = 8.0f;
    const EntityHandle victim = world.registry.spawn(0, victim_seed);
    CHECK(owner.valid() && victim.valid());

    CollisionModel person;
    person.sections.resize(1);
    person.sections[0].radius = fx(1.0);
    CollisionWorld collision;
    const int32_t model_id = collision.add_model(std::move(person));
    collision.assign_entity(victim, model_id);

    const int32_t first_pose[3] = {fx(5.0), 0, 0};
    CHECK(collision.publish_entity_section_matrices(
            victim, {collision_matrix_from_heading(0, first_pose)}));
    collision.build_tick_tables(world);

    ProjectileTrace first_trace;
    first_trace.owner = owner;
    first_trace.start = FixedVec3{0, 0, 0};
    first_trace.end = FixedVec3{fx(10.0), 0, 0};
    CHECK(collision.trace_projectile(world, first_trace).hit_class ==
          ProjectileHitClass::Person);

    // Animation publishes a newer pose before another round in this logic
    // tick. That round must consume the new matrices, not the cached copy.
    const int32_t moved_pose[3] = {fx(5.0), fx(2.0), 0};
    CHECK(collision.publish_entity_section_matrices(
            victim, {collision_matrix_from_heading(0, moved_pose)}));
    ProjectileTrace moved_trace;
    moved_trace.owner = owner;
    moved_trace.start = FixedVec3{0, fx(2.0), 0};
    moved_trace.end = FixedVec3{fx(10.0), fx(2.0), 0};
    const ProjectileHit moved_hit =
            collision.trace_projectile(world, moved_trace);
    CHECK(moved_hit.hit_class == ProjectileHitClass::Person);
    CHECK(moved_hit.geometry_entity == victim);
    CHECK(moved_hit.bone_index == 0);

    collision.clear_entity_section_matrices(victim);
    CHECK(!collision.trace_projectile(world, moved_trace).hit());

    CHECK(collision.publish_entity_section_matrices(
            victim, {collision_matrix_from_heading(0, moved_pose)}));
    CHECK(collision.trace_projectile(world, moved_trace).hit_class ==
          ProjectileHitClass::Person);
    collision.remove_entity_instance(victim);
    CHECK(!collision.trace_projectile(world, moved_trace).hit());
}

void test_projectile_trace_cache_observes_husk_assignment_same_tick() {
    World world;
    world.registry.configure_pool(0, 4);

    Entity owner_seed;
    owner_seed.kind = EntityKind::Organic;
    owner_seed.position = Vec3{0.0f, 100.0f, 0.0f};
    const EntityHandle owner = world.registry.spawn(0, owner_seed);
    Entity victim_seed;
    victim_seed.kind = EntityKind::Organic;
    victim_seed.position = Vec3{5.0f, 0.0f, 0.0f};
    victim_seed.engine_flags = 0x4u;
    const EntityHandle victim = world.registry.spawn(0, victim_seed);
    CHECK(owner.valid() && victim.valid());

    CollisionModel intact;
    intact.sections.resize(1);
    intact.sections[0].radius = fx(1.0);
    CollisionModel husk;
    husk.sections.resize(2);
    mark_unset_person_sections_absent(husk);
    husk.sections[1].radius = fx(1.0);

    CollisionWorld collision;
    const int32_t intact_id = collision.add_model(std::move(intact));
    const int32_t husk_id = collision.add_model(std::move(husk));
    collision.assign_entity(victim, intact_id);
    CountingMatrixProvider provider;
    collision.set_section_matrix_provider(&provider);
    collision.build_tick_tables(world);

    ProjectileTrace trace;
    trace.owner = owner;
    trace.start = FixedVec3{0, 0, 0};
    trace.end = FixedVec3{fx(10.0), 0, 0};
    const ProjectileHit intact_hit = collision.trace_projectile(world, trace);
    CHECK(intact_hit.hit_class == ProjectileHitClass::Person);
    CHECK(intact_hit.bone_index == 0);

    collision.assign_entity_husk(victim, husk_id);
    const ProjectileHit husk_hit = collision.trace_projectile(world, trace);
    CHECK(husk_hit.hit_class == ProjectileHitClass::Person);
    CHECK(husk_hit.geometry_entity == victim);
    CHECK(husk_hit.bone_index == 1);
    CHECK(provider.calls_for(victim) == 2);
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
    mark_unset_person_sections_absent(old_main);
    old_main.sections[3].radius = fx(1.0);
    const int32_t old_main_id = collision.add_model(std::move(old_main));
    CollisionModel old_husk;
    old_husk.sections.resize(3);
    mark_unset_person_sections_absent(old_husk);
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
    mark_unset_person_sections_absent(replacement);
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
        mark_unset_person_sections_absent(model);
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

    // Retail does not discard an authored radius of zero: every COBJ still
    // receives the fixed 0xCCC floor. The trailing whole-body mesh row in JO
    // person models relies on this exact narrow footprint.
    PersonSectionsFixture inside_zero_floor(1);
    inside_zero_floor.set_section(0, 5.0, 0.049, 0.0, 0.0);
    CHECK(collision_raycast_person_sections(
            inside_zero_floor.target, start, end, 0, 0, hit));
    CHECK(hit.primary_section == 0);
    CHECK(hit.secondary_section == 0);
    CHECK(hit.dist == fx(5.0));

    PersonSectionsFixture outside_zero_floor(1);
    outside_zero_floor.set_section(0, 5.0, 0.051, 0.0, 0.0);
    CHECK(!collision_raycast_person_sections(
            outside_zero_floor.target, start, end, 0, 0, hit));

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

void test_projectile_static_slot_signed_coordinates() {
    // Pool-2 proximity coordinates are signed whole units carried in u16 table
    // storage. The broad phase must sign-extend the bit pattern: -10 is 0xFFF6,
    // not a world-space center at +65526.
    Rig rig(wall_triangle_model(), -10.0, 10.0);
    const CollisionWorld::StaticSlotView slot = rig.cw.static_slot(0);
    CHECK(slot.x == 0xFFF6u);

    ProjectileTrace q;
    q.owner = rig.soldier;
    q.start = FixedVec3{fx(-15.0), fx(10.0), fx(1.0)};
    q.end = FixedVec3{fx(-5.0), fx(10.0), fx(1.0)};
    const ProjectileHit hit = rig.cw.trace_projectile(rig.world, q);
    CHECK(hit.hit_class == ProjectileHitClass::StaticEntity);
    CHECK(hit.geometry_entity == rig.building);
    CHECK(std::abs(hit.position_q16.x - fx(-10.0)) < 8);
}

void test_projectile_trace_profile_is_opt_in_and_parity_neutral() {
    World world;
    world.registry.configure_pool(0, 8);
    world.registry.configure_pool(1, 8);
    world.registry.configure_pool(2, 8);

    Entity owner_seed;
    owner_seed.kind = EntityKind::Organic;
    owner_seed.position = Vec3{0.0f, 100.0f, 0.0f};
    const EntityHandle owner = world.registry.spawn(0, owner_seed);
    Entity person_seed;
    person_seed.kind = EntityKind::Organic;
    person_seed.position = Vec3{9.0f, 0.0f, 0.0f};
    const EntityHandle person = world.registry.spawn(0, person_seed);
    Entity dynamic_seed;
    dynamic_seed.kind = EntityKind::Item;
    dynamic_seed.position = Vec3{7.0f, 0.0f, 0.0f};
    dynamic_seed.yaw = 90;
    dynamic_seed.alive = true;
    const EntityHandle dynamic = world.registry.spawn(1, dynamic_seed);
    Entity static_seed = dynamic_seed;
    static_seed.kind = EntityKind::Building;
    static_seed.position.x = 5.0f;
    const EntityHandle statik = world.registry.spawn(2, static_seed);
    CHECK(owner.valid() && person.valid() && dynamic.valid() && statik.valid());

    CollisionWorld collision;
    const int32_t static_model = collision.add_model(wall_triangle_model());
    const int32_t dynamic_model = collision.add_model(wall_triangle_model());
    collision.assign_entity(statik, static_model);
    collision.assign_entity(dynamic, dynamic_model);
    collision.build_tick_tables(world);

    ProjectileTrace trace;
    trace.owner = owner;
    trace.start = FixedVec3{0, 0, fx(0.9)};
    trace.end = FixedVec3{fx(12.0), 0, fx(0.9)};

    const auto profile_is_empty = [](const CollisionWorld::TraceProfile &p) {
        return p.calls == 0 && p.terrain_us == 0 && p.static_us == 0 &&
               p.dynamic_us == 0 && p.person_us == 0 &&
               p.static_survivors == 0 && p.dynamic_survivors == 0 &&
               p.person_survivors == 0 && p.static_faces == 0 &&
               p.dynamic_faces == 0;
    };
    const auto hits_match = [](const ProjectileHit &a, const ProjectileHit &b) {
        return a.hit_class == b.hit_class &&
               a.geometry_entity == b.geometry_entity && a.t_q16 == b.t_q16 &&
               a.position_q16.x == b.position_q16.x &&
               a.position_q16.y == b.position_q16.y &&
               a.position_q16.z == b.position_q16.z &&
               a.normal_q16.x == b.normal_q16.x &&
               a.normal_q16.y == b.normal_q16.y &&
               a.normal_q16.z == b.normal_q16.z &&
               a.section_index == b.section_index &&
               a.face_index == b.face_index && a.bone_index == b.bone_index &&
               a.hit_zone == b.hit_zone && a.surface_type == b.surface_type &&
               a.material_flags == b.material_flags;
    };

    CHECK(!collision.trace_profile_enabled());
    const ProjectileHit unprofiled = collision.trace_projectile(world, trace);
    CHECK(unprofiled.hit_class == ProjectileHitClass::StaticEntity);
    CHECK(unprofiled.geometry_entity == statik);
    CHECK(profile_is_empty(collision.trace_profile()));

    collision.set_trace_profile_enabled(true);
    CHECK(collision.trace_profile_enabled());
    CHECK(profile_is_empty(collision.trace_profile()));
    const ProjectileHit profiled = collision.trace_projectile(world, trace);
    CHECK(hits_match(unprofiled, profiled));
    const CollisionWorld::TraceProfile &captured = collision.trace_profile();
    CHECK(captured.calls == 1);
    CHECK(captured.static_survivors == 1);
    CHECK(captured.dynamic_survivors == 1);
    CHECK(captured.person_survivors == 1);
    CHECK(captured.static_faces == 1);
    CHECK(captured.dynamic_faces == 1);
    CHECK(captured.terrain_us >= 0 && captured.static_us >= 0 &&
          captured.dynamic_us >= 0 && captured.person_us >= 0);

    // Repeating a state is not an edge and preserves this tick's snapshot.
    collision.set_trace_profile_enabled(true);
    CHECK(collision.trace_profile().calls == 1);
    // Tick boundaries reset an active capture.
    collision.build_tick_tables(world);
    CHECK(profile_is_empty(collision.trace_profile()));
    CHECK(hits_match(unprofiled, collision.trace_projectile(world, trace)));
    CHECK(collision.trace_profile().calls == 1);

    // Disable is an edge: clear once, then keep the ordinary path untouched.
    collision.set_trace_profile_enabled(false);
    CHECK(!collision.trace_profile_enabled());
    CHECK(profile_is_empty(collision.trace_profile()));
    CHECK(hits_match(unprofiled, collision.trace_projectile(world, trace)));
    CHECK(profile_is_empty(collision.trace_profile()));
}

void test_projectile_dynamic_fallback_encloses_scaled_diagonal() {
    // Headless callers can attach a model without stamping entity+0's retail
    // header bound. This CFAC sits near the YZ AABB corner: max(|axis|) is 2u,
    // but the unscaled corner radius is sqrt(8), and the live matrix doubles it.
    CollisionModel model = wall_triangle_model();
    model.vertices[0].p[1] = fx(1.5);
    model.vertices[0].p[2] = fx(1.5);
    model.vertices[1].p[1] = fx(2.0);
    model.vertices[1].p[2] = fx(1.5);
    model.vertices[2].p[1] = fx(1.5);
    model.vertices[2].p[2] = fx(2.0);
    model.faces[0].min[1] = model.faces[0].min[2] = fx(1.5);
    model.faces[0].max[1] = model.faces[0].max[2] = fx(2.0);

    World world;
    CollisionWorld cw;
    world.registry.configure_pool(1, 4);
    Entity dynamic;
    dynamic.kind = EntityKind::Item;
    dynamic.position = {5.0f, 0.0f, 0.0f};
    dynamic.yaw = 90; // identity collision placement
    dynamic.uniform_scale_q16 = fx(2.0);
    dynamic.alive = true;
    const EntityHandle target = world.registry.spawn(1, dynamic);
    cw.assign_entity(target, cw.add_model(std::move(model)));
    cw.build_tick_tables(world);

    ProjectileTrace q;
    q.start = FixedVec3{fx(0.0), fx(3.25), fx(3.25)};
    q.end = FixedVec3{fx(10.0), fx(3.25), fx(3.25)};
    const ProjectileHit hit = cw.trace_projectile(world, q);
    CHECK(hit.hit_class == ProjectileHitClass::DynamicEntity);
    CHECK(hit.geometry_entity == target);
    CHECK(std::abs(hit.position_q16.x - fx(5.0)) < 8);
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

// Mission-authored refNum groups (BMS byte 153 -> entity+533) carry self-site
// immunity: the person leg compares the candidate against the SHOOTER's refNum
// [orig: @ 0x4e4688-0x4e46a3], the item leg against the ray[18] MOUNT
// exclusion's refNum [orig: @ 0x4e4d40].
void test_refnum_group_immunity() {
    World world;
    world.registry.configure_pool(0, 4);
    world.registry.configure_pool(1, 4);

    Entity owner_seed;
    owner_seed.kind = EntityKind::Organic;
    owner_seed.ref_num = 7;
    const EntityHandle owner = world.registry.spawn(0, owner_seed);

    Entity person_seed;
    person_seed.kind = EntityKind::Organic;
    person_seed.position = Vec3{5.0f, 0.0f, 0.0f};
    person_seed.health = 100;
    person_seed.ref_num = 7;
    const EntityHandle person = world.registry.spawn(0, person_seed);

    CollisionWorld cw;
    cw.build_tick_tables(world);

    ProjectileTrace q;
    q.owner = owner;
    q.start = FixedVec3{0, 0, fx(0.9)};
    q.end = FixedVec3{fx(10.0), 0, fx(0.9)};
    CHECK(!cw.trace_projectile(world, q).hit()); // shared group 7: immune

    world.registry.get(person)->ref_num = 3;
    CHECK(cw.trace_projectile(world, q).hit_class == ProjectileHitClass::Person);

    world.registry.get(person)->ref_num = 7;
    world.registry.get(owner)->ref_num = 0; // a zero shooter refNum never gates
    CHECK(cw.trace_projectile(world, q).hit_class == ProjectileHitClass::Person);

    // The ITEM leg keys off the MOUNT exclusion's refNum, not the shooter's.
    Entity gun_seed;
    gun_seed.kind = EntityKind::Item;
    gun_seed.position = Vec3{100.0f, 100.0f, 100.0f};
    gun_seed.bound_radius = 0.5f;
    gun_seed.ref_num = 9;
    const EntityHandle gun = world.registry.spawn(1, gun_seed);

    Entity plate_seed;
    plate_seed.kind = EntityKind::Item;
    plate_seed.position = Vec3{4.0f, 0.0f, 0.9f};
    plate_seed.bound_radius = 1.0f; // compatibility sphere, no resolved model
    plate_seed.ref_num = 9;
    const EntityHandle plate = world.registry.spawn(1, plate_seed);
    CHECK(gun.valid() && plate.valid());
    world.registry.get(person)->position = Vec3{100.0f, 0.0f, 0.0f}; // off the ray
    cw.build_tick_tables(world);

    Entity *shooter = world.registry.get(owner);
    shooter->mounted = true;
    shooter->mount_target = gun;
    shooter->mount_type = SeatType::Gunner;
    CHECK(!cw.trace_projectile(world, q).hit()); // the site's plate shares the gun's 9

    shooter->mounted = false; // dismounted: ray[18] is empty, the plate hits
    shooter->mount_type = SeatType::None;
    CHECK(cw.trace_projectile(world, q).hit_class ==
          ProjectileHitClass::DynamicEntity);

    shooter->mounted = true; // mounted again, but a different plate group
    shooter->mount_type = SeatType::Gunner;
    world.registry.get(plate)->ref_num = 3;
    CHECK(cw.trace_projectile(world, q).hit_class ==
          ProjectileHitClass::DynamicEntity);
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
    victim_seed.has_item_def = true;
    victim_seed.item_type = 3;
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
    mark_unset_person_sections_absent(model);
    model.sections[2].radius = fx(1.0);
    model.sections[14].radius = fx(1.0);
    CollisionWorld collision;
    const int32_t model_id = collision.add_model(std::move(model));
    collision.assign_entity(victim, model_id);
    const int32_t victim_pos[3] = {fx(5.0), 0, 0};
    std::vector<CollisionMatrix> pose(
        15, collision_matrix_from_heading(0, victim_pos));
    CHECK(collision.publish_entity_section_matrices(victim, std::move(pose)));
    collision.build_tick_tables(world);

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

void test_projectile_polygon_raycast() {
    CollisionModel model = wall_triangle_model(/*material_flags=*/0x20,
                                                /*poly_type=*/7);
    model.finalize_sections();
    const int32_t pos[3] = {fx(5.0), 0, 0};
    CollisionMatrix matrix = collision_matrix_from_heading(0, pos);
    CollisionTargetView target;
    target.model = &model;
    target.matrices = &matrix;

    CollisionRay ray;
    ray.start[0] = fx(0.0);
    ray.start[1] = 0;
    ray.start[2] = fx(1.0);
    ray.end[0] = fx(10.0);
    ray.end[1] = 0;
    ray.end[2] = fx(1.0);
    ray.refresh();
    CollisionPolygonHit hit;
    CHECK(collision_raycast_polygons(target, ray, fx(10.0), 0, hit));
    CHECK(std::abs(hit.distance_q16 - fx(5.0)) <= 2);
    CHECK(std::abs(hit.position_q16[0] - fx(5.0)) <= 2);
    CHECK(hit.normal_q16[0] < -fx(0.99));
    CHECK(hit.section_index == 0 && hit.face_index == 0 &&
          hit.section_face_index == 0);
    CHECK(hit.material_flags == 0x20u && hit.poly_type == 7);

    // The face AABB alone is insufficient: this point is inside its Y/Z box
    // but outside the actual triangle.
    CollisionRay outside = ray;
    outside.start[1] = outside.end[1] = fx(1.5);
    outside.refresh();
    CHECK(!collision_raycast_polygons(target, outside, fx(10.0), 0, hit));

    // Equal-distance faces overwrite in source order within a table query.
    model.faces.push_back(model.faces[0]);
    model.sections[0].face_count = 2;
    CHECK(collision_raycast_polygons(target, ray, fx(10.0), 0, hit));
    CHECK(hit.section_face_index == 1 && hit.face_index == 1);
    model.faces.resize(1);
    model.sections[0].face_count = 1;

    model.faces[0].material_flags = 0x100;
    CHECK(!collision_raycast_polygons(target, ray, fx(10.0), 0, hit));
    model.faces[0].material_flags = 0;
    model.faces[0].poly_type = 17;
    CHECK(collision_raycast_polygons(target, ray, fx(10.0), 0, hit));
    CHECK(!collision_raycast_polygons(target, ray, fx(10.0), 0x04000000u, hit));

    // 0x800 is one-sided for projectile backfaceArg=1. The reverse crossing
    // misses with 0x800, but an ordinary face remains two-directional.
    CollisionRay reverse;
    for (int axis = 0; axis < 3; ++axis) {
        reverse.start[axis] = ray.end[axis];
        reverse.end[axis] = ray.start[axis];
    }
    reverse.refresh();
    model.faces[0].poly_type = 7;
    model.faces[0].material_flags = 0x800;
    CHECK(collision_raycast_polygons(target, ray, fx(10.0), 0, hit));
    CHECK(!collision_raycast_polygons(target, reverse, fx(10.0), 0, hit));
    model.faces[0].material_flags = 0x801;
    CHECK(collision_raycast_polygons(target, reverse, fx(10.0), 0, hit));
    model.faces[0].material_flags = 0;
    CHECK(collision_raycast_polygons(target, reverse, fx(10.0), 0, hit));

    // A plane crossing at the segment endpoint is valid for CFAC and touches
    // the zero-thickness face AABB rather than being strictly disjoint.
    CollisionRay endpoint = ray;
    endpoint.end[0] = fx(5.0);
    endpoint.refresh();
    CHECK(collision_raycast_polygons(target, endpoint, fx(5.0), 0, hit));
    CHECK(std::abs(hit.distance_q16 - fx(5.0)) <= 2);

    for (int state = 1; state <= 3; ++state) {
        matrix.m[15] = state;
        CHECK(!collision_raycast_polygons(target, ray, fx(10.0), 0, hit));
    }
    matrix.m[15] = 0;

    // Section-local source indices are runs, not global face/vertex ordinals.
    CollisionModel indexed = wall_triangle_model();
    indexed.vertices.insert(indexed.vertices.begin(), CollisionVertex{});
    indexed.normals.insert(indexed.normals.begin(), CollisionNormal{});
    indexed.faces.insert(indexed.faces.begin(), CollisionFace{});
    indexed.sections[0].vertex_start = 1;
    indexed.sections[0].normal_start = 1;
    indexed.sections[0].face_start = 1;
    indexed.finalize_sections();
    CollisionTargetView indexed_target = target;
    indexed_target.model = &indexed;
    CHECK(collision_raycast_polygons(indexed_target, ray, fx(10.0), 0, hit));
    CHECK(hit.face_index == 1 && hit.section_face_index == 0);

    // Every dominant-axis projection accepts the same centered hit.
    const int32_t origin[3] = {};
    CollisionMatrix identity = collision_matrix_from_heading(0, origin);
    for (int normal_axis = 0; normal_axis < 3; ++normal_axis) {
        CollisionModel axis_model = axis_triangle_model(normal_axis);
        axis_model.finalize_sections();
        CollisionTargetView axis_target;
        axis_target.model = &axis_model;
        axis_target.matrices = &identity;
        CollisionRay axis_ray;
        axis_ray.start[normal_axis] = fx(-5.0);
        axis_ray.end[normal_axis] = fx(5.0);
        axis_ray.refresh();
        CHECK(collision_raycast_polygons(axis_target, axis_ray, fx(10.0), 0, hit));
        CHECK(hit.normal_q16[normal_axis] < -fx(0.99));
    }

    // A quarter-turn section pose rotates local +X into world +Y and carries
    // both the hit point and normal through the authored section matrix.
    CollisionModel rotated_model = wall_triangle_model();
    rotated_model.finalize_sections();
    const int32_t rotated_pos[3] = {fx(2.0), fx(5.0), 0};
    CollisionMatrix rotated_matrix =
        collision_matrix_from_heading(static_cast<int32_t>(0x40000000u), rotated_pos);
    CollisionTargetView rotated_target;
    rotated_target.model = &rotated_model;
    rotated_target.matrices = &rotated_matrix;
    CollisionRay rotated_ray;
    rotated_ray.start[0] = rotated_ray.end[0] = fx(2.0);
    rotated_ray.start[1] = 0;
    rotated_ray.end[1] = fx(10.0);
    rotated_ray.start[2] = rotated_ray.end[2] = fx(1.0);
    rotated_ray.refresh();
    CHECK(collision_raycast_polygons(rotated_target, rotated_ray, fx(10.0), 0, hit));
    CHECK(std::abs(hit.position_q16[1] - fx(5.0)) <= 2);
    CHECK(hit.normal_q16[1] < -fx(0.99));

    // A 2x authored matrix needs the recovered scale-aware inverse: this ray is
    // outside the unscaled local triangle but inside its doubled world extent.
    CollisionModel scale_model = wall_triangle_model();
    scale_model.finalize_sections();
    const int32_t scale_pos[3] = {fx(5.0), 0, 0};
    CollisionMatrix scale_matrix = collision_matrix_from_heading(0, scale_pos);
    constexpr int scale_rotation_indices[] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
    for (int index : scale_rotation_indices) scale_matrix.m[index] *= 2;
    CollisionTargetView scale_target;
    scale_target.model = &scale_model;
    scale_target.matrices = &scale_matrix;
    scale_target.uniform_scale_q16 = fx(2.0);
    CollisionRay scale_ray = ray;
    scale_ray.start[1] = scale_ray.end[1] = fx(2.0);
    scale_ray.refresh();
    CHECK(collision_raycast_polygons(scale_target, scale_ray, fx(10.0), 0, hit));
    scale_target.uniform_scale_q16 = 0;
    CHECK(!collision_raycast_polygons(scale_target, scale_ray, fx(10.0), 0, hit));
}

void test_projectile_trace_authority_radius_and_damage_gates() {
    World world;
    world.registry.configure_pool(0, 8);
    Entity owner;
    owner.kind = EntityKind::Organic;
    owner.position = {0.0f, 0.0f, 0.0f};
    const EntityHandle oh = world.registry.spawn(0, owner);
    Entity target;
    target.kind = EntityKind::Organic;
    target.position = {5.0f, 0.0f, 0.0f};
    const EntityHandle th = world.registry.spawn(0, target);

    CollisionWorld cw;
    cw.build_tick_tables(world);
    ProjectileTrace q;
    q.start = FixedVec3{fx(0.0), fx(0.65), fx(0.9)};
    q.end = FixedVec3{fx(10.0), fx(0.65), fx(0.9)};
    q.owner = oh;
    q.radius_q16 = 0;
    ProjectileHit hit = cw.trace_projectile(world, q);
    CHECK(!hit.hit()); // no unconditional 0.1u expansion

    Entity *shooter = world.registry.get(oh);
    shooter->flags |= 0x100u;
    world.mp_session = true;
    world.projectile_authority = true;
    world.fat_bullets = true;
    world.cached.local_player = th; // owner is a remote player on this authority
    hit = cw.trace_projectile(world, q);
    CHECK(hit.hit_class == ProjectileHitClass::Person);
    CHECK(hit.geometry_entity == th);
    CHECK(hit.t_q16 > fx(0.35) && hit.t_q16 < fx(0.5));

    // Indestructibility/death are downstream damage gates, not transparent
    // geometry. The exact same trace must still stop on the body.
    Entity *dead = world.registry.get(th);
    dead->health = 0;
    dead->alive = false;
    dead->engine_flags |= 0x4000000u;
    hit = cw.trace_projectile(world, q);
    CHECK(hit.hit_class == ProjectileHitClass::Person);
    CHECK(hit.geometry_entity == th);

    // FatBullets never expands the local authority player's own shot.
    world.cached.local_player = oh;
    CHECK(!cw.trace_projectile(world, q).hit());
    world.cached.local_player = th;

    ProjectileTrace zero;
    zero.start = zero.end = FixedVec3{fx(2.0), fx(3.0), fx(4.0)};
    const ProjectileHit miss = cw.trace_projectile(world, zero);
    CHECK(!miss.hit());
    CHECK(miss.position_q16.x == zero.end.x && miss.position_q16.z == zero.end.z);
    CHECK(miss.section_index == -1 && miss.face_index == -1 && miss.bone_index == -1);
}

void test_published_person_bone_pose() {
    World world;
    world.registry.configure_pool(0, 8);
    Entity owner;
    owner.kind = EntityKind::Organic;
    const EntityHandle oh = world.registry.spawn(0, owner);
    Entity person;
    person.kind = EntityKind::Organic;
    person.position = {5.0f, 0.0f, 0.0f};
    const EntityHandle ph = world.registry.spawn(0, person);

    CollisionModel model;
    CollisionSection bone;
    bone.authored_bounds = true;
    bone.min_x = bone.min_y = bone.min_z = fx(-1.0);
    bone.max_x = bone.max_y = bone.max_z = fx(1.0);
    bone.radius = fx(1.0);
    model.sections.push_back(bone);

    CollisionWorld cw;
    const int32_t model_id = cw.add_model(std::move(model));
    cw.assign_entity(ph, model_id);
    CHECK(!cw.publish_entity_section_matrices(ph, {}));
    const int32_t posed_at[3] = {fx(5.0), 0, fx(0.9)};
    std::vector<CollisionMatrix> pose{
        collision_matrix_from_heading(0, posed_at),
    };
    CHECK(cw.publish_entity_section_matrices(ph, pose));
    cw.build_tick_tables(world);

    ProjectileTrace q;
    q.owner = oh;
    q.start = FixedVec3{0, 0, fx(0.9)};
    q.end = FixedVec3{fx(10.0), 0, fx(0.9)};
    const ProjectileHit hit = cw.trace_projectile(world, q);
    CHECK(hit.hit_class == ProjectileHitClass::Person);
    CHECK(hit.geometry_entity == ph);
    CHECK(hit.section_index == 0 && hit.bone_index == 0 && hit.hit_zone == 0);
    CHECK(std::abs(hit.position_q16.x - fx(4.5)) <= 4);
}

// Every JO person model authors its trailing CFAC mesh row with radius 0 and
// sentinel inverted bounds (Indo01 sec 19). Retail's bone walk reads the
// AUTHORED mid/radius [orig: Physics_RaycastAgainstBoneSections @ 0x4e4670],
// so the row keeps only the fixed 0xCCC floor. A vertex-derived bound sphere
// minted a phantom shootable "bone" on whatever pose slot the row landed on
// (the F3 big off-body organic sphere).
void test_person_mesh_row_keeps_authored_zero_sphere() {
    World world;
    world.registry.configure_pool(0, 8);
    Entity owner;
    owner.kind = EntityKind::Organic;
    const EntityHandle oh = world.registry.spawn(0, owner);
    Entity person;
    person.kind = EntityKind::Organic;
    person.position = {5.0f, 0.0f, 0.0f};
    const EntityHandle ph = world.registry.spawn(0, person);

    CollisionModel model;
    CollisionSection bone;
    bone.authored_bounds = true;
    bone.min_x = bone.min_y = bone.min_z = fx(-0.3);
    bone.max_x = bone.max_y = bone.max_z = fx(0.3);
    bone.radius = fx(0.3);
    model.sections.push_back(bone);

    // The retail-shaped mesh row: a vertex run, authored radius 0, sentinel
    // inverted AABB, no BVOLs.
    CollisionSection mesh_row;
    mesh_row.authored_bounds = false;
    mesh_row.min_x = mesh_row.min_y = mesh_row.min_z = fx(10000.0);
    mesh_row.max_x = mesh_row.max_y = mesh_row.max_z = fx(-10000.0);
    mesh_row.radius = 0;
    mesh_row.vertex_start = 0;
    mesh_row.vertex_count = 2;
    model.vertices.push_back(CollisionVertex{{fx(-1.0), fx(-1.0), fx(-1.0)}});
    model.vertices.push_back(CollisionVertex{{fx(1.0), fx(1.0), fx(1.0)}});
    model.sections.push_back(mesh_row);
    model.finalize_sections();

    // The AABB derives for the model-level union; the bound sphere does not.
    CHECK(model.sections[1].radius == 0);
    CHECK(model.sections[1].center[0] == 0 && model.sections[1].center[2] == 0);
    CHECK(model.sections[1].min_x == fx(-1.0) && model.sections[1].max_x == fx(1.0));
    CHECK(model.max[0] >= fx(1.0) && model.min[0] <= fx(-1.0));

    CollisionWorld cw;
    const int32_t model_id = cw.add_model(std::move(model));
    cw.assign_entity(ph, model_id);
    const int32_t bone_at[3] = {fx(5.0), 0, fx(0.9)};
    const int32_t mesh_slot_at[3] = {fx(5.0), fx(3.0), fx(0.9)}; // an off-body pose slot
    CHECK(cw.publish_entity_section_matrices(
            ph, {collision_matrix_from_heading(0, bone_at),
                 collision_matrix_from_heading(0, mesh_slot_at)}));
    cw.build_tick_tables(world);

    // The authored zero still receives retail's tiny 0xCCC floor at its raw
    // center, and F3 must report that same effective radius.
    ProjectileTrace at_mesh_center;
    at_mesh_center.owner = oh;
    at_mesh_center.start = FixedVec3{0, fx(3.0), fx(0.9)};
    at_mesh_center.end = FixedVec3{fx(10.0), fx(3.0), fx(0.9)};
    const ProjectileHit mesh_hit = cw.trace_projectile(world, at_mesh_center);
    CHECK(mesh_hit.hit_class == ProjectileHitClass::Person);
    CHECK(mesh_hit.bone_index == 1 && mesh_hit.hit_zone == 1);

    const int32_t debug_anchor[3] = {};
    const auto debug_sections = cw.debug_person_sections(world, debug_anchor, -1, 8);
    CHECK(debug_sections.size() == 2);
    if (debug_sections.size() == 2) {
        CHECK(debug_sections[1].section == 1);
        CHECK(debug_sections[1].authored_radius == 0);
        CHECK(debug_sections[1].radius == 0xCCC);
    }

    // One tenth of a unit off-center remains a clean miss: the old derived
    // vertex sphere would have made this whole area shootable.
    ProjectileTrace outside_mesh_floor;
    outside_mesh_floor.owner = oh;
    outside_mesh_floor.start = FixedVec3{0, fx(3.1), fx(0.9)};
    outside_mesh_floor.end = FixedVec3{fx(10.0), fx(3.1), fx(0.9)};
    CHECK(!cw.trace_projectile(world, outside_mesh_floor).hit());

    // The real bone still hits.
    ProjectileTrace at_bone;
    at_bone.owner = oh;
    at_bone.start = FixedVec3{0, 0, fx(0.9)};
    at_bone.end = FixedVec3{fx(10.0), 0, fx(0.9)};
    const ProjectileHit hit = cw.trace_projectile(world, at_bone);
    CHECK(hit.hit_class == ProjectileHitClass::Person);
    CHECK(hit.bone_index == 0);
}

void test_projectile_trace_world_ordering() {
    ProjectileTrace q;
    q.start = FixedVec3{fx(0.0), 0, fx(1.0)};
    q.end = FixedVec3{fx(10.0), 0, fx(1.0)};

    // A resolved BVOL-only model is not substituted for projectile CFAC in retail.
    // This covers every named gameplay family in this pass: CB, CL, CA, VC, BB,
    // CD, CT, CF, DH/DM/DL, and CP. Unresolved entities retain the separately
    // tested compatibility-sphere fallback.
    for (const int type : {1, 4, 6, 7, 8, 9, 10, 13, 16, 17, 18, 19}) {
        Rig bvol_only(box_model(type, 0, 1.0, 1.0, 2.0), 5.0, 0.0);
        q.owner = bvol_only.soldier;
        CHECK(!bvol_only.cw.trace_projectile(bvol_only.world, q).hit());
    }

    // Static CFAC is class 1 and wins over the later person pass.
    Rig rig(wall_triangle_model(), 5.0, 0.0);
    Entity target;
    target.kind = EntityKind::Organic;
    target.position = {8.0f, 0.0f, 0.0f};
    const EntityHandle farther = rig.world.registry.spawn(0, target);
    rig.rebuild();

    q.owner = rig.soldier;
    ProjectileHit hit = rig.cw.trace_projectile(rig.world, q);
    CHECK(hit.hit_class == ProjectileHitClass::StaticEntity);
    CHECK(hit.geometry_entity == rig.building);
    CHECK(hit.geometry_entity != farther);
    CHECK(hit.section_index == 0 && hit.bone_index == 0 && hit.face_index == 0);
    CHECK(hit.surface_type == 5);
    CHECK(std::abs(hit.position_q16.x - fx(5.0)) < 8);
    CHECK(hit.normal_q16.x < 0);

    // A dynamic CFAC at the same distance loses to static on equality. Ignoring
    // the static exposes the same model as class 2.
    rig.world.registry.configure_pool(1, 8);
    Entity dynamic;
    dynamic.kind = EntityKind::Item;
    dynamic.position = {5.0f, 0.0f, 0.0f};
    dynamic.yaw = 90;
    dynamic.alive = true;
    const EntityHandle dynamic_h = rig.world.registry.spawn(1, dynamic);
    const int32_t dynamic_model = rig.cw.add_model(wall_triangle_model());
    rig.cw.assign_entity(dynamic_h, dynamic_model);
    rig.rebuild();
    hit = rig.cw.trace_projectile(rig.world, q);
    CHECK(hit.hit_class == ProjectileHitClass::StaticEntity);
    CHECK(hit.geometry_entity == rig.building);
    q.extra_ignore = rig.building;
    hit = rig.cw.trace_projectile(rig.world, q);
    CHECK(hit.hit_class == ProjectileHitClass::DynamicEntity);
    CHECK(hit.geometry_entity == dynamic_h);
    q.extra_ignore = EntityHandle{};

    // Terrain is seeded first; a strictly nearer entity replaces it, while a
    // farther entity cannot replace the refined ground crossing.
    World world;
    world.registry.configure_pool(0, 8);
    Field flat(0);
    CollisionWorld cw;
    cw.terrain = &flat.field;
    Entity e;
    e.kind = EntityKind::Organic;
    e.position = {8.0f, 0.0f, 0.0f};
    const EntityHandle far = world.registry.spawn(0, e);
    cw.build_tick_tables(world);
    ProjectileTrace down;
    down.start = FixedVec3{fx(0.0), 0, fx(5.0)};
    down.end = FixedVec3{fx(10.0), 0, fx(-5.0)};
    hit = cw.trace_projectile(world, down);
    CHECK(hit.hit_class == ProjectileHitClass::Terrain);
    CHECK(!hit.geometry_entity.valid());
    CHECK(std::abs(hit.position_q16.z) < fx(0.02));
    CHECK(hit.normal_q16.z > fx(0.99));

    down.ammo_flags = 0x80u;
    CHECK(!cw.trace_projectile(world, down).hit());
    down.ammo_flags = 0;

    Entity *near = world.registry.get(far);
    near->position = {3.0f, 0.0f, 1.1f}; // torso center lies on the descending ray
    cw.build_tick_tables(world);
    hit = cw.trace_projectile(world, down);
    CHECK(hit.hit_class == ProjectileHitClass::Person);
    CHECK(hit.geometry_entity == far);
    CHECK(hit.t_q16 < fx(0.5));

    // The person proximity routine returns on its first qualifying table slot,
    // even when a later person would be geometrically nearer.
    World people;
    people.registry.configure_pool(0, 8);
    Entity owner;
    owner.kind = EntityKind::Organic;
    const EntityHandle people_owner = people.registry.spawn(0, owner);
    Entity first;
    first.kind = EntityKind::Organic;
    first.position = {8.0f, 0.0f, 0.0f};
    const EntityHandle first_h = people.registry.spawn(0, first);
    Entity second = first;
    second.position = {4.0f, 0.0f, 0.0f};
    people.registry.spawn(0, second);
    CollisionWorld people_collision;
    people_collision.build_tick_tables(people);
    q.owner = people_owner;
    hit = people_collision.trace_projectile(people, q);
    CHECK(hit.hit_class == ProjectileHitClass::Person);
    CHECK(hit.geometry_entity == first_h);

    // Water is pass 4's hit class but pass two in arbitration, and endpoints on
    // the plane are deliberately not crossings.
    World water;
    water.env.water_z = fx(2.0);
    CollisionWorld water_collision;
    ProjectileTrace wet;
    wet.start = FixedVec3{0, 0, fx(5.0)};
    wet.end = FixedVec3{0, 0, fx(-5.0)};
    hit = water_collision.trace_projectile(water, wet);
    CHECK(hit.hit_class == ProjectileHitClass::Water);
    CHECK(hit.position_q16.z == water.env.water_z);
    wet.start.z = water.env.water_z;
    CHECK(!water_collision.trace_projectile(water, wet).hit());
}

void test_idle_round_tick_clears_stale_terrain() {
    World world;
    CollisionWorld collision;
    Field old_field(0);
    world.collision = &collision;
    collision.terrain = &old_field.field;
    CHECK(world.round_sim.active_count == 0);
    world.round_sim.tick(world, nullptr);
    CHECK(collision.terrain == nullptr);
    world.round_sim.tick(world, &old_field.field);
    CHECK(collision.terrain == &old_field.field);
}

int main() {
    test_matrix_roundtrip();
    test_retail_render_pose_matrix_roundtrip_and_order();
    test_blink_query_and_refresh();
    test_negative_static_slot_candidates_and_blink();
    test_sound_occlusion();
    test_ray_clip();
    test_ground_probe_roof();
    test_resolver_wall_pushout();
    test_mounted_resolver_keeps_touch_without_parent_pushout();
    test_secondary_vertical_force_is_full_strength();
    test_ladder_contact_uses_positive_authored_pitch();
    test_vehicle_collision_volume_selection();
    test_named_gameplay_volume_dispatch();
    test_resolver_damage_grades_and_zones();
    test_idle_skip_throttle();
    test_slice_cadence_and_invuln();
    test_pool1_item_is_one_collision_candidate();
    test_ladder_contact_bookkeeping_is_not_ground();
    test_cb_ground_probe_sets_ground_target();
    test_debug_seams();
    test_raycast_clear_los();
    test_raycast_clear_table_readiness();
    test_raycast_uses_stamped_bound_for_live_section_pose();
    test_los_point_bias();
    test_proximity_tables_use_host_bound_radius();
    test_face_raycast();
    test_face_raycast_flags_and_materials();
    test_face_raycast_husk_swap();
    test_face_raycast_husk_omits_spawned_piece_sections();
    test_face_raycast_uses_callback_matrix_per_section();
    test_person_section_raycast_uses_posed_bone_matrix();
    test_ground_and_resolver_prefilter_stale_candidates_before_section_matrices();
    test_vehicle_hull_prefilters_stale_candidates_before_section_matrices();
    test_f3_debug_prefilters_before_building_section_matrices();
    test_iris_static_rays_prefilter_before_section_matrices();
    test_sound_los_prefilters_candidates_before_section_matrices();
    test_late_person_instance_is_demand_resolved_for_rounds_and_debug();
    test_projectile_person_broad_gate_skips_far_provider();
    test_projectile_trace_view_cache_is_tick_scoped();
    test_projectile_trace_cache_revalidates_reused_registry_slot();
    test_projectile_trace_cache_observes_published_pose_same_tick();
    test_projectile_trace_cache_observes_husk_assignment_same_tick();
    test_reused_registry_slot_rejects_old_collision_identity();
    test_person_section_reverse_scan_and_mask();
    test_person_section_retail_radius_rules();
    test_face_raycast_rolled_entity();
    test_face_raycast_pitched_entity();
    test_round_inside_bound_sphere_hits_wall();
    test_projectile_static_slot_signed_coordinates();
    test_projectile_trace_profile_is_opt_in_and_parity_neutral();
    test_projectile_dynamic_fallback_encloses_scaled_diagonal();
    test_round_equal_distance_uses_retail_pool_order();
    test_round_item_skip_mask();
    test_refnum_group_immunity();
    test_round_indestructible_organic_still_collides();
    test_round_person_sections_drive_hit_and_death_animation();
    test_projectile_polygon_raycast();
    test_projectile_trace_authority_radius_and_damage_gates();
    test_published_person_bone_pose();
    test_person_mesh_row_keeps_authored_zero_sphere();
    test_projectile_trace_world_ordering();
    test_idle_round_tick_clears_stale_terrain();
    if (failures == 0) std::printf("collision_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
