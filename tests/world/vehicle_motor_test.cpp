// Host-authoritative ground-vehicle motor (the retail-join v33 "can't drive" gap):
// items.def physics-block scaling, the drive input mapping, the steering chase, the
// speed pipeline, and the no-uplink drive-authority model — a mounted ctrl/drvr
// player's replicated input moves the vehicle on the authority.
// [orig: ItemDef_ParsePhysicsProperty @0x49d870; Entity_UpdateVehiclePhysics @0x48af00;
//  drive gate @0x48b0ff; net-re §5.13 drive-authority witness 2026-07-04]
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/dir_table.h>
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_motor_detail.h>
#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/world.h>
#include <runtime/world/local_player.h>
#include <runtime/terrain_query/height_field.h>

#include <formats/def/def.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace opennova::world;
using namespace opennova::def;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

// The JOX ASH_I5A dune buggy's authored physics block (items.def id 101291), pre-scaled
// per ItemDef_ParsePhysicsProperty @0x49d870: player_speed 94 km/h, accel 15, decel 70,
// turn_rate 65 deg/s, turn_rate2 41 deg/s, physics 1.
VehicleTraits buggy_traits() {
    VehicleTraits t;
    t.physics = 1;
    t.player_speed = 94 * 293;      // 27542 = 16.16 u/tick
    t.acceleration = 15 * 4;        // 60
    t.deceleration = 70 * 4;        // 280
    t.turn_rate = 65 * 192426;      // BAM/tick
    t.turn_rate2 = 41 * 192426;
    t.player_control = true;
    return t;
}

void load_transport_sound_profile(World &world) {
    static constexpr char kTransportProfile[] =
            "begin \"SP_Transport\"\n"
            "  Soundloop_1 V_TRUCK_ILP .8 1.2\n"
            "  Soundloop_2 V_TRUCK_DLP .7 1.1\n"
            "  Soundloop_3 V_TRUCK_REVSE .8 1.2\n"
            "  enginestop V_TRUCK_STOP\n"
            "  enginereverse V_TRUCK_SHIFT\n"
            "end\n";
    CHECK(world.tables.sound_profiles.parse(kTransportProfile,
                                     sizeof(kTransportProfile) - 1) == 1);
}

CollisionModel vehicle_sound_test_wall() {
    CollisionModel box;
    auto plane = [&](int nx, int ny, int nz, double d) {
        CollisionPlane p;
        p.nx = static_cast<int16_t>(nx);
        p.ny = static_cast<int16_t>(ny);
        p.nz = static_cast<int16_t>(nz);
        p.dist = static_cast<int32_t>(d * 65536.0);
        box.planes.push_back(p);
    };
    plane(16384, 0, 0, -2.0);
    plane(-16384, 0, 0, -2.0);
    plane(0, 16384, 0, -25.0);
    plane(0, -16384, 0, -25.0);
    plane(0, 0, 16384, -5.0);
    plane(0, 0, -16384, 0.0);

    CollisionVolume volume;
    volume.type = 1;
    volume.min_x = -(2 << 16);
    volume.max_x = 2 << 16;
    volume.min_y = -(25 << 16);
    volume.max_y = 25 << 16;
    volume.min_z = 0;
    volume.max_z = 5 << 16;
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
    section.center[2] = static_cast<int32_t>(2.5 * 65536.0);
    section.radius = 26 << 16;
    box.sections.push_back(section);
    return box;
}

struct Rig {
    World w;
    EntityHandle veh_h, drv_h;
    Rig() {
        w.registry.configure_pool(0, 16); // organics (the driver)
        w.registry.configure_pool(1, 16); // items (the buggy)

        Entity veh;
        veh.net_id = 200;
        veh.bms_id = 77;
        veh.spawn_origin = (1u << 24) | 3u;
        veh.kind = EntityKind::Item;
        veh.item_id = 1291; // wire type of the dune buggy (items.def id - 100000)
        veh.has_item_def = true; // pool-1 sources need an ItemDef (retail gate)
        veh.position = {100.0f, 200.0f, 10.0f};
        veh.yaw = 0;
        veh.health = 3000;
        veh.health_max = 3000;
        veh.alive = true;
        Seat ctrl;
        ctrl.type = SeatType::Controller;
        ctrl.bone_index = 1;
        ctrl.source_name = "ctrlx00";
        veh.seats.push_back(ctrl);
        veh_h = w.registry.spawn(1, veh);

        Entity drv;
        drv.kind = EntityKind::Organic;
        drv.item_id = 5305; // player template
        drv.player_class = 8;
        drv.position = {100.0f, 199.0f, 10.0f};
        drv.health = 150;
        drv.health_max = 150;
        drv.alive = true;
        drv_h = w.registry.spawn(0, drv);
    }
    Entity &veh() { return *w.registry.get(veh_h); }
    Entity &drv() { return *w.registry.get(drv_h); }
    void mount() { CHECK(w.vehicles.process_attach(drv_h, veh_h, 1)); }
    void tick(int n, const VehicleTraits &t) {
        for (int i = 0; i < n; ++i) w.vehicles.tick_motor(veh(), t);
    }
};

EntityHandle add_second_control_occupant(Rig &r) {
    Seat driver;
    driver.type = SeatType::Driver;
    driver.bone_index = 2;
    driver.source_name = "drvrx00";
    r.veh().seats.push_back(driver);

    Entity occupant;
    occupant.kind = EntityKind::Organic;
    occupant.item_id = 5305;
    occupant.player_class = 8;
    occupant.health = 150;
    occupant.health_max = 150;
    occupant.alive = true;
    return r.w.registry.spawn(0, occupant);
}

// items.def physics-block scaling — parse the real buggy block and pin the pre-scaled
// fields [orig: ItemDef_ParsePhysicsProperty @0x49d870 imuls].
void test_def_physics_scaling() {
    const char *src =
            "begin \"Drivable Dune Buggy\"\n"
            "  id 101291\n"
            "  type vehicle\n"
            "  attrib: AIData noscar neutral PlayerControl forceasset DynamicShadow\n"
            "  hp 3000\n"
            "  criticalhp 300\n"
            "  criticaldrain 15\n"
            "  ai_function cveh\n"
            "  move_function cveh\n"
            "    turn_rate 65\n"
            "\tturn_rate2 41\n"
            "    acceleration 15\n"
            "    deceleration 70\n"
            "    slip_speed 0\n"
            "    player_speed 94\n"
            "    slip_slope\t50\n"
            "    max_slope\t60\n"
            "    physics     1\n"
            "    torque 3\n"
            "end\n";
    DefItemsFile file;
    std::memset(&file, 0, sizeof(file));
    CHECK(def_parse_items_memory(reinterpret_cast<const uint8_t *>(src), std::strlen(src),
                                 &file) == 0);
    CHECK(file.count == 1);
    if (file.count == 1) {
        const DefItemDef &d = file.entries[0];
        CHECK(d.physics == 1);
        CHECK(d.torque == 3); // raw shift count [orig: @0x49dcca]
        CHECK(d.turn_rate == 65 * 192426);
        CHECK(d.turn_rate2 == 41 * 192426);
        CHECK(d.acceleration == 15 * 4);
        CHECK(d.deceleration == 70 * 4);
        CHECK(d.player_speed == 94 * 293);
        CHECK(d.slip_speed == 0);
        CHECK(d.max_slope == 60 * 11930464);
        CHECK(d.slip_slope == 50 * 11930464);
        CHECK(d.critical_hp == 300);
        CHECK(d.critical_drain == 15);
        CHECK((d.attrib & 0x40u) != 0); // PlayerControl
    }
    def_free_items(&file);
}

// deceleration defaults to 2*acceleration when only acceleration is authored
// [orig: @0x49da4b].
void test_def_decel_default() {
    const char *src =
            "begin \"accel only\"\n  id 100001\n  acceleration 10\nend\n"
            "begin \"decel first\"\n  id 100002\n  deceleration 5\n  acceleration 10\nend\n";
    DefItemsFile file;
    std::memset(&file, 0, sizeof(file));
    CHECK(def_parse_items_memory(reinterpret_cast<const uint8_t *>(src), std::strlen(src),
                                 &file) == 0);
    CHECK(file.count == 2);
    if (file.count == 2) {
        CHECK(file.entries[0].acceleration == 40);
        CHECK(file.entries[0].deceleration == 80); // 2x default
        CHECK(file.entries[1].deceleration == 20); // authored value wins
    }
    def_free_items(&file);
}

// The cveh render callback publishes the high steer word and saturated wrapped
// speed magnitude. Pin the word orientation and every signed boundary,
// especially INT_MIN where std::abs(int32_t) would be undefined.
// [orig: Entity_CacheVehicleHUDStats @0x4929B0;
//  steering @0x4929C0..0x4929D7; speed @0x4929DC..0x4929F1]
void test_ctrl_register_projection() {
    Entity::VehicleMotorState state;

    state.steer_state = static_cast<int32_t>(0x1234ABCDu);
    state.speed = 0x1234;
    VehicleCtrlRegisters ctrl = vehicle_ctrl_registers(state);
    CHECK(ctrl.steering == 0x1234);
    CHECK(ctrl.speed == 0x1234);

    state.steer_state = static_cast<int32_t>(0xFEDC0001u);
    state.speed = -0x1234;
    ctrl = vehicle_ctrl_registers(state);
    CHECK(ctrl.steering == 0xFEDC);
    CHECK(ctrl.speed == 0x1234);

    state.speed = 0x10000;
    CHECK(vehicle_ctrl_registers(state).speed == 0x10000);
    state.speed = -0x10000;
    CHECK(vehicle_ctrl_registers(state).speed == 0x10000);
    state.speed = 0x10001;
    CHECK(vehicle_ctrl_registers(state).speed == 0x10000);
    state.speed = -0x10001;
    CHECK(vehicle_ctrl_registers(state).speed == 0x10000);
    state.speed = std::numeric_limits<int32_t>::max();
    CHECK(vehicle_ctrl_registers(state).speed == 0x10000);
    state.speed = std::numeric_limits<int32_t>::min();
    CHECK(vehicle_ctrl_registers(state).speed == 0x10000);
}

// Forward drive: moving bit + dir 0 ramps speed by the accel clamp toward
// player_speed and advances the position along the heading.
// Boarding clears the player's impact-scar ring [orig: Entity_AttachCarriedObject
// @0x43c155 -> Scar_ClearEntriesByEntity @0x5ccec0].
void test_boarding_clears_the_scar_ring() {
    Rig r;
    // A scar ring leases before the mount and is found; boarding releases it.
    CHECK(r.w.out.scars.ring_for(r.drv_h, 1) != nullptr);
    CHECK(r.w.out.scars.find(r.drv_h) != nullptr);
    r.mount();
    CHECK(r.w.out.scars.find(r.drv_h) == nullptr);
}

void test_drive_forward() {
    Rig r;
    const VehicleTraits t = buggy_traits();
    r.mount();
    r.drv().net_move_input = 0x08; // moving, dir 0 (forward)
    r.drv().yaw = 0;               // steer target = vehicle heading

    const Vec3 start = r.veh().position;
    r.tick(1, t);
    // Launch from standstill rides the UNCLAMPED 1/32 chase (the target>0 && speed<=0
    // branch skips both clamps): (27542 - 0 + 16) >> 5 = 861 on the first tick
    // [orig: the @0x48bb46 branch tree — direction changes keep the raw step].
    CHECK(r.veh().veh.speed == 861);
    r.tick(9, t);
    // Once same-direction, the accel clamp holds +60/tick [orig: ±itemDef->acceleration].
    CHECK(r.veh().veh.speed == 861 + 60 * 9);
    r.tick(52, t);
    CHECK(r.veh().veh.speed > 3000);
    CHECK(r.veh().veh.speed <= t.player_speed);
    const float dx = r.veh().position.x - start.x;
    const float dy = r.veh().position.y - start.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    CHECK(dist > 1.0f);   // it MOVED
    CHECK(r.veh().yaw == 0); // straight line: heading holds
    CHECK(std::fabs(r.veh().position.z - start.z) < 0.01f); // flat-plane ground hold
}

// Reverse: dir 4 commands -player_speed/2 -> speed goes negative.
void test_reverse() {
    Rig r;
    const VehicleTraits t = buggy_traits();
    r.mount();
    r.drv().net_move_input = 0x08 | 4; // moving, dir 4 (back)
    r.drv().yaw = 0;
    r.tick(40, t);
    CHECK(r.veh().veh.speed < 0);
    CHECK(r.veh().veh.speed >= -(t.player_speed / 2) - 64);
}

// Turn in place (dir 2, strafe-left key): commanded speed is zero and the wheel-rate
// steering is speed-proportional, so a standing buggy does NOT rotate
// [orig: @0x48b51c case 2 `[136] = 0`; yaw rate = -speed * wheel @0x48ba33].
void test_turn_in_place_holds() {
    Rig r;
    const VehicleTraits t = buggy_traits();
    r.mount();
    r.drv().net_move_input = 0x08 | 2;
    r.drv().yaw = 0;
    r.tick(60, t);
    CHECK(r.veh().veh.speed == 0);
    CHECK(r.veh().yaw == 0);
}

// Mouse steer: the steer target is the DRIVER's replicated yaw; while driving the
// vehicle heading chases it [orig: @0x48b4c0 `[132] = v61->Yaw`; the chase @0x48b9e9].
void test_steer_toward_driver_yaw() {
    Rig r;
    const VehicleTraits t = buggy_traits();
    r.mount();
    r.drv().net_move_input = 0x08; // forward
    r.drv().yaw = 40;              // want: vehicle turns toward mission yaw 40
    r.tick(220, t);
    const int yaw = r.veh().yaw;
    CHECK(yaw > 15);  // turned substantially toward the target...
    CHECK(yaw < 70);  // ...on the correct side (no runaway/oscillation)
}

// Signed axes may cancel in the freelook latch while still commanding pedal
// steering. Preserve the driver's full BAM view and its local look alias.
// [orig: Entity_UpdateVehiclePhysics @0x48B7AA..0x48B7D9]
void test_analog_driver_view_turn_and_freelook() {
	for (const VehicleFamily family :
			{ VehicleFamily::Ground, VehicleFamily::Bike, VehicleFamily::Watercraft }) {
		for (int mode = 0; mode < 3; ++mode) {
			Rig r;
			auto t = buggy_traits();
			t.family = family;
			t.water_speed = t.player_speed;
			r.mount();
			auto &body = *r.w.ai.at(r.w.ai.attach(r.drv_h));
			body.heading = body.inf.target_heading = 0;
			r.drv().net_analog_x = mode == 2 ? 0 : -64;
			r.drv().net_analog_z = 64;
			r.drv().net_move_input = mode == 1 ? Entity::kMoveOrderFreeLook : 0;
			r.tick(1, t);
			const int32_t expected = mode == 0 ? -(192426 * 64 / 2) : 0;
			CHECK(body.heading == expected);
			CHECK(body.inf.target_heading == expected);
			CHECK((r.drv().net_move_input & Entity::kMoveOrderFreeLook) != 0 || mode == 0);
			if (mode == 0)
				CHECK(r.veh().veh.steer_target_bam == expected);
		}
	}
}

// Ground's crash/settle bytes hold yaw rate; the bike only tests airborne.
// [orig: ground Entity_UpdateVehiclePhysics @0x48AF00 (site @0x48C146..0x48C18F:
//  `test Flags, 0x2000; cmp +0x2EC, 0; cmp +0x2F0, 0` before the +0xA4 write);
//  Entity_UpdateLightVehiclePhysics @0x483FE0]
void test_ground_crash_and_settle_hold_yaw_rate() {
	for (const VehicleFamily family : { VehicleFamily::Ground, VehicleFamily::Bike }) {
		for (int gate = 0; gate < 2; ++gate) {
			Rig r;
			auto t = buggy_traits();
			t.family = family;
			auto &m = r.veh().veh;
			m.speed = 1000;
			m.steer_state = 1900;
			m.wheel_rate_bam = 123456;
			m.crashed = gate == 0;
			m.settle_2f0 = gate == 1;
			r.tick(1, t);
			CHECK((m.wheel_rate_bam == 123456) == (family == VehicleFamily::Ground));
		}
	}
}

// No controller: commanded speed zeroes and the vehicle coasts to a stop through the
// decel clamps [orig: @0x48b2ec no-occupant leg].
void test_no_driver_coasts() {
    Rig r;
    const VehicleTraits t = buggy_traits();
    r.mount();
    r.drv().net_move_input = 0x08;
    r.drv().yaw = 0;
    r.tick(30, t);
    CHECK(r.veh().veh.speed > 0);
    r.w.vehicles.detach(r.drv_h);
    // The 1/32 exponential decay reaches the |speed| < 48 deadzone in ~130 ticks from
    // ~2.6k [orig: @0x48bbf7 the zero-target deadzone].
    r.tick(200, t);
    CHECK(r.veh().veh.speed == 0);
}

// A stale Controller/Driver handle is also a control-stop edge. The occupant may already
// be partially torn down, so the vehicle's own identity must carry the host notification.
void test_stale_driver_publishes_control_stop() {
    Rig r;
    const VehicleTraits t = buggy_traits();
    r.mount();
    r.w.out.effects.clear();
    r.w.registry.despawn(r.drv_h); // the vehicle-side seat handle now resolves to nothing

    r.tick(1, t);
    CHECK(!r.veh().seats[0].occupant.valid());
    CHECK(r.w.out.effects.entries().size() == 1);
    if (r.w.out.effects.entries().size() == 1) {
        const auto &stopped = r.w.out.effects.entries()[0];
        CHECK(stopped.kind == "vehicle_control_stopped");
        CHECK(stopped.a == 200);
        CHECK(stopped.b == 77);
        CHECK(stopped.c == 0x01000003);
        CHECK(stopped.d == r.veh_h.packed);
    }
    r.tick(1, t);
    CHECK(r.w.out.effects.entries().size() == 1); // cleared once, never repeated
}

// The +368 claimant protocol: the FIRST control occupant claims; losing THE claimant
// stops the engine effect even while a second, valid Controller/Driver occupant remains
// — the survivor does not inherit the claim (retail re-arms only on a fresh attach).
// [orig: Entity_AttachToVehicleSlot @0x4946d0 empty-or-same claim; Entity_DetachFromVehicle
// @0x4356e9 claimant-only stop leg]
void test_stale_claimant_stops_despite_second_controller() {
    Rig r;
    const VehicleTraits t = buggy_traits();
    const EntityHandle second = add_second_control_occupant(r);
    r.mount();
    CHECK(r.w.vehicles.process_attach(second, r.veh_h, 2));
    CHECK(r.w.out.effects.entries().size() == 1); // one started for the first claim only
    r.w.out.effects.clear();

    r.w.registry.despawn(r.drv_h); // the claimant goes away
    r.tick(1, t);
    CHECK(!r.veh().seats[0].occupant.valid());
    CHECK(r.veh().seats[1].occupant == second);
    CHECK(r.w.out.effects.entries().size() == 1);
    if (r.w.out.effects.entries().size() == 1)
        CHECK(r.w.out.effects.entries()[0].kind == "vehicle_control_stopped");
    CHECK(!r.veh().primary_occupant.valid());

    // The surviving second controller never re-claims in place...
    r.w.out.effects.clear();
    r.tick(1, t);
    CHECK(r.w.out.effects.entries().empty());

    // ...but a fresh attach does [orig: the +368 claim runs at attach time only].
    CHECK(r.w.vehicles.detach(second));
    r.w.out.effects.clear();
    CHECK(r.w.vehicles.process_attach(second, r.veh_h, 2));
    CHECK(r.w.out.effects.entries().size() == 1);
    if (r.w.out.effects.entries().size() == 1) {
        CHECK(r.w.out.effects.entries()[0].kind == "vehicle_control_started");
        CHECK(r.w.out.effects.entries()[0].d == r.veh_h.packed);
    }
}

// A non-claimant control occupant leaving publishes nothing; only the claimant's
// departure is the stop edge.
void test_second_controller_departure_is_silent() {
    Rig r;
    const VehicleTraits t = buggy_traits();
    const EntityHandle second = add_second_control_occupant(r);
    r.mount();
    CHECK(r.w.vehicles.process_attach(second, r.veh_h, 2));
    r.w.out.effects.clear();

    CHECK(r.w.vehicles.detach(second));
    r.tick(1, t);
    CHECK(r.w.out.effects.entries().empty());
    CHECK(r.veh().primary_occupant == r.drv_h);
}

// A gun user on an EMPTY vehicle claims +368 and starts the engine effect; the pilot
// arriving later does not re-claim, so the GUNNER leaving is the stop edge even while
// the pilot keeps flying. [orig: the UseGun empty-only claim @0x494944..0x49495e; the
// claimant-only stop leg @0x4356e9]
void test_gunner_first_claims_and_stops() {
    Rig r;
    const VehicleTraits t = buggy_traits();
    Seat gun;
    gun.type = SeatType::Gunner;
    gun.bone_index = 3;
    gun.source_name = "UseGun";
    r.veh().seats.push_back(gun);

    Entity gunner_e;
    gunner_e.kind = EntityKind::Organic;
    gunner_e.item_id = 5305;
    gunner_e.player_class = 8;
    gunner_e.health = 150;
    gunner_e.health_max = 150;
    gunner_e.alive = true;
    const EntityHandle gunner = r.w.registry.spawn(0, gunner_e);

    CHECK(r.w.vehicles.process_attach(gunner, r.veh_h, 3));
    CHECK(r.veh().primary_occupant == gunner);
    CHECK(r.w.out.effects.entries().size() == 1);
    if (r.w.out.effects.entries().size() == 1)
        CHECK(r.w.out.effects.entries()[0].kind == "vehicle_control_started");
    r.w.out.effects.clear();

    r.mount(); // the driver arrives second: no re-claim, no event
    CHECK(r.veh().primary_occupant == gunner);
    CHECK(r.w.out.effects.entries().empty());

    CHECK(r.w.vehicles.detach(gunner)); // the claimant leaves
    CHECK(r.w.out.effects.entries().size() == 1);
    if (r.w.out.effects.entries().size() == 1) {
        CHECK(r.w.out.effects.entries()[0].kind == "vehicle_control_stopped");
        CHECK(r.w.out.effects.entries()[0].d == r.veh_h.packed);
    }
    CHECK(!r.veh().primary_occupant.valid());
    CHECK(r.veh().seats[0].occupant == r.drv_h); // the driver still flies
}

// Several control slots can go stale in one pass; the claimant edge still publishes
// exactly one stop.
void test_multiple_stale_controls_publish_one_stop() {
    Rig r;
    const VehicleTraits t = buggy_traits();
    const EntityHandle second = add_second_control_occupant(r);
    r.mount();
    CHECK(r.w.vehicles.process_attach(second, r.veh_h, 2));
    r.w.out.effects.clear();

    r.w.registry.despawn(r.drv_h);
    r.w.registry.despawn(second);
    r.tick(1, t);
    CHECK(!r.veh().seats[0].occupant.valid());
    CHECK(!r.veh().seats[1].occupant.valid());
    CHECK(r.w.out.effects.entries().size() == 1);
    if (r.w.out.effects.entries().size() == 1)
        CHECK(r.w.out.effects.entries()[0].kind == "vehicle_control_stopped");
    r.tick(1, t);
    CHECK(r.w.out.effects.entries().size() == 1);
}

// A dead vehicle stops responding to the driver [orig: the wreck/dead gates
// @0x48af61 + the 0x10000002 occupant-block mask].
void test_dead_vehicle_holds() {
    Rig r;
    const VehicleTraits t = buggy_traits();
    r.mount();
    r.drv().net_move_input = 0x08;
    r.drv().yaw = 0;
    r.veh().health = 0;
    r.veh().flags |= 2u;
    r.tick(40, t);
    CHECK(r.veh().veh.speed == 0);
}

// Selector zero runs the simpler motor. [orig: Entity_DispatchPhysics_cveh @0x48EFC0]
void test_physics_selector_gate() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.physics = 0;
    r.mount();
    r.drv().net_move_input = 0x08;
    r.tick(20, t);
	CHECK(r.veh().veh.speed > 0);
	const Vec3 p = r.veh().position;
	CHECK(p.x != 100.0f || p.y != 200.0f);
}

// Numeric witnesses for the separate selector-zero callbacks and suspension.
// [orig: Entity_ProcessInfantryPhysics @0x46E100;
// Entity_ProcessAirVehiclePhysics @0x46FA00; Entity_ProcessVehicleSuspension @0x463C60]
// A zero playerSpeed pins the steering blend fraction at 0 in every ground
// family, so the effective rate is minRate, not turnRate [orig:
// Entity_UpdateVehiclePhysics @0x48AF00 (site @0x48C0A9..0x48C0D8 `cmp eax, edx;
// jz -> xor ecx, ecx`); the cbik twin @0x485283..0x4852B0]. Hand trace:
//   minRate = turn_rate2 = 41 * 192426 = 7889466
//   eff     = minRate + ((turnRate - minRate) * 0 + 0x8000) >> 16 = 7889466
//   delta   = min((0x40000000 - 0 + 32) >> 6 = 16777216, eff) = 7889466
//   steer   = (4 - 32 * 7889466 - 0) >> 3 = -252462908 >> 3 = -31557864
// (the retracted full-rate default gave eff = 12507690, steer = -50030760).
void test_zero_player_speed_turns_at_min_rate() {
	for (const VehicleFamily family : { VehicleFamily::Ground, VehicleFamily::Bike }) {
		World world;
		VehicleTraits t;
		t.physics = 1;
		t.family = family;
		t.player_speed = 0;
		t.turn_rate = 65 * 192426;
		t.turn_rate2 = 41 * 192426;
		Entity e;
		e.health = e.health_max = 10000;
		e.veh.yaw_seeded = true;
		e.veh.yaw_bam = 0;
		e.veh.steer_target_bam = 0x40000000;
		e.position.z = 10.0f;
		world.vehicles.tick_motor(e, t);
		CHECK(e.veh.steer_state == -31557864);
	}
}

// The grazing correction multiplies penetration by the -2r gradient with the
// two-operand IMUL — a 32-bit low dword — before the signed divide [orig:
// Entity_CheckCollisionState @0x462A30 (site @0x462C5F `imul eax, ebx; cdq;
// idiv ebp` and @0x462C77 for Y); Entity_ComputeCollisionForces @0x462150
// (site @0x4623BF)]. A 45-degree ramp h = x (heightmap[z][x] = x * 256) with a
// wheel probe at X = 10 u, Y = -5 u (terrain z = +5 u), r = 2 u samples exactly
// 8/12/10/10 u, so, hand-run:
//   rel  = Z - 4 * avg = 768042 - 655360 = 112682
//   gx = 262144, gy = 0, gz = -262144, norm = int(sqrt(2) * 262144) = 370727
//   fx = 262144*131072/370727 = 92682 (truncated), fy = 0
//   pen = rel + (-34359738368 / 370727 = -92682) = 20000 (terrain_gap 20000)
//   pen * gz = -5242880000 -> low dword -947912704 -> / gx = -3616
//     (the wide product would give -20000)
//   |-3616| < |92682| -> fx = 92682 - 3616 = 89066, hit, pen = 0
//   mag = int(sqrt(89066^2 + 112682^2)) = 143631
//   cosr = (112682 << 22) / 143631 = 3290533 < soft (0x400000) -> severity 3:
//   out.fx -= 89066, out.fy -= 0, out.fz -= 0.
void test_plat_terrain_probe_grazing_uses_wrapped_product() {
	World world;
	std::vector<uint16_t> heightmap(64 * 64);
	for (int z = 0; z < 64; ++z)
		for (int x = 0; x < 64; ++x)
			heightmap[z * 64 + x] = uint16_t(x * 256);
	std::vector<int> sector_grid(256, 1);
	opennova::terrain::TerrainHeightField field{};
	field.heightmap = heightmap.data();
	field.dim = 64;
	field.layout.sector_grid = sector_grid.data();
	field.layout.origin_x = 0;
	field.layout.origin_y = 0;
	world.tables.terrain = &field;
	detail::PlatProbeForce out{};
	const int32_t severity = detail::plat_terrain_probe(world, 10 << 16, -(5 << 16), 768042,
			2 << 16, 0x400000, 0x400000, out, /*wheel_probe=*/true);
	CHECK(severity == 3);
	CHECK(out.terrain_gap == 20000);
	CHECK(out.fx == -89066);
	CHECK(out.fy == 0);
	CHECK(out.fz == 0);
}

void test_simple_motors_and_contact() {
	World world;
	VehicleTraits t;
	t.acceleration = 1;
	t.deceleration = 1;
	t.player_speed = 32000;
	Entity e;
	e.health = e.health_max = 10000;
	e.veh.yaw_seeded = true;
	e.veh.speed = 16000;
	e.veh.cmd_speed = -16000;
	e.position.z = 10.0f;
	world.vehicles.tick_motor(e, t);
	CHECK(e.veh.speed_accel == -1000);
	CHECK(e.veh.speed == 15000);
	CHECK(e.veh.slide_z == -167);
	CHECK(to_fixed(e.position.z) == 655360 - 167);
	CHECK(e.veh.wheel_phase == 15000 * 8192);

	// A lateral Y mismatch alone does not trip the retail duplicated-X test.
	e = Entity{};
	e.health = e.health_max = 10000;
	e.veh.yaw_seeded = true;
	e.veh.cmd_speed = e.veh.speed = 1024;
	e.veh.vel_x = 1024;
	e.veh.vel_y = 100000;
	t.slip_speed = 100;
	world.vehicles.tick_motor(e, t);
	CHECK(!e.veh.skid_sound_latched);
	CHECK(e.veh.vel_y == 0);
	e.veh.vel_x = 5000;
	world.vehicles.tick_motor(e, t);
	CHECK(e.veh.skid_sound_latched);
	CHECK(e.veh.vel_x > 1024);

	// On water, selector zero has its own heave wave and no vertical thrust.
	t = VehicleTraits{};
	t.family = VehicleFamily::Watercraft;
	t.acceleration = 64;
	t.water_speed = 16384;
	world.env.water_z = 100 * 65536;
	e = Entity{};
	e.health = e.health_max = 10000;
	e.veh.yaw_seeded = true;
	e.flags = 0x8000;
	e.position.z = 100.0f;
	e.veh.cmd_speed = 16384;
	Entity remote = e;
	remote.veh.net_predicted = true;
	remote.veh.net_interp_progress = 20;
	remote.veh.net_recv_speed = 16384;
	world.vehicles.tick_watercraft_motor(e, t);
	world.ai.is_authority = false;
	world.vehicles.watercraft_client_tick(remote, t);
	CHECK(e.veh.vel_x == 252);
	CHECK(e.veh.slide_z == -512);
	CHECK(e.veh.air_pitch_bam == 983040);
	CHECK(e.veh.air_roll_bam == 0);
	CHECK(e.veh.wheel_phase == 16384 * 8192);
	CHECK(remote.veh.vel_x == e.veh.vel_x);
	CHECK(remote.veh.slide_z == e.veh.slide_z);
	CHECK(remote.veh.air_pitch_bam == e.veh.air_pitch_bam);
	CHECK(remote.position.x == e.position.x && remote.position.z == e.position.z);

	// Nonzero phases pin the wave's x87 formula [orig: Entity_ProcessAirVehiclePhysics
	// @0x46FA00 (site @0x47115D..0x4711D9)]: P = (((x+y)>>10) + tick*4) * (1/256).
	// Case A: x = 2.0 u (131072 >> 10 = 128), tick 0 -> P = 0.5:
	//   pitch  = ftol(cos(0.7)*327680 + cos(0.5)*655360)
	//          = ftol(250623.488 + 575132.508 = 825755.9957) = 825755
	//   roll   = ftol(sin(0.4)*524288 + sin(0.6)*655360)
	//          = ftol(204167.364 + 370044.091 = 574211.4552) = 574211
	//            (the retracted `* 1.6` on the roll path gave 918738)
	//   height = -1024 - ftol(sin(0.8) * -1024) = -1024 - ftol(-734.5726) = -290
	//            (sin(P) instead of sin(1.6P) gave -534)
	//   slide_z = (water_z + height - z) >> 1 = -290 >> 1 = -145 (z == water_z).
	// Case B: x = 0, tick 24 (phase word 96) -> P = 0.375:
	//   pitch = ftol(283549.349 + 609817.475 = 893366.8243) = 893366
	//   roll  = ftol(154937.698 + 285059.012 = 439996.7105) = 439996
	//   height = -1024 - ftol(-578.1939) = -446 -> slide_z = -223.
	// Every truncated value sits >= 0.004 from an integer, beyond any libm ulp.
	{
		Entity wave;
		wave.health = wave.health_max = 10000;
		wave.veh.yaw_seeded = true;
		wave.flags = 0x8000;
		wave.position = { 2.0f, 0.0f, 100.0f };
		world.logic_tick = 0;
		world.vehicles.tick_watercraft_motor(wave, t);
		CHECK(wave.veh.air_pitch_bam == 825755);
		CHECK(wave.veh.air_roll_bam == 574211);
		CHECK(wave.veh.slide_z == -145);

		Entity wave_b;
		wave_b.health = wave_b.health_max = 10000;
		wave_b.veh.yaw_seeded = true;
		wave_b.flags = 0x8000;
		wave_b.position = { 0.0f, 0.0f, 100.0f };
		world.logic_tick = 24;
		world.vehicles.tick_watercraft_motor(wave_b, t);
		CHECK(wave_b.veh.air_pitch_bam == 893366);
		CHECK(wave_b.veh.air_roll_bam == 439996);
		CHECK(wave_b.veh.slide_z == -223);
		world.logic_tick = 0;
	}

	// catv selects that boat path before its input/health/sound families.
	t.family = VehicleFamily::Ground;
	t.amphibian = true;
	Entity amphibian;
	amphibian.health = amphibian.health_max = 10000;
	amphibian.flags = 0x8000;
	amphibian.position.z = 100.0f;
	amphibian.veh.yaw_seeded = true;
	amphibian.veh.cmd_speed = 16384;
	world.ai.is_authority = true;
	world.vehicles.tick_motor(amphibian, t);
	CHECK(amphibian.health == 10000);
	CHECK(amphibian.veh.vel_x == 252);
	CHECK(amphibian.veh.air_pitch_bam == 983040);

	// Simple suspension sleeps without the physical motor's occupant/energy gate.
	t.box_y_lo = -65536;
	t.box_y_hi = 65536;
	t.box_x_lo = t.foot_x_lo = -131072;
	t.box_x_hi = t.foot_x_hi = 131072;
	t.foot_y_lo = -65536;
	t.foot_y_hi = 65536;
	t.box_z_lo = -32768;
	t.box_z_hi = 65536;
	e = Entity{};
	e.veh.slide_z = -167;
	e.veh.spring_energy = 9000;
	int32_t x = 0, y = 0, z = 655360 - 167;
	detail::vehicle_simple_contact(world, e, t, x, y, z);
	CHECK(z == 655360 && e.veh.slide_z == -84);
	e.flags = 0x40;
	e.veh.slide_z = -167;
	world.env.water_z = 0;
	z = 655360 - 167;
	detail::vehicle_simple_contact(world, e, t, x, y, z);
	CHECK((e.flags & 0x40u) == 0 && (e.flags & kEntityFlagInAir) != 0);
	CHECK(e.veh.plat_airborne_ticks == 1 && z == 655360 - 167);

	// Water support clears airborne and advances the actual four-spring bank.
	world.env.water_z = 655360;
	t.spring = 4;
	t.shock = 4;
	e.flags = kEntityFlagInAir;
	e.veh.slide_z = -100;
	z = 655360 - 4096;
	detail::vehicle_simple_contact(world, e, t, x, y, z);
	CHECK((e.flags & 0x8000u) != 0 && (e.flags & kEntityFlagInAir) == 0);
	CHECK(e.veh.wheel_comp[0] > 0);
	CHECK(e.veh.wheel_comp[0] == e.veh.wheel_comp[1]);
	CHECK(e.veh.plat_airborne_ticks == 0);
	CHECK(world.out.vehicle_effects.size() == 1);
	CHECK(world.out.vehicle_effects[0].effect == "Effect_SmlSplash");
	CHECK(world.out.vehicle_effects[0].position.z == 10.0f);
	CHECK(world.out.water_crossings.events.size() == 1);
	detail::vehicle_simple_contact(world, e, t, x, y, z);
	CHECK(world.out.vehicle_effects.size() == 1); // latched while submerged
	CHECK(world.out.water_crossings.events.size() == 1);
}

// An occupied PlayerControl vehicle continuously registers its stationary idle
// emitter. Occupancy is claimant-based, not local-player-based: the controller here
// is an NPC taking the AI command leg. The event is the portable sound-emitter seam
// consumed by every host.
// [orig: Entity_ProcessMovementSoundEffects @0x5294a0; emitter param block
// @0x529270; SoundEmitter_RegisterSetLayers @0x528340]
void test_npc_claimant_registers_stationary_idle_sound() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.sound_profile = "SP_Transport";
    load_transport_sound_profile(r.w);
    r.drv().player_class = 0; // an NPC, not the local/remote player body path
    r.mount();
    r.w.out.sound_emitters.clear(); // ignore mount-edge effects; inspect the motor tick

    VehicleDriveCmd parked;
    parked.ai_drive = true;
    parked.steer_target_bam = 0;
    parked.cmd_speed = 0;
    r.w.vehicles.tick_motor(r.veh(), t, &parked);

    CHECK(r.w.out.sound_emitters.size() == 1);
    if (r.w.out.sound_emitters.size() == 1) {
        const SoundEmitterEvent &idle = r.w.out.sound_emitters[0];
        CHECK(idle.source_spawn_id == r.veh().registry_spawn_id);
        CHECK(idle.source_handle == r.veh_h.packed);
        CHECK(idle.pos.x == 100.0f);
        CHECK(idle.pos.y == 200.0f);
        CHECK(idle.pos.z == 10.0f);
        CHECK(idle.source_bms_id == 77);
        CHECK(idle.emitted_tick == 1);
        CHECK(idle.lane == 0);
        CHECK(idle.lifetime_ticks == 30);
        CHECK(idle.pitch_q16 == 0x10000);
        CHECK(idle.volume_q8_8 == 0xFFFF);
        CHECK(idle.slot == 0); // Soundloop_1
        CHECK(idle.set_name == "V_TRUCK_ILP");
    }
}

// +368 is the engine-running claimant. A valid second Controller/Driver does not
// inherit it when the claimant leaves, so the surviving seat occupant must not keep
// a vehicle loop alive until a fresh attach claims the vehicle.
// [orig: Entity_DetachFromVehicle @0x4356e9 claimant-only stop/clear leg]
void test_controller_without_claimant_is_silent() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.sound_profile = "SP_Transport";
    load_transport_sound_profile(r.w);
    const EntityHandle second = add_second_control_occupant(r);
    r.mount();
    CHECK(r.w.vehicles.process_attach(second, r.veh_h, 2));
    CHECK(r.w.vehicles.detach(r.drv_h));
    CHECK(!r.veh().primary_occupant.valid());
    CHECK(r.veh().seats[1].occupant == second);

    r.w.out.sound_emitters.clear();
    r.w.vehicles.tick_motor(r.veh(), t);
    CHECK(r.w.out.sound_emitters.empty());
}

// ItemDef_ResolveAllResources copies the profile slots, then a non-empty per-item
// soundloop_N name re-resolves over the copied value. Keep that authoring precedence
// at the portable vehicle-traits boundary.
// [orig: ItemDef_ResolveAllResources @0x49e5f0/@0x49e7f0]
void test_item_soundloop_override_wins_over_profile() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.sound_profile = "SP_Transport";
    t.sound_loops[0] = "V_TRUCK_CUSTOM_IDLE";
    load_transport_sound_profile(r.w);
    r.drv().player_class = 0;
    r.mount();
    r.w.out.sound_emitters.clear();

    VehicleDriveCmd parked;
    parked.ai_drive = true;
    r.w.vehicles.tick_motor(r.veh(), t, &parked);

    CHECK(r.w.out.sound_emitters.size() == 1);
    if (r.w.out.sound_emitters.size() == 1) {
        CHECK(r.w.out.sound_emitters[0].slot == 0);
        CHECK(r.w.out.sound_emitters[0].set_name == "V_TRUCK_CUSTOM_IDLE");
    }
}

// Moving and idle remain independent lanes in the shared table. The forward
// lane ramps across the first 1/16 of playerSpeed; pitch interpolates from the
// authored p2/p3 pair, while idle fades over the full speed range.
// [orig: @0x5295e6..0x529619, @0x52971a..0x529882]
void test_forward_sound_gain_pitch_and_idle_crossfade() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.player_speed = 32000;
    t.sound_profile = "SP_Transport";
    load_transport_sound_profile(r.w);
    r.drv().player_class = 0;
    r.mount();
    r.w.out.sound_emitters.clear();
    r.veh().veh.speed = 1000; // playerSpeed / 32

    r.w.vehicles.update_ground_sound(r.veh(), t, false, false);

    CHECK(r.w.out.sound_emitters.size() == 2);
    if (r.w.out.sound_emitters.size() == 2) {
        const SoundEmitterEvent &drive = r.w.out.sound_emitters[0];
        const SoundEmitterEvent &idle = r.w.out.sound_emitters[1];
        CHECK(drive.lane == 10);
        CHECK(drive.slot == 1);
        CHECK(drive.set_name == "V_TRUCK_DLP");
        CHECK(drive.volume_q8_8 == 0x8000);
        CHECK(drive.pitch_q16 == 46694); // .7 + 1/32 * (1.1 - .7), Q16 rounded
        CHECK(idle.lane == 0);
        CHECK(idle.volume_q8_8 == 0xF800);
        CHECK(idle.pitch_q16 == 0x10000);
    }
}

// A REMOTE (non-authority) vehicle drives the same witnessed fold: the client
// pass reaches update_ground_vehicle_sound for every trait-ful pool-1 row, and
// it reads `veh.speed` — which the joiner bridge restores from the wire's
// prediction registers. This pins the moving-remote case the D-SND-17 row was
// still carrying as open: identical inputs must produce the identical lane-10
// emitter whether the speed came from the authority motor or the wire.
void test_remote_speed_drives_the_same_movement_sound() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.player_speed = 32000;
    t.sound_profile = "SP_Transport";
    load_transport_sound_profile(r.w);
    r.drv().player_class = 0;
    r.mount();
    r.w.out.sound_emitters.clear();
    // The wire-restored register — no local driver, no authority integration.
    r.veh().veh.speed = 1000; // playerSpeed / 32, as above
    r.veh().veh.cmd_speed = 1000;

    r.w.vehicles.update_ground_sound(r.veh(), t, false, false);

    CHECK(r.w.out.sound_emitters.size() == 2);
    if (r.w.out.sound_emitters.size() == 2) {
        const SoundEmitterEvent &drive = r.w.out.sound_emitters[0];
        CHECK(drive.lane == 10);
        CHECK(drive.volume_q8_8 == 0x8000);
        CHECK(drive.pitch_q16 == 46694);   // the same witnessed pitch
    }
}

// A frozen row must fall silent on the motion lane. The joiner bridge clears
// the motor speed registers when a row goes wire-frozen (dead pose / carried /
// state bit0), because the client pass still runs the movement-sound tail for
// it — a row frozen mid-motion would otherwise keep its moving lane alive on
// state nothing advances any more.
void test_zero_speed_row_emits_only_the_idle_lane() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.player_speed = 32000;
    t.sound_profile = "SP_Transport";
    load_transport_sound_profile(r.w);
    r.drv().player_class = 0;
    r.mount();
    r.w.out.sound_emitters.clear();
    r.veh().veh.speed = 0;
    r.veh().veh.cmd_speed = 0;

    r.w.vehicles.update_ground_sound(r.veh(), t, false, false);

    // Stationary: the idle lane at full volume, no motion lane.
    bool idle_full = false;
    for (size_t i = 0; i < r.w.out.sound_emitters.size(); ++i) {
        const SoundEmitterEvent &e = r.w.out.sound_emitters[i];
        CHECK(e.lane != 10);
        if (e.lane == 0 && e.volume_q8_8 == 0xFFFF) idle_full = true;
    }
    CHECK(idle_full);
}

// The p4>1 forward profile subdivides the speed range into repeating gear
// ramps. Keep the sequential low/high quarter-drop arithmetic byte-faithful.
// [orig: @0x52968f..0x529765]
void test_forward_sound_gear_pitch_sawtooth() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.player_speed = 32000;
    t.sound_profile = "SP_Geared";
    static constexpr char kGearedProfile[] =
            "begin \"SP_Geared\"\n"
            "  Soundloop_1 V_IDLE 1 1\n"
            "  Soundloop_2 V_DRIVE .8 1.2 4\n"
            "end\n";
    CHECK(r.w.tables.sound_profiles.parse(kGearedProfile,
                                   sizeof(kGearedProfile) - 1) == 1);
    r.drv().player_class = 0;
    r.mount();
    r.w.out.sound_emitters.clear();
    r.veh().veh.speed = 10000; // gear 1, one quarter through its 8000 band

    r.w.vehicles.update_ground_sound(r.veh(), t, false, false);

    CHECK(!r.w.out.sound_emitters.empty());
    if (!r.w.out.sound_emitters.empty()) {
        CHECK(r.w.out.sound_emitters[0].lane == 10);
        CHECK(r.w.out.sound_emitters[0].pitch_q16 == 55757);
    }
}

// Reverse motion selects lane 20, and the caller-side direction latch plays
// profile slot 32 exactly on each command-direction edge.
// [orig: lane branch @0x529787; latch @0x48d1d1..0x48d222]
void test_reverse_sound_and_direction_shift_edges() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.player_speed = 32000;
    t.sound_profile = "SP_Transport";
    load_transport_sound_profile(r.w);
    r.drv().player_class = 0;
    r.mount();
    r.w.out.sound_emitters.clear();
    r.w.out.slot_sounds.clear();
    r.veh().veh.speed = -16000;
    r.veh().veh.cmd_speed = -32000;

    r.w.vehicles.update_ground_sound(r.veh(), t, false, false);

    CHECK(r.w.out.sound_emitters.size() == 2);
    if (r.w.out.sound_emitters.size() == 2) {
        CHECK(r.w.out.sound_emitters[0].lane == 20);
        CHECK(r.w.out.sound_emitters[0].set_name == "V_TRUCK_REVSE");
        CHECK(r.w.out.sound_emitters[0].volume_q8_8 == 0xFFFF);
        CHECK(r.w.out.sound_emitters[0].pitch_q16 == 0x10000);
        CHECK(r.w.out.sound_emitters[1].lane == 0);
        CHECK(r.w.out.sound_emitters[1].volume_q8_8 == 0x8000);
    }
    CHECK(r.veh().veh.reverse_sound_latched);
    CHECK(r.w.out.slot_sounds.size() == 1);
    if (r.w.out.slot_sounds.size() == 1) {
        CHECK(r.w.out.slot_sounds[0].slot == 32);
        CHECK(std::strcmp(r.w.out.slot_sounds[0].set_name, "V_TRUCK_SHIFT") == 0);
    }

    r.w.out.slot_sounds.clear();
    r.w.vehicles.update_ground_sound(r.veh(), t, false, false);
    CHECK(r.w.out.slot_sounds.empty());
    r.veh().veh.cmd_speed = 32000;
    r.w.vehicles.update_ground_sound(r.veh(), t, false, false);
    CHECK(!r.veh().veh.reverse_sound_latched);
    CHECK(r.w.out.slot_sounds.size() == 1);
}

// The claimant detach edge immediately clears both moving lanes and plays the
// profile's engine-stop one-shot on the departing occupant, while its eye
// clears the water plane. Idle receives no refresh and retires through its
// existing 30-tick keep-alive; the direction and engine latches stay set.
// [orig: Entity_DetachFromVehicle @0x4356e9..0x43577c, stop on the occupant
//  @0x43571B..0x43573E]
void test_claimant_detach_clears_motion_lanes_and_plays_stop() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.sound_profile = "SP_Transport";
    load_transport_sound_profile(r.w);
    r.w.vehicles.traits.set(r.veh().item_id, t);
    r.mount();
    r.w.vehicles.update_ground_sound(r.veh(), t, false, false);
    r.veh().veh.reverse_sound_latched = true;
    r.veh().veh.engine_sound_latched = true;
    r.w.out.sound_emitters.clear();
    r.w.out.slot_sounds.clear();

    CHECK(r.w.vehicles.detach(r.drv_h));

    CHECK(r.w.out.sound_emitters.size() == 2);
    if (r.w.out.sound_emitters.size() == 2) {
        CHECK(r.w.out.sound_emitters[0].lane == 10);
        CHECK(r.w.out.sound_emitters[0].pitch_q16 == 0);
        CHECK(r.w.out.sound_emitters[0].volume_q8_8 == 0);
        CHECK(r.w.out.sound_emitters[1].lane == 20);
        CHECK(r.w.out.sound_emitters[1].pitch_q16 == 0);
        CHECK(r.w.out.sound_emitters[1].volume_q8_8 == 0);
    }
    CHECK(r.w.out.slot_sounds.size() == 1);
    if (r.w.out.slot_sounds.size() == 1) {
        CHECK(r.w.out.slot_sounds[0].slot == 31);
        CHECK(std::strcmp(r.w.out.slot_sounds[0].set_name, "V_TRUCK_STOP") == 0);
        CHECK(r.w.out.slot_sounds[0].source_handle == r.drv_h.packed);
        CHECK(r.w.out.slot_sounds[0].pos[0] == to_fixed(r.drv().position.x));
    }
    CHECK(r.veh().veh.reverse_sound_latched);
    CHECK(r.veh().veh.engine_sound_latched);

    // An occupant whose eye sits under the water plane leaves silently.
    {
        Rig u;
        load_transport_sound_profile(u.w);
        u.w.vehicles.traits.set(u.veh().item_id, t);
        u.mount();
        u.w.env.water_z = to_fixed(u.drv().position.z) + u.drv().eye_offset_z;
        u.w.out.slot_sounds.clear();
        CHECK(u.w.vehicles.detach(u.drv_h));
        CHECK(u.w.out.slot_sounds.empty());
    }

    // The idle lane was already drained into the host before detach. While its
    // keep-alive expires, later unoccupied motor ticks publish only the moved
    // entity anchor and do not renew any lane controls.
    r.w.out.sound_emitters.clear();
    r.veh().position.x += 5.0f;
    ++r.w.logic_tick;
    r.w.vehicles.update_ground_sound(r.veh(), t, false, false);
    CHECK(r.w.out.sound_emitters.size() == 1);
    if (r.w.out.sound_emitters.size() == 1) {
        const SoundEmitterEvent &anchor = r.w.out.sound_emitters[0];
        CHECK(anchor.source_only);
        CHECK(anchor.source_spawn_id == r.veh().registry_spawn_id);
        CHECK(anchor.pos.x == r.veh().position.x);
        CHECK(anchor.emitted_tick == r.w.logic_tick + 1);
        CHECK(anchor.pitch_q16 == 0);
        CHECK(anchor.volume_q8_8 == 0);
    }
}

// The PlayerControl claimant's detach also releases the +0x1CC smoke emitter;
// a claimant leaving a non-PlayerControl item keeps it.
// [orig: Entity_DetachFromVehicle @0x435746..0x435759]
void test_claimant_detach_releases_smoke_emitter() {
    for (const bool player_control : {true, false}) {
        Rig r;
        VehicleTraits t = buggy_traits();
        t.player_control = player_control;
        r.w.vehicles.traits.set(r.veh().item_id, t);
        r.mount();
        CHECK(r.veh().primary_occupant == r.drv_h);
        r.veh().veh.damage_smoke_active = true;
        r.w.out.destruction.effects.clear();
        CHECK(r.w.vehicles.detach(r.drv_h));
        int releases = 0;
        for (const DestructionEffectEvent &fx : r.w.out.destruction.effects)
            if (fx.family == 4 && fx.release && fx.attach_net_id == r.veh().net_id) ++releases;
        CHECK(releases == (player_control ? 1 : 0));
        CHECK(r.veh().veh.damage_smoke_active == !player_control);
    }
}

// The airborne-streak sound argument is distinct from wreck/all-zero: it explicitly clears reverse
// then forward, but still refreshes idle at full volume for this tick.
// [orig: collision branch @0x5297db..0x529882]
void test_hull_collision_clears_motion_lanes_and_forces_full_idle() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.player_speed = 32000;
    t.sound_profile = "SP_Transport";
    load_transport_sound_profile(r.w);
    r.w.vehicles.traits.set(r.veh().item_id, t);
    r.drv().player_class = 0;
    r.mount();

    r.veh().veh.speed = 1000;
	r.veh().veh.cmd_speed = 1000;
	r.veh().veh.plat_airborne_ticks = 31;
	r.w.out.sound_emitters.clear();
	r.w.vehicles.update_ground_sound(r.veh(), t, false, r.veh().veh.plat_airborne_ticks > 30);

	CHECK(r.w.out.sound_emitters.size() == 3);
	if (r.w.out.sound_emitters.size() == 3) {
		const SoundEmitterEvent &reverse_clear = r.w.out.sound_emitters[0];
        const SoundEmitterEvent &forward_clear = r.w.out.sound_emitters[1];
        const SoundEmitterEvent &idle = r.w.out.sound_emitters[2];
        CHECK(reverse_clear.lane == 20);
        CHECK(reverse_clear.pitch_q16 == 0);
        CHECK(reverse_clear.volume_q8_8 == 0);
        CHECK(forward_clear.lane == 10);
        CHECK(forward_clear.pitch_q16 == 0);
        CHECK(forward_clear.volume_q8_8 == 0);
        CHECK(idle.lane == 0);
        CHECK(idle.slot == 0);
        CHECK(idle.pitch_q16 == 0x10000);
        CHECK(idle.volume_q8_8 == 0xFFFF);
        CHECK(idle.set_name == "V_TRUCK_ILP");
	}
}

// Model contacts move the other vehicle on clients too; health is authority
// owned. The scrape latch also gates momentum, so a sustained contact cannot
// reapply the initial impulse every tick.
void test_vehicle_impact_role_and_momentum_edge() {
	for (bool authority : { false, true }) {
		Rig r;
		auto t = buggy_traits();
		t.player_control = false;
		t.mass = 30;
		t.box_x_lo = t.foot_x_lo = -2 * 65536;
		t.box_x_hi = t.foot_x_hi = 2 * 65536;
		t.box_y_lo = t.foot_y_lo = -65536;
		t.box_y_hi = t.foot_y_hi = 65536;
		t.box_z_lo = 0;
		t.box_z_hi = 2 * 65536;
		t.max_slope = 50 * 11930464;
		t.slip_slope = 35 * 11930464;
		r.w.vehicles.traits.set(r.veh().item_id, t);
		r.w.vehicles.traits.set(900, t);
		r.w.ai.is_authority = authority;
		CollisionWorld collision;
		r.w.ai.collision = &collision;
		Entity other;
		other.kind = EntityKind::Item;
		other.item_id = 900;
		other.item_type = 1;
		other.has_item_def = true;
		other.item_attrib = 0x40;
		other.position = { 150.0f, 200.0f, 10.0f };
		other.yaw = 90;
		other.bound_radius = 3.0f;
		const EntityHandle target = r.w.registry.spawn(1, other);
		collision.assign_entity(target, collision.add_model(vehicle_sound_test_wall()));
		r.veh().position = { 146.0f, 200.0f, 10.0f };
		r.veh().yaw = 90;
		r.veh().bound_radius = 3.0f;
		r.veh().health = 1000;
		r.veh().veh.speed = r.veh().veh.cmd_speed = 30000;
		for (int i = 0; i < 17; ++i)
			collision.build_tick_tables(r.w);
		stamp_saved_live_pose(r.veh());
		r.w.vehicles.tick_motor(r.veh(), t);
		CHECK(r.veh().veh.collision_sound_latched);
		CHECK(r.veh().health == (authority ? 984 : 1000));
		Entity &hit = *r.w.registry.get(target);
		CHECK(hit.veh.vel_x == 11250);
		CHECK(hit.position.x > 150.0f);
		const int32_t transferred = hit.veh.vel_x;
		r.veh().position = { 146.0f, 200.0f, 10.0f };
		r.veh().veh.speed = r.veh().veh.cmd_speed = 30000;
		r.veh().veh.grounded = true;
		r.veh().veh.air_pitch_bam = r.veh().veh.air_roll_bam = 0;
		r.veh().flags &= ~kEntityFlagInAir;
		stamp_saved_live_pose(r.veh());
		r.w.vehicles.tick_motor(r.veh(), t);
		CHECK(hit.veh.vel_x == transferred);
	}
}

// Claimant detach always clears keyed motion lanes, but slot 31 is strictly
// above-water. At the plane itself retail suppresses the one-shot.
// [orig: Entity_DetachFromVehicle @0x435716..0x43573e]
void test_claimant_detach_at_water_plane_clears_without_stop_oneshot() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.sound_profile = "SP_Transport";
    load_transport_sound_profile(r.w);
    r.w.vehicles.traits.set(r.veh().item_id, t);
    r.w.env.water_z = 10 << 16;
    r.mount();
    r.w.out.sound_emitters.clear();
    r.w.out.slot_sounds.clear();

    CHECK(r.w.vehicles.detach(r.drv_h));
    CHECK(r.w.out.sound_emitters.size() == 2);
    if (r.w.out.sound_emitters.size() == 2) {
        CHECK(r.w.out.sound_emitters[0].pitch_q16 == 0);
        CHECK(r.w.out.sound_emitters[0].volume_q8_8 == 0);
        CHECK(r.w.out.sound_emitters[1].pitch_q16 == 0);
        CHECK(r.w.out.sound_emitters[1].volume_q8_8 == 0);
    }
    CHECK(r.w.out.slot_sounds.empty());
}

// The portable producer accepts authored/modded int32 values. INT_MIN magnitudes,
// a maximal gear count, and an extreme p2/p3 span must be deterministic rather
// than invoking signed-overflow/abs(INT_MIN) undefined behavior.
void test_vehicle_sound_extreme_ints_are_saturating() {
    {
        Rig r;
        VehicleTraits t = buggy_traits();
        t.player_speed = std::numeric_limits<int32_t>::min();
        t.sound_profile = "SP_Transport";
        load_transport_sound_profile(r.w);
        r.mount();
        r.w.out.sound_emitters.clear();
        r.veh().veh.speed = std::numeric_limits<int32_t>::min();

        r.w.vehicles.update_ground_sound(r.veh(), t, false, false);

        CHECK(r.w.out.sound_emitters.size() == 1);
        if (r.w.out.sound_emitters.size() == 1) {
            CHECK(r.w.out.sound_emitters[0].lane == 20);
            CHECK(r.w.out.sound_emitters[0].pitch_q16 == 52428);
            CHECK(r.w.out.sound_emitters[0].volume_q8_8 == 0xFFFF);
        }
    }

    {
        Rig r;
        VehicleTraits t = buggy_traits();
        t.player_speed = std::numeric_limits<int32_t>::min();
        t.sound_profile = "SP_Extreme";
        static constexpr char kExtremeProfile[] =
                "begin \"SP_Extreme\"\n"
                "  Soundloop_1 V_IDLE 1 1\n"
                "  Soundloop_2 V_DRIVE .8 1.2 2147483647\n"
                "end\n";
        CHECK(r.w.tables.sound_profiles.parse(kExtremeProfile,
                                       sizeof(kExtremeProfile) - 1) == 1);
        r.mount();
        r.w.out.sound_emitters.clear();
        r.veh().veh.speed = std::numeric_limits<int32_t>::max();

        r.w.vehicles.update_ground_sound(r.veh(), t, false, false);

        CHECK(!r.w.out.sound_emitters.empty());
        if (!r.w.out.sound_emitters.empty()) {
            CHECK(r.w.out.sound_emitters[0].lane == 10);
            CHECK(r.w.out.sound_emitters[0].pitch_q16 == 52428);
        }
    }

    {
        Rig r;
        VehicleTraits t = buggy_traits();
        t.player_speed = 1;
        t.sound_profile = "SP_ExtremeSpan";
        static constexpr char kExtremeSpanProfile[] =
                "begin \"SP_ExtremeSpan\"\n"
                "  Soundloop_1 V_IDLE 1 1\n"
                "  Soundloop_2 V_DRIVE -32768 32767\n"
                "end\n";
        CHECK(r.w.tables.sound_profiles.parse(kExtremeSpanProfile,
                                       sizeof(kExtremeSpanProfile) - 1) == 1);
        r.mount();
        r.w.out.sound_emitters.clear();
        r.veh().veh.speed = 0xFFFF;

        r.w.vehicles.update_ground_sound(r.veh(), t, false, false);

        CHECK(!r.w.out.sound_emitters.empty());
        if (!r.w.out.sound_emitters.empty()) {
            CHECK(r.w.out.sound_emitters[0].lane == 10);
            CHECK(r.w.out.sound_emitters[0].pitch_q16 ==
                  std::numeric_limits<int32_t>::max());
        }
    }
}

} // namespace

// A helicopter does not leave the instant its first passenger climbs in. While a
// seat is still free and someone is walking over to take it, the hull holds:
// heading pinned to its own, both command words zeroed, powered bit dropped.
// The rotor is gated on the OCCUPANT rather than on power, so the blades keep
// turning through the wait -- engine running, going nowhere.
// [orig: the wait-for-boarders block of Entity_ProcessAirVehiclePhysics,
//  kong line 94590]
static void test_helo_waits_for_its_boarders() {
    World w;
    w.registry.configure_pool(0, 8);
    w.registry.configure_pool(1, 4);
    AiSystem &ai = w.ai;
    ai.is_authority = true;

    Entity helo{};
    helo.net_id = 420;
    helo.alive = true;
    helo.health = 100;
    helo.has_item_def = true;
    Seat ctrl; ctrl.type = SeatType::Controller;
    Seat pax;  pax.type  = SeatType::Passenger;
    helo.seats.push_back(ctrl);
    helo.seats.push_back(pax);
    const EntityHandle hh = w.registry.spawn(1, helo);
    ai.attach(hh);

    // A body on foot, running the BOARD order at this helo.
    Entity walker{};
    walker.net_id = 900;
    walker.alive = true;
    walker.health = 100;
    walker.has_item_def = true;
    walker.kind = EntityKind::Organic;
    const EntityHandle wh = w.registry.spawn(0, walker);
    AiEntity &wb = *ai.at(ai.attach(wh));
    // The board order lives in the walker's AiSlot (entity+0x68), not its brain
    // [orig: the hold's `mov ecx,[eax+68h]` walk, cveh @0x48BFC1..0x48BFDA;
    //  the writer WacCmd_SsnToSsn @0x4F73E4..0x4F73F7].
    wb.slot.f[37] = 125;    // "Goto SSN and board"
    wb.slot.f[38] = 420;    // ...this helo
    wb.brain.f[37] = 16;    // a brain word of the same index plays no part
    wb.brain.f[38] = 0;

    Entity *hv = w.registry.get(hh);
    CHECK(ai.vehicle_waits_for_boarders(w, *hv));

    // Seat him: nobody is walking any more, so the hold releases.
    Entity *we = w.registry.get(wh);
    we->mounted = true;
    CHECK(!ai.vehicle_waits_for_boarders(w, *hv));
    we->mounted = false;
    CHECK(ai.vehicle_waits_for_boarders(w, *hv));

    // A body boarding a DIFFERENT hull never holds this one.
    wb.slot.f[38] = 421;
    CHECK(!ai.vehicle_waits_for_boarders(w, *hv));
    wb.slot.f[38] = 420;

    // Neither does one that is not running the board order at all, even with
    // the brain words of the same indices set.
    wb.slot.f[37] = 16;
    wb.brain.f[37] = 125;
    wb.brain.f[38] = 420;
    CHECK(!ai.vehicle_waits_for_boarders(w, *hv));
    wb.slot.f[37] = 125;

    // Dead or hidden bodies are skipped -- retail's (Flags & 3) == 0 gate.
    we->flags |= 2u;
    CHECK(!ai.vehicle_waits_for_boarders(w, *hv));
    we->flags &= ~2u;
    we->flags |= 1u;
    CHECK(!ai.vehicle_waits_for_boarders(w, *hv));
    we->flags &= ~1u;
    CHECK(ai.vehicle_waits_for_boarders(w, *hv));

    // With every seat taken there is nothing left to wait for, however many
    // bodies are still walking -- retail asks Entity_CanEnterVehicle first.
    hv->seats[0].occupant = wh;
    hv->seats[1].occupant = wh;
    CHECK(!ai.vehicle_waits_for_boarders(w, *hv));
}

// Every mover ends by rebuilding the entity matrix and setting Flags 0x20000
// (kEntityFlagMatrixBuilt, homed on engine_flags), and a dead hull's bail jumps to
// that same tail, so every live and every dead vehicle a mover ran carries it —
// the retail load stream's live 0x20400. [orig: cveh @0x48D451; ctan
// @0x48AEC9; cbik @0x486A17; cbot @0x48EF63 (dead bail @0x48DDFA); aircraft
// @0x492766 (dead bail @0x490CD0); selector-zero ground @0x46F9C5 and boat
// @0x471683 (dead bails @0x46E89D / @0x470398)]
void test_every_mover_tail_sets_the_matrix_bit() {
	for (int physics : {1, 0}) {
		for (VehicleFamily family : { VehicleFamily::Ground, VehicleFamily::Bike,
					VehicleFamily::Tank, VehicleFamily::Watercraft, VehicleFamily::Helicopter,
					VehicleFamily::Plane }) {
			for (bool dead : {false, true}) {
				Rig r;
				VehicleTraits t = buggy_traits();
				t.family = family;
				t.physics = physics;
				r.veh().engine_flags = 0;
				r.veh().flags = dead ? kEntityFlagDead : 0u;
				if (vehicle_family_uses_direct_air_mover(family)) {
					r.veh().veh.ai_drive = true;
					r.w.vehicles.aircraft_client_tick(r.veh(), t);
				} else if (family == VehicleFamily::Watercraft) {
					r.w.vehicles.tick_watercraft_motor(r.veh(), t);
				} else {
					r.w.vehicles.tick_motor(r.veh(), t);
				}
				CHECK((r.veh().engine_flags & kEntityFlagMatrixBuilt) != 0);
				CHECK((r.veh().flags & kEntityFlagMatrixBuilt) == 0);
			}
		}
	}
	// An aircraft row the reimpl's guard skips (no pilot, no AI drive, not
	// predicted) still ran retail's mover to its tail.
	Rig parked;
	VehicleTraits air = buggy_traits();
	air.family = VehicleFamily::Helicopter;
	parked.veh().engine_flags = 0;
	parked.w.vehicles.aircraft_client_tick(parked.veh(), air);
	CHECK((parked.veh().engine_flags & kEntityFlagMatrixBuilt) != 0);
}

// Exercise the real family entries: same cadence, distinct water behavior.
void test_family_health_cadence_and_submersion() {
	for (VehicleFamily family : { VehicleFamily::Ground, VehicleFamily::Bike, VehicleFamily::Tank,
				 VehicleFamily::Watercraft, VehicleFamily::Helicopter, VehicleFamily::Plane }) {
		Rig r;
		VehicleTraits t = buggy_traits();
		t.family = family;
		t.critical_hp = 100;
		t.critical_drain = 7;
		t.non_critical_regen = 10;
		t.climb_speed = 1000;
		r.veh().net_id = 1;
		r.veh().health_max = 500;
		const auto tick = [&] {
			if (vehicle_family_uses_direct_air_mover(family)) {
				r.veh().veh.ai_drive = true;
				r.w.vehicles.aircraft_client_tick(r.veh(), t);
			} else if (family == VehicleFamily::Watercraft) {
				r.w.vehicles.tick_watercraft_motor(r.veh(), t);
			} else {
				r.w.vehicles.tick_motor(r.veh(), t);
			}
		};
		r.veh().health = 200;
		r.w.logic_tick = 55; // the incorrect x9 cadence would fire here
		tick();
		CHECK(r.veh().health == 200);
		r.w.logic_tick = 28; // (28 + 36 * 1) & 63 == 0
		tick();
		CHECK(r.veh().health == 210);
		r.veh().health = 490;
		tick();
		CHECK(r.veh().health == 490); // strict max - regen boundary
		r.veh().health = 100;
		tick();
		CHECK(r.veh().health == 93);
		r.veh().health = 3;
		tick();
		CHECK(r.veh().health == 0);
		tick();
		CHECK(r.veh().health == 0);
		r.w.ai.is_authority = false;
		r.veh().health = 100;
		tick();
		CHECK(r.veh().health == 100);

		if (family == VehicleFamily::Ground || family == VehicleFamily::Bike ||
				family == VehicleFamily::Tank) {
			r.w.logic_tick = 29; // isolate per-tick drown damage
			r.veh().flags |= 0x8000u;
			tick();
			CHECK(r.veh().health == 100); // no client-side health mutation
			r.w.ai.is_authority = true;
			r.veh().flags |= 0x8000u;
			tick();
			CHECK(r.veh().health == 98);
			r.veh().health = 1;
			r.veh().last_attacker = r.drv().handle;
			r.veh().flags |= 0x8000u;
			tick();
			CHECK(r.veh().health == 0);
			CHECK(!r.veh().last_attacker.valid());
		}
	}
}

void test_damage_effects_release_on_respawn() {
	Rig r;
	auto t = buggy_traits();
	t.critical_hp = 100;
	t.critical_drain = 7;
	r.veh().health_max = 500;
	r.veh().health = 120;
	r.w.logic_tick = 1;
	r.w.vehicles.traits.set(r.veh().item_id, t);
	r.tick(1, t);
	CHECK(r.w.out.destruction.effects.size() == 1);
	CHECK(r.veh().veh.damage_smoke_active && !r.veh().veh.damage_fire_active);
	r.veh().health = 100;
	r.tick(1, t);
	CHECK(r.w.out.destruction.effects.size() == 2);
	CHECK(r.veh().veh.damage_fire_active);
	CHECK(r.w.out.destruction.effects.back().effect == "Effect_vehicleFireMed");
	// Both motor effects carry the vehicle as the descriptor tag [orig:
	// Entity_UpdateVehiclePhysics push esi @ 0x48B0BB / 0x48B0F0].
	for (const auto &e : r.w.out.destruction.effects)
		CHECK(e.section_tagged);
	r.tick(1, t);
	CHECK(r.w.out.destruction.effects.size() == 2); // persistent slots spawn once
	r.w.out.destruction.clear();
	r.w.vehicles.respawn(r.veh());
	CHECK(!r.veh().veh.damage_fire_active && !r.veh().veh.damage_smoke_active);
	CHECK(r.w.out.destruction.effects.size() == 14); // 3 x 4 wreck slots + smoke/fire
	for (const auto &e : r.w.out.destruction.effects) {
		CHECK(e.release && e.attach_wire_handle == r.veh_h.packed);
	}
	CHECK(r.w.out.destruction.husk_swaps.size() == 1);
	CHECK(r.w.out.destruction.husk_swaps.front().restore_intact);
}

void test_zero_speed_displacement_and_submerged_sound() {
	Rig r;
	VehicleTraits t = buggy_traits();
	t.player_speed = 41600;
	t.sound_profile = "SP_Transport";
	load_transport_sound_profile(r.w);
	r.mount();
	stamp_saved_live_pose(r.veh());
	r.veh().position.x += 300.0f / 65536.0f;
	r.veh().position.y += 400.0f / 65536.0f;
	r.veh().position.z += 1200.0f / 65536.0f;
	r.w.out.sound_emitters.clear();
	r.w.vehicles.update_ground_sound(r.veh(), t, false, false);
	CHECK(r.w.out.sound_emitters.size() == 2);
	if (r.w.out.sound_emitters.size() == 2) {
		CHECK(r.w.out.sound_emitters[0].lane == 10);
		CHECK(r.w.out.sound_emitters[0].volume_q8_8 == 0x8000); // length=1300
		CHECK(r.w.out.sound_emitters[1].lane == 0);
	}
	r.veh().veh.speed = 1; // only EXACTLY zero substitutes displacement
	r.w.out.sound_emitters.clear();
	r.w.vehicles.update_ground_sound(r.veh(), t, false, false);
	CHECK(r.w.out.sound_emitters.size() == 1);
	if (!r.w.out.sound_emitters.empty())
		CHECK(r.w.out.sound_emitters[0].lane == 0);
	r.veh().veh.speed = 1000;
	r.drv().eye_offset_z = 65536;
	r.w.env.water_z = static_cast<int32_t>(r.drv().position.z * 65536.0f) + 65536;
	r.w.out.sound_emitters.clear();
	r.w.vehicles.update_ground_sound(r.veh(), t, false, true);
	CHECK(r.w.out.sound_emitters.size() == 2);
	for (size_t i = 0; i < r.w.out.sound_emitters.size(); ++i) {
		const auto &event = r.w.out.sound_emitters[i];
		CHECK(event.lane != 0); // underwater stop takes precedence over collision idle
		CHECK(event.volume_q8_8 == 0 && event.pitch_q16 == 0);
	}
}

void test_engine_and_light_sound_edges() {
	Rig r;
	static constexpr char profile[] =
			"begin \"SP_Edges\"\n"
			"SSAudio1 V_LIGHT\nenginestart V_START\nenginestop V_STOP\nend\n";
	CHECK(r.w.tables.sound_profiles.parse(profile, sizeof(profile) - 1) == 1);
	VehicleTraits t = buggy_traits();
	t.sound_profile = "SP_Edges";
	r.w.vehicles.traits.set(r.veh().item_id, t);
	r.mount();
	r.veh().flags |= 0x80;
	r.w.vehicles.update_engine_sound(r.veh(), t);
	CHECK(r.w.out.slot_sounds.size() == 2);
	CHECK(r.w.out.slot_sounds[0].slot == opennova::audio::kSlotAudio1);
	CHECK(r.w.out.slot_sounds[1].slot == opennova::audio::kSlotEngineStart);
	r.w.out.slot_sounds.clear();
	r.w.vehicles.update_engine_sound(r.veh(), t);
	CHECK(r.w.out.slot_sounds.empty());
	r.veh().flags &= ~0x80u;
	r.w.vehicles.update_engine_sound(r.veh(), t);
	r.veh().flags |= 0x80;
	r.w.vehicles.update_engine_sound(r.veh(), t);
	CHECK(r.w.out.slot_sounds.size() == 1);
	CHECK(r.w.out.slot_sounds[0].slot == opennova::audio::kSlotAudio1);
	r.w.out.slot_sounds.clear();
	CHECK(r.w.vehicles.detach(r.drv_h));
	CHECK(r.w.out.slot_sounds.size() == 1);
	CHECK(r.w.out.slot_sounds[0].slot == opennova::audio::kSlotEngineStop);
	CHECK(r.w.out.slot_sounds[0].source_handle == r.drv_h.packed);
	r.w.out.slot_sounds.clear();
	// Detach leaves bit 0 set, so the mover's leave edge sounds the hull stop.
	// [orig: cveh @0x48D3DA..0x48D421]
	r.w.vehicles.update_engine_sound(r.veh(), t);
	CHECK(r.w.out.slot_sounds.size() == 1);
	if (r.w.out.slot_sounds.size() == 1) {
		CHECK(r.w.out.slot_sounds[0].slot == opennova::audio::kSlotEngineStop);
		CHECK(r.w.out.slot_sounds[0].source_handle == r.veh_h.packed);
	}
	r.w.out.slot_sounds.clear();
	r.w.vehicles.update_engine_sound(r.veh(), t);
	CHECK(r.w.out.slot_sounds.empty()); // the edge fires once

	r.mount();
	t.family = VehicleFamily::Helicopter;
	r.veh().veh.part_spin = {};
	r.w.vehicles.part_anim_tick(r.veh(), t);
	CHECK(r.w.out.slot_sounds.size() == 1);
	CHECK(r.w.out.slot_sounds[0].slot == opennova::audio::kSlotEngineStart);
	CHECK(r.w.out.slot_sounds[0].source_handle == r.drv_h.packed);
	r.w.out.slot_sounds.clear();
	r.w.vehicles.part_anim_tick(r.veh(), t);
	CHECK(r.w.out.slot_sounds.empty());
	r.veh().veh.part_spin = {};
	r.w.env.water_z = 20 << 16;
	r.w.vehicles.part_anim_tick(r.veh(), t);
	CHECK(r.w.out.slot_sounds.empty()); // submerged hull cannot start its sound

	// The authority boat entry owns the same claimant edge, independently
	// of the decoded/client mover. It does not play the ground light slot.
	Rig boat;
	CHECK(boat.w.tables.sound_profiles.parse(profile, sizeof(profile) - 1) == 1);
	t.family = VehicleFamily::Watercraft;
	t.water_speed = t.player_speed;
	boat.w.vehicles.traits.set(boat.veh().item_id, t);
	boat.mount();
	boat.veh().flags |= 0x80u;
	boat.w.vehicles.tick_watercraft_motor(boat.veh(), t, nullptr);
	CHECK(boat.w.out.slot_sounds.size() == 1);
	CHECK(boat.w.out.slot_sounds[0].slot == opennova::audio::kSlotEngineStart);
	boat.w.out.slot_sounds.clear();
	boat.w.vehicles.tick_watercraft_motor(boat.veh(), t, nullptr);
	CHECK(boat.w.out.slot_sounds.empty());
}

// The selector-zero movers' sound tails: the ground twin shares cveh's lights
// and claimant edges (slot 24, then slot 30 while the occupant eye is above
// water, once), the boat twin has only its lights leg beside the movement fold
// and never latches the engine.
// [IDB: Entity_ProcessInfantryPhysics @0x46E100 (the selector-zero ground
// mover) lights @0x46F8C3..0x46F8FC, claimant edge @0x46F8FC..0x46F99C;
// Entity_ProcessAirVehiclePhysics @0x46FA00 (the selector-zero boat mover)
// lights @0x4714EA..0x471523, fold @0x471523..0x4715D7, tail @0x47166F]
void test_selector_zero_sound_tails() {
	static constexpr char profile[] =
			"begin \"SP_Zero\"\n"
			"SSAudio1 V_LIGHT\nenginestart V_START\nenginestop V_STOP\nend\n";
	Rig ground;
	CHECK(ground.w.tables.sound_profiles.parse(profile, sizeof(profile) - 1) == 1);
	VehicleTraits t = buggy_traits();
	t.physics = 0;
	t.sound_profile = "SP_Zero";
	ground.w.vehicles.traits.set(ground.veh().item_id, t);
	ground.mount();
	// Lights are the occupant's order, not hull state: both selector-zero
	// input blocks rewrite Flags 0x80 from the controller's MoveOrder bit
	// 0x20 every tick, so a flag poked on the hull is gone before the tail
	// [orig: ground `test byte [occ+12Ch],20h` @0x46EB47 -> `or [ent+24h],80h`
	//  @0x46EB50 / `and [ent+24h],0FFFFFF7Fh` @0x46EB59; boat @0x47062B..0x47063D].
	ground.drv().net_move_input = 0x20;
	ground.w.vehicles.tick_motor(ground.veh(), t);
	CHECK(ground.w.out.slot_sounds.size() == 2);
	if (ground.w.out.slot_sounds.size() == 2) {
		CHECK(ground.w.out.slot_sounds[0].slot == opennova::audio::kSlotAudio1);
		CHECK(ground.w.out.slot_sounds[1].slot == opennova::audio::kSlotEngineStart);
	}
	CHECK(ground.veh().veh.engine_sound_latched);
	ground.w.out.slot_sounds.clear();
	ground.w.vehicles.tick_motor(ground.veh(), t);
	CHECK(ground.w.out.slot_sounds.empty());

	// The boat runs the claimant start/stop edge at its HEAD (the PlayerControl
	// block before the authority split) and the lights edge in its tail, so a
	// first tick with a fresh claimant and the lights order emits the engine
	// start BEFORE the lights one-shot; the claimant's departure fires the stop
	// one-shot on the hull +0x18000 above the water plane.
	// [orig: Entity_ProcessAirVehiclePhysics @0x46FA00 claimant edge
	//  @0x470055..0x4700EB, lights @0x4714EA..0x471523]
	Rig boat;
	CHECK(boat.w.tables.sound_profiles.parse(profile, sizeof(profile) - 1) == 1);
	t.family = VehicleFamily::Watercraft;
	t.water_speed = t.player_speed;
	boat.w.vehicles.traits.set(boat.veh().item_id, t);
	boat.mount();
	boat.drv().net_move_input = 0x20; // the lights order (see above)
	boat.w.vehicles.tick_motor(boat.veh(), t);
	CHECK(boat.w.out.slot_sounds.size() == 2);
	if (boat.w.out.slot_sounds.size() == 2) {
		CHECK(boat.w.out.slot_sounds[0].slot == opennova::audio::kSlotEngineStart);
		CHECK(boat.w.out.slot_sounds[1].slot == opennova::audio::kSlotAudio1);
	}
	CHECK(boat.veh().veh.engine_sound_latched);
	boat.w.out.slot_sounds.clear();
	boat.w.vehicles.tick_motor(boat.veh(), t);
	CHECK(boat.w.out.slot_sounds.empty());
	CHECK(boat.veh().veh.engine_sound_latched);
	// The claimant's departure: the detach leg fires the stop on the departing
	// occupant without touching the latch, so the mover's own leave edge fires
	// a second stop on the hull. [orig: Entity_DetachFromVehicle
	// @0x43571B..0x43573E; selector-zero boat leave edge @0x4700A1..0x4700EB]
	boat.w.out.slot_sounds.clear();
	CHECK(boat.w.vehicles.detach(boat.drv_h));
	CHECK(boat.veh().veh.engine_sound_latched);
	boat.w.vehicles.tick_motor(boat.veh(), t);
	CHECK(!boat.veh().veh.engine_sound_latched);
	int stops = 0;
	for (const auto &sound : boat.w.out.slot_sounds)
		if (sound.slot == opennova::audio::kSlotEngineStop) ++stops;
	CHECK(stops == 2);
	if (boat.w.out.slot_sounds.size() == 2) {
		CHECK(boat.w.out.slot_sounds[0].source_handle == boat.drv_h.packed);
		CHECK(boat.w.out.slot_sounds[1].source_handle == boat.veh_h.packed);
	}
}

// The critical warning (profile slot 34) cadence: `& 0x1F` in the ground
// mover, `& 0x3F` in the direct-air mover. Tick 60 satisfies the 32-tick
// mask only ((60 + 36) & 31 == 0, & 63 == 32); tick 28 satisfies both.
// [orig: Entity_UpdateVehiclePhysics @0x48B08C; Entity_UpdateAircraftPhysics
//  @0x4904D7..0x4904F0]
void test_warning_cadence_by_family() {
	static constexpr char profile[] = "begin \"SP_Warn\"\nwarning V_WARN\nend\n";
	for (VehicleFamily family : { VehicleFamily::Ground, VehicleFamily::Helicopter }) {
		Rig r;
		CHECK(r.w.tables.sound_profiles.parse(profile, sizeof(profile) - 1) == 1);
		VehicleTraits t = buggy_traits();
		t.family = family;
		t.critical_hp = 100;
		t.critical_drain = 0;
		t.climb_speed = 1000;
		t.sound_profile = "SP_Warn";
		r.veh().net_id = 1;
		r.veh().health_max = 500;
		r.veh().health = 100;
		r.w.ai.is_authority = false; // stay in the critical band without draining
		const auto warnings = [&] {
			int count = 0;
			for (const auto &sound : r.w.out.slot_sounds)
				count += sound.slot == opennova::audio::kSlotWarning ? 1 : 0;
			r.w.out.slot_sounds.clear();
			return count;
		};
		const auto tick = [&] {
			if (vehicle_family_uses_direct_air_mover(family)) {
				r.veh().veh.ai_drive = true;
				r.w.vehicles.aircraft_client_tick(r.veh(), t);
			} else {
				r.w.vehicles.tick_motor(r.veh(), t);
			}
		};
		r.w.logic_tick = 60;
		tick();
		CHECK(warnings() == (family == VehicleFamily::Ground ? 1 : 0));
		r.w.logic_tick = 28;
		tick();
		CHECK(warnings() == 1);
	}
}

// A sideways skid keeps momentum across release, then turns toward the body
// only after the authored recovery window. Exercise the public motor so input,
// acceleration, wheel phase and velocity all participate.
void test_handbrake_skid_and_grip_recovery() {
	for (VehicleFamily family : { VehicleFamily::Ground, VehicleFamily::Bike }) {
		Rig r;
		auto t = buggy_traits();
		t.family = family;
		t.hand_brake = 1;
		t.tire_slip = 5;
		r.mount();
		r.drv().player_class = 0; // NPC command registers are retained.
		auto &v = r.veh();
		auto &m = v.veh;
		m.yaw_seeded = true;
		m.yaw_bam = m.steer_target_bam = 0;
		m.speed = m.cmd_speed = 20000;
		m.bike_ground_contact_ticks = 10;
		m.contact_direction[1] = 65536;
		v.flags |= 8;
		r.w.logic_tick = 100;
		const float x = v.position.x, y = v.position.y;
		r.tick(1, t);
		CHECK(m.handbrake_latched == 1);
		CHECK(m.speed == 19720);
		CHECK(m.vel_x == 0 && m.vel_y == 19720);
		CHECK(v.position.x == x && v.position.y > y);
		CHECK(m.slip_started_tick == 0);
		if (family == VehicleFamily::Ground)
			CHECK(m.wheel_phase == 0);

		v.flags &= ~8u;
		m.cmd_speed = 20000;
		r.w.logic_tick = 101;
		r.tick(1, t);
		CHECK(m.handbrake_latched == 0);
		CHECK(m.slip_started_tick == 101);
		CHECK(m.vel_x == 0 && m.vel_y > 0);
		CHECK(m.wheel_slip_phase == -397934592);
		CHECK(m.skid_effects_requested);
		r.w.logic_tick = 126; // inclusive 5*tire_slip window
		r.tick(1, t);
		CHECK(m.vel_x == 0);
		const int32_t recovery_speed = m.speed;
		r.w.logic_tick = 127;
		r.tick(1, t);
		CHECK(m.speed == recovery_speed + (t.acceleration >> 4));
		CHECK(m.vel_x > 0 && m.vel_y > m.vel_x);
		CHECK(m.contact_direction[0] > 0);

		// Alignment ends the skid; the carry animates before its zero-command
		// reset, and no stale slip vector survives the grip transition.
		m.contact_direction[0] = 65536;
		m.contact_direction[1] = m.contact_direction[2] = 0;
		r.w.logic_tick = 128;
		r.tick(1, t);
		CHECK(m.contact_direction[0] == 0 && m.contact_direction[1] == 0);
		CHECK(m.slip_started_tick == 0);
		CHECK(m.vel_y == 0 && m.vel_x > 0);
		m.cmd_speed = 0;
		r.w.logic_tick = 129;
		r.tick(1, t);
		CHECK(m.wheel_slip_phase == 0);
	}
}

void test_tank_pivot_retains_direction_and_previous_track_rate() {
	Rig r;
	auto t = buggy_traits();
	t.family = VehicleFamily::Tank;
	r.mount();
	r.drv().player_class = 0;
	auto &m = r.veh().veh;
	m.yaw_seeded = true;
	m.yaw_bam = m.steer_target_bam = 0;
	m.steer_state = 119304640; // ten degrees, still above four after chase
	m.speed = 12000;
	m.cmd_speed = 0;
	m.contact_direction[1] = 65536;
	m.wheel_rate_bam = 40000;
	const int32_t old_rate = m.wheel_rate_bam;
	r.tick(1, t);
	CHECK(m.speed == 12000 - 2 * t.deceleration);
	CHECK(m.vel_x == 0 && m.vel_y == m.speed);
	CHECK(m.wheel_rate_bam == int32_t((-int64_t(16384) * (m.steer_state >> 2) + 0x8000) >> 16));
	int32_t expected[2] = {};
	opennova::world::track_phase_tick(expected, m.speed, old_rate);
	CHECK(m.track_phase[0] == expected[0] && m.track_phase[1] == expected[1]);
}

// Tank pivot uses the yaw rate (entity+0xA4), not vertical velocity (+0xA0).
// The loop consumes the prior latch; the edge updates it afterward.
// [orig: Entity_UpdateTankVehiclePhysics @ 0x488AB0, @ 0x48AAE0 and @ 0x48ACB5]
void test_tank_pivot_sound_latch_and_loop() {
	for (bool authority : { false, true }) {
		Rig r;
		r.w.ai.is_authority = authority;
		r.mount();
		auto t = buggy_traits();
		t.family = VehicleFamily::Tank;
		t.sound_profile = "SP_TankPivot";
		static constexpr char profile[] =
				"begin \"SP_TankPivot\"\n"
				" Soundloop_1 TANK_IDLE 1 1\n"
				" Soundloop_4 TANK_PIVOT 1 1\n"
				" swivel_shift TANK_SHIFT\nend\n";
		CHECK(r.w.tables.sound_profiles.parse(profile, sizeof(profile) - 1) == 1);
		auto &m = r.veh().veh;
		m.yaw_bam = 0;
		m.slide_z = -123; // deliberately distinct from the yaw-rate volume
		m.wheel_rate_bam = 40000;
		const auto fold = [&](bool collided = false) {
			r.w.out.sound_emitters.clear();
			r.w.out.slot_sounds.clear();
			r.w.vehicles.update_ground_sound(r.veh(), t, false, collided);
			r.w.vehicles.update_traction_sound(r.veh(), t);
			int volume = -1;
			for (size_t i = 0; i < r.w.out.sound_emitters.size(); ++i) {
				const auto &event = r.w.out.sound_emitters[i];
				if (event.lane != 40) continue;
				CHECK(event.slot == 3 && event.set_name == "TANK_PIVOT");
				CHECK(event.pitch_q16 == 65536 && event.lifetime_ticks == 30);
				volume = event.volume_q8_8;
			}
			return volume;
		};
		m.steer_target_bam = 0x071C71C0; // strict > ten degrees
		CHECK(fold() == -1);
		CHECK(r.w.out.slot_sounds.empty());
		++m.steer_target_bam;
		CHECK(fold() == -1); // first tick only arms and plays swivel_shift
		CHECK(r.w.out.slot_sounds.size() == 1);
		if (!r.w.out.slot_sounds.empty()) CHECK(r.w.out.slot_sounds[0].slot == 46);
		m.steer_target_bam = 0; // the latch persists inside the start threshold
		CHECK(fold() == 40000);
		CHECK(r.w.out.slot_sounds.empty());
		m.wheel_rate_bam = -50000;
		CHECK(fold() == 50000); // old latch is consumed before direction reversal clears it
		CHECK(fold() == -1);
		m.steer_target_bam = -0x071C71C1;
		CHECK(fold() == -1);
		CHECK(r.w.out.slot_sounds.size() == 1);
		m.wheel_rate_bam = -100000;
		CHECK(fold(true) == 65535); // collision idle still reaches the clamped pivot loop
		m.settle_2f0 = 1;
		CHECK(fold() == -1); // crash-settled tanks skip the fold and retain the latch
		m.settle_2f0 = 0;
		m.speed = t.player_speed;
		CHECK(fold() == -1); // no idle registration at maximum speed; also clears latch
		m.speed = 0;
		m.wheel_rate_bam = 0;
		CHECK(fold() == -1);
	}
}

// A tank rides its recoil / heavy-hit impulse: the unit direction scaled by
// trunc(amplitude * 28.16f) joins the velocity every tick while the chassis
// contact byte holds, and the direction clears on the tick it drops.
// [orig: Entity_UpdateTankVehiclePhysics @0x48A8C0..0x48A9C0]
void test_tank_impulse_pushes_velocity() {
	Rig r;
	auto t = buggy_traits();
	t.family = VehicleFamily::Tank;
	t.player_control = false;
	auto &m = r.veh().veh;
	m.yaw_seeded = true;
	m.chassis_impulse_direction[0] = -65536;
	m.chassis_impulse_amplitude = 10; // the stock WPN_M1TURRET fire action_value
	m.chassis_contact_active = true;
	const float x0 = r.veh().position.x;
	r.tick(1, t);
	// trunc(10 * 28.16f) = 281, then (-65536 * 281 + 0x8000) >> 16 = -281.
	CHECK(m.vel_x == -281);
	CHECK(m.chassis_impulse_direction[0] == -65536);
	CHECK(r.veh().position.x < x0);
	m.chassis_contact_active = false;
	r.tick(1, t);
	CHECK(m.vel_x == -281); // the last push lands before the clear
	CHECK(m.chassis_impulse_direction[0] == 0);
	r.tick(1, t);
	CHECK(m.vel_x == 0);
	// Other families never read the impulse.
	Rig g;
	auto gt = buggy_traits();
	gt.player_control = false;
	g.veh().veh.yaw_seeded = true;
	g.veh().veh.chassis_impulse_direction[0] = -65536;
	g.veh().veh.chassis_impulse_amplitude = 10;
	g.veh().veh.chassis_contact_active = true;
	g.tick(1, gt);
	CHECK(g.veh().veh.vel_x == 0);
}

// The ground core stops a crashed hull outright; the tank also needs +0x2EF.
// [orig: cveh @0x48C086..0x48C08F; Entity_UpdateTankVehiclePhysics
//  @0x489C06..0x489C1C]
void test_tank_crash_stop_needs_2ef() {
	for (const VehicleFamily family : { VehicleFamily::Tank, VehicleFamily::Ground }) {
		for (int latch = 0; latch < 2; ++latch) {
			Rig r;
			auto t = buggy_traits();
			t.family = family;
			t.player_control = false;
			auto &m = r.veh().veh;
			m.yaw_seeded = true;
			m.crashed = 1;
			m.byte_2ef = static_cast<uint8_t>(latch);
			m.cmd_speed = 16384;
			r.tick(1, t);
			const bool stopped = family == VehicleFamily::Ground || latch == 1;
			CHECK((m.cmd_speed == 0) == stopped);
		}
	}
}

// The tank's recovery arm stores slideDecay even on a crashed hull; only the
// straight arm keeps a crashed hull's vertical velocity.
// [orig: Entity_UpdateTankVehiclePhysics store @0x48A5D4; straight arm Z gate
//  @0x48A28D..0x48A2AE]
void test_tank_crashed_vertical_velocity_arms() {
	Rig r;
	auto t = buggy_traits();
	t.family = VehicleFamily::Tank;
	auto &m = r.veh().veh;
	m.crashed = 1;
	m.slide_z = -5000;
	m.speed = 4000; // commanded, |speed| <= 0x2000: the recovery arm
	CHECK(detail::vehicle_traction_velocity(r.w, r.veh(), t, 4000));
	CHECK(m.slide_z == 0); // speed * forward.z on the level row
	m.slide_z = -5000;
	m.speed = 20000; // the straight arm
	CHECK(detail::vehicle_traction_velocity(r.w, r.veh(), t, 20000));
	CHECK(m.slide_z == -5000);
	m.crashed = 0;
	CHECK(detail::vehicle_traction_velocity(r.w, r.veh(), t, 20000));
	CHECK(m.slide_z == 0);
}

// Movement trails sample only at the end of the contact arms: the airborne
// and off-contact arms jump past them. [orig: ctan @0x48A60D..0x48A67F vs
// @0x48A684; cveh @0x48CD93..0x48CDFD vs @0x48CE02; cbik @0x486170..0x4861F1
// vs @0x4861F6]
void test_trails_sample_only_in_contact_arms() {
	for (const VehicleFamily family :
			{ VehicleFamily::Tank, VehicleFamily::Ground, VehicleFamily::Bike }) {
		Rig r;
		auto t = buggy_traits();
		t.family = family;
		t.player_control = false;
		for (int i = 0; i < 2; ++i) {
			t.trails[i].effect = "trail" + std::to_string(i + 1);
			t.trails[i].mask = static_cast<uint16_t>(1u << i);
		}
		t.trail_point_count = 2;
		t.trail_points[0] = { { 65536, 0, 0 }, { 0, 0, 65536 } };
		t.trail_points[1] = { { -65536, 0, 0 }, { 0, 0, 65536 } };
		r.w.out.fire_sounds.set_listener(r.veh().position);
		auto &m = r.veh().veh;
		m.yaw_seeded = true;
		m.contact_solved_once = true;
		m.cmd_speed = 16384;
		m.speed = 16384;
		const int trail_lane = family == VehicleFamily::Ground ? 1 : 0;
		r.veh().flags |= kEntityFlagInAir; // airborne arm
		r.w.logic_tick = 4;
		r.tick(1, t);
		CHECK(m.trails.points[trail_lane].definition == 0);
		r.veh().flags &= ~kEntityFlagInAir;
		m.grounded = false; // off-contact arm
		r.w.logic_tick = 8;
		r.tick(1, t);
		CHECK(m.trails.points[trail_lane].definition == 0);
		m.grounded = true; // contact arm
		r.w.logic_tick = 12;
		r.tick(1, t);
		CHECK(m.trails.points[trail_lane].definition != 0);
	}
}

// The slope factor samples the runtime table at (pitch + 0x200000) >> 22 rather
// than a continuous cosine. A boxless row reads its placement pitch in the
// spawn form: at -16 degrees that samples entry 979 where -16 x 11930464
// would sample 978. [orig: Entity_UpdateTankVehiclePhysics
//  @0x489CE6..0x489D01; cveh @0x48C1C6..0x48C1D7; cbik @0x485362..0x48537E;
//  Entity_SpawnFromBMSRecord @0x40EB69..0x40EB86]
void test_slope_factor_samples_quantized_table() {
	for (const VehicleFamily family :
			{ VehicleFamily::Tank, VehicleFamily::Ground, VehicleFamily::Bike })
	for (const int16_t pitch : { int16_t(10), int16_t(-16) }) {
		Rig r;
		auto t = buggy_traits();
		t.family = family;
		t.player_control = false;
		auto &m = r.veh().veh;
		m.yaw_seeded = true;
		r.veh().pitch = pitch;
		m.speed = 20000;
		m.cmd_speed = 20000;
		const int32_t pitch_bam = spawn_angle_bam(pitch);
		int32_t c = 0, s = 0;
		quantized_dir(pitch_bam, c, s);
		const auto target_for = [](int32_t cos22) {
			const int32_t c2 = static_cast<int32_t>((static_cast<int64_t>(cos22) * cos22) >> 22);
			return static_cast<int32_t>((static_cast<int64_t>(c2) * 20000) >> 22);
		};
		const int32_t target = target_for(c);
		const int32_t continuous = target_for(
				static_cast<int32_t>(std::cos(pitch_bam * opennova::io::kRadiansPerBam) * 4194304.0));
		CHECK(continuous != target); // the case discriminates the two samplers
		r.tick(1, t);
		const int32_t accel =
				std::clamp((target - 20000 + 16) >> 5, -t.acceleration, t.acceleration);
		CHECK(m.speed == 20000 + accel);
	}
}

// A zero-command tank whose parent is an NPC keeps the raw servo instead of
// the deceleration clamp, as in the ground core.
// [orig: Entity_UpdateTankVehiclePhysics @0x489EBA..0x489ECB; cveh
//  @0x48C428..0x48C43F]
void test_tank_npc_parent_coast_keeps_raw_servo() {
	for (const bool npc_parent : { false, true }) {
		Rig r;
		auto t = buggy_traits();
		t.family = VehicleFamily::Tank;
		t.player_control = false;
		Entity parent;
		parent.kind = EntityKind::Item;
		parent.flags = npc_parent ? 0u : 0x100u;
		const EntityHandle parent_h = r.w.registry.spawn(1, parent);
		r.veh().mount_target = parent_h;
		auto &m = r.veh().veh;
		m.yaw_seeded = true;
		m.speed = 20000;
		m.cmd_speed = 0;
		r.tick(1, t);
		const int32_t raw = (0 - 20000 + 16) >> 5;
		CHECK(m.speed == 20000 + (npc_parent ? raw : -t.deceleration));
	}
}

// The ctank and cbike rows call their movers without testing the selector.
// [orig: Entity_DispatchPhysics_ctank @0x48F000..0x48F007;
//  Entity_DispatchPhysics_cbike @0x48EFF0..0x48EFF7; cveh tests it @0x48EFC7]
void test_tank_and_bike_ignore_physics_selector() {
	Rig r;
	auto t = buggy_traits();
	t.family = VehicleFamily::Tank;
	t.player_control = false;
	t.physics = 0;
	auto &m = r.veh().veh;
	m.yaw_seeded = true;
	m.speed = 10000;
	m.cmd_speed = 10000;
	r.tick(1, t);
	CHECK(m.track_phase[0] != 0 && m.track_phase[1] != 0); // the tank mover ran
	Rig b;
	auto bt = buggy_traits();
	bt.family = VehicleFamily::Bike;
	bt.player_control = false;
	bt.physics = 0;
	b.veh().veh.yaw_seeded = true;
	b.veh().veh.speed = 8192;
	b.veh().flags |= 0x20u;
	b.tick(1, bt);
	CHECK(b.veh().veh.wheelie_active != 0); // the light mover's wheelie arm ran
}

// A predicting client runs the PlayerControl tail with the real row: an
// unclaimed hull skips the movement fold, and a claimant arms the start edge.
// Only the input block is role-gated. [orig: ctan input gate @0x489522..0x489545;
// tail gates @0x48AAC4..0x48AADA and @0x48AD94..0x48AE3D]
void test_prediction_keeps_player_control_tail() {
	Rig r;
	static constexpr char profile[] =
			"begin \"SP_Pred\"\n Soundloop_1 IDLE 1 1\n enginestart V_START\nend\n";
	CHECK(r.w.tables.sound_profiles.parse(profile, sizeof(profile) - 1) == 1);
	auto t = buggy_traits();
	t.family = VehicleFamily::Tank;
	t.sound_profile = "SP_Pred";
	r.w.vehicles.traits.set(r.veh().item_id, t);
	r.w.ai.is_authority = false;
	auto &m = r.veh().veh;
	m.net_predicted = true;
	m.yaw_seeded = true;
	r.w.vehicles.ground_client_tick(r.veh(), t);
	bool idle = false;
	for (size_t i = 0; i < r.w.out.sound_emitters.size(); ++i)
		idle = idle || (r.w.out.sound_emitters[i].lane == 0 &&
								r.w.out.sound_emitters[i].volume_q8_8 != 0);
	CHECK(!idle); // no claimant: the PlayerControl caller skips the fold
	CHECK(r.w.out.slot_sounds.empty());
	r.mount();
	r.w.env.water_z = -(1 << 16); // the claimant's eye sits above the water plane
	r.w.out.slot_sounds.clear();
	r.w.vehicles.ground_client_tick(r.veh(), t);
	bool started = false;
	for (const auto &sound : r.w.out.slot_sounds)
		started = started || sound.slot == opennova::audio::kSlotEngineStart;
	CHECK(started);
	CHECK(m.engine_sound_latched);
}

// Each mover family keeps its own crash/settle gates in the client chase.
// [orig: ctan radius @0x48913B..0x48916E, snap heading @0x4891D5..0x489208,
//  heading step @0x48933D..0x48935B; cveh snap @0x48B5FB..0x48B622, Z step
//  @0x48B798..0x48B7B9; cbik snap @0x4846EB..0x48472D; cbot Z step
//  @0x48DD8A..0x48DDAB]
void test_client_chase_family_gates() {
	struct Result {
		Vec3 pos;
		int32_t yaw;
	};
	// A record 8 u ahead and 1 u up, heading +0x10000000: beyond the idle 2 u
	// snap radius, inside the tank's widened 12 u one.
	const auto chase = [](VehicleChaseFamily family, bool crashed, bool air, int32_t up_z,
								uint8_t wheelie) {
		Entity e;
		e.position = { 0.0f, 0.0f, 0.0f };
		if (air) e.flags |= kEntityFlagInAir;
		auto &m = e.veh;
		m.crashed = crashed ? 1 : 0;
		m.wheelie_active = wheelie;
		m.net_smooth_target[0] = 8 << 16;
		m.net_smooth_target[2] = 1 << 16;
		m.net_smooth_heading = 0x10000000;
		vehicle_client_chase(e, family, up_z);
		return Result{ e.position, m.yaw_bam };
	};
	// The tank widens a latched hull's radius, so it interpolates instead.
	Result r = chase(VehicleChaseFamily::Tank, true, false, 0x10000, 0);
	CHECK(r.pos.x > 0.0f && r.pos.x < 8.0f);
	CHECK(r.yaw == 0); // neither the snap nor the step moves a latched heading
	r = chase(VehicleChaseFamily::Tank, false, false, 0x10000, 0);
	CHECK(r.pos.x == 8.0f && r.pos.z == 1.0f && r.yaw == 0x10000000);
	// The ground core keeps heading and Z while crashed, and snaps both otherwise.
	r = chase(VehicleChaseFamily::Ground, true, false, 0x10000, 0);
	CHECK(r.pos.x == 8.0f && r.pos.z == 0.0f && r.yaw == 0);
	r = chase(VehicleChaseFamily::Ground, false, false, 0x10000, 0);
	CHECK(r.pos.x == 8.0f && r.pos.z == 1.0f && r.yaw == 0x10000000);
	// The bike always moves Z but holds heading while wheeling or inverted.
	r = chase(VehicleChaseFamily::Bike, false, false, 0x10000, 1);
	CHECK(r.pos.z == 1.0f && r.yaw == 0);
	r = chase(VehicleChaseFamily::Bike, false, false, -1, 0);
	CHECK(r.pos.z == 1.0f && r.yaw == 0);
	r = chase(VehicleChaseFamily::Bike, false, false, 0, 0);
	CHECK(r.yaw == 0x10000000);

	// The per-tick heading step: ungated on the ground core, gated on the tank.
	const auto step = [](VehicleChaseFamily family, bool crashed, bool air) {
		Entity e;
		if (air) e.flags |= kEntityFlagInAir;
		auto &m = e.veh;
		m.crashed = crashed ? 1 : 0;
		m.net_smooth_target[0] = 1 << 16; // 1 u: inside the snap radius
		m.net_smooth_target[2] = 1 << 16;
		m.net_smooth_heading = 20 * 1000;
		vehicle_client_chase(e, family, 0x10000);
		return Result{ e.position, m.yaw_bam };
	};
	CHECK(step(VehicleChaseFamily::Ground, true, false).yaw == 1000);
	CHECK(step(VehicleChaseFamily::Tank, true, false).yaw == 0);
	// Z steps on the plain template always; the ground families step it only
	// while airborne and unlatched.
	CHECK(step(VehicleChaseFamily::Plain, false, false).pos.z > 0.0f);
	CHECK(step(VehicleChaseFamily::Ground, false, false).pos.z == 0.0f);
	CHECK(step(VehicleChaseFamily::Ground, true, true).pos.z == 0.0f);
	CHECK(step(VehicleChaseFamily::Ground, false, true).pos.z > 0.0f);
}

// The mover prologue stamps savedLivePose from its entry pose, including the
// direct call Game_StartMission makes; a predicting client stamps before its
// chase moves the hull.
// [orig: ctan @0x488B24..0x488B50, chase @0x48912B; Game_StartMission
//  @0x526010]
void test_mover_prologue_stamps_saved_live_pose() {
	auto t = buggy_traits();
	t.family = VehicleFamily::Tank;
	{
		Rig r;
		r.w.vehicles.traits.set(r.veh().item_id, t);
		r.veh().yaw = 1;
		r.veh().pitch = 5;
		r.veh().roll = -3;
		const int32_t entry[3] = { to_fixed(r.veh().position.x), to_fixed(r.veh().position.y),
			to_fixed(r.veh().position.z) };
		CHECK(!r.veh().saved_live_valid);
		r.w.vehicles.tick_motor(r.veh(), t);
		CHECK(r.veh().saved_live_valid);
		for (int axis = 0; axis < 3; ++axis)
			CHECK(r.veh().saved_live_pos[axis] == entry[axis]);
		// The stamp copies the BAM attitude the mover starts from: an unmoved
		// row's placement angles in the spawn form, ((deg << 16) / 360) << 16.
		// [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EBA6;
		//  Entity_UpdateTankVehiclePhysics @0x488B2A..0x488B3C]
		CHECK(r.veh().saved_live_yaw == 1061748736);  // 90 - 1 = 89
		CHECK(r.veh().saved_live_pitch == 59637760);  // 5
		CHECK(r.veh().saved_live_roll == -35782656);  // -3
	}
	{
		Rig r;
		r.w.vehicles.traits.set(r.veh().item_id, t);
		r.w.ai.is_authority = false;
		auto &m = r.veh().veh;
		m.net_predicted = true;
		m.yaw_seeded = true;
		const int32_t entry[3] = { to_fixed(r.veh().position.x), to_fixed(r.veh().position.y),
			to_fixed(r.veh().position.z) };
		m.net_smooth_target[0] = entry[0] + (8 << 16);
		m.net_smooth_target[1] = entry[1];
		m.net_smooth_target[2] = entry[2];
		r.w.vehicles.ground_client_tick(r.veh(), t);
		CHECK(to_fixed(r.veh().position.x) != entry[0]); // the chase moved the hull
		CHECK(r.veh().saved_live_valid);
		for (int axis = 0; axis < 3; ++axis)
			CHECK(r.veh().saved_live_pos[axis] == entry[axis]);
	}
}

// The tread cue: every even tick adds this and the previous even tick's speed;
// a sum at or past +-0x80000 plays slot 45 on the hull and restarts. Odd ticks
// are skipped, and the doubled sum's top bit is dropped.
// [orig: Entity_UpdateTankVehiclePhysics @0x48AE45..0x48AEA1]
void test_tank_tread_sound_accumulates_even_ticks() {
	Rig r;
	static constexpr char profile[] = "begin \"SP_Tread\"\n drive_repeat V_TREADS\nend\n";
	CHECK(r.w.tables.sound_profiles.parse(profile, sizeof(profile) - 1) == 1);
	auto t = buggy_traits();
	t.family = VehicleFamily::Tank;
	t.sound_profile = "SP_Tread";
	auto &m = r.veh().veh;
	m.speed = 0x10000;
	r.w.logic_tick = 1;
	r.w.vehicles.update_tread_sound(r.veh(), t);
	CHECK(m.tread_sound_accum == 0 && m.tread_sound_prev_speed == 0); // odd tick
	r.w.logic_tick = 2;
	r.w.vehicles.update_tread_sound(r.veh(), t);
	CHECK(m.tread_sound_accum == 0x10000 && m.tread_sound_prev_speed == 0x10000);
	for (uint32_t tick = 4; tick <= 8; tick += 2) {
		r.w.logic_tick = tick;
		r.w.vehicles.update_tread_sound(r.veh(), t);
	}
	CHECK(m.tread_sound_accum == 0x70000);
	CHECK(r.w.out.slot_sounds.empty());
	r.w.logic_tick = 10;
	r.w.vehicles.update_tread_sound(r.veh(), t); // 0x70000 + 0x20000 passes 0x80000
	CHECK(m.tread_sound_accum == 0);
	CHECK(r.w.out.slot_sounds.size() == 1);
	if (r.w.out.slot_sounds.size() == 1) {
		CHECK(r.w.out.slot_sounds[0].slot == opennova::audio::kSlotDriveRepeat);
		CHECK(r.w.out.slot_sounds[0].source_handle == r.veh_h.packed);
	}
	// Reverse travel sounds the same cue at -0x80000.
	r.w.out.slot_sounds.clear();
	m.speed = -0x40000;
	m.tread_sound_prev_speed = -0x40000;
	r.w.logic_tick = 12;
	r.w.vehicles.update_tread_sound(r.veh(), t);
	CHECK(r.w.out.slot_sounds.size() == 1 && m.tread_sound_accum == 0);
	// `add eax, eax; sar eax, 1`: bit 31 of the sum is dropped.
	m.speed = 0x40000000;
	m.tread_sound_prev_speed = 0x40000000;
	m.tread_sound_accum = 0;
	r.w.out.slot_sounds.clear();
	r.w.logic_tick = 14;
	r.w.vehicles.update_tread_sound(r.veh(), t);
	CHECK(r.w.out.slot_sounds.empty() && m.tread_sound_accum == 0);
	// The motor tail runs it for every tank, crash-settled or not.
	Rig s;
	CHECK(s.w.tables.sound_profiles.parse(profile, sizeof(profile) - 1) == 1);
	s.veh().veh.settle_2f0 = 1;
	s.veh().veh.tread_sound_accum = 0x7FFFF;
	s.veh().veh.speed = 0;
	s.veh().veh.tread_sound_prev_speed = 1;
	s.w.logic_tick = 2;
	auto st = t;
	st.player_control = false;
	s.w.vehicles.tick_motor(s.veh(), st);
	bool tread = false;
	for (const auto &sound : s.w.out.slot_sounds)
		tread = tread || sound.slot == opennova::audio::kSlotDriveRepeat;
	CHECK(tread);
}

// The movement fold and its high-rev, skid and pivot sections run only on the
// last logic tick of an outer-loop batch; the tank's +0x328 store is inside the
// gate and the rev timer is not. [orig: Game_MainLoop @0x52BA24..0x52BA3A;
// Entity_UpdateTankVehiclePhysics `jz loc_48AD5B` @0x48AAA5]
void test_catch_up_ticks_skip_the_movement_fold() {
	Rig r;
	static constexpr char profile[] =
			"begin \"SP_Gate\"\n Soundloop_1 IDLE 1 1\n swivel_shift SHIFT\nend\n";
	CHECK(r.w.tables.sound_profiles.parse(profile, sizeof(profile) - 1) == 1);
	auto t = buggy_traits();
	t.family = VehicleFamily::Tank;
	t.sound_profile = "SP_Gate";
	r.mount();
	auto &m = r.veh().veh;
	m.wheel_rate_bam = 40000;
	m.steer_target_bam = 0x10000000;
	m.pivot_sound_prev_rate = 7;
	r.w.rules.last_tick_of_batch = false;
	const uint32_t rev = m.rev_sound_ticks;
	r.w.vehicles.update_ground_sound(r.veh(), t, false, false);
	r.w.vehicles.update_traction_sound(r.veh(), t);
	CHECK(r.w.out.sound_emitters.size() == 0);
	CHECK(r.w.out.slot_sounds.empty());
	CHECK(!m.pivot_sound_latched);
	CHECK(m.pivot_sound_prev_rate == 7);
	CHECK(m.rev_sound_ticks == rev + 1);
	r.w.rules.last_tick_of_batch = true;
	r.w.vehicles.update_ground_sound(r.veh(), t, false, false);
	r.w.vehicles.update_traction_sound(r.veh(), t);
	CHECK(r.w.out.sound_emitters.size() != 0);
	CHECK(m.pivot_sound_latched);
	CHECK(m.pivot_sound_prev_rate == 40000);
}

// A crash-settled tank skips the fold but keeps its registered lanes anchored
// on the hull until they expire. [orig: Entity_UpdateTankVehiclePhysics
// jump @0x48AABE]
void test_crash_settled_tank_keeps_lane_anchor() {
	Rig r;
	static constexpr char profile[] = "begin \"SP_Anchor\"\n Soundloop_1 IDLE 1 1\nend\n";
	CHECK(r.w.tables.sound_profiles.parse(profile, sizeof(profile) - 1) == 1);
	auto t = buggy_traits();
	t.family = VehicleFamily::Tank;
	t.sound_profile = "SP_Anchor";
	r.mount();
	r.w.vehicles.update_ground_sound(r.veh(), t, false, false); // registers idle
	CHECK(r.veh().veh.sound_anchor_until_tick != 0);
	r.veh().veh.settle_2f0 = 1;
	r.w.out.sound_emitters.clear();
	++r.w.logic_tick;
	r.veh().position.x += 1.0f;
	r.w.vehicles.update_ground_sound(r.veh(), t, false, false);
	CHECK(r.w.out.sound_emitters.size() == 1);
	if (r.w.out.sound_emitters.size() == 1) {
		CHECK(r.w.out.sound_emitters[0].source_only);
		CHECK(r.w.out.sound_emitters[0].pos.x == r.veh().position.x);
	}
	r.w.out.sound_emitters.clear();
	r.w.logic_tick += 40; // past the 30-tick keep-alive
	r.w.vehicles.update_ground_sound(r.veh(), t, false, false);
	CHECK(r.w.out.sound_emitters.size() == 0);
}

// The handbrake latch reads the +0x170 claimant rather than the input block's
// controller, so a predicting client latches it too.
// [orig: cveh `cmp [esi+170h], edx` @0x48C03C; cbik @0x4851E8]
void test_prediction_latches_handbrake_from_claimant() {
	Rig r;
	auto t = buggy_traits();
	t.hand_brake = 1;
	r.mount();
	r.w.ai.is_authority = false;
	auto &m = r.veh().veh;
	m.net_predicted = true;
	m.yaw_seeded = true;
	m.net_recv_speed = 16384;
	r.veh().flags |= 0x8u; // the lean-right mirror
	r.w.vehicles.ground_client_tick(r.veh(), t);
	CHECK(m.handbrake_latched == 1);
	CHECK(m.cmd_speed == 0);
}

void test_skid_effects_and_sound_edges() {
	Rig r;
	r.mount();
	r.drv().player_class = 0;
	auto t = buggy_traits();
	t.skid_effect = "dust";
	t.skid_snow_effect = "snow";
	t.skid_points.push_back({ { 65536, 0, 0 }, { 0, 65536, 0 } });
	auto &v = r.veh();
	auto &m = v.veh;
	m.yaw_seeded = true;
	m.yaw_bam = m.steer_target_bam = 0;
	v.yaw = 90;
	v.uniform_scale_q16 = 2 << 16;
	m.speed = m.cmd_speed = 20000;
	m.contact_direction[1] = 65536;
	r.w.logic_tick = 100;
	r.w.out.fire_sounds.set_listener(v.position);
	const Vec3 before = v.position;
	r.tick(1, t);
	CHECK(r.w.out.vehicle_effects.size() == 1);
	if (!r.w.out.vehicle_effects.empty()) {
		const auto &fx = r.w.out.vehicle_effects[0];
		CHECK(fx.effect == "dust" && fx.source_tick == 101);
		CHECK(std::abs(fx.position.x - (before.x + 2.0f)) < 0.001f);
		CHECK(std::abs(fx.position.y - before.y) < 0.001f);
		CHECK(std::abs(fx.direction.y - 2.0f) < 0.001f);
	}
	auto sounds = r.w.out.fire_sounds.drain();
	CHECK(sounds.size() == 1);
	if (!sounds.empty())
		CHECK(sounds[0].set_name == "TIRE_SKID");
	r.w.vehicles.update_traction_sound(v, t);
	CHECK(r.w.out.fire_sounds.drain().empty());
	v.flags |= kEntityFlagInAir;
	r.w.vehicles.update_traction_sound(v, t);
	CHECK(!m.skid_sound_latched);
	v.flags &= ~kEntityFlagInAir;

	// The ordinary-tick distance window closes at 300 units; the fourth
	// tick admits the same 400-unit emitter and selects its snow alternate.
	r.w.out.vehicle_effects.clear();
	r.w.out.fire_sounds.set_listener({ v.position.x + 400, v.position.y, v.position.z });
	r.w.logic_tick = 101;
	r.tick(1, t);
	CHECK(r.w.out.vehicle_effects.empty());
	uint8_t snow[4] = { 3, 3, 3, 3 };
	int grid[256];
	for (int &cell : grid)
		cell = 1;
	r.w.tables.surface_map.data = snow;
	r.w.tables.surface_map.width = r.w.tables.surface_map.height = 2;
	r.w.tables.surface_map.sector_grid = grid;
	r.w.logic_tick = 104;
	r.tick(1, t);
	CHECK(r.w.out.vehicle_effects.size() == 1);
	if (!r.w.out.vehicle_effects.empty())
		CHECK(r.w.out.vehicle_effects[0].effect == "snow");
	r.w.out.vehicle_effects.clear();
	m.movement_effects_disabled = true;
	r.tick(1, t);
	CHECK(r.w.out.vehicle_effects.empty());

	static constexpr char profile[] = "begin \"rev\"\n enginehighrev HIGH_REV\n end\n";
	CHECK(r.w.tables.sound_profiles.parse(profile, sizeof(profile) - 1) == 1);
	t.sound_profile = "rev";
	m.rev_sound_ticks = 124;
	m.plat_airborne_ticks = 31;
	r.w.out.slot_sounds.clear();
	r.w.vehicles.update_traction_sound(v, t);
	CHECK(r.w.out.slot_sounds.empty());
	r.w.vehicles.update_traction_sound(v, t);
	CHECK(m.rev_sound_ticks == 1);
	CHECK(r.w.out.slot_sounds.size() == 1);
	if (!r.w.out.slot_sounds.empty())
		CHECK(r.w.out.slot_sounds[0].slot == 33);
}

void test_vehicle_carrier_follow_and_refresh() {
	Rig r;
	CollisionWorld collision;
	Entity deck;
	deck.kind = EntityKind::Item;
	deck.item_id = 500;
	deck.has_item_def = true;
	deck.item_type = 1;
	deck.position = { 100.0f, 200.0f, 5.0f };
	deck.bound_radius = 30.0f;
	deck.alive = true;
	deck.yaw = 90;
	deck.veh.yaw_seeded = true;
	deck.veh.yaw_bam = 0;
	const EntityHandle parent_h = r.w.registry.spawn(1, deck);
	CHECK(parent_h.valid());
	Entity &parent = *r.w.registry.get(parent_h);
	collision.assign_entity(parent_h, collision.add_model(vehicle_sound_test_wall()));
	auto &v = r.veh();
	v.position = { 101.0f, 200.0f, 10.25f };
	v.bound_radius = 3.0f;
	v.veh.yaw_seeded = true;
	v.veh.yaw_bam = 0;
	v.health = 0;
	VehicleTraits t = buggy_traits();
	r.w.vehicles.traits.set(v.item_id, t);
	r.w.ai.collision = &collision;
	for (int i = 0; i < 17; ++i)
		collision.build_tick_tables(r.w);
	stamp_saved_live_pose(parent);
	// A ground mover refreshes its ground link on every eighth ENTITY UPDATE
	// (the entity-update counter), not on the tick.
	// [orig: Entity_UpdateVehiclePhysics @0x48AFB9, `test byte ptr
	//  g_entity_update_counter,7`]
	r.w.logic_tick = 0;
	r.w.entity_update_counter = 0;
	r.w.vehicles.tick_motor(v, t);
	CHECK(v.ground_target == parent_h);

	const float before_z = v.position.z;
	stamp_saved_live_pose(parent);
	parent.position.x += 3.0f;
	parent.position.y -= 2.0f;
	parent.position.z += 1.0f;
	parent.veh.yaw_bam = 0x40000000;
	r.w.logic_tick = 1;
	r.w.entity_update_counter = 1;
	r.w.vehicles.tick_motor(v, t);
	CHECK(std::abs(v.position.x - 103.0f) < 0.002f);
	CHECK(std::abs(v.position.y - 199.0f) < 0.002f);
	CHECK(std::abs(v.position.z - (before_z + 1.0f)) < 0.02f);
	CHECK(v.veh.yaw_bam == 0x40000000);

	stamp_saved_live_pose(parent);
	v.position.x += 100.0f;
	r.w.logic_tick = 9;
	r.w.entity_update_counter = 8;
	r.w.vehicles.tick_motor(v, t);
	CHECK(!v.ground_target.valid());
}

// A fresh brain has zero working speed. Boarding promotes 0 -> PRETTY at the
// mover head and hands back to FOLLOWWP, whose routeless think keeps it stopped.
// The class callback does not invoke movement-controller row 4 beforehand.
// [orig: Entity_UpdatePool1Slot @0x4B8E1B..0x4B8E53;
// Entity_UpdateVehiclePhysics @0x48AFAC..0x48AFB2 / @0x48BC16; D-AI-14]
void test_state0_brain_with_ai_driver_holds() {
	Rig r;
	r.mount();
	r.drv().player_class = 0; // an NPC driver: the AI-driver leg, not the player leg
	const VehicleTraits t = buggy_traits();
	r.w.vehicles.traits.set(r.veh().item_id, t);
	r.w.ai.is_authority = true;
	const int idx = r.w.ai.attach(r.veh_h);
	AiEntity &ae = *r.w.ai.at(idx);
	ae.brain.f[AiBrain::kCurState] = 0;
	ae.brain.f[AiBrain::kPendState] = 0;
	ae.brain.f[AiBrain::kFallback] = 0;
	ae.brain.f[AiBrain::kStep] = 16;
	ae.brain.f[AiBrain::kSpeedA] = 16019; // d_5ton combat speed 55 (x65536/225)
	ae.brain.f[AiBrain::kSpeedB] = 16019;
	ae.profile.type = 2;
	ae.profile.flags100 = 0x5; // FOLLOW_WP | FLEE — the d_5ton combat_flags
	const Vec3 start = r.veh().position;
	TickContext ctx{};
	ctx.world = &r.w;
	ctx.is_authority = true;
	r.w.update_all_entities(ctx);
	// One pass: 0 -> 22 at the head, then the AI leg's 22 -> 16 hand-back.
	CHECK(ae.brain.f[AiBrain::kCurState] == kAiGroundFollowWp);
	CHECK(ae.brain.f[AiBrain::kOutSpeed] == 0);
	CHECK(r.veh().veh.cmd_speed == 0);
	CHECK(r.veh().veh.speed == 0);
	for (int i = 1; i < 200; ++i) {
		ctx.logic_tick = static_cast<uint32_t>(i);
		r.w.update_all_entities(ctx);
	}
	const Vec3 late = r.veh().position;
	for (int i = 200; i < 224; ++i) {
		ctx.logic_tick = static_cast<uint32_t>(i);
		r.w.update_all_entities(ctx);
	}
	const float moved = std::hypot(r.veh().position.x - start.x, r.veh().position.y - start.y);
	const float moved_late = std::hypot(r.veh().position.x - late.x, r.veh().position.y - late.y);
	CHECK(ae.brain.f[AiBrain::kCurState] == kAiGroundFollowWp);
	CHECK(r.veh().veh.cmd_speed == 0); // no route: the row-16 tick froze the out-speed
	CHECK(r.veh().veh.speed == 0);
	CHECK(moved_late == 0.0f); // at rest: no sustained drive
	CHECK(moved == 0.0f); // no invented speed seed or boarding pulse
}

// A deck rider in a lower pool-1 slot than its carrier rides the carrier's
// CURRENT motor delta: the pool-1 walk visits a row's ground-entity chain
// first, so the carrier's mover runs before the rider's in the same pass.
// [orig: Entity_UpdateAllEntities @0x4C2188..0x4C21E9 (the +0x28 chain; the
//  Entity_UpdatePool1Slot calls @0x4C21E0 then @0x4C21E9)]
static void test_rider_below_its_carrier_follows_the_same_pass() {
    Rig r;
    const auto traits = buggy_traits();
    // The carrier moves up to slot 5; the rider takes slot 0 below it, on the
    // carrier's origin (so a carrier yaw cannot move it sideways).
    Entity carrier = r.veh();
    r.w.registry.despawn(r.veh_h);
    r.veh_h = r.w.registry.spawn_at(EntityHandle::make(1, 5), carrier);
    CHECK(r.veh_h.valid());
    Entity rider = carrier;
    rider.net_id = 201;
    rider.item_id = 1292;
    rider.seats.clear();
    rider.position.z = carrier.position.z + 2.0f;
    rider.ground_target = r.veh_h;
    const EntityHandle rider_h = r.w.registry.spawn(1, rider);
    CHECK(rider_h.slot() == 0);
    r.w.vehicles.traits.set(carrier.item_id, traits);
    r.w.vehicles.traits.set(rider.item_id, traits);
    auto &body = *r.w.ai.at(r.w.ai.attach(r.drv_h));
    body.inf.active = true;
    body.inf.is_local_player = true;
    body.health = r.drv().health;
    r.w.cached.local_player = r.drv_h;
    r.w.ai.is_authority = true;
    r.mount();
    LocalPlayer local(r.w);
    local.input.forward = true;
    TickContext ctx{};
    ctx.world = &r.w;
    ctx.is_authority = true;
    float travelled = 0.0f;
    for (uint32_t t = 1; t <= 40; ++t) {
        local.apply_player_input_pre_tick();
        const Vec3 c0 = r.veh().position;
        const Vec3 p0 = r.w.registry.get(rider_h)->position;
        ctx.logic_tick = t;
        r.w.logic_tick = t;
        r.w.update_all_entities(ctx);
        const Vec3 c1 = r.veh().position;
        const Vec3 p1 = r.w.registry.get(rider_h)->position;
        CHECK(std::fabs((c1.x - c0.x) - (p1.x - p0.x)) < 1e-3f);
        CHECK(std::fabs((c1.y - c0.y) - (p1.y - p0.y)) < 1e-3f);
        travelled += std::hypot(c1.x - c0.x, c1.y - c0.y);
    }
    CHECK(travelled > 1.0f);
}

// Input is packed before pool-1 motors; pool-0 body posing follows them.
// [orig: Player_PackInputStateToEntity @0x4DF450;
// Entity_UpdateAllEntities @0x4C2158 / @0x4C2426]
static void test_local_controls_reach_first_carrier_tick() {
    Rig r;
    const auto traits = buggy_traits();
    r.w.vehicles.traits.set(r.veh().item_id, traits);
    auto &body = *r.w.ai.at(r.w.ai.attach(r.drv_h));
    body.inf.active = true;
    body.inf.is_local_player = true;
    body.health = r.drv().health;
    r.w.cached.local_player = r.drv_h;
    r.w.ai.is_authority = true;
    r.mount();
    LocalPlayer local(r.w);
    constexpr int32_t heading = 0x23456789;
    local.input.forward = true;
    local.input.look_heading = heading;
    local.apply_player_input_pre_tick();
    TickContext ctx{};
    ctx.world = &r.w;
    ctx.is_authority = true;
    ctx.logic_tick = 1;
    r.w.update_all_entities(ctx);
    CHECK(r.veh().veh.cmd_speed == traits.player_speed);
    CHECK(r.veh().veh.steer_target_bam == heading);
    local.input.forward = false;
    local.apply_player_input_pre_tick();
    ++ctx.logic_tick;
    r.w.update_all_entities(ctx);
    CHECK(r.veh().veh.cmd_speed == 0); // release reaches this motor tick too
}

int main() {
    test_local_controls_reach_first_carrier_tick();
    test_rider_below_its_carrier_follows_the_same_pass();
	test_state0_brain_with_ai_driver_holds();
	test_vehicle_carrier_follow_and_refresh();
	test_tank_pivot_sound_latch_and_loop();
	test_tank_impulse_pushes_velocity();
	test_tank_crash_stop_needs_2ef();
	test_tank_crashed_vertical_velocity_arms();
	test_trails_sample_only_in_contact_arms();
	test_slope_factor_samples_quantized_table();
	test_tank_npc_parent_coast_keeps_raw_servo();
	test_tank_and_bike_ignore_physics_selector();
	test_prediction_keeps_player_control_tail();
	test_prediction_latches_handbrake_from_claimant();
	test_client_chase_family_gates();
	test_mover_prologue_stamps_saved_live_pose();
	test_tank_tread_sound_accumulates_even_ticks();
	test_catch_up_ticks_skip_the_movement_fold();
	test_crash_settled_tank_keeps_lane_anchor();
	test_skid_effects_and_sound_edges();
	test_handbrake_skid_and_grip_recovery();
	test_tank_pivot_retains_direction_and_previous_track_rate();
	test_engine_and_light_sound_edges();
	test_selector_zero_sound_tails();
	test_warning_cadence_by_family();
	test_family_health_cadence_and_submersion();
	test_every_mover_tail_sets_the_matrix_bit();
	test_damage_effects_release_on_respawn();
	test_zero_speed_displacement_and_submerged_sound();

	test_def_physics_scaling();
	test_def_decel_default();
	test_ctrl_register_projection();
    test_boarding_clears_the_scar_ring();
    test_drive_forward();
    test_reverse();
    test_turn_in_place_holds();
    test_steer_toward_driver_yaw();
	test_analog_driver_view_turn_and_freelook();
	test_ground_crash_and_settle_hold_yaw_rate();
	test_no_driver_coasts();
	test_stale_driver_publishes_control_stop();
    test_stale_claimant_stops_despite_second_controller();
    test_second_controller_departure_is_silent();
    test_gunner_first_claims_and_stops();
    test_multiple_stale_controls_publish_one_stop();
    test_dead_vehicle_holds();
    test_physics_selector_gate();
	test_simple_motors_and_contact();
	test_zero_player_speed_turns_at_min_rate();
	test_plat_terrain_probe_grazing_uses_wrapped_product();
	test_npc_claimant_registers_stationary_idle_sound();
	test_controller_without_claimant_is_silent();
	test_item_soundloop_override_wins_over_profile();
    test_forward_sound_gain_pitch_and_idle_crossfade();
    test_remote_speed_drives_the_same_movement_sound();
    test_zero_speed_row_emits_only_the_idle_lane();
    test_forward_sound_gear_pitch_sawtooth();
    test_reverse_sound_and_direction_shift_edges();
    test_claimant_detach_clears_motion_lanes_and_plays_stop();
    test_claimant_detach_releases_smoke_emitter();
    test_hull_collision_clears_motion_lanes_and_forces_full_idle();
	test_vehicle_impact_role_and_momentum_edge();
	test_claimant_detach_at_water_plane_clears_without_stop_oneshot();
	test_vehicle_sound_extreme_ints_are_saturating();
	test_helo_waits_for_its_boarders();
    if (failures == 0) std::printf("vehicle_motor_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
