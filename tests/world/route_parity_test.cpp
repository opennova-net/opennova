// AI route parity (the 2026-09-22 parity pass, route stream): the nav table the
// BMS loader builds, the helicopter AI leg's state words, the player pilot's
// PRETTY hold, and the vehicle/boat/aircraft avoid-brake gates.
//  - Mission_LoadBMSFile @0x40F4E0: the waypoint block and its normalization
//  - Entity_UpdateAircraftPhysics @0x490310: the parked/crewed/player legs
//  - the avoid-brake walks' ItemTypeIndex gates (ground, boat, aircraft)
#include <formats/def/def.h>
#include <formats/mission/bms.h>
#include <runtime/devtools/tick_profile.h>
#include <runtime/mission/item_traits.h>
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

// ItemTypeIndex is the items.def load-order ordinal of the FIRST row carrying
// the entity's type id, and 0 when no row does: each `begin` appends the next
// row and the lookup scans from row 0.
// [orig: Entity_SpawnFromBMSRecord `mov [esi+1Ch],ebp` @0x40EBFC;
//  ItemList_FindIndexByTypeId @0x49E100; ItemDef_AllocateWithDefaults bumps
//  gItemCount @0x49E3BE]
void test_item_type_index_is_the_first_row_ordinal() {
    static const char kItems[] = R"(begin "Null"
  id 100000
end

begin "Flyable"
  id 100172
end

begin "Truck"
  id 101294
end

begin "Flyable again"
  id 100172
end
)";
    def::DefItemsFile items = {};
    CHECK(def::def_parse_items_memory(reinterpret_cast<const uint8_t *>(kItems),
                                      sizeof(kItems) - 1, &items) == 0);
    CHECK(items.count == 4);
    auto wp = std::make_unique<World>();
    World &w = *wp;
    w.registry.configure_pool(1, 8);
    const auto spawn = [&](int32_t item_id) {
        Entity e{};
        e.kind = EntityKind::Item;
        e.alive = true;
        e.item_id = item_id;
        e.item_type_index = -1;
        return w.registry.spawn(1, e);
    };
    const EntityHandle null_h = spawn(0);
    const EntityHandle flyable_h = spawn(172);
    const EntityHandle truck_h = spawn(1294);
    const EntityHandle stray_h = spawn(4242);
    mission::resolve_item_traits(w, items, [](int) -> uint8_t { return 0; });
    CHECK(w.registry.get(null_h)->item_type_index == 0);
    CHECK(w.registry.get(flyable_h)->item_type_index == 1); // not the later duplicate
    CHECK(w.registry.get(truck_h)->item_type_index == 2);
    CHECK(w.registry.get(stray_h)->item_type_index == 0);
    def::def_free_items(&items);
}

// The avoid-brake walk admits a pool-1 neighbour by its ItemTypeIndex
// (entity+0x1C, the items.def load-order ordinal), and the families disagree:
// the ground walk skips index 0 only, while the boat and aircraft walks load 1
// into ecx for their count decrement and compare the index against that
// register, so they brake behind index-1 neighbours (the second items.def row)
// and nothing else.
// [orig: Entity_UpdateVehiclePhysics `cmp dword ptr [edi+1Ch],0` @0x48BDD9;
//  Entity_UpdateWatercraftPhysics `mov ecx,1` @0x48E5AA, `cmp [edi+1Ch],ecx`
//  @0x48E5C5; Entity_UpdateAircraftPhysics @0x4919D1 / @0x4919E6]
void test_avoid_brake_item_index_gates() {
    enum class Leg { Ground, Boat, Air };
    struct Case { Leg leg; int32_t index; bool brakes; };
    const Case cases[] = {
        {Leg::Ground, 0, false}, {Leg::Ground, 1, true}, {Leg::Ground, 5, true},
        {Leg::Boat, 0, false},   {Leg::Boat, 1, true},   {Leg::Boat, 5, false},
        {Leg::Air, 0, false},    {Leg::Air, 1, true},    {Leg::Air, 5, false},
    };
    for (const Case &c : cases) {
        AirRig r;
        r.seat_pilot();
        r.helo().bound_radius = 5.0f;
        r.helo().veh.yaw_bam = 0; // nose along +x
        r.helo().veh.yaw_seeded = true;
        AiBrain &b = r.brain();
        b.f[AiBrain::kOutSpeed] = 65536;
        // A hull 6 u dead ahead: the two 5 u footprints overlap.
        Entity rock{};
        rock.kind = EntityKind::Item;
        rock.alive = true;
        rock.item_id = 999;
        rock.item_type_index = c.index;
        rock.position = {6.0f, 0.0f, 50.0f};
        rock.bound_radius = 5.0f;
        rock.veh.yaw_seeded = true;
        r.w.registry.spawn(1, rock);
        // One brake: ((id + (frame << 8)) & 0x7FFF) + 0x4000 over 65536.
        const int32_t braked = static_cast<int32_t>(
                ((static_cast<uint32_t>(r.helo().net_id) +
                  (static_cast<uint32_t>(r.w.logic_tick) << 8)) & 0x7FFFu) + 0x4000u);
        int32_t speed = 0;
        if (c.leg == Leg::Air) {
            // Crewed, no route: the forward command is the out-speed straight
            // into the brake.
            b.f[AiBrain::kCurState] = 7;
            r.w.ai.chel_ai_drive(r.w, r.helo(), &r.pilot(), r.traits);
            speed = r.helo().veh.cmd_speed;
        } else {
            VehicleTraits t;
            t.player_speed = t.water_speed = 65536;
            t.player_control = true;
            b.f[AiBrain::kCurState] = 16;
            VehicleDriveCmd cmd;
            if (c.leg == Leg::Boat)
                r.w.ai.watercraft_ai_drive(r.w, r.helo(), &r.pilot(), t, cmd);
            else
                r.w.ai.vehicle_ai_drive(r.w, r.helo(), &r.pilot(), t, cmd);
            CHECK(cmd.ai_drive);
            speed = cmd.cmd_speed;
        }
        const int32_t expect = c.brakes ? braked : 65536;
        if (speed != expect) {
            std::printf("FAIL %s:%d  leg %d index %d: speed %d, want %d\n", __FILE__,
                        __LINE__, static_cast<int>(c.leg), c.index, speed, expect);
            ++failures;
        }
    }
}

} // namespace

int main() {
    test_single_node_lists_are_one_shot();
    test_air_leg_stamps_current_state_only();
    test_player_pilot_holds_pretty();
    test_item_type_index_is_the_first_row_ordinal();
    test_avoid_brake_item_index_gates();
    if (failures == 0) std::printf("route_parity: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
