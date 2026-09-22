// PR #645 vehicle follow-ups exercised through the motor and contact solves.
// [orig: Entity_UpdateLightVehiclePhysics @ 0x483FE0;
// Entity_ProcessLightVehiclePhysics @ 0x479600;
// Entity_ProcessTrackedVehiclePhysics @ 0x47C1C0;
// Entity_ProcessWheeledVehiclePhysics @ 0x475DE0;
// Entity_ProcessAircraftContactPhysics @ 0x47EF10]
#include <runtime/world/world.h>
#include <runtime/world/vehicle_motor_detail.h>
#include <runtime/world/vehicle_suspension.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/angle.h>
#include <runtime/world/destruction.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/terrain_query/height_field.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova::world;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)

struct Rig {
    std::unique_ptr<World> owner = std::make_unique<World>();
    World &world = *owner;
    std::vector<uint16_t> heights = std::vector<uint16_t>(64 * 64, 0);
    std::vector<int> sectors = std::vector<int>(256, 1);
    opennova::terrain::TerrainHeightField terrain{};
    EntityHandle handle{}, driver{};
    Rig() {
        world.registry.configure_pool(0, 4);
        world.registry.configure_pool(1, 4);
        terrain.heightmap = heights.data();
        terrain.dim = 64;
        terrain.layout.sector_grid = sectors.data();
        terrain.layout.origin_x = terrain.layout.origin_y = 0;
        Entity v;
        v.kind = EntityKind::Item;
        v.has_item_def = true;
        v.item_id = 1291;
        v.health = v.health_max = 3000;
        v.alive = true;
        v.position = {10, -10, 0.25f};
        v.veh.yaw_seeded = true;
        v.veh.speed = v.veh.cmd_speed = 6000;
        Seat seat;
        seat.type = SeatType::Controller;
        seat.bone_index = 1;
        v.seats.push_back(seat);
        handle = world.registry.spawn(1, v);
        Entity d;
        d.kind = EntityKind::Organic;
        d.health = d.health_max = 100;
        d.alive = true;
        driver = world.registry.spawn(0, d);
        CHECK(world.vehicles.process_attach(driver, handle, 1));
    }
    Entity &entity() { return *world.registry.get(handle); }
    VehicleTraits traits(VehicleFamily family, bool boxed = false) {
        VehicleTraits t;
        t.family = family;
        t.physics = 1;
        t.player_control = true;
        t.player_speed = 30000;
        t.acceleration = 200;
        t.deceleration = 280;
        t.mass = 20;
        if (boxed) {
            t.box_x_lo = t.foot_x_lo = -2 * 65536;
            t.box_x_hi = t.foot_x_hi = 2 * 65536;
            t.box_y_lo = t.foot_y_lo = -65536;
            t.box_y_hi = t.foot_y_hi = 65536;
            t.box_z_lo = -65536;
            t.box_z_hi = 65536;
            t.max_slope = t.slip_slope = 0x20000000;
            world.tables.terrain = &terrain;
        }
        return t;
    }
    void contact(const VehicleTraits &t, int32_t &z) {
        auto &v = entity();
        int32_t x = to_fixed(v.position.x), y = to_fixed(v.position.y);
        switch (t.family) {
        case VehicleFamily::Ground:
            detail::ground_contact_solve(world, v, t, v.veh, x, y, x, y, z); break;
        case VehicleFamily::Tank:
            detail::wheeled_contact_solve(world, v, t, v.veh, x, y, x, y, z); break;
        case VehicleFamily::Bike:
            detail::light_contact_solve(world, v, t, v.veh, x, y, x, y, z); break;
        default:
            detail::aircraft_contact_solve(world, v, t, v.veh, x, y, x, y, z); break;
        }
    }
};

void wheelie_request_and_acceleration() {
    Rig r;
    auto t = r.traits(VehicleFamily::Bike);
    auto &v = r.entity();
    auto &m = v.veh;
    v.flags |= 0x20;
    m.speed_accel = 10;
    m.bike_launch_direction[0] = 65536;
    r.world.vehicles.tick_motor(v, t);
    CHECK(m.wheelie_active == 1 && m.wheelie_request == 0);
    CHECK(m.speed_accel == 35 && m.speed == 6035);
    r.world.vehicles.tick_motor(v, t);
    CHECK(m.speed_accel == 60 && m.speed == 6095);
    v.flags &= ~0x20u;
    r.world.vehicles.tick_motor(v, t);
    CHECK(m.wheelie_active == 1 && m.wheelie_request == 0);
    CHECK(m.speed_accel != 85);

    // The brake requires retained contact, even if the def has no handbrake.
    for (int active : {0, 1}) {
        Rig brake;
        auto bt = brake.traits(VehicleFamily::Bike);
        auto &b = brake.entity();
        b.flags |= 8;
        b.veh.bike_ground_contact_ticks = 3;
        b.veh.wheelie_active = uint8_t(active);
        b.veh.bike_launch_direction[0] = 65536;
        brake.world.vehicles.tick_motor(b, bt);
        CHECK(b.veh.handbrake_latched == (active ? 0 : 1));
    }
    Rig fresh;
    auto ft = fresh.traits(VehicleFamily::Bike);
    ft.hand_brake = 1;
    fresh.entity().flags |= 8;
    fresh.world.vehicles.tick_motor(fresh.entity(), ft);
    CHECK(fresh.entity().veh.handbrake_latched == 0);
}

void wheelie_launch_velocity_and_input() {
    Rig r;
    auto t = r.traits(VehicleFamily::Bike);
    auto &v = r.entity();
    auto &m = v.veh;
    m.wheelie_active = 1;
    m.speed = 10000;
    m.bike_launch_direction[1] = 39321;
    m.bike_launch_direction[2] = 52428;
    m.grounded = true;
    detail::vehicle_traction_velocity(r.world, v, t, m.cmd_speed);
    CHECK(m.vel_x == 0 && std::abs(m.vel_y - 6000) <= 1);
    CHECK(std::abs(m.slide_z - 8000) <= 1);
    m.grounded = false;
    m.contact_solved_once = true;
    m.slide_z = -1234;
    m.vel_x = m.vel_y = -99;
    detail::vehicle_traction_velocity(r.world, v, t, m.cmd_speed);
    CHECK(m.vel_x == 0 && std::abs(m.vel_y - 6000) <= 1 && m.slide_z == -1234);
    v.flags |= kEntityFlagInAir;

    auto &driver = *r.world.registry.get(r.driver);
    driver.net_move_input = 10; // moving, direction 2: normally rotates the command
    m.yaw_bam = 0;
    stage_player_vehicle_input(r.world, v, driver, t);
    CHECK(m.steer_target_bam == 0);
    CHECK(m.cmd_speed == t.player_speed);
    m.wheelie_active = 0;
    v.flags &= ~kEntityFlagInAir;
    stage_player_vehicle_input(r.world, v, driver, t);
    CHECK(m.steer_target_bam != 0);
}

void wheelie_vertical_cap_and_catchup() {
    for (int active : {0, 1}) for (int airborne : {0, 1}) {
        Rig r;
        auto t = r.traits(VehicleFamily::Bike);
        auto &v = r.entity();
        v.veh.wheelie_active = uint8_t(active);
        v.veh.grounded = false;
        v.veh.slide_z = 22000;
        if (airborne) v.flags |= kEntityFlagInAir;
        r.world.vehicles.tick_motor(v, t);
        CHECK(v.veh.slide_z == ((active && !airborne) ? 21750 : 16134));
    }
    for (int held : {0, 1}) {
        Rig r;
        auto t = r.traits(VehicleFamily::Bike);
        t.spring = 8;
        auto &v = r.entity();
        v.veh.wheelie_request = uint8_t(held);
        v.veh.plat_acc[0] = 6100;
        v.veh.slide_z = -1000;
        int32_t adjustment[4] = {};
        r.world.vehicles.suspension_airborne_loop(v, t, 2, 100, adjustment);
        CHECK(adjustment[0] == (held ? 0 : -6000));
        CHECK(v.veh.landing_2ee == 1);
    }
}

void bike_axle_and_release() {
    for (int held : {0, 1}) {
        Rig r;
        auto t = r.traits(VehicleFamily::Bike, true);
        auto &v = r.entity();
        auto &m = v.veh;
        m.light_rear_contact_ticks = 2;
        m.bike_ground_contact_ticks = 9;
        m.wheelie_active = 1;
        m.wheelie_request = uint8_t(held);
        int32_t z = to_fixed(v.position.z);
        r.contact(t, z);
        CHECK(m.bike_ground_contact_ticks == 10);
        CHECK(m.wheelie_active == held);
    }
    Rig banked;
    auto t = banked.traits(VehicleFamily::Bike, true);
    auto &v = banked.entity();
    v.veh.air_roll_bam = 0x10000000; // 22.5 degrees, side axis remains banked
    int32_t z = to_fixed(v.position.z);
    banked.contact(t, z);
    CHECK(std::abs(int64_t(v.veh.air_roll_bam) - 0x10000000) < 20000);
    CHECK(std::abs(int64_t(v.veh.air_pitch_bam)) < 20000);

    // A carried pose is distinct from the previous live pose used by the launch clock.
    Rig carried;
    auto ct = carried.traits(VehicleFamily::Bike, true);
    auto &c = carried.entity();
    c.position.z = 10;
    c.saved_live_valid = true;
    c.saved_live_pos[0] = to_fixed(c.position.x);
    c.saved_live_pos[1] = to_fixed(c.position.y);
    c.saved_live_pos[2] = 9 * 65536;
    z = 10 * 65536;
    carried.contact(ct, z);
    CHECK(c.veh.bike_launch_direction[0] == 46340);
    CHECK(c.veh.bike_launch_direction[1] == 0);
    CHECK(c.veh.bike_launch_direction[2] == 46340);
}

// The not-crashed off-contact bike normalizes the PLANAR forward row when no
// wheelie is active, so a pitched bike keeps its full planar speed
// [orig: Entity_UpdateLightVehiclePhysics +3DE gate @0x4863DA; planar
// normalize @0x48645D..0x4864AB; X/Y stores @0x486572..0x486578].
void bike_off_contact_planar_forward() {
    Rig r;
    auto t = r.traits(VehicleFamily::Bike);
    auto &v = r.entity();
    auto &m = v.veh;
    m.wheelie_active = 0;
    m.speed = 10000;
    m.grounded = false;
    m.contact_solved_once = true;
    m.air_pitch_bam = 0x20000000; // 45 degrees: |forward.xy| = cos 45
    m.slide_z = -777;
    // The pitch must tilt the forward row, or a 3-D normalize would pass too.
    const auto basis = detail::vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
    CHECK(std::abs(basis.q22.m[8] >> 6) > 20000);
    detail::vehicle_traction_velocity(r.world, v, t, m.cmd_speed);
    const double planar = std::sqrt(double(m.vel_x) * m.vel_x + double(m.vel_y) * m.vel_y);
    CHECK(std::abs(planar - 10000.0) <= 2.0);
    CHECK(m.slide_z == -777);
}

// Entity_RespawnVehicle re-seeds the +0x1B0 destroy timer from def+0x1A4
// [orig: @0x46007C..0x460082]; the infantry/marker spawn reset does not.
void vehicle_respawn_reseeds_destroy_timer() {
    Rig r;
    ItemDeathTraits traits;
    traits.destroy_timing_ticks[0] = 777;
    r.world.tables.item_death_traits.set(1291, traits);
    auto &v = r.entity();
    v.destroy_timer = 5;
    v.destroy_timer_initialized = false;
    r.world.vehicles.respawn(v);
    CHECK(v.destroy_timer == 777);
    CHECK(v.destroy_timer_initialized);
    // Entity_ResetToSpawnState @0x4B9610 writes no +0x1B0: the organic reset
    // leaves the timer alone even when the def carries a timing word.
    auto &d = *r.world.registry.get(r.driver);
    d.item_id = 1291;
    d.destroy_timer = 5;
    d.destroy_timer_initialized = true;
    entity_reset_to_spawn_state(r.world, r.world.ai, d);
    CHECK(d.destroy_timer == 5);
}

void crashed_upright_depth_precedes_springs() {
    for (auto family : {VehicleFamily::Ground, VehicleFamily::Tank,
            VehicleFamily::Bike, VehicleFamily::Helicopter}) {
        Rig r;
        auto t = r.traits(family, true);
        t.spring = 8;
        t.spring_comp = 100;
        t.shock = 4;
        auto &v = r.entity();
        v.flags |= kEntityFlagSuspensionCrashed;
        v.veh.crashed = 1;
        v.veh.slide_z = -1000;
        for (auto &osc : v.veh.wheel_osc) osc.energy = 8192;
        int32_t z = to_fixed(v.position.z);
        r.contact(t, z);
        // Flat ground at zero, hull bottom at -1: the retained depth raises the
        // quarter-unit starting height to one, regardless of absorbed spring travel.
        CHECK(z == 65536);
    }
}
// A climbing track can be supported by its inboard belly station while its
// outer wheel is clear. Retail uses max(wheel, belly) for sink growth, spring
// catch-up and the retained-contact tail. [orig: @ 0x4784CC..0x47853A,
// @ 0x478DA4, @ 0x47931B..0x47934C]
void tank_belly_support_keeps_corners_grounded() {
    for (bool authority : {false, true}) {
        Rig r;
        r.world.rules.logic_authority = authority;
        r.world.ai.is_authority = authority;
        auto t = r.traits(VehicleFamily::Tank, true);
        t.spring = 8;
        t.spring_comp = 100;
        auto &m = r.entity().veh;
        m.air_pitch_bam = bam_from_degrees_wrapped(10);
        m.plat_acc[0] = m.plat_acc[1] = 1000;
        int32_t z = to_fixed(0.8f);
        r.contact(t, z);
        CHECK(m.plat_acc[0] == 0 && m.plat_acc[1] == 0);
        CHECK((r.entity().flags & kEntityFlagInAir) == 0);
    }
}

// A falling corner keeps its accumulated drop in the airborne chassis fit.
// Otherwise the tank holds its attitude until a wheel touches, then snaps.
// [orig: @ 0x478834..0x47884D; fit @ 0x478AA7..0x478AC3]
void tank_airborne_fit_applies_corner_drop() {
    Rig r;
    auto t = r.traits(VehicleFamily::Tank, true);
    auto &m = r.entity().veh;
    m.plat_acc[0] = m.plat_acc[1] = 1000;
    int32_t z = 10 * 65536;
    r.contact(t, z);
    CHECK((r.entity().flags & kEntityFlagInAir) != 0);
    CHECK(m.air_pitch_bam < 0);
    CHECK(std::abs(m.air_roll_bam) < 1000);
}
} // namespace

int main() {
    tank_belly_support_keeps_corners_grounded();
    tank_airborne_fit_applies_corner_drop();
    wheelie_request_and_acceleration();
    wheelie_launch_velocity_and_input();
    wheelie_vertical_cap_and_catchup();
    bike_axle_and_release();
    crashed_upright_depth_precedes_springs();
    bike_off_contact_planar_forward();
    vehicle_respawn_reseeds_destroy_timer();
    return failures ? 1 : 0;
}
