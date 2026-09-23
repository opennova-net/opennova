// AI route parity (the 2026-09-22 parity pass, route stream): the nav table the
// BMS loader builds, the helicopter AI leg's state words, the player pilot's
// PRETTY hold, and the vehicle/boat/aircraft avoid-brake gates.
//  - Mission_LoadBMSFile @0x40F4E0: the waypoint block and its normalization
//  - Entity_UpdateAircraftPhysics @0x490310: the parked/crewed/player legs
#include <formats/mission/bms.h>
#include <runtime/devtools/tick_profile.h>
#include <runtime/mission/promote.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>

using namespace opennova;
using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

bms::Entity waypoint_marker(int32_t x, int32_t wp_distance) {
    bms::Entity e{};
    e.type = bms::ItemType::Marker;
    e.type_id = 6005; // "waypoint" (items.def 106005)
    e.x = x;
    e.wp_distance = wp_distance;
    return e;
}

// A record whose count is exactly 1 is one-shot whatever the author set: the
// loader ORs bit 0 into its flags right after the block read, so a unit that
// reaches the only node of such a list ends its route instead of looping on it.
// The team-route bits survive the OR.
// [orig: Mission_LoadBMSFile @0x40FB72..0x40FBB2; the mover's one-shot end
//  AI_UpdateWaypointMovement @0x457CAA..0x457D17]
void test_single_node_lists_are_one_shot() {
    bms::File m{};
    m.markers.push_back(waypoint_marker(100 << 16, 2));
    m.markers.push_back(waypoint_marker(300 << 16, 2));
    m.waypoint_records.resize(4);
    m.waypoint_records[1].flags = bms::WaypointFlags::None;
    m.waypoint_records[1].marker_count = 1;
    m.waypoint_records[1].waypoint_numbers = {0};
    m.waypoint_records[2].flags = bms::WaypointFlags::None;
    m.waypoint_records[2].marker_count = 2;
    m.waypoint_records[2].waypoint_numbers = {0, 1};
    m.waypoint_records[3].flags = bms::WaypointFlags::BlueTeam;
    m.waypoint_records[3].marker_count = 1;
    m.waypoint_records[3].waypoint_numbers = {1};

    auto wp = std::make_unique<World>();
    World &w = *wp;
    w.ai.is_authority = true;
    mission::promote_mission(m, w, mission::PromoteOptions{});
    const NavChannel *single = w.ai.nav.channel(1);
    const NavChannel *pair = w.ai.nav.channel(2);
    const NavChannel *blue = w.ai.nav.channel(3);
    CHECK(single != nullptr && single->loopflag == 1);
    CHECK(pair != nullptr && pair->loopflag == 0);
    CHECK(blue != nullptr && blue->loopflag == 3);

    // A vehicle brain standing on the only node of list 1 ends its route: the
    // waypoint type drops to 0 and the mover stops, where a looping list
    // would keep the node as its target at speed.
    w.registry.configure_pool(1, 4);
    Entity hull{};
    hull.kind = EntityKind::Item;
    hull.alive = true;
    hull.health = 100;
    const EntityHandle h = w.registry.spawn(1, hull);
    AiEntity &ai = *w.ai.at(w.ai.attach(h));
    ai.pos[0] = 100 << 16;
    AiBrain &b = ai.brain;
    b.f[AiBrain::kCurState] = 16;
    b.f[AiBrain::kSpeedB] = 4096;
    b.f[AiBrain::kStep] = 16;
    b.f[AiBrain::kWpType] = 1;
    b.f[AiBrain::kWpChannel] = 1;
    b.f[AiBrain::kWpNode] = 0;
    w.ai.update_waypoint_movement(ai, w);
    CHECK(b.f[AiBrain::kWpType] == 0);
    CHECK(b.f[AiBrain::kWpNode] == 0);
    CHECK(b.f[AiBrain::kOutSpeed] == 0);
}

struct AirRig {
    std::unique_ptr<World> wp = std::make_unique<World>();
    World &w = *wp;
    EntityHandle helo_h, pilot_h;
    VehicleTraits traits;

    AirRig() {
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 4);
        w.ai.is_authority = true;
        Entity helo{};
        helo.kind = EntityKind::Item;
        helo.item_id = 0x07DA; // Blackhawk wire type (items.def 102010)
        helo.alive = true;
        helo.health = 100;
        helo.position = {0.0f, 0.0f, 50.0f};
        Seat ctrl;
        ctrl.type = SeatType::Controller;
        helo.seats.push_back(ctrl);
        helo_h = w.registry.spawn(1, helo);
        w.ai.attach(helo_h);
        Entity pilot{};
        pilot.kind = EntityKind::Organic;
        pilot.alive = true;
        pilot.health = 100;
        pilot.position = {0.0f, 0.0f, 50.0f};
        pilot_h = w.registry.spawn(0, pilot);
        traits.family = VehicleFamily::Helicopter;
        traits.player_control = true;
    }
    Entity &helo() { return *w.registry.get(helo_h); }
    Entity &pilot() { return *w.registry.get(pilot_h); }
    AiBrain &brain() { return w.ai.for_handle(helo_h)->brain; }

    // Seat the pilot in the controller seat the way the attach leaves it.
    void seat_pilot() {
        pilot().mounted = true;
        pilot().mount_target = helo_h;
        pilot().mount_type = SeatType::Controller;
        helo().seats[0].occupant = pilot_h;
        helo().primary_occupant = pilot_h;
    }
};

// The aircraft AI leg writes the CURRENT state word only. Parked (no occupant,
// or Flags & 0x10000002) it stamps PRETTY 14 and leaves the pending word
// alone; crewed it hands 14 back as FOLLOWWP 7, again on the current word
// only, so the machine keeps committing its pending word after each think.
// There is no hull or pilot health term in the parked test.
// [orig: Entity_UpdateAircraftPhysics — parked gate @0x490F16..0x490F25, the
//  parked stamp @0x491C66, the crewed hand-back @0x49158A..0x491590]
void test_air_leg_stamps_current_state_only() {
    AirRig r;
    AiBrain &b = r.brain();

    // Crewed: 14 -> 7, the class init's pending 0 untouched.
    b.f[AiBrain::kCurState] = 14;
    b.f[AiBrain::kPendState] = 0;
    r.w.ai.chel_ai_drive(r.w, r.helo(), &r.pilot(), r.traits);
    CHECK(b.f[AiBrain::kCurState] == 7);
    CHECK(b.f[AiBrain::kPendState] == 0);

    // A crewed hull at zero health whose dead bit is not up keeps the AI leg.
    r.helo().health = 0;
    b.f[AiBrain::kCurState] = 14;
    b.f[AiBrain::kPendState] = 13;
    r.w.ai.chel_ai_drive(r.w, r.helo(), &r.pilot(), r.traits);
    CHECK(b.f[AiBrain::kCurState] == 7);
    CHECK(b.f[AiBrain::kPendState] == 13);
    r.helo().health = 100;

    // Parked (no occupant): PRETTY on the current word, the pended order kept.
    b.f[AiBrain::kCurState] = 7;
    b.f[AiBrain::kPendState] = 10;
    r.w.ai.chel_ai_drive(r.w, r.helo(), nullptr, r.traits);
    CHECK(b.f[AiBrain::kCurState] == 14);
    CHECK(b.f[AiBrain::kPendState] == 10);

    // The dead bit parks a crewed hull, and the stamp still lands.
    r.helo().flags |= kEntityFlagDead;
    b.f[AiBrain::kCurState] = 7;
    b.f[AiBrain::kPendState] = 15;
    r.w.ai.chel_ai_drive(r.w, r.helo(), &r.pilot(), r.traits);
    CHECK(b.f[AiBrain::kCurState] == 14);
    CHECK(b.f[AiBrain::kPendState] == 15);
}

// A player in the pilot seat parks the aircraft brain at PRETTY every visit, so
// an AI state left by an earlier AI pilot stops running; a player pilot whose
// eye is under the water plane hands the hull to the AI leg instead.
// [orig: Entity_UpdateAircraftPhysics — the player test `test [ebp+24h],100h`
//  @0x490F36, the eye test @0x490F3F..0x490F4B, the player-leg stamp
//  `mov dword ptr [ebx+10h],0Eh` @0x490F6A]
void test_player_pilot_holds_pretty() {
    AirRig r;
    r.pilot().player_class = 1;
    r.seat_pilot();
    r.w.vehicles.traits.set(r.helo().item_id, r.traits);
    AiBrain &b = r.brain();
    devtools::ProfileLap lap(r.w.profile);

    b.f[AiBrain::kCurState] = 8; // left in combat by an earlier AI pilot
    r.w.vehicles.tick_motors(true, lap);
    CHECK(b.f[AiBrain::kCurState] == 14);

    // Eye under the water plane: the AI leg runs and hands PRETTY back.
    r.w.env.water_z = to_fixed(60.0f);
    r.pilot().eye_offset_z = to_fixed(1.5f);
    b.f[AiBrain::kCurState] = 14;
    r.w.vehicles.tick_motors(true, lap);
    CHECK(b.f[AiBrain::kCurState] == 7);
}

} // namespace

int main() {
    test_single_node_lists_are_one_shot();
    test_air_leg_stamps_current_state_only();
    test_player_pilot_holds_pretty();
    if (failures == 0) std::printf("route_parity: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
