// The blink-quad and blink-letter rules behind the "occlusion issues in a
// building" report (DX-1 / DX-2 of the indoor-visibility diagnosis):
//
//  * DX-1: the movement resolver ORs the blink letters (and latches the
//    indoors bit) per FIRST-pass candidate only; the relaxation pass's blink
//    hits reach the entity's quad and nothing else.
//    [orig: Entity_MovementCollisionResolver @0x4b34c0..0x4b3502 inside the
//     first-pass loop (back-edge @0x4b3530); the quad store @0x4b36f0]
//  * DX-2: a child/rider/dropped object takes its parent's blink quad — the
//    word the collector's blink-hits gate and the render waves' render_TOC
//    early rule read — instead of keeping a stale or empty one:
//      the dropped object      [orig: Entity_DropCarriedObject @0x439ec6..0x439ef7]
//      the ewep class callback [orig: Entity_UpdateChildAttachment @0x4409e6..0x440a12]
//      the ewep move function  [orig: Entity_UpdateTransformAndTurret @0x440ecb..0x440eef]
//      a seated rider          [orig: Entity_UpdateInfantryPlayerBody @0x4b662f..0x4b667e;
//                               Entity_UpdateInfantryAI @0x4beee7..0x4bef30]
//  * the drop's own legs: the pose (Z plus 0x4000, heading plus a quarter
//    turn), the motion and the fall it installs, the ground query's out word,
//    and the definition pose's heading.
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/destruction.h>
#include <runtime/world/entity.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

using namespace opennova::world;
using opennova::terrain::TerrainHeightField;

static int failures = 0;
#define CHECK(c)                                                                            \
    do {                                                                                    \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr int32_t fx(double units) { return static_cast<int32_t>(units * 65536.0); }

struct Field {
    static constexpr int kDim = 512;
    std::vector<uint16_t> heightmap;
    std::vector<int> sector_grid;
    TerrainHeightField field;

    Field() : heightmap(kDim * kDim, 0), sector_grid(256, 1) {
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
        field.layout.origin_x = 0;
        field.layout.origin_y = 0;
    }
};

// One axis-aligned volume [x0,x1]x[y0,y1]x[z0,z1] (section-local, 16.16) with
// its six outward Q14 planes (inside = dot>>14 + dist < 0).
void add_box(CollisionModel &m, int32_t type, uint32_t flags, double x0, double x1, double y0,
             double y1, double z0, double z1) {
    const int32_t plane_start = static_cast<int32_t>(m.planes.size());
    auto plane = [&](int nx, int ny, int nz, double d) {
        CollisionPlane p;
        p.nx = static_cast<int16_t>(nx);
        p.ny = static_cast<int16_t>(ny);
        p.nz = static_cast<int16_t>(nz);
        p.dist = fx(d);
        m.planes.push_back(p);
    };
    plane(16384, 0, 0, -x1);
    plane(-16384, 0, 0, x0);
    plane(0, 16384, 0, -y1);
    plane(0, -16384, 0, y0);
    plane(0, 0, 16384, -z1);
    plane(0, 0, -16384, z0);
    CollisionVolume v;
    v.type = type;
    v.flags = flags;
    v.min_x = fx(x0);
    v.max_x = fx(x1);
    v.min_y = fx(y0);
    v.max_y = fx(y1);
    v.min_z = fx(z0);
    v.max_z = fx(z1);
    v.plane_start = plane_start;
    v.plane_count = 6;
    m.volumes.push_back(v);
}

// A building whose one section holds an optional solid CB box x in [-1, 1]
// and a blink box x in [1.9, 5] (flags 0x28: accum 0x2E, the indoors letter
// set). Both span y [-2, 2] and z [-2, 3], so a body standing at local x 0.5
// is pushed out through the +x face.
CollisionModel doorway_model(bool with_solid) {
    CollisionModel m;
    if (with_solid) add_box(m, 1, 0, -1.0, 1.0, -2.0, 2.0, -2.0, 3.0);
    add_box(m, 8, 0x28, 1.9, 5.0, -2.0, 2.0, -2.0, 3.0);
    CollisionSection s;
    s.volume_start = 0;
    s.volume_count = static_cast<int32_t>(m.volumes.size());
    m.sections.push_back(s);
    return m;
}

struct Rig {
    std::unique_ptr<World> wp = std::make_unique<World>();
    World &w = *wp;
    CollisionWorld cw;
    Field field;
    EntityHandle building;
    EntityHandle soldier;

    // The building at (10, 10, 0) with identity rotation (mission yaw 90 is
    // engine heading 0); the local Player body at (x, y, 0).
    Rig(CollisionModel model, double x, double y) {
        w.registry.configure_pool(0, 8);
        w.registry.configure_pool(1, 8);
        w.registry.configure_pool(2, 8);
        cw.terrain = &field.field;
        w.collision = &cw;
        // Pool-2 slot 0 packs a blink hit to a zero low word; keep it taken.
        Entity filler;
        filler.kind = EntityKind::Marker;
        filler.net_id = 99;
        w.registry.spawn(2, filler);
        Entity b;
        b.kind = EntityKind::Building;
        b.net_id = 100;
        b.position = {10.0f, 10.0f, 0.0f};
        b.yaw = 90;
        b.alive = true;
        building = w.registry.spawn(2, b);
        Entity s;
        s.kind = EntityKind::Organic;
        s.net_id = 1;
        s.position = {static_cast<float>(x), static_cast<float>(y), 0.0f};
        s.bound_radius = 1.0f;
        s.alive = true;
        s.engine_flags |= kEntityFlagPlayer;
        soldier = w.registry.spawn(0, s);
        w.cached.local_player = soldier;
        cw.local_player = soldier;
        cw.assign_entity(building, cw.add_model(std::move(model)));
        // Candidate slices rebuild on the 17-tick cadence.
        for (int i = 0; i < 17; ++i) cw.build_tick_tables(w);
    }

    // One full resolve of the local Player body at its position; returns the
    // x displacement the push-out applied.
    double resolve() {
        Entity *s = w.registry.get(soldier);
        int32_t pos[3] = {fx(s->position.x), fx(s->position.y), fx(s->position.z)};
        const int32_t x0 = pos[0];
        int32_t vel[3] = {};
        int16_t health = 100;
        CollisionWorld::ResolveState state;
        cw.resolve_entity(w, soldier, state, pos, vel, vel[2], 0, fx(1.8), 0, 0,
                          /*is_player_class=*/true, /*is_authority=*/true, 0, 43, 0u, health);
        return (pos[0] - x0) / 65536.0;
    }

    Entity &body() { return *w.registry.get(soldier); }
};

// DX-1. The control: without the solid, the body's first-pass capsule points
// (local x 0.5; the head sphere reaches x 1.4) miss the blink box starting at
// x 1.9. With the solid, the first pass pushes the body out through the +x
// face and the relaxation pass tests the shifted points, which reach the box:
// that hit lands in the quad, while the letters and the indoors bit stay
// first-pass only.
void test_relaxation_blink_hits_reach_only_the_quad() {
    {
        Rig rig(doorway_model(/*with_solid=*/false), 10.5, 10.0);
        rig.resolve();
        CHECK(rig.body().blink_hits[0] == 0u);
        CHECK(rig.cw.local_player_blink_flags == 0u);
    }
    {
        Rig rig(doorway_model(/*with_solid=*/true), 10.5, 10.0);
        const double pushed = rig.resolve();
        std::printf("indoor_visibility_fixes: the push-out moved the body %.4f u\n", pushed);
        CHECK(pushed > 0.5);
        CHECK(rig.body().blink_hits[0] != 0u);
        CHECK(BlinkAccum::hit_pool_entity_index(rig.body().blink_hits[0]) == rig.building.slot());
        CHECK((rig.cw.local_player_blink_flags & kBlinkIndoorsBit) == 0u);
        CHECK(rig.cw.local_player_blink_flags == 0u);
        CHECK((rig.body().flags & kEntityFlagIndoors) == 0u);
    }
    {
        // A body inside the blink box on the first pass takes the letters
        // (0x28 ^ 6) and the indoors bit.
        Rig rig(doorway_model(/*with_solid=*/false), 13.0, 10.0);
        rig.resolve();
        CHECK(rig.body().blink_hits[0] != 0u);
        CHECK(rig.cw.local_player_blink_flags == 0x2Eu);
        CHECK((rig.body().flags & kEntityFlagIndoors) != 0u);
    }
}

// DX-2, the drop: the dropped object re-queries at the drop point, so a flag
// carried in from outside and dropped inside the blink box carries that box.
void test_dropped_object_takes_a_quad_at_the_drop_point() {
    Rig rig(doorway_model(/*with_solid=*/false), 13.0, 10.0);
    rig.resolve(); // the carrier's own quad and letters
    Entity f;
    f.kind = EntityKind::Item;
    f.net_id = 7;
    f.position = {60.0f, 60.0f, 0.0f};
    f.alive = true;
    f.flags |= kEntityFlagCarried;
    const EntityHandle flag_h = rig.w.registry.spawn(1, f);
    rig.body().mounted_child = flag_h;
    rig.w.registry.get(flag_h)->primary_occupant = rig.soldier;
    CHECK(rig.w.registry.get(flag_h)->blink_hits[0] == 0u);
    rig.w.match.drop_carried_object(rig.w, rig.soldier);
    const Entity &flag = *rig.w.registry.get(flag_h);
    CHECK(!rig.body().mounted_child.valid());
    CHECK(flag.position.x == rig.body().position.x && flag.position.y == rig.body().position.y);
    CHECK(flag.blink_hits[0] != 0u);
    CHECK(BlinkAccum::hit_pool_entity_index(flag.blink_hits[0]) == rig.building.slot());
    CHECK((flag.flags & kEntityFlagIndoors) != 0u);
}

// The drop's own legs: the pose, the motion and the fall it installs as the
// object's update callback.
// [orig: Entity_DropCarriedObject @0x439e54..0x439ec3;
//  Entity_UpdatePositionAndTransform @0x4adef0; Entity_ComputeClampedDisplacement
//  @0x4ad6a0; Entity_SyncPositionFromDefinition @0x43a9f8 / @0x43aa96]

// A pool-1 deck whose one CFAC triangle spans local x/y [-2, 2] at local z 0
// (the minefield ground query's floor rig).
CollisionModel deck_floor_model() {
    CollisionModel floor;
    for (const auto &xy : {std::pair<int, int>{-2, -2}, {2, -2}, {0, 2}}) {
        CollisionVertex vertex;
        vertex.p[0] = xy.first * 65536;
        vertex.p[1] = xy.second * 65536;
        floor.vertices.push_back(vertex);
    }
    CollisionNormal normal;
    normal.n[2] = 16384;
    normal.dominant_axis = 1;
    floor.normals.push_back(normal);
    CollisionFace face;
    face.normal_index = 0;
    face.vertex_index[0] = 0;
    face.vertex_index[1] = 1;
    face.vertex_index[2] = 2;
    face.min[0] = face.min[1] = -2 * 65536;
    face.max[0] = face.max[1] = 2 * 65536;
    floor.faces.push_back(face);
    CollisionSection section;
    section.vertex_count = 3;
    section.normal_count = 1;
    section.face_count = 1;
    floor.sections.push_back(section);
    return floor;
}

struct DropRig {
    std::unique_ptr<World> wp = std::make_unique<World>();
    World &w = *wp;
    CollisionWorld cw;
    Field field;
    EntityHandle carrier;

    // A Player body at `at` whose body record carries its live heading and
    // pitch words.
    DropRig(Vec3 at, int32_t heading, int32_t pitch) {
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 8);
        w.registry.configure_pool(2, 4);
        cw.terrain = &field.field;
        w.collision = &cw;
        Entity s;
        s.kind = EntityKind::Organic;
        s.net_id = 1;
        s.position = at;
        s.alive = true;
        s.engine_flags |= kEntityFlagPlayer;
        carrier = w.registry.spawn(0, s);
        AiEntity *body = w.ai.at(w.ai.attach(carrier));
        body->heading = heading;
        body->pitch = pitch;
    }
    EntityHandle add_deck(Vec3 at) {
        Entity deck;
        deck.kind = EntityKind::Item;
        deck.item_id = 42;
        deck.yaw = 90;
        deck.position = at;
        deck.bound_radius = 4;
        const EntityHandle h = w.registry.spawn(1, deck);
        cw.assign_entity(h, cw.add_model(deck_floor_model()));
        cw.build_tick_tables(w);
        return h;
    }
    // A blue flag the body carries, authored at `home` with placement yaw 30.
    EntityHandle carry_flag(Vec3 home) {
        Entity f;
        f.kind = EntityKind::Item;
        f.item_id = 4091;
        f.has_item_def = true;
        f.net_id = 7;
        f.yaw = 30;
        f.position = home;
        f.spawn_position = home;
        f.alive = true;
        f.flags |= kEntityFlagCarried;
        f.primary_occupant = carrier;
        const EntityHandle h = w.registry.spawn(1, f);
        w.registry.get(carrier)->mounted_child = h;
        return h;
    }
    Entity &at(EntityHandle h) { return *w.registry.get(h); }
    // One pool-1 visit of the row (its +0x1C4 leg runs the installed fall).
    void visit(EntityHandle h) {
        TickContext ctx;
        ctx.world = &w;
        ctx.is_authority = true;
        w.update_pool1_slot(at(h), ctx);
    }
};

// The pose and the motion: the carrier's X/Y, its Z plus 0x4000, its heading
// plus a quarter turn, no horizontal velocity, and sin(pitch) in Q22 shifted
// down 12 (the sine truncates toward zero, the shift floors) as the vertical
// one; then the fall is the installed callback.
void test_drop_pose_and_motion() {
    DropRig rig({40.0f, 40.0f, 5.0f}, 0x12345678, 0x10000000);
    const EntityHandle flag_h = rig.carry_flag({0.0f, 0.0f, 0.0f});
    rig.at(flag_h).veh.vel_x = 77; // stale words the drop must clear
    rig.at(flag_h).veh.vel_y = -77;
    rig.w.match.drop_carried_object(rig.w, rig.carrier);
    const Entity &flag = rig.at(flag_h);
    CHECK(!rig.at(rig.carrier).mounted_child.valid());
    CHECK((flag.flags & kEntityFlagCarried) == 0u && !flag.primary_occupant.valid());
    CHECK(flag.position.x == 40.0f && flag.position.y == 40.0f);
    CHECK(to_fixed(flag.position.z) == 5 * 65536 + 0x4000);
    CHECK(flag.veh.yaw_seeded);
    CHECK(flag.veh.yaw_bam == 0x52345678);
    CHECK(flag.yaw == 334); // the whole-degree mirror: 90 - 115.6 deg, wrapped
    CHECK(flag.veh.air_pitch_bam == 0 && flag.veh.air_roll_bam == 0);
    CHECK(flag.veh.vel_x == 0 && flag.veh.vel_y == 0);
    CHECK(flag.veh.slide_z == 391);
    CHECK(flag.drop_motion == DropMotion::Fall);
    const std::pair<int32_t, int32_t> lifts[] = {
        {-0x10000000, -392}, {0x40000000, 1023}, {-0x40000000, -1024},
        {0x7FFFFFFF, -1}, {0, 0}};
    for (const auto &[pitch, want] : lifts) {
        DropRig other({40.0f, 40.0f, 5.0f}, 0, pitch);
        const EntityHandle h = other.carry_flag({0.0f, 0.0f, 0.0f});
        other.w.match.drop_carried_object(other.w, other.carrier);
        CHECK(other.at(h).veh.slide_z == want);
    }
}

// The fall over bare terrain, stepped by the pool-1 visit: position plus
// velocity, gravity 167 per step, until the clamped ground stops it; the
// landing takes the ground height, no ground link, no velocity, and ends the
// callback.
void test_drop_fall_lands_on_the_terrain() {
    DropRig rig({40.0f, 40.0f, 5.0f}, 0, 0x10000000);
    const EntityHandle flag_h = rig.carry_flag({0.0f, 0.0f, 0.0f});
    rig.w.match.drop_carried_object(rig.w, rig.carrier);
    rig.visit(flag_h);
    CHECK(to_fixed(rig.at(flag_h).position.z) == 344064 + 391);
    CHECK(rig.at(flag_h).veh.slide_z == 391 - 167);
    rig.visit(flag_h);
    CHECK(to_fixed(rig.at(flag_h).position.z) == 344064 + 391 + 224);
    for (int step = 3; step < 68; ++step) rig.visit(flag_h);
    CHECK(rig.at(flag_h).drop_motion == DropMotion::Fall && rig.at(flag_h).position.z > 0.0f);
    rig.visit(flag_h); // the 68th step lands
    const Entity &flag = rig.at(flag_h);
    CHECK(flag.drop_motion == DropMotion::None); // a flag landing on no entity
    CHECK(flag.position.z == 0.0f);
    CHECK(flag.position.x == 40.0f && flag.position.y == 40.0f);
    CHECK(!flag.ground_target.valid());
    CHECK(flag.veh.vel_x == 0 && flag.veh.vel_y == 0 && flag.veh.slide_z == 0);
    rig.visit(flag_h); // no callback installed: the row stays put
    CHECK(rig.at(flag_h).position.z == 0.0f);
}

// A fall onto an entity's CFAC floor lands on it and links it as the ground.
void test_drop_fall_lands_on_a_deck() {
    DropRig rig({60.0f, 60.0f, 5.0f}, 0, 0);
    const EntityHandle deck = rig.add_deck({60.0f, 60.0f, 2.0f});
    const EntityHandle flag_h = rig.carry_flag({0.0f, 0.0f, 0.0f});
    rig.w.match.drop_carried_object(rig.w, rig.carrier);
    CHECK(rig.at(flag_h).veh.slide_z == 0);
    for (int step = 1; step < 52; ++step) rig.visit(flag_h);
    CHECK(rig.at(flag_h).drop_motion == DropMotion::Fall);
    rig.visit(flag_h); // the 52nd step lands
    const Entity &flag = rig.at(flag_h);
    CHECK(flag.drop_motion == DropMotion::Ride); // a flag on a ground entity rides it
    CHECK(to_fixed(flag.position.z) == 2 * 65536);
    CHECK(flag.ground_target == deck);
}

// The ride: the landed flag follows its ground entity's per-tick delta (the
// translation, then the rotation about it with the heading adopted), keeps
// still while the entity does, and drops back into the fall, 0x4000 a tick
// with no ground link, once the entity dies; landing on the dead entity again
// rides it again, so the flag rests on the wreck.
// [orig: Entity_UpdateParentTransform @0x4a88b0 (the follow @0x4a890e..0x4a8cb7,
//  the re-fall @0x4a8cba..0x4a8cdc); the successor select @0x4adfc9..0x4ae00e]
void test_landed_flag_rides_its_ground_entity() {
    DropRig rig({60.5f, 60.0f, 5.0f}, 0, 0);
    const EntityHandle deck_h = rig.add_deck({60.0f, 60.0f, 2.0f});
    const EntityHandle flag_h = rig.carry_flag({0.0f, 0.0f, 0.0f});
    rig.w.match.drop_carried_object(rig.w, rig.carrier);
    for (int step = 1; step <= 52; ++step) rig.visit(flag_h);
    CHECK(rig.at(flag_h).drop_motion == DropMotion::Ride);
    CHECK(rig.at(flag_h).ground_target == deck_h);
    Entity &deck = rig.at(deck_h);
    // The deck carries a live BAM attitude, as a mover's twin does.
    deck.veh.yaw_seeded = true;
    // A still deck: the flag keeps its landing pose.
    stamp_saved_live_pose(deck);
    rig.visit(flag_h);
    CHECK(rig.at(flag_h).position.x == 60.5f && rig.at(flag_h).position.y == 60.0f);
    CHECK(to_fixed(rig.at(flag_h).position.z) == 2 * 65536);
    // One tick moves the deck 1.5 u east and 0.25 u up: the flag with it.
    stamp_saved_live_pose(deck);
    deck.position.x += 1.5f;
    deck.position.z += 0.25f;
    rig.visit(flag_h);
    CHECK(rig.at(flag_h).position.x == 62.0f && rig.at(flag_h).position.y == 60.0f);
    CHECK(to_fixed(rig.at(flag_h).position.z) == 2 * 65536 + 0x4000);
    CHECK(rig.at(flag_h).veh.yaw_bam == 0x40000000);
    // The next tick turns it a quarter turn about its origin: the flag's
    // 0.5 u east offset swings to 0.5 u north and its heading turns with it.
    stamp_saved_live_pose(deck);
    deck.veh.yaw_bam = 0x40000000;
    rig.visit(flag_h);
    CHECK(std::fabs(rig.at(flag_h).position.x - 61.5f) < 0.001f);
    CHECK(std::fabs(rig.at(flag_h).position.y - 60.5f) < 0.001f);
    CHECK(to_fixed(rig.at(flag_h).position.z) == 2 * 65536 + 0x4000);
    CHECK(static_cast<uint32_t>(rig.at(flag_h).veh.yaw_bam) == 0x80000000u);
    const Vec3 on_deck = rig.at(flag_h).position;
    // The deck dies: the ride's next visit drops the flag back into the fall.
    stamp_saved_live_pose(deck);
    deck.flags |= kEntityFlagDead;
    rig.cw.build_tick_tables(rig.w);
    rig.visit(flag_h);
    CHECK(rig.at(flag_h).drop_motion == DropMotion::Fall);
    CHECK(rig.at(flag_h).veh.slide_z == -0x4000);
    CHECK(!rig.at(flag_h).ground_target.valid());
    CHECK(rig.at(flag_h).position.x == on_deck.x && rig.at(flag_h).position.z == on_deck.z);
    // It lands on the wreck at once and rides it again.
    rig.visit(flag_h);
    CHECK(rig.at(flag_h).drop_motion == DropMotion::Ride);
    CHECK(rig.at(flag_h).ground_target == deck_h);
    CHECK(rig.at(flag_h).position.z == on_deck.z);
    rig.visit(flag_h);
    CHECK(rig.at(flag_h).drop_motion == DropMotion::Fall);
}

// A def with a physics selector skips the ride whole; a non-flag object
// follows a dead ground entity plainly (no re-fall) and rides a live one; a
// row without a def stops at its landing.
// [orig: Entity_UpdateParentTransform @0x4a88bb; the successor select
//  @0x4adfc9..0x4ae00e; Entity_InterpolateFromParentDelta @0x4a8d60]
void test_ride_gates_and_non_flag_successors() {
    {
        DropRig rig({60.0f, 60.0f, 5.0f}, 0, 0);
        const EntityHandle deck_h = rig.add_deck({60.0f, 60.0f, 2.0f});
        const EntityHandle flag_h = rig.carry_flag({0.0f, 0.0f, 0.0f});
        ItemDeathTraits traits;
        traits.death_class = ItemDeathClass::kNone; // no class event on the visit
        traits.physics = 1;
        rig.w.tables.item_death_traits.set(4091, traits);
        rig.w.match.drop_carried_object(rig.w, rig.carrier);
        for (int step = 1; step <= 52; ++step) rig.visit(flag_h);
        CHECK(rig.at(flag_h).drop_motion == DropMotion::Ride);
        stamp_saved_live_pose(rig.at(deck_h));
        rig.at(deck_h).position.x += 1.5f;
        rig.visit(flag_h);
        CHECK(rig.at(flag_h).position.x == 60.0f);
    }
    const auto land = [](bool deck_dead, bool has_def) {
        DropRig rig({60.0f, 60.0f, 5.0f}, 0, 0);
        const EntityHandle deck_h = rig.add_deck({60.0f, 60.0f, 2.0f});
        if (deck_dead) rig.at(deck_h).flags |= kEntityFlagDead;
        Entity box;
        box.kind = EntityKind::Item;
        box.item_id = 5000;
        box.has_item_def = has_def;
        box.alive = true;
        const EntityHandle box_h = rig.w.registry.spawn(1, box);
        drop_object_from_carrier(rig.w, rig.at(box_h), drop_carrier_pose(rig.w, rig.at(rig.carrier)));
        for (int step = 1; step <= 52; ++step) rig.visit(box_h);
        const DropMotion landed = rig.at(box_h).drop_motion;
        // The plain follow never drops back into the fall.
        rig.visit(box_h);
        return std::make_pair(landed, rig.at(box_h).drop_motion);
    };
    CHECK(land(true, true) == std::make_pair(DropMotion::Follow, DropMotion::Follow));
    CHECK(land(false, true) == std::make_pair(DropMotion::Ride, DropMotion::Ride));
    CHECK(land(false, false) == std::make_pair(DropMotion::None, DropMotion::None));
}

// The drop sound: the object's def +0x6F3 name (items.def door_open_sound_id)
// plays at the carrier through the distance-attenuated play; the stock flags
// author none and play nothing.
// [orig: Entity_DropCarriedObject @0x439efc..0x439f1b]
void test_drop_plays_the_def_sound_at_the_carrier() {
    for (const bool named : {true, false}) {
        DropRig rig({40.0f, 40.0f, 5.0f}, 0, 0);
        const EntityHandle flag_h = rig.carry_flag({0.0f, 0.0f, 0.0f});
        if (named)
            std::snprintf(rig.at(flag_h).door_open_sound, sizeof(rig.at(flag_h).door_open_sound),
                          "%s", "DROP_SET");
        rig.w.out.fire_sounds.set_listener({40.0f, 40.0f, 5.0f});
        rig.w.match.drop_carried_object(rig.w, rig.carrier);
        const std::vector<ReadyFireSound> ready = rig.w.out.fire_sounds.drain();
        if (named) {
            CHECK(ready.size() == 1u);
            CHECK(!ready.empty() && ready[0].set_name == "DROP_SET" &&
                  ready[0].pos.x == 40.0f && ready[0].pos.y == 40.0f && ready[0].pos.z == 5.0f);
        } else {
            CHECK(ready.empty());
        }
    }
}

// The ride's tail feeds the object's ambient emitter, for a live, unhusked
// object on the batch's last tick.
// [orig: Entity_UpdateParentTransform @0x4a8d3d..0x4a8d4f ->
//  Entity_UpdateEnvSoundEmitter @0x4a8080]
void test_ride_feeds_its_ambient_emitter() {
    const auto ride_emits = [](bool last_tick, bool dead) {
        DropRig rig({60.0f, 60.0f, 5.0f}, 0, 0);
        rig.add_deck({60.0f, 60.0f, 2.0f});
        const EntityHandle flag_h = rig.carry_flag({0.0f, 0.0f, 0.0f});
        ItemDeathTraits traits;
        traits.death_class = ItemDeathClass::kNone; // no class event on the visit
        for (std::string &loop : traits.regional_loops) loop = "LOOP_A";
        rig.w.tables.item_death_traits.set(4091, traits);
        rig.w.match.drop_carried_object(rig.w, rig.carrier);
        for (int step = 1; step <= 52; ++step) rig.visit(flag_h);
        CHECK(rig.at(flag_h).drop_motion == DropMotion::Ride);
        rig.w.out.sound_emitters.clear();
        rig.w.rules.last_tick_of_batch = last_tick;
        if (dead) rig.at(flag_h).flags |= kEntityFlagDead;
        rig.visit(flag_h);
        return !rig.w.out.sound_emitters.empty() &&
               rig.w.out.sound_emitters[0].set_name == "LOOP_A" &&
               rig.w.out.sound_emitters[0].source_handle == flag_h.packed;
    };
    CHECK(ride_emits(true, false));
    CHECK(!ride_emits(false, false));
    CHECK(!ride_emits(true, true));
}

// A client traces its pool-1 movers as wire proxies: the clamped ground
// query names the proxy's registry twin, the row the retail client's own
// table holds, so a flag the client drops onto a mover rides it.
// [orig: Entity_ComputeClampedDisplacement @0x4ad6a0 (the out word
//  @0x4ad80e..0x4ad810) over Projectile_RaycastProximitySlots @0x4e5340]
void test_client_ground_query_names_the_wire_twin() {
    DropRig rig({60.0f, 60.0f, 5.0f}, 0, 0);
    rig.w.rules.mp_session = true;
    rig.w.rules.projectile_authority = false;
    Entity twin_seed;
    twin_seed.kind = EntityKind::Item;
    twin_seed.item_id = 42;
    twin_seed.yaw = 90;
    twin_seed.position = {60.0f, 60.0f, 2.0f};
    const EntityHandle twin = rig.w.registry.spawn(1, twin_seed);
    WireDynamicCollisionProxy proxy;
    proxy.wire_handle = twin.packed;
    proxy.registry_twin = twin;
    proxy.model_id = rig.cw.add_model(deck_floor_model());
    proxy.position_q16 = FixedVec3{fx(60.0), fx(60.0), fx(2.0)};
    proxy.bound_radius_q16 = fx(4.0);
    rig.cw.replace_wire_collision_proxies({}, {proxy});
    EntityHandle ground;
    CHECK(rig.cw.minefield_ground(rig.w, {}, {fx(60.0), fx(60.0), fx(4.0)}, true, &ground) ==
          fx(2.0));
    CHECK(ground == twin);
    const EntityHandle flag_h = rig.carry_flag({0.0f, 0.0f, 0.0f});
    rig.w.match.drop_carried_object(rig.w, rig.carrier);
    for (int step = 1; step <= 52; ++step) rig.visit(flag_h);
    CHECK(rig.at(flag_h).drop_motion == DropMotion::Ride);
    CHECK(rig.at(flag_h).ground_target == twin);
}

// The clamped ground query's out word names the entity that clamped the
// height, kept even where the terrain floor then wins.
void test_ground_query_names_the_clamping_entity() {
    DropRig rig({0.0f, 0.0f, 0.0f}, 0, 0);
    std::fill(rig.field.heightmap.begin(), rig.field.heightmap.end(), uint16_t{256});
    const EntityHandle deck = rig.add_deck({0.0f, 0.0f, 2.0f});
    EntityHandle ground;
    CHECK(rig.cw.minefield_ground(rig.w, {}, {0, 0, 4 * 65536}, false, &ground) == 2 * 65536);
    CHECK(ground == deck);
    CHECK(rig.cw.minefield_ground(rig.w, {}, {10 * 65536, 0, 4 * 65536}, false, &ground) ==
          65536);
    CHECK(!ground.valid());
    const EntityHandle low = rig.add_deck({20.0f, 0.0f, 0.5f});
    CHECK(rig.cw.minefield_ground(rig.w, {}, {20 * 65536, 0, 4 * 65536}, false, &ground) ==
          65536);
    CHECK(ground == low);
    CHECK(rig.cw.minefield_ground(rig.w, {}, {20 * 65536, 0, 4 * 65536}, true, &ground) ==
          32768);
    CHECK(ground == low);
}

// The definition pose includes the heading: a far flag snaps its position
// and heading home, the pose itself sends nothing, and a flag at home but
// turned is off the pose (the near path re-publishes, heading kept).
void test_sync_restores_the_authored_heading() {
    DropRig rig({40.0f, 40.0f, 5.0f}, 0x12345678, 0);
    const EntityHandle flag_h = rig.carry_flag({0.0f, 0.0f, 0.0f});
    rig.w.match.drop_carried_object(rig.w, rig.carrier);
    rig.w.match.drain_gameplay_events();
    CHECK(rig.w.match.sync_flag_to_authored_pose(rig.w, flag_h));
    CHECK(rig.at(flag_h).position.x == 0.0f && rig.at(flag_h).position.y == 0.0f &&
          rig.at(flag_h).position.z == 0.0f);
    CHECK(rig.at(flag_h).yaw == 30);
    CHECK(rig.at(flag_h).veh.yaw_bam == spawn_angle_bam(60));
    rig.w.match.drain_gameplay_events();
    CHECK(!rig.w.match.sync_flag_to_authored_pose(rig.w, flag_h));
    CHECK(rig.w.match.drain_gameplay_events().empty());
    rig.at(flag_h).veh.yaw_bam = spawn_angle_bam(60) + 0x40000000;
    CHECK(!rig.w.match.sync_flag_to_authored_pose(rig.w, flag_h));
    CHECK(rig.w.match.drain_gameplay_events().size() == 1u);
    CHECK(rig.at(flag_h).veh.yaw_bam == spawn_angle_bam(60) + 0x40000000);
}

// A vehicle carrier with a known quad and an ewep child hung on it.
struct CarrierRig {
    std::unique_ptr<World> wp = std::make_unique<World>();
    World &w = *wp;
    EntityHandle vehicle_h, gun_h;
    static constexpr uint32_t kQuad[4] = {0x02815000u, 0x02812000u, 0u, 0u};

    CarrierRig() {
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 4);
        Entity v;
        v.kind = EntityKind::Item;
        v.has_item_def = true;
        v.item_type = 1;
        v.health = 100;
        v.alive = true;
        for (int i = 0; i < 4; ++i) v.blink_hits[i] = kQuad[i];
        vehicle_h = w.registry.spawn(1, v);
        Entity g;
        g.kind = EntityKind::Item;
        g.item_id = 700;
        g.health = 100;
        g.alive = true;
        g.ground_target = vehicle_h;
        g.emplacement_parent = vehicle_h;
        gun_h = w.registry.spawn(1, g);
    }
    Entity &gun() { return *w.registry.get(gun_h); }
    Entity &vehicle() { return *w.registry.get(vehicle_h); }
    bool gun_has_vehicle_quad() {
        for (int i = 0; i < 4; ++i)
            if (gun().blink_hits[i] != kQuad[i]) return false;
        return true;
    }
};

// DX-2, the ewep class callback: an unoccupied child on a carrier copies the
// carrier's quad and re-arms its clock to 62; an occupied one copies nothing.
void test_ewep_class_callback_rides_the_carrier_quad() {
    {
        CarrierRig rig;
        ItemDeathTraits traits;
        traits.death_class = ItemDeathClass::kEwep;
        rig.w.tables.item_death_traits.set(700, traits);
        destruction_notify_item_damage(rig.w, rig.gun(), 0);
        CHECK(rig.gun_has_vehicle_quad());
        CHECK(rig.gun().class_think_ticks == 62);
    }
    {
        CarrierRig rig;
        ItemDeathTraits traits;
        traits.death_class = ItemDeathClass::kEwep;
        rig.w.tables.item_death_traits.set(700, traits);
        Entity gunner;
        gunner.kind = EntityKind::Organic;
        gunner.alive = true;
        rig.gun().primary_occupant = rig.w.registry.spawn(0, gunner);
        destruction_notify_item_damage(rig.w, rig.gun(), 0);
        CHECK(rig.gun().blink_hits[0] == 0u);
    }
}

// DX-2, the ewep move function: a vehicle carrier (def type 1) hands its quad
// to the child every update behind the slot gate; another carrier type does
// not.
void test_ewep_move_function_copies_a_vehicle_quad() {
    for (const int carrier_type : {1, 5}) {
        CarrierRig rig;
        rig.vehicle().item_type = static_cast<uint8_t>(carrier_type);
        rig.gun().emplaced_update = true;
        rig.w.tables.weapons.entries.resize(1);
        rig.w.tables.weapons.entries[0].valid = true;
        rig.w.tables.weapons.entries[0].name = "CHILD_GUN";
        rig.gun().primary_weapon_slot_adm = 0;
        tick_emplaced_weapon_class_update(rig.w, rig.gun());
        if (carrier_type == 1) CHECK(rig.gun_has_vehicle_quad());
        else CHECK(rig.gun().blink_hits[0] == 0u);
    }
}

// DX-2, a seated rider: every seated tick copies the parent's quad.
void test_seated_rider_rides_the_parent_quad() {
    std::unique_ptr<World> wp = std::make_unique<World>();
    World &w = *wp;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);
    Entity gun_seed;
    gun_seed.kind = EntityKind::Item;
    gun_seed.health = 100;
    gun_seed.alive = true;
    gun_seed.emplaced_ctrl_publisher = true;
    Seat usegun;
    usegun.type = SeatType::Gunner;
    usegun.bone_index = 6;
    usegun.source_name = "UseGun";
    gun_seed.seats.push_back(usegun);
    gun_seed.blink_hits[0] = 0x02815000u;
    gun_seed.blink_hits[1] = 0x02812000u;
    const EntityHandle gun_h = w.registry.spawn(1, gun_seed);
    Entity gunner;
    gunner.kind = EntityKind::Organic;
    gunner.player_class = 8;
    gunner.health = 100;
    gunner.alive = true;
    const EntityHandle gunner_h = w.registry.spawn(0, gunner);
    AiEntity *body = w.ai.at(w.ai.attach(gunner_h));
    body->health = 100;
    body->inf.active = true;
    CHECK(w.vehicles.process_attach(gunner_h, gun_h, 6));
    CHECK(w.registry.get(gunner_h)->blink_hits[0] == 0u);
    CHECK(w.ai.pose_if_mounted(*body, w));
    const Entity &rider = *w.registry.get(gunner_h);
    CHECK(rider.blink_hits[0] == 0x02815000u);
    CHECK(rider.blink_hits[1] == 0x02812000u);
}

} // namespace

int main() {
    test_relaxation_blink_hits_reach_only_the_quad();
    test_dropped_object_takes_a_quad_at_the_drop_point();
    test_drop_pose_and_motion();
    test_drop_fall_lands_on_the_terrain();
    test_drop_fall_lands_on_a_deck();
    test_landed_flag_rides_its_ground_entity();
    test_ride_gates_and_non_flag_successors();
    test_drop_plays_the_def_sound_at_the_carrier();
    test_ride_feeds_its_ambient_emitter();
    test_client_ground_query_names_the_wire_twin();
    test_ground_query_names_the_clamping_entity();
    test_sync_restores_the_authored_heading();
    test_ewep_class_callback_rides_the_carrier_quad();
    test_ewep_move_function_copies_a_vehicle_quad();
    test_seated_rider_rides_the_parent_quad();
    if (failures == 0) std::printf("indoor_visibility_fixes: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
