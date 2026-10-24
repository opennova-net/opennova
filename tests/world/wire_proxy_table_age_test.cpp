// D-NET-196 (L5-s15 row 0): a joiner's decoded rows join the proximity tables at the
// entity update's own build, after its pool-1 walk and its projectile pass, so the
// projectile pass reads the table the previous update built: a row spawned since that
// build joins no trace until the next one, and the coarse gate reads where a row stood
// at the build, the narrow phase where it stands now. The pool-1 movers' proxies keep
// the same copy / live split as the persons'.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9, the
//  Weapon_UpdateAllProjectiles call @0x4C223A, the Entity_BuildProximityLists_Pool01 call
//  @0x4C240A, the pool-0 walk @0x4C2426..0x4C2472; Entity_BuildProximityLists_Pool01
//  @0x4B9340 (the pool-1 copies @0x4B9389..0x4B93B3); Projectile_RaycastProximitySlots
//  @0x4E5340 (the slot gate on g_DynProxY/X/Z @0x4E55E6..0x4E575A, g_DynProxEntity
//  @0x4E5760, the Physics_RaycastAgainstBoneCollision call @0x4E578D)]
#include <cstdio>
#include <memory>
#include <vector>

#include <runtime/world/ammo_table.h>
#include <runtime/world/collision.h>
#include <runtime/world/geom.h> // to_fixed
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) {                                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

constexpr uint16_t kSelfH = 0x0000;   // the joiner's own wire identity
constexpr uint16_t kRemoteH = 0x0003; // a remote person row
constexpr uint16_t kHullH = 0x1002;   // a remote pool-1 mover row
constexpr int32_t kQ = 65536;

// A non-authority session world (a joiner's): its own body L off the rounds' path,
// a collision world the entity update builds, and a round that flies 10 units a tick
// along +x at the torso stand-in's centre height.
struct Joiner {
    std::unique_ptr<World> owner = std::make_unique<World>();
    World &world = *owner;
    CollisionWorld collision;
    EntityHandle local;

    Joiner() {
        world.registry.configure_pool(0, 8);
        world.rules.mp_session = true;
        world.rules.projectile_authority = false;
        world.rules.logic_authority = false;
        Entity self;
        self.kind = EntityKind::Organic;
        self.item_type_index = 3;
        self.has_item_def = true;
        self.item_type = 3;
        self.flags = kEntityFlagPlayer;
        self.position = {-50.0f, 0.0f, 0.0f}; // out of the rounds' path
        self.health = 100;
        local = world.registry.spawn(0, self);
        world.cached.local_player = local;
        world.collision = &collision;
        world.ai.collision = &collision;

        AmmoTableEntry ammo;
        ammo.name = "PROXY_ROUND";
        ammo.valid = true;
        ammo.velocity = 620; // 10 units a tick
        ammo.max_age_ticks = 20;
        ammo.weight_in_grains = 875;
        ammo.max_damage = 25;
        world.tables.ammo.entries.push_back(ammo);
    }

    // The joiner's own round along +x from the origin; the next entity update's
    // projectile pass flies it through x = 0..10.
    void fire() {
        RoundSpawnParams params;
        params.owner = local;
        params.shooter_handle = kSelfH;
        params.origin = {0.0f, 0.0f, kOrganicStandInCenterZ};
        params.ammo_index = 0;
        CHECK(world.round_sim.spawn(world, params, RoundConsequenceMode::VisualOnly) >= 0);
    }
};

WirePersonCollisionProxy person_at(int32_t x, int32_t y) {
    WirePersonCollisionProxy proxy;
    proxy.wire_handle = kRemoteH;
    proxy.position_q16 = FixedVec3{x * kQ, y * kQ, 0};
    return proxy;
}

// The embedder's two legs run in the update's order: the proximity build's after the
// projectile pass and ahead of the pool-0 walk's.
void test_the_build_leg_runs_ahead_of_the_pool0_leg() {
    Joiner j;
    std::vector<int> order;
    j.world.before_proximity_build = [&order] { order.push_back(1); };
    j.world.before_pool0_walk = [&order] { order.push_back(2); };
    j.world.run_logic_tick(/*is_authority=*/false);
    CHECK(order.size() == 2 && order[0] == 1 && order[1] == 2);
}

// The pool-1 slot leg (L5-s16, design (c)): the walk calls it at every pool-1 slot in slot
// order, a slot the registry holds no row at included, a row's ground chain visited
// first (the chain's slot then takes no second call), and every call ahead of the
// proximity build's leg, so ahead of the projectile pass between them. Each row's call
// comes as its +0x1C4 legs end: a falling dropped object (a genx row's class motion)
// has taken this tick's step when its leg reads it.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the walk @0x4C2158..0x4C21E9 (the
//  Entity_UpdatePool1Slot calls @0x4C21B9 / @0x4C21D0 / @0x4C21E0 / @0x4C21E9), the
//  Weapon_UpdateAllProjectiles call @0x4C223A, the proximity build @0x4C240A;
//  Entity_UpdatePool1Slot @0x4B8DD0 - the +0x1C4 call @0x4B8E41..0x4B8E53]
void test_the_pool1_slot_leg_runs_at_each_slot_in_slot_order() {
    Joiner j;
    j.world.registry.configure_pool(1, 4);
    Entity item;
    item.kind = EntityKind::Item;
    item.position = {40.0f, 40.0f, 20.0f};
    Entity falling = item;
    falling.drop_motion = DropMotion::Fall;
    falling.veh.vel_x = kQ; // one unit a tick along +x, well above the ground
    CHECK(j.world.registry.spawn_at(EntityHandle{0x1001}, falling) == EntityHandle{0x1001});
    Entity rider = item;
    rider.ground_target = EntityHandle{0x1003}; // the slot past it carries it
    CHECK(j.world.registry.spawn_at(EntityHandle{0x1002}, rider) == EntityHandle{0x1002});
    CHECK(j.world.registry.spawn_at(EntityHandle{0x1003}, item) == EntityHandle{0x1003});
    std::vector<uint16_t> order;
    int32_t falling_x_at_leg = 0;
    j.world.pool1_slot_leg = [&](EntityHandle handle) {
        order.push_back(handle.packed);
        if (handle.packed == 0x1001)
            falling_x_at_leg = j.world.registry.get(handle)->position.x.q16;
    };
    j.world.before_proximity_build = [&order] { order.push_back(0xFFFF); };
    j.world.run_logic_tick(/*is_authority=*/false);
    const std::vector<uint16_t> expected = {0x1000, 0x1001, 0x1003, 0x1002, 0xFFFF};
    CHECK(order == expected);
    CHECK(falling_x_at_leg == 41 * kQ);
}

// A remote person the receive spawned since the last build: this update's projectile
// pass flies a round straight through where it stands and finds no table row for it;
// the build after the pass adds it, and the next update's round strikes it.
void test_a_row_spawned_since_the_build_misses_this_updates_projectiles() {
    Joiner j;
    j.collision.replace_wire_collision_proxies({}, {}, kSelfH); // the previous build
    j.collision.build_tick_tables(j.world);
    j.world.before_proximity_build = [&j] {
        j.collision.replace_wire_collision_proxies({person_at(5, 0)}, {}, kSelfH);
    };
    j.fire();
    j.world.run_logic_tick(/*is_authority=*/false);
    CHECK(j.world.round_sim.impacts.empty());
    CHECK(j.collision.wire_person_proxy(kRemoteH) != nullptr);
    j.fire();
    j.world.run_logic_tick(/*is_authority=*/false);
    CHECK(j.world.round_sim.impacts.size() == 1);
    if (!j.world.round_sim.impacts.empty()) {
        const int32_t x = to_fixed(j.world.round_sim.impacts[0].position.x);
        CHECK(x > 4 * kQ && x < 5 * kQ);
    }
}

// A remote person who stepped onto the rounds' path since the last build: its live
// words follow it (the joiner's sync ahead of the update), but the projectile pass's
// coarse gate reads the table's copy, 50 units off the path, and passes it over; the
// build after the pass copies where it stands, and the next update's round strikes it.
void test_the_projectile_pass_gates_on_the_previous_builds_copy() {
    Joiner j;
    j.collision.replace_wire_collision_proxies({person_at(5, 50)}, {}, kSelfH);
    j.collision.build_tick_tables(j.world);
    j.collision.set_wire_person_proxy_live(kRemoteH, FixedVec3{5 * kQ, 0, 0}, false, 0);
    j.world.before_proximity_build = [&j] {
        j.collision.replace_wire_collision_proxies({person_at(5, 0)}, {}, kSelfH);
    };
    j.fire();
    j.world.run_logic_tick(/*is_authority=*/false);
    CHECK(j.world.round_sim.impacts.empty());
    const WirePersonCollisionProxy *proxy = j.collision.wire_person_proxy(kRemoteH);
    CHECK(proxy != nullptr && proxy->position_q16.y == 0 && proxy->live_position_q16.y == 0);
    j.fire();
    j.world.run_logic_tick(/*is_authority=*/false);
    CHECK(j.world.round_sim.impacts.size() == 1);
}

// A one-section q8 CFAC quad at section-local z = 1, normal +z, spanning (+-1, +-1):
// the projectile suite's proxy fixture shape.
CollisionModel quad_model() {
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
        f.plane_dist = -0x10000; // on-plane at z=1
        f.min[0] = -0x10000; f.max[0] = 0x10000;
        f.min[1] = -0x10000; f.max[1] = 0x10000;
        f.min[2] = 0x10000;  f.max[2] = 0x10000;
        f.material = 7;
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

// A remote pool-1 mover's proxy: the slot gate reads the table's copy (radius 3 u at
// x = 12), the posed faces the live mover. A ray through the copy whose live mover
// moved 8 u off it hits nothing; a ray through the live mover 8 u from the copy is
// passed over by the slot gate; a live mover still within the copy's reach is struck
// where it stands now.
void test_a_dynamic_proxy_gates_on_the_copy_and_poses_the_live_mover() {
    Joiner j;
    const int32_t model_id = j.collision.add_model(quad_model());
    WireDynamicCollisionProxy hull;
    hull.wire_handle = kHullH;
    hull.model_id = model_id;
    hull.position_q16 = FixedVec3{12 * kQ, 0, 0};
    hull.bound_radius_q16 = 3 * kQ;
    j.collision.replace_wire_collision_proxies({}, {hull}, kSelfH);
    j.collision.build_tick_tables(j.world);
    const auto trace_down_at = [&j](int32_t x_q16) {
        ProjectileTrace trace;
        trace.start = FixedVec3{x_q16, 0, 3 * kQ};
        trace.end = FixedVec3{x_q16, 0, -3 * kQ};
        trace.owner = j.local;
        trace.include_wire_proxies = true;
        return j.collision.trace_projectile(j.world, trace);
    };
    // The build seeds the live words from the copy.
    const WireDynamicCollisionProxy *proxy = j.collision.wire_dynamic_proxy(kHullH);
    CHECK(proxy != nullptr && proxy->live_position_q16.x == 12 * kQ);
    CHECK(trace_down_at(12 * kQ).hit_class == ProjectileHitClass::DynamicEntity);
    // The mover drove 8 u on since the build.
    j.collision.set_wire_dynamic_proxy_live(kHullH, FixedVec3{20 * kQ, 0, 0}, 0, 0, 0, 0);
    CHECK(!trace_down_at(12 * kQ).hit());
    CHECK(!trace_down_at(20 * kQ).hit());
    proxy = j.collision.wire_dynamic_proxy(kHullH);
    CHECK(proxy != nullptr && proxy->position_q16.x == 12 * kQ);
    // One unit on: the copy's reach still admits a ray 1.5 u from it, and the faces
    // posed at the live mover take it.
    j.collision.set_wire_dynamic_proxy_live(kHullH, FixedVec3{13 * kQ, 0, 0}, 0, 0, 0, 0);
    const ProjectileHit hit = trace_down_at(13 * kQ + kQ / 2);
    CHECK(hit.hit_class == ProjectileHitClass::DynamicEntity);
    CHECK(hit.surface_type == 7);
}

// V-ROLL-R's add-on (L5-s16): past the coarse gate both traces read the live entity's Flags
// and pass over a carried (0x1) or minefield (0x2000000) row. The build skips a bit-0 row,
// so the live test is what passes over a row that gained a bit after the build.
// [orig: Physics_RaycastAgainstProximityList @0x4E4A30 (`test [eax+24h], 2000001h`
//  @0x4E4C33); Projectile_RaycastProximitySlots @0x4E5340 (the test @0x4E5767);
//  Entity_BuildProximityLists_Pool01 @0x4B9340 (the bit-0 skips @0x4B9382 / @0x4B93E4);
//  Minefield_InitEntity @0x4417D0 (`or [esi+24h], 2000000h` @0x441A23)]
void test_a_row_flagged_since_the_build_is_passed_over() {
    {
        // A remote person on the rounds' path, carried since the build.
        Joiner j;
        j.collision.replace_wire_collision_proxies({person_at(5, 0)}, {}, kSelfH);
        j.collision.build_tick_tables(j.world);
        j.collision.set_wire_person_proxy_live(kRemoteH, FixedVec3{5 * kQ, 0, 0}, false,
                                               kEntityFlagCarried);
        j.fire();
        j.world.run_logic_tick(/*is_authority=*/false);
        CHECK(j.world.round_sim.impacts.empty());
        j.collision.set_wire_person_proxy_live(kRemoteH, FixedVec3{5 * kQ, 0, 0}, false, 0);
        j.fire();
        j.world.run_logic_tick(/*is_authority=*/false);
        CHECK(j.world.round_sim.impacts.size() == 1);
    }
    {
        // A pool-1 row under a downward ray: struck clear, passed over minefield-marked or
        // carried.
        Joiner j;
        const int32_t model_id = j.collision.add_model(quad_model());
        WireDynamicCollisionProxy hull;
        hull.wire_handle = kHullH;
        hull.model_id = model_id;
        hull.position_q16 = FixedVec3{12 * kQ, 0, 0};
        hull.bound_radius_q16 = 3 * kQ;
        j.collision.replace_wire_collision_proxies({}, {hull}, kSelfH);
        j.collision.build_tick_tables(j.world);
        const auto trace_down = [&j]() {
            ProjectileTrace trace;
            trace.start = FixedVec3{12 * kQ, 0, 3 * kQ};
            trace.end = FixedVec3{12 * kQ, 0, -3 * kQ};
            trace.owner = j.local;
            trace.include_wire_proxies = true;
            return j.collision.trace_projectile(j.world, trace);
        };
        const FixedVec3 at{12 * kQ, 0, 0};
        CHECK(trace_down().hit_class == ProjectileHitClass::DynamicEntity);
        j.collision.set_wire_dynamic_proxy_live(kHullH, at, 0, 0, 0, 0x02000000u);
        CHECK(!trace_down().hit());
        j.collision.set_wire_dynamic_proxy_live(kHullH, at, 0, 0, 0, kEntityFlagCarried);
        CHECK(!trace_down().hit());
        j.collision.set_wire_dynamic_proxy_live(kHullH, at, 0, 0, 0, 0);
        CHECK(trace_down().hit_class == ProjectileHitClass::DynamicEntity);
    }
}

} // namespace

int main() {
    test_the_build_leg_runs_ahead_of_the_pool0_leg();
    test_the_pool1_slot_leg_runs_at_each_slot_in_slot_order();
    test_a_row_spawned_since_the_build_misses_this_updates_projectiles();
    test_the_projectile_pass_gates_on_the_previous_builds_copy();
    test_a_dynamic_proxy_gates_on_the_copy_and_poses_the_live_mover();
    test_a_row_flagged_since_the_build_is_passed_over();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("wire_proxy_table_age: ok\n");
    return 0;
}
