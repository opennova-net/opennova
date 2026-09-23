// An unmoved carrier's Yaw/Pitch/Roll are its placement angles in the spawn
// form, ((deg << 16) / 360) << 16 with the low half zero: every lazy BAM seed
// and every unseeded read of a placed vehicle, boat, aircraft or gun reads that
// form, not deg x 11930464 or the continuous 2^32/360 scale.
// [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66 (heading), @0x40EB69..0x40EBA6
//  (pitch, roll); the movers' entry copies Entity_UpdateVehiclePhysics @0x48AF7A..0x48AF8C,
//  Entity_UpdateWatercraftPhysics @0x48D4B2..0x48D4CB, Entity_UpdateAircraftPhysics
//  @0x49034C..0x49035E, Entity_ProcessInfantryPhysics @0x46E133..0x46E14C]
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_mount.h>
#include <runtime/world/world.h>
#include <base/io/bam.h>

#include <cstdio>
#include <memory>

using namespace opennova;
using namespace opennova::world;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

namespace {

// The placement every case uses: yaw 1, pitch 10, roll -3.
constexpr int32_t kHeading89 = 1061748736; // ((89 << 16) / 360) << 16
constexpr int32_t kPitch10 = 119275520;    // ((10 << 16) / 360) << 16
constexpr int32_t kRollMinus3 = -35782656; // ((-3 << 16) / 360) << 16

struct Rig {
    std::unique_ptr<World> storage = std::make_unique<World>();
    World &w = *storage;
    EntityHandle veh_h;
    Rig() {
        w.registry.configure_pool(0, 8);
        w.registry.configure_pool(1, 8);
        Entity veh;
        veh.kind = EntityKind::Item;
        veh.item_id = 1291;
        veh.has_item_def = true;
        veh.net_id = 200;
        veh.position = {100.0f, 200.0f, 10.0f};
        veh.yaw = 1;
        veh.pitch = 10;
        veh.roll = -3;
        veh.health = veh.health_max = 3000;
        veh.alive = true;
        veh_h = w.registry.spawn(1, veh);
    }
    Entity &veh() { return *w.registry.get(veh_h); }
    void kill() {
        veh().health = 0;
        veh().engine_flags |= kEntityFlagDead;
    }
};

VehicleTraits motor_traits(VehicleFamily family, int physics = 1) {
    VehicleTraits t;
    t.family = family;
    t.physics = physics;
    t.player_speed = 94 * 293;
    t.acceleration = 60;
    t.deceleration = 280;
    t.turn_rate = 65 * 192426;
    t.turn_rate2 = 41 * 192426;
    return t;
}

// The ground/tank/bike authority mover and its prediction twin stamp
// savedLivePose from the angles they seed.
void test_ground_seeds_read_the_spawn_form() {
    {
        Rig r;
        const VehicleTraits t = motor_traits(VehicleFamily::Tank);
        r.w.vehicles.tick_motor(r.veh(), t);
        CHECK(r.veh().saved_live_yaw == kHeading89);
        CHECK(r.veh().saved_live_pitch == kPitch10);
        CHECK(r.veh().saved_live_roll == kRollMinus3);
    }
    {
        Rig r;
        const VehicleTraits t = motor_traits(VehicleFamily::Tank);
        r.w.ai.is_authority = false;
        r.veh().veh.net_predicted = true;
        r.w.vehicles.ground_client_tick(r.veh(), t);
        CHECK(r.veh().saved_live_yaw == kHeading89);
    }
}

// The selector-zero mover seeds all three and stamps them at its entry.
void test_simple_motor_seed_reads_the_spawn_form() {
    Rig r;
    const VehicleTraits t = motor_traits(VehicleFamily::Ground, 0);
    r.w.vehicles.tick_simple_motor(r.veh(), t, nullptr, false);
    CHECK(r.veh().saved_live_yaw == kHeading89);
    CHECK(r.veh().saved_live_pitch == kPitch10);
    CHECK(r.veh().saved_live_roll == kRollMinus3);
}

// A dead hull skips the boat mover's input and integration, so the authority
// seed stands as read; the predicting client with no pending record and no
// speed leaves its seeded yaw untouched as well.
void test_watercraft_seeds_read_the_spawn_form() {
    {
        Rig r;
        r.kill();
        const VehicleTraits t = motor_traits(VehicleFamily::Watercraft);
        r.w.vehicles.tick_watercraft_motor(r.veh(), t);
        CHECK(r.veh().veh.yaw_bam == kHeading89);
        CHECK(r.veh().veh.air_pitch_bam == kPitch10);
        CHECK(r.veh().veh.air_roll_bam == kRollMinus3);
    }
    {
        Rig r;
        const VehicleTraits t = motor_traits(VehicleFamily::Watercraft);
        r.w.ai.is_authority = false;
        auto &m = r.veh().veh;
        m.net_predicted = true;
        m.net_interp_progress = 20;
        r.w.vehicles.watercraft_client_tick(r.veh(), t);
        CHECK(m.yaw_bam == kHeading89);
    }
}

// The aircraft mover's own seed (a dead predicted hull returns before its live
// controls) and the AI drive's seed ahead of it (a non-PlayerControl aircraft
// returns right after copying its command registers).
void test_aircraft_seeds_read_the_spawn_form() {
    {
        Rig r;
        r.kill();
        const VehicleTraits t = motor_traits(VehicleFamily::Helicopter);
        r.w.ai.is_authority = false;
        auto &m = r.veh().veh;
        m.net_predicted = true;
        m.net_interp_progress = 20;
        r.w.vehicles.aircraft_client_tick(r.veh(), t);
        CHECK(m.yaw_bam == kHeading89);
    }
    {
        Rig r;
        const VehicleTraits t = motor_traits(VehicleFamily::Helicopter);
        r.w.ai.attach(r.veh_h);
        r.w.ai.chel_ai_drive(r.w, r.veh(), nullptr, t);
        CHECK(r.veh().veh.yaw_bam == kHeading89);
    }
}

// The carrier-delta readers see an unseeded row in the same form the seeds
// write, so a delta taken across the first seed does not jump.
void test_unseeded_carrier_pose_reads_the_spawn_form() {
    Rig r;
    int32_t pos[3], yaw = 0, pitch = 0, roll = 0;
    carrier_pose_fixed(r.veh(), pos, yaw, pitch, roll);
    CHECK(yaw == kHeading89);
    CHECK(pitch == kPitch10);
    CHECK(roll == kRollMinus3);
}

// A static emplacement's frame is its placement.
// [orig: Entity_UpdateChildAttachment @0x440A51 (Pitch) / @0x440A58 (Yaw)]
void test_emplaced_gun_frame_reads_the_spawn_form() {
    Rig r;
    CHECK(emplaced_gun_frame_heading(r.veh()) == kHeading89);
    CHECK(emplaced_gun_frame_pitch(r.veh()) == kPitch10);
}

// A seat point on an unmoved carrier goes through the same fixed-point euler
// matrix as a moved one, built from the spawn-form angles: flat or tilted, it
// lands where the seeded twin's does.
// [orig: Entity_GetBoneTransformAndOrientation @0x4b0c50; Math_BuildFixedPointMatrixFromEulerAngles
//  @0x613f40]
void test_seat_point_reads_the_spawn_form() {
    const Vec3 local{3.0f, 2.0f, 1.0f};
    for (const bool tilted : {false, true}) {
        Rig r;
        Entity &placed = r.veh();
        placed.yaw = 37;
        placed.pitch = tilted ? -12 : 0;
        placed.roll = tilted ? 17 : 0;
        Entity seeded = placed;
        seeded.veh.yaw_seeded = true;
        seeded.veh.yaw_bam = spawn_angle_bam(90 - 37);
        seeded.veh.air_pitch_bam = spawn_angle_bam(placed.pitch);
        seeded.veh.air_roll_bam = spawn_angle_bam(placed.roll);
        const Vec3 a = entity_local_point_world(placed, local);
        const Vec3 b = entity_local_point_world(seeded, local);
        CHECK(a.x == b.x && a.y == b.y && a.z == b.z);
    }
}

// A non-UseGun seat pre-snaps the requester to the seat bone's yaw: the
// carrier's placement heading turned by the seat's authored facing.
// [orig: Entity_RequestVehicleAttach @0x4365BF..0x4365C3]
void test_seat_presnap_reads_the_spawn_form() {
    Rig r;
    Seat seat;
    seat.type = SeatType::Passenger;
    seat.bone_index = 1;
    seat.source_name = "sitex00";
    seat.yaw_offset = 90;
    r.veh().seats.push_back(seat);
    Entity rider;
    rider.kind = EntityKind::Organic;
    rider.health = rider.health_max = 100;
    rider.alive = true;
    const EntityHandle rider_h = r.w.registry.spawn(0, rider);
    AiEntity &body = *r.w.ai.at(r.w.ai.attach(rider_h));
    body.inf.active = true;
    CHECK(r.w.vehicles.process_attach(rider_h, r.veh_h, 1));
    CHECK(body.heading == io::bam_sub(kHeading89, 0x40000000)); // 89 - 90 degrees
}

} // namespace

int main() {
    test_ground_seeds_read_the_spawn_form();
    test_simple_motor_seed_reads_the_spawn_form();
    test_watercraft_seeds_read_the_spawn_form();
    test_aircraft_seeds_read_the_spawn_form();
    test_unseeded_carrier_pose_reads_the_spawn_form();
    test_emplaced_gun_frame_reads_the_spawn_form();
    test_seat_point_reads_the_spawn_form();
    test_seat_presnap_reads_the_spawn_form();
    std::printf("vehicle placement angles: %s (%d failures)\n",
                failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
