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
// [orig: @ 0x478834..0x47884D; fit @ 0x478AF7..0x478B1C]
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

// The tank's contact cues: SSAudio2 (slot 25) and the three tumble strikes.
VehicleTraits tank_with_contact_cues(Rig &r) {
    static constexpr char kProfile[] =
            "begin \"SP_TankContact\"\n"
            "  SSAudio2 TANK_SKID\n"
            "  tumble_hithard TANK_HIT_HARD\n"
            "  tumble_hitmed TANK_HIT_MED\n"
            "  tumble_hitsoft TANK_HIT_SOFT\n"
            "end\n";
    CHECK(r.world.tables.sound_profiles.parse(kProfile, sizeof(kProfile) - 1) == 1);
    auto t = r.traits(VehicleFamily::Tank, true);
    t.sound_profile = "SP_TankContact";
    r.world.vehicles.traits.set(r.entity().item_id, t);
    return t;
}

int slot_sound_count(const World &world, int slot) {
    int count = 0;
    for (const auto &sound : world.out.slot_sounds)
        count += sound.slot == slot ? 1 : 0;
    return count;
}

int32_t up_z16_of(const Entity &v) {
    return detail::vehicle_euler_basis(v.veh.yaw_bam, v.veh.air_pitch_bam, v.veh.air_roll_bam)
            .q22.m[10] >> 6;
}

// The stability byte is cleared by the crash, park and wreck latches, so a
// crashed or parked tank leaves the driving arms of its mover.
// [orig: Entity_ProcessWheeledVehiclePhysics @0x475DE0, @0x477D2F..0x477DA5;
//  consumers Entity_UpdateTankVehiclePhysics @0x489F2B, @0x489FFF, @0x48A68C]
void tank_latched_hull_loses_stability() {
    for (int latch = 0; latch < 4; ++latch) {
        Rig r;
        auto t = r.traits(VehicleFamily::Tank, true);
        auto &v = r.entity();
        v.flags |= kEntityFlagSuspensionCrashed; // no tail recovery
        if (latch == 1) v.veh.crashed = 1;
        if (latch == 2) v.veh.settle_2f0 = 1;
        if (latch == 3) v.veh.wreck_2fc = 1;
        int32_t z = 65536;
        r.contact(t, z);
        CHECK(v.veh.grounded == (latch == 0));
    }
}

// An inverted hull still touching the ground rights itself at the tail
// unless the crash bit parks it. [orig: @0x47948A..0x4794A8]
void tank_inverted_hull_rights_itself() {
    for (bool parked : {false, true}) {
        Rig r;
        auto t = tank_with_contact_cues(r);
        auto &v = r.entity();
        v.veh.air_roll_bam = int32_t(0x80000000u);
        if (parked) v.flags |= kEntityFlagSuspensionCrashed;
        int32_t z = 16384; // the spine, upside down, sits under the ground
        r.contact(t, z);
        if (!parked) {
            CHECK(up_z16_of(v) > 0);
            CHECK(v.veh.crashed == 0 && v.veh.byte_2ef == 0);
        } else {
            // Striking the spine enters the crash state with the skid cue.
            // [orig: @0x47893F..0x4789DD]
            CHECK(up_z16_of(v) < 0);
            CHECK(v.veh.crashed == 1 && v.veh.byte_2ef == 1);
            CHECK(v.veh.skid_sound_latched);
            CHECK(slot_sound_count(r.world, 25) == 1);
            CHECK((v.flags & kEntityFlagInAir) == 0);
            CHECK(z > 16384);
        }
    }
}

// A crashed tank landing fast on its tracks rebounds at half its fall speed
// and plays one tumble cue for its speed band; resting on its tracks it
// re-enters the crash state every tick, and a diagonal of depths marks it
// respawned. [orig: @0x478BDD..0x478D07; Entity_ApplyWheelSuspensionForces
//  @0x463560]
void tank_crash_landing_rebounds_and_latches() {
    {
        Rig r;
        auto t = tank_with_contact_cues(r);
        auto &v = r.entity();
        v.flags |= kEntityFlagInAir | kEntityFlagSuspensionCrashed;
        v.veh.crashed = 1;
        v.veh.slide_z = -0x6000;
        v.veh.speed = 0x3800;
        int32_t z = 60000;
        r.contact(t, z);
        CHECK(v.veh.slide_z == 0x3000);
        CHECK((v.flags & kEntityFlagInAir) == 0);
        CHECK(v.veh.crashed == 1);
        CHECK(v.veh.tumble_med_latched && !v.veh.tumble_hard_latched);
        CHECK(slot_sound_count(r.world, 48) == 1);
    }
    {
        Rig r;
        auto t = tank_with_contact_cues(r);
        auto &v = r.entity();
        v.flags |= kEntityFlagSuspensionCrashed;
        v.veh.crashed = 1;
        v.veh.fresh_2f1 = 0;
        int32_t z = 60000;
        r.contact(t, z);
        CHECK(v.veh.byte_2ef == 1 && v.veh.crashed == 1);
        CHECK(v.veh.fresh_2f1 == 1);
        CHECK(r.world.out.slot_sounds.empty());
    }
}

// The airborne branch keeps the wheel compressions: the terrain-gap release
// runs only for an upright, uncrashed grounded hull, and the airborne tail
// clears the crash-sound byte and both tumble latches.
// [orig: jmp @0x478B64 to @0x479445; release @0x4790A1..0x47930F;
//  @0x478AA7..0x478ABC]
void tank_airborne_tail() {
    Rig r;
    auto t = r.traits(VehicleFamily::Tank, true);
    auto &v = r.entity();
    v.veh.wheel_comp[0] = 20000;
    v.veh.byte_2ef = 1;
    v.veh.tumble_hard_latched = v.veh.tumble_med_latched = true;
    int32_t z = 10 * 65536;
    r.contact(t, z);
    CHECK(v.veh.wheel_comp[0] == 20000);
    CHECK(v.veh.byte_2ef == 0);
    CHECK(!v.veh.tumble_hard_latched && !v.veh.tumble_med_latched);
    CHECK((v.flags & kEntityFlagInAir) != 0);
}

// A crashed, still-falling hull lifted by its spine rebounds with the soft
// tumble cue unless the medium latch holds it. [orig: @0x4788A4..0x478924]
void tank_spine_strike_rebounds() {
    for (bool held : {false, true}) {
        Rig r;
        auto t = tank_with_contact_cues(r);
        auto &v = r.entity();
        v.veh.air_roll_bam = int32_t(0x80000000u);
        v.flags |= kEntityFlagInAir | kEntityFlagSuspensionCrashed;
        v.veh.crashed = 1;
        v.veh.byte_2ef = 1;
        v.veh.slide_z = -0x16000;
        v.veh.tumble_med_latched = held;
        int32_t z = 16384;
        r.contact(t, z);
        CHECK(v.veh.slide_z == 0xA000);
        CHECK(slot_sound_count(r.world, 49) == (held ? 0 : 1));
        CHECK((v.flags & kEntityFlagInAir) == 0);
    }
}

// A settled wreck: parked upright on a diagonal of depths it latches the wreck,
// and the wreck keeps it crashed and parked. [orig: @0x47853C..0x478643;
//  @0x47604E..0x476067]
void tank_wreck_latch() {
    {
        Rig r;
        auto t = r.traits(VehicleFamily::Tank, true);
        auto &v = r.entity();
        v.flags |= kEntityFlagSuspensionCrashed;
        v.veh.settle_2f0 = 1;
        v.veh.plat_acc[1] = 500;
        int32_t z = 60000;
        r.contact(t, z);
        CHECK(v.veh.wreck_2fc == 1 && v.veh.crashed == 1 && v.veh.byte_2ef == 1);
        CHECK(v.veh.plat_acc[1] == 0);
    }
    {
        Rig r;
        auto t = r.traits(VehicleFamily::Tank, true);
        auto &v = r.entity();
        v.flags |= kEntityFlagSuspensionCrashed;
        v.veh.wreck_2fc = 1;
        int32_t z = 10 * 65536;
        r.contact(t, z);
        CHECK(v.veh.crashed == 1 && v.veh.settle_2f0 == 1);
    }
}

// The client window runs at the grounded entry after the growth, stamping the
// landing tick from the previous in-air flag. [orig: @0x478B6C..0x478BD6]
void tank_client_window_opens_on_landing() {
    Rig r;
    r.world.rules.logic_authority = false;
    r.world.ai.is_authority = false;
    auto t = r.traits(VehicleFamily::Tank, true);
    auto &v = r.entity();
    v.veh.fresh_2f1 = 0;
    v.veh.plat_acc[0] = 1000;
    v.flags |= kEntityFlagInAir;
    r.world.logic_tick = 5;
    int32_t z = 10 * 65536;
    r.contact(t, z);
    CHECK(v.veh.airborne_stamp_2f8 == 0);
    CHECK(v.veh.plat_acc[0] == 1250);
    r.world.logic_tick = 6;
    z = 60000;
    r.contact(t, z);
    // The window stamps the landing tick; the diagonal of depths that follows
    // it marks the row respawned. [orig: @0x478CDA..0x478D07]
    CHECK(v.veh.airborne_stamp_2f8 == 6);
    CHECK(v.veh.fresh_2f1 == 1);
}

// The crash-state block precedes the growth: its settled-hull recovery clears
// the crash byte before the sinks grow. [orig: @0x4780CD..0x478381, growth
//  @0x478510]
void tank_crash_state_precedes_growth() {
    Rig r;
    auto t = r.traits(VehicleFamily::Tank, true);
    auto &v = r.entity();
    v.veh.crashed = 1;
    v.veh.settle_2f0 = 1;
    v.veh.plat_acc[0] = 1000;
    int32_t z = 10 * 65536;
    r.contact(t, z);
    CHECK(v.veh.crashed == 0);
    CHECK(v.veh.plat_acc[0] == 1250);
}

// The tank never clears its sinks for an airborne hull below the water plane.
// [orig: tank sink stores @0x47810D, @0x4785E6, @0x47902E, @0x47931F]
void tank_sinks_survive_water() {
    Rig r;
    auto t = r.traits(VehicleFamily::Tank, true);
    r.world.env.water_z = 20 * 65536;
    auto &v = r.entity();
    v.flags |= kEntityFlagInAir;
    v.veh.plat_acc[0] = 1000;
    int32_t z = 10 * 65536;
    r.contact(t, z);
    CHECK(v.veh.plat_acc[0] == 1250);
}

// An axis-aligned box collision model, half extents hx/hy, z from 0 to hz.
CollisionModel box_model(double hx, double hy, double hz) {
    CollisionModel box;
    auto plane = [&](int nx, int ny, int nz, double d) {
        CollisionPlane p;
        p.nx = static_cast<int16_t>(nx);
        p.ny = static_cast<int16_t>(ny);
        p.nz = static_cast<int16_t>(nz);
        p.dist = static_cast<int32_t>(d * 65536.0);
        box.planes.push_back(p);
    };
    plane(16384, 0, 0, -hx);
    plane(-16384, 0, 0, -hx);
    plane(0, 16384, 0, -hy);
    plane(0, -16384, 0, -hy);
    plane(0, 0, 16384, -hz);
    plane(0, 0, -16384, 0.0);
    CollisionVolume volume;
    volume.type = 1;
    volume.min_x = -int32_t(hx * 65536.0);
    volume.max_x = int32_t(hx * 65536.0);
    volume.min_y = -int32_t(hy * 65536.0);
    volume.max_y = int32_t(hy * 65536.0);
    volume.min_z = 0;
    volume.max_z = int32_t(hz * 65536.0);
    volume.plane_start = 0;
    volume.plane_count = 6;
    box.volumes.push_back(volume);
    CollisionSection section;
    section.volume_start = 0;
    section.volume_count = 1;
    section.min_x = volume.min_x;
    section.max_x = volume.max_x;
    section.min_y = volume.min_y;
    section.max_y = volume.max_y;
    section.min_z = volume.min_z;
    section.max_z = volume.max_z;
    section.center[2] = int32_t(hz * 0.5 * 65536.0);
    section.radius = int32_t((hx + hy + hz) * 65536.0);
    box.sections.push_back(section);
    return box;
}

// A square block standing against the tank's +y rails, 0.1 u into the pads.
EntityHandle side_block(Rig &r, CollisionWorld &collision, int32_t attrib) {
    Entity block;
    block.kind = EntityKind::Item;
    block.item_id = 901;
    block.item_type = 1;
    block.has_item_def = true;
    block.item_attrib = attrib;
    block.alive = true;
    block.position = {10.0f, -10.0f + 0.9f + 3.0f, -1.0f};
    block.bound_radius = 3.0f;
    const EntityHandle h = r.world.registry.spawn(1, block);
    collision.assign_entity(h, collision.add_model(box_model(3.0, 3.0, 5.0)));
    r.entity().bound_radius = 3.0f;
    r.world.ai.collision = &collision;
    for (int i = 0; i < 17; ++i)
        collision.build_tick_tables(r.world);
    return h;
}

// Only a steep model face sets a wall byte, and any wall contact stops the
// hull outright, head-on or not. [orig: Entity_CheckCollisionState wall bytes
//  @0x462A30; stop @0x477B7B..0x477B87]
void tank_wall_contact_stops_hull() {
    Rig r;
    auto t = r.traits(VehicleFamily::Tank, true);
    r.world.vehicles.traits.set(r.entity().item_id, t);
    CollisionWorld collision;
    side_block(r, collision, 0);
    auto &v = r.entity();
    v.veh.vel_x = 5000;
    int32_t z = 65536;
    r.contact(t, z);
    CHECK(v.veh.speed == 0 && v.veh.vel_x == 0 && v.veh.vel_y == 0);
}

// A steep terrain face pushes the probes back without a wall byte, so it
// neither stops the hull nor unsettles its axle.
// [orig: wall bytes only in the model branch of Entity_CheckCollisionState
//  @0x462A30; walk @0x477940..0x477AF8]
void tank_terrain_slope_sets_no_wall_bytes() {
    Rig r;
    for (int zi = 0; zi < 64; ++zi)
        for (int xi = 0; xi < 64; ++xi)
            r.heights[size_t(zi * 64 + xi)] = uint16_t(xi * 2 * 256); // 2:1 uphill in +x
    auto t = r.traits(VehicleFamily::Tank, true);
    int32_t z = 20 * 65536;
    r.contact(t, z);
    CHECK(r.entity().veh.speed != 0);
}

// The probe records keep the last pass unscaled for the springs and the Z
// select; only the averaged copy takes the mass share. [orig: @0x4770BB,
//  @0x47720D; records @0x478D7C..0x478F5D]
void tank_mass_share_leaves_suspension_depths() {
    Rig r;
    auto t = r.traits(VehicleFamily::Tank, true);
    r.world.vehicles.traits.set(r.entity().item_id, t);
    r.world.vehicles.traits.set(901, t);
    CollisionWorld collision;
    side_block(r, collision, 0x40);
    int32_t z = 49152;
    r.contact(t, z);
    CHECK(z == 65536);
}

// A queued wheel force (the client strike path queues without consuming) is
// consumed by the next solve's landing pass at the current quad.
// [orig: Entity_ClearSuspensionForces call @0x477FB6]
void tank_landing_pass_consumes_queued_forces() {
    Rig r;
    auto t = r.traits(VehicleFamily::Tank, true);
    auto &v = r.entity();
    v.veh.chassis_forces[0].rate = 4000;
    v.veh.chassis_forces[0].direction[2] = -65536;
    int32_t z = 10 * 65536;
    r.contact(t, z);
    CHECK(v.veh.chassis_forces[0].rate == 0);
}

// The sleep gate also compares the attitude with the mover-entry pose.
// [orig: Transform_ComparePartial @0x459180, call @0x475EFB]
void tank_sleep_gate_compares_attitude() {
    for (bool turned : {false, true}) {
        Rig r;
        auto t = r.traits(VehicleFamily::Tank, true);
        auto &v = r.entity();
        v.primary_occupant = {};
        v.veh.speed = v.veh.cmd_speed = 0;
        v.veh.slide_z = -100;
        v.veh.contact_solved_once = false;
        stamp_saved_live_pose(v);
        if (turned) v.veh.yaw_bam = opennova::io::bam_add(v.veh.yaw_bam, 1);
        int32_t z = 65536;
        r.contact(t, z);
        CHECK(v.veh.contact_solved_once == turned);
    }
}
} // namespace

int main() {
    tank_belly_support_keeps_corners_grounded();
    tank_airborne_fit_applies_corner_drop();
    tank_latched_hull_loses_stability();
    tank_inverted_hull_rights_itself();
    tank_crash_landing_rebounds_and_latches();
    tank_airborne_tail();
    tank_spine_strike_rebounds();
    tank_wreck_latch();
    tank_client_window_opens_on_landing();
    tank_crash_state_precedes_growth();
    tank_sinks_survive_water();
    tank_sleep_gate_compares_attitude();
    tank_landing_pass_consumes_queued_forces();
    tank_wall_contact_stops_hull();
    tank_terrain_slope_sets_no_wall_bytes();
    tank_mass_share_leaves_suspension_depths();
    wheelie_request_and_acceleration();
    wheelie_launch_velocity_and_input();
    wheelie_vertical_cap_and_catchup();
    bike_axle_and_release();
    crashed_upright_depth_precedes_springs();
    bike_off_contact_planar_forward();
    vehicle_respawn_reseeds_destroy_timer();
    return failures ? 1 : 0;
}
