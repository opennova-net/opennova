// Host-authoritative ground-vehicle motor (the retail-join v33 "can't drive" gap):
// items.def physics-block scaling, the drive input mapping, the steering chase, the
// speed pipeline, and the no-uplink drive-authority model — a mounted ctrl/drvr
// player's replicated input moves the vehicle on the authority.
// [orig: ItemDef_ParsePhysicsProperty @0x49d870; Entity_UpdateVehiclePhysics @0x48af00;
//  drive gate @0x48b0ff; net-re §5.13 drive-authority witness 2026-07-04]
#include "world/entity.h"
#include "world/vehicle_attach.h"
#include "world/vehicle_motor.h"
#include "world/world.h"

#include "def/def.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace opennova::world;

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
    void mount() { CHECK(entity_process_vehicle_attach(w, drv_h, veh_h, 1)); }
    void tick(int n, const VehicleTraits &t) {
        for (int i = 0; i < n; ++i) tick_vehicle_motor(w, veh(), t);
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
    entity_detach_from_vehicle(r.w, r.drv_h);
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
    r.w.effects.clear();
    r.w.registry.despawn(r.drv_h); // the vehicle-side seat handle now resolves to nothing

    r.tick(1, t);
    CHECK(!r.veh().seats[0].occupant.valid());
    CHECK(r.w.effects.entries().size() == 1);
    if (r.w.effects.entries().size() == 1) {
        const auto &stopped = r.w.effects.entries()[0];
        CHECK(stopped.kind == "vehicle_control_stopped");
        CHECK(stopped.a == 200);
        CHECK(stopped.b == 77);
        CHECK(stopped.c == 0x01000003);
        CHECK(stopped.d == r.veh_h.packed);
    }
    r.tick(1, t);
    CHECK(r.w.effects.entries().size() == 1); // cleared once, never repeated
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
    CHECK(entity_process_vehicle_attach(r.w, second, r.veh_h, 2));
    CHECK(r.w.effects.entries().size() == 1); // one started for the first claim only
    r.w.effects.clear();

    r.w.registry.despawn(r.drv_h); // the claimant goes away
    r.tick(1, t);
    CHECK(!r.veh().seats[0].occupant.valid());
    CHECK(r.veh().seats[1].occupant == second);
    CHECK(r.w.effects.entries().size() == 1);
    if (r.w.effects.entries().size() == 1)
        CHECK(r.w.effects.entries()[0].kind == "vehicle_control_stopped");
    CHECK(!r.veh().primary_occupant.valid());

    // The surviving second controller never re-claims in place...
    r.w.effects.clear();
    r.tick(1, t);
    CHECK(r.w.effects.entries().empty());

    // ...but a fresh attach does [orig: the +368 claim runs at attach time only].
    CHECK(entity_detach_from_vehicle(r.w, second));
    r.w.effects.clear();
    CHECK(entity_process_vehicle_attach(r.w, second, r.veh_h, 2));
    CHECK(r.w.effects.entries().size() == 1);
    if (r.w.effects.entries().size() == 1) {
        CHECK(r.w.effects.entries()[0].kind == "vehicle_control_started");
        CHECK(r.w.effects.entries()[0].d == r.veh_h.packed);
    }
}

// A non-claimant control occupant leaving publishes nothing; only the claimant's
// departure is the stop edge.
void test_second_controller_departure_is_silent() {
    Rig r;
    const VehicleTraits t = buggy_traits();
    const EntityHandle second = add_second_control_occupant(r);
    r.mount();
    CHECK(entity_process_vehicle_attach(r.w, second, r.veh_h, 2));
    r.w.effects.clear();

    CHECK(entity_detach_from_vehicle(r.w, second));
    r.tick(1, t);
    CHECK(r.w.effects.entries().empty());
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

    CHECK(entity_process_vehicle_attach(r.w, gunner, r.veh_h, 3));
    CHECK(r.veh().primary_occupant == gunner);
    CHECK(r.w.effects.entries().size() == 1);
    if (r.w.effects.entries().size() == 1)
        CHECK(r.w.effects.entries()[0].kind == "vehicle_control_started");
    r.w.effects.clear();

    r.mount(); // the driver arrives second: no re-claim, no event
    CHECK(r.veh().primary_occupant == gunner);
    CHECK(r.w.effects.entries().empty());

    CHECK(entity_detach_from_vehicle(r.w, gunner)); // the claimant leaves
    CHECK(r.w.effects.entries().size() == 1);
    if (r.w.effects.entries().size() == 1) {
        CHECK(r.w.effects.entries()[0].kind == "vehicle_control_stopped");
        CHECK(r.w.effects.entries()[0].d == r.veh_h.packed);
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
    CHECK(entity_process_vehicle_attach(r.w, second, r.veh_h, 2));
    r.w.effects.clear();

    r.w.registry.despawn(r.drv_h);
    r.w.registry.despawn(second);
    r.tick(1, t);
    CHECK(!r.veh().seats[0].occupant.valid());
    CHECK(!r.veh().seats[1].occupant.valid());
    CHECK(r.w.effects.entries().size() == 1);
    if (r.w.effects.entries().size() == 1)
        CHECK(r.w.effects.entries()[0].kind == "vehicle_control_stopped");
    r.tick(1, t);
    CHECK(r.w.effects.entries().size() == 1);
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

// physics 0 = the motor never runs [orig: Entity_DispatchPhysics_cveh @0x48efc7].
void test_physics_selector_gate() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.physics = 0;
    r.mount();
    r.drv().net_move_input = 0x08;
    r.tick(20, t);
    CHECK(r.veh().veh.speed == 0);
    const Vec3 p = r.veh().position;
    CHECK(p.x == 100.0f && p.y == 200.0f);
}

} // namespace

int main() {
    test_def_physics_scaling();
    test_def_decel_default();
    test_ctrl_register_projection();
    test_drive_forward();
    test_reverse();
    test_turn_in_place_holds();
    test_steer_toward_driver_yaw();
    test_no_driver_coasts();
    test_stale_driver_publishes_control_stop();
    test_stale_claimant_stops_despite_second_controller();
    test_second_controller_departure_is_silent();
    test_gunner_first_claims_and_stops();
    test_multiple_stale_controls_publish_one_stop();
    test_dead_vehicle_holds();
    test_physics_selector_gate();
    if (failures == 0) std::printf("vehicle_motor_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
