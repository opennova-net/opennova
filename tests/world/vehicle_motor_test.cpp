// Host-authoritative ground-vehicle motor (the retail-join v33 "can't drive" gap):
// items.def physics-block scaling, the drive input mapping, the steering chase, the
// speed pipeline, and the no-uplink drive-authority model — a mounted ctrl/drvr
// player's replicated input moves the vehicle on the authority.
// [orig: ItemDef_ParsePhysicsProperty @0x49d870; Entity_UpdateVehiclePhysics @0x48af00;
//  drive gate @0x48b0ff; net-re §5.13 drive-authority witness 2026-07-04]
#include "world/ai.h"
#include "world/angle.h"
#include "world/collision.h"
#include "world/entity.h"
#include "world/vehicle_attach.h"
#include "world/vehicle_motor.h"
#include "world/vehicle_sound.h"
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

void load_transport_sound_profile(World &world) {
    static constexpr char kTransportProfile[] =
            "begin \"SP_Transport\"\n"
            "  Soundloop_1 V_TRUCK_ILP .8 1.2\n"
            "  Soundloop_2 V_TRUCK_DLP .7 1.1\n"
            "  Soundloop_3 V_TRUCK_REVSE .8 1.2\n"
            "  enginestop V_TRUCK_STOP\n"
            "  enginereverse V_TRUCK_SHIFT\n"
            "end\n";
    CHECK(world.sound_profiles.parse(kTransportProfile,
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
    r.w.sound_emitters.clear(); // ignore mount-edge effects; inspect the motor tick

    VehicleDriveCmd parked;
    parked.ai_drive = true;
    parked.steer_target_bam = 0;
    parked.cmd_speed = 0;
    tick_vehicle_motor(r.w, r.veh(), t, &parked);

    CHECK(r.w.sound_emitters.size() == 1);
    if (r.w.sound_emitters.size() == 1) {
        const SoundEmitterEvent &idle = r.w.sound_emitters[0];
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
    CHECK(entity_process_vehicle_attach(r.w, second, r.veh_h, 2));
    CHECK(entity_detach_from_vehicle(r.w, r.drv_h));
    CHECK(!r.veh().primary_occupant.valid());
    CHECK(r.veh().seats[1].occupant == second);

    r.w.sound_emitters.clear();
    tick_vehicle_motor(r.w, r.veh(), t);
    CHECK(r.w.sound_emitters.empty());
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
    r.w.sound_emitters.clear();

    VehicleDriveCmd parked;
    parked.ai_drive = true;
    tick_vehicle_motor(r.w, r.veh(), t, &parked);

    CHECK(r.w.sound_emitters.size() == 1);
    if (r.w.sound_emitters.size() == 1) {
        CHECK(r.w.sound_emitters[0].slot == 0);
        CHECK(r.w.sound_emitters[0].set_name == "V_TRUCK_CUSTOM_IDLE");
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
    r.w.sound_emitters.clear();
    r.veh().veh.speed = 1000; // playerSpeed / 32

    update_ground_vehicle_sound(r.w, r.veh(), t, false, false);

    CHECK(r.w.sound_emitters.size() == 2);
    if (r.w.sound_emitters.size() == 2) {
        const SoundEmitterEvent &drive = r.w.sound_emitters[0];
        const SoundEmitterEvent &idle = r.w.sound_emitters[1];
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
    CHECK(r.w.sound_profiles.parse(kGearedProfile,
                                   sizeof(kGearedProfile) - 1) == 1);
    r.drv().player_class = 0;
    r.mount();
    r.w.sound_emitters.clear();
    r.veh().veh.speed = 10000; // gear 1, one quarter through its 8000 band

    update_ground_vehicle_sound(r.w, r.veh(), t, false, false);

    CHECK(!r.w.sound_emitters.empty());
    if (!r.w.sound_emitters.empty()) {
        CHECK(r.w.sound_emitters[0].lane == 10);
        CHECK(r.w.sound_emitters[0].pitch_q16 == 55757);
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
    r.w.sound_emitters.clear();
    r.w.slot_sounds.clear();
    r.veh().veh.speed = -16000;
    r.veh().veh.cmd_speed = -32000;

    update_ground_vehicle_sound(r.w, r.veh(), t, false, false);

    CHECK(r.w.sound_emitters.size() == 2);
    if (r.w.sound_emitters.size() == 2) {
        CHECK(r.w.sound_emitters[0].lane == 20);
        CHECK(r.w.sound_emitters[0].set_name == "V_TRUCK_REVSE");
        CHECK(r.w.sound_emitters[0].volume_q8_8 == 0xFFFF);
        CHECK(r.w.sound_emitters[0].pitch_q16 == 0x10000);
        CHECK(r.w.sound_emitters[1].lane == 0);
        CHECK(r.w.sound_emitters[1].volume_q8_8 == 0x8000);
    }
    CHECK(r.veh().veh.reverse_sound_latched);
    CHECK(r.w.slot_sounds.size() == 1);
    if (r.w.slot_sounds.size() == 1) {
        CHECK(r.w.slot_sounds[0].slot == 32);
        CHECK(std::strcmp(r.w.slot_sounds[0].set_name, "V_TRUCK_SHIFT") == 0);
    }

    r.w.slot_sounds.clear();
    update_ground_vehicle_sound(r.w, r.veh(), t, false, false);
    CHECK(r.w.slot_sounds.empty());
    r.veh().veh.cmd_speed = 32000;
    update_ground_vehicle_sound(r.w, r.veh(), t, false, false);
    CHECK(!r.veh().veh.reverse_sound_latched);
    CHECK(r.w.slot_sounds.size() == 1);
}

// The claimant detach edge immediately clears both moving lanes and plays the
// profile's engine-stop one-shot. Idle receives no refresh and retires through
// its existing 30-tick keep-alive.
// [orig: Entity_DetachFromVehicle @0x4356e9..0x43577c]
void test_claimant_detach_clears_motion_lanes_and_plays_stop() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.sound_profile = "SP_Transport";
    load_transport_sound_profile(r.w);
    r.w.vehicle_traits.set(r.veh().item_id, t);
    r.mount();
    update_ground_vehicle_sound(r.w, r.veh(), t, false, false);
    r.w.sound_emitters.clear();
    r.w.slot_sounds.clear();

    CHECK(entity_detach_from_vehicle(r.w, r.drv_h));

    CHECK(r.w.sound_emitters.size() == 2);
    if (r.w.sound_emitters.size() == 2) {
        CHECK(r.w.sound_emitters[0].lane == 10);
        CHECK(r.w.sound_emitters[0].pitch_q16 == 0);
        CHECK(r.w.sound_emitters[0].volume_q8_8 == 0);
        CHECK(r.w.sound_emitters[1].lane == 20);
        CHECK(r.w.sound_emitters[1].pitch_q16 == 0);
        CHECK(r.w.sound_emitters[1].volume_q8_8 == 0);
    }
    CHECK(r.w.slot_sounds.size() == 1);
    if (r.w.slot_sounds.size() == 1) {
        CHECK(r.w.slot_sounds[0].slot == 31);
        CHECK(std::strcmp(r.w.slot_sounds[0].set_name, "V_TRUCK_STOP") == 0);
    }

    // The idle lane was already drained into the host before detach. While its
    // keep-alive expires, later unoccupied motor ticks publish only the moved
    // entity anchor and do not renew any lane controls.
    r.w.sound_emitters.clear();
    r.veh().position.x += 5.0f;
    ++r.w.logic_tick;
    update_ground_vehicle_sound(r.w, r.veh(), t, false, false);
    CHECK(r.w.sound_emitters.size() == 1);
    if (r.w.sound_emitters.size() == 1) {
        const SoundEmitterEvent &anchor = r.w.sound_emitters[0];
        CHECK(anchor.source_only);
        CHECK(anchor.source_spawn_id == r.veh().registry_spawn_id);
        CHECK(anchor.pos.x == r.veh().position.x);
        CHECK(anchor.emitted_tick == r.w.logic_tick + 1);
        CHECK(anchor.pitch_q16 == 0);
        CHECK(anchor.volume_q8_8 == 0);
    }
}

// Hull collision is distinct from wreck/all-zero: it explicitly clears reverse
// then forward, but still refreshes idle at full volume for this tick.
// [orig: collision branch @0x5297db..0x529882]
void test_hull_collision_clears_motion_lanes_and_forces_full_idle() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.player_speed = 32000;
    t.sound_profile = "SP_Transport";
    load_transport_sound_profile(r.w);
    r.w.vehicle_traits.set(r.veh().item_id, t);
    r.drv().player_class = 0;
    r.mount();

    AiSystem ai;
    CollisionWorld collision;
    ai.collision = &collision;
    r.w.ai = &ai;
    r.w.registry.configure_pool(2, 4);

    Entity wall;
    wall.kind = EntityKind::Building;
    wall.position = {150.0f, 200.0f, 10.0f};
    wall.yaw = 90;
    wall.alive = true;
    wall.health = 30000;
    const EntityHandle wall_h = r.w.registry.spawn(2, wall);
    CHECK(wall_h.valid());
    collision.assign_entity(wall_h, collision.add_model(vehicle_sound_test_wall()));

    r.veh().position = {146.8f, 200.0f, 10.0f};
    r.veh().yaw = 90;
    r.veh().veh.yaw_seeded = false;
    r.veh().veh.speed = 1000;
    for (int i = 0; i < 17; ++i) collision.build_tick_tables(r.w);
    CHECK(collision.candidate_count(r.veh_h) == 1);

    VehicleDriveCmd drive;
    drive.ai_drive = true;
    drive.steer_target_bam = bam_heading_from_mission_yaw_deg(90.0);
    drive.cmd_speed = 1000;
    r.w.sound_emitters.clear();
    tick_vehicle_motor(r.w, r.veh(), t, &drive);

    CHECK(r.w.sound_emitters.size() == 3);
    if (r.w.sound_emitters.size() == 3) {
        const SoundEmitterEvent &reverse_clear = r.w.sound_emitters[0];
        const SoundEmitterEvent &forward_clear = r.w.sound_emitters[1];
        const SoundEmitterEvent &idle = r.w.sound_emitters[2];
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

// Claimant detach always clears keyed motion lanes, but slot 31 is strictly
// above-water. At the plane itself retail suppresses the one-shot.
// [orig: Entity_DetachFromVehicle @0x435716..0x43573e]
void test_claimant_detach_at_water_plane_clears_without_stop_oneshot() {
    Rig r;
    VehicleTraits t = buggy_traits();
    t.sound_profile = "SP_Transport";
    load_transport_sound_profile(r.w);
    r.w.vehicle_traits.set(r.veh().item_id, t);
    r.w.env.water_z = 10 << 16;
    r.mount();
    r.w.sound_emitters.clear();
    r.w.slot_sounds.clear();

    CHECK(entity_detach_from_vehicle(r.w, r.drv_h));
    CHECK(r.w.sound_emitters.size() == 2);
    if (r.w.sound_emitters.size() == 2) {
        CHECK(r.w.sound_emitters[0].pitch_q16 == 0);
        CHECK(r.w.sound_emitters[0].volume_q8_8 == 0);
        CHECK(r.w.sound_emitters[1].pitch_q16 == 0);
        CHECK(r.w.sound_emitters[1].volume_q8_8 == 0);
    }
    CHECK(r.w.slot_sounds.empty());
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
        r.w.sound_emitters.clear();
        r.veh().veh.speed = std::numeric_limits<int32_t>::min();

        update_ground_vehicle_sound(r.w, r.veh(), t, false, false);

        CHECK(r.w.sound_emitters.size() == 1);
        if (r.w.sound_emitters.size() == 1) {
            CHECK(r.w.sound_emitters[0].lane == 20);
            CHECK(r.w.sound_emitters[0].pitch_q16 == 52428);
            CHECK(r.w.sound_emitters[0].volume_q8_8 == 0xFFFF);
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
        CHECK(r.w.sound_profiles.parse(kExtremeProfile,
                                       sizeof(kExtremeProfile) - 1) == 1);
        r.mount();
        r.w.sound_emitters.clear();
        r.veh().veh.speed = std::numeric_limits<int32_t>::max();

        update_ground_vehicle_sound(r.w, r.veh(), t, false, false);

        CHECK(!r.w.sound_emitters.empty());
        if (!r.w.sound_emitters.empty()) {
            CHECK(r.w.sound_emitters[0].lane == 10);
            CHECK(r.w.sound_emitters[0].pitch_q16 == 52428);
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
        CHECK(r.w.sound_profiles.parse(kExtremeSpanProfile,
                                       sizeof(kExtremeSpanProfile) - 1) == 1);
        r.mount();
        r.w.sound_emitters.clear();
        r.veh().veh.speed = 0xFFFF;

        update_ground_vehicle_sound(r.w, r.veh(), t, false, false);

        CHECK(!r.w.sound_emitters.empty());
        if (!r.w.sound_emitters.empty()) {
            CHECK(r.w.sound_emitters[0].lane == 10);
            CHECK(r.w.sound_emitters[0].pitch_q16 ==
                  std::numeric_limits<int32_t>::max());
        }
    }
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
    test_npc_claimant_registers_stationary_idle_sound();
    test_controller_without_claimant_is_silent();
    test_item_soundloop_override_wins_over_profile();
    test_forward_sound_gain_pitch_and_idle_crossfade();
    test_forward_sound_gear_pitch_sawtooth();
    test_reverse_sound_and_direction_shift_edges();
    test_claimant_detach_clears_motion_lanes_and_plays_stop();
    test_hull_collision_clears_motion_lanes_and_forces_full_idle();
    test_claimant_detach_at_water_plane_clears_without_stop_oneshot();
    test_vehicle_sound_extreme_ints_are_saturating();
    if (failures == 0) std::printf("vehicle_motor_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
