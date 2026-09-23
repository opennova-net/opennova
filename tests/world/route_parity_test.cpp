// AI route parity (the 2026-09-22 parity pass, route stream): the nav table the
// BMS loader builds, the helicopter AI leg's state words, the player pilot's
// PRETTY hold, and the vehicle/boat/aircraft avoid-brake gates.
//  - Mission_LoadBMSFile @0x40F4E0: the waypoint block and its normalization
//  - Entity_UpdateAircraftPhysics @0x490310: the parked/crewed/player legs
//  - the avoid-brake walks' ItemTypeIndex gates (ground, boat, aircraft)
//  - the unbounded nav reads, the marker fields by type, the verbatim route seed
//  - the aircraft parked spin, the flight floor's fresh sample, the ground-link
//    refresh of every ground sample, the boat slip bearing's truncation
#include <formats/def/def.h>
#include <formats/mission/bms.h>
#include <runtime/devtools/tick_profile.h>
#include <runtime/mission/collision_resolve.h>
#include <runtime/mission/item_traits.h>
#include <runtime/mission/promote.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <vector>

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
    b.f[AiBrain::kCurState] = 8; // left in combat by an earlier AI pilot
    r.w.vehicles.update_motor(r.helo(), true);
    CHECK(b.f[AiBrain::kCurState] == 14);

    // Eye under the water plane: the AI leg runs and hands PRETTY back.
    r.w.env.water_z = to_fixed(60.0f);
    r.pilot().eye_offset_z = to_fixed(1.5f);
    b.f[AiBrain::kCurState] = 14;
    r.w.vehicles.update_motor(r.helo(), true);
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
    // The shared by-id lookup the seat specs and the model probes use resolves
    // the same first row [orig: ItemList_FindIndexByTypeId @0x49E100].
    CHECK(mission::find_item_def(items, 100172) == &items.entries[1]);
    CHECK(mission::find_item_def(items, 104242) == nullptr);
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
        // One brake: ((id + (counter << 8)) & 0x7FFF) + 0x4000 over 65536,
        // keyed on the entity-update counter, which here differs from the tick.
        // [orig: Entity_UpdateWatercraftPhysics @0x48E712,
        //  Entity_UpdateAircraftPhysics @0x491B2D, Entity_UpdateVehiclePhysics
        //  @0x48BF26 (the dword_24C1948 reads)]
        r.w.entity_update_counter = 5;
        const int32_t braked = static_cast<int32_t>(
                ((static_cast<uint32_t>(r.helo().net_id) +
                  (r.w.entity_update_counter << 8)) & 0x7FFFu) + 0x4000u);
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

// A mission whose waypoint block exercises the unbounded reads: list 1 counts 34
// over 32 slots naming marker 1, so its nodes 32 and 33 are list 2's flags and
// count words (0 and 3); list 3 counts 2 but carries a stray slot word 3 at slot
// 5; list 4's second node names pool-3 slot 40, which no marker fills. Marker 3
// sits at x = 300 u, marker 0 at the origin.
bms::File unbounded_routes_mission() {
    bms::File m{};
    for (int i = 0; i < 4; ++i) m.markers.push_back(waypoint_marker((100 * i) << 16, 1 + i));
    m.waypoint_records.resize(5);
    bms::WaypointRecord &long_list = m.waypoint_records[1];
    long_list.flags = bms::WaypointFlags::None;
    long_list.marker_count = 34;
    long_list.waypoint_numbers.assign(32, 1u);
    bms::WaypointRecord &next = m.waypoint_records[2];
    next.flags = bms::WaypointFlags::None;
    next.marker_count = 3;
    next.waypoint_numbers = {2, 2, 2};
    next.padding.assign(128 - 3 * 4, 0);
    bms::WaypointRecord &stray = m.waypoint_records[3];
    stray.flags = bms::WaypointFlags::None;
    stray.marker_count = 2;
    stray.waypoint_numbers = {1, 2};
    stray.padding.assign(128 - 2 * 4, 0);
    stray.padding[(5 - 2) * 4] = 3; // slot 5 = 3, little-endian
    bms::WaypointRecord &hole = m.waypoint_records[4];
    hole.flags = bms::WaypointFlags::None;
    hole.marker_count = 2;
    hole.waypoint_numbers = {1, 40};
    return m;
}

// The loader keeps each record's raw count and all 32 slot words, and the movers
// index the block flat with no bound on the node: a count past 32 walks on into
// the next record's words, a start node past the count reads the raw slot word,
// and a slot no marker fills resolves to the zeroed pool-3 entry at the origin
// (radius 0) instead of failing the refresh.
// [orig: Mission_LoadBMSFile fread(Buffer, 0x88, 0x80) @0x40FB56;
//  AIWaypoint_UpdateTarget @0x457469..0x457481; Entity_FindNearestTriggerByType
//  @0x407F0B..0x407F6B; Pool_GetEntryUnchecked @0x441FC0]
void test_nav_reads_are_unbounded() {
    const bms::File m = unbounded_routes_mission();
    auto wp = std::make_unique<World>();
    World &w = *wp;
    w.ai.is_authority = true;
    mission::promote_mission(m, w, mission::PromoteOptions{});
    const NavNodeTable &nav = w.ai.nav;
    CHECK(nav.channel(1) != nullptr && nav.channel(1)->count == 34);
    CHECK(nav.entry_index(1, 31) == 1);
    CHECK(nav.entry_index(1, 32) == 0); // list 2's flags word
    CHECK(nav.entry_index(1, 33) == 3); // list 2's count word
    CHECK(nav.entry_index(3, 5) == 3);  // the stray slot word past list 3's count
    CHECK(nav.entry_index(4, 1) == 40);
    CHECK(nav.entry_index(4, 34) == 0); // past the block

    w.registry.configure_pool(1, 4);
    Entity hull{};
    hull.kind = EntityKind::Item;
    hull.alive = true;
    const EntityHandle h = w.registry.spawn(1, hull);
    AiEntity &ai = *w.ai.at(w.ai.attach(h));

    // Standing on marker 3, the nearest node of list 1 is its node 33.
    ai.pos[0] = 300 << 16;
    CHECK(w.ai.nearest_route_node(ai, 1) == 33);
    // At the origin, list 4's unfilled slot 40 measures zero: node 1.
    ai.pos[0] = 0;
    ai.pos[1] = 10 << 16;
    CHECK(w.ai.nearest_route_node(ai, 4) == 1);

    AiBrain &b = ai.brain;
    b.f[AiBrain::kWpType] = 1;
    b.f[AiBrain::kWpChannel] = 1;
    b.f[AiBrain::kWpNode] = 33;
    CHECK(ai_waypoint_update_target(b, ai.pos, nav) == 0);
    CHECK(b.f[AiBrain::kWpResolved] == 3);
    CHECK(b.f[AiBrain::kWpNodeVal] == (4 << 16)); // marker 3's wp_distance 4
    b.f[AiBrain::kWpChannel] = 3;
    b.f[AiBrain::kWpNode] = 5;
    CHECK(ai_waypoint_update_target(b, ai.pos, nav) == 0);
    CHECK(b.f[AiBrain::kWpResolved] == 3);
    b.f[AiBrain::kWpChannel] = 4;
    b.f[AiBrain::kWpNode] = 1;
    CHECK(ai_waypoint_update_target(b, ai.pos, nav) == 0);
    CHECK(b.f[AiBrain::kWpResolved] == 40);
    CHECK(b.f[AiBrain::kWpNodeVal] == 0);
    CHECK(b.f[AiBrain::kWpExtra] == 0);
}

// entity+0 is the arrival radius only for the waypoint (6005), KOTH centre (6006)
// and named-location (2044) markers, and the hold word only for the waypoint:
// any other marker keeps the spawn memset's zeros. The facing is the spawn angle,
// ((90 - yaw) << 16) / 360 truncated, then << 16.
// [orig: Entity_SpawnFromBMSRecord — hold @0x40F066..0x40F08D, radius
//  @0x40F096..0x40F0A4 / @0x40F157..0x40F16D / @0x40F213..0x40F221, the memset
//  @0x40EA27, the angle @0x40EB42..0x40EB66]
void test_marker_fields_follow_the_marker_type() {
    struct Row { int32_t type_id, wp_distance; int16_t spawns, yaw; int32_t radius, wait, facing; };
    const Row rows[] = {
        {6005, 3, 2, 0, 3 << 16, 124, 0x40000000},
        {6006, 0, 2, 45, 0x8000, 0, 0x20000000},
        {2044, 4, 5, 200, 4 << 16, 0, -20024 * 65536},
        {2043, 7, 9, 90, 0, 0, 0},
        {6001, 5, 3, -90, 0, 0, static_cast<int32_t>(0x80000000u)},
    };
    bms::File m{};
    for (const Row &r : rows) {
        bms::Entity e = waypoint_marker(0, r.wp_distance);
        e.type_id = r.type_id;
        e.spawns = r.spawns;
        e.yaw = r.yaw;
        m.markers.push_back(e);
    }
    auto wp = std::make_unique<World>();
    mission::promote_mission(m, *wp, mission::PromoteOptions{});
    const NavNodeTable &nav = wp->ai.nav;
    CHECK(nav.nodes.size() == std::size(rows));
    for (size_t i = 0; i < std::size(rows) && i < nav.nodes.size(); ++i) {
        const NavEntry &n = nav.nodes[i];
        if (n.f[0] != rows[i].radius || n.wait_ticks != rows[i].wait ||
            n.f[4] != rows[i].facing) {
            std::printf("FAIL %s:%d  marker type %d: radius %d wait %d facing %d\n",
                        __FILE__, __LINE__, rows[i].type_id, n.f[0], n.wait_ticks, n.f[4]);
            ++failures;
        }
    }
}

// A vehicle's class init copies the slot's route words into the brain verbatim:
// no count test and no clamp, so a route onto an empty list or past the count is
// kept as authored. An organic's heading is the same truncated spawn angle.
// [orig: Entity_InitVehicleAIFromDef @0x46885E..0x46887F;
//  Entity_InitHelicopterAIFromDef @0x46852D..0x468552;
//  Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66]
void test_route_seed_is_the_slot_words() {
    constexpr int32_t kTruck = 1294;
    bms::File m = unbounded_routes_mission();
    m.waypoint_records.resize(27); // list 26 stays unauthored (count 0)
    bms::Entity parked{};
    parked.type = bms::ItemType::Item;
    parked.type_id = kTruck;
    parked.waypoint_id = 26;
    parked.wp_number = 7;
    m.items.push_back(parked);
    bms::Entity late{};
    late.type = bms::ItemType::Item;
    late.type_id = kTruck;
    late.waypoint_id = 2;
    late.wp_number = 9;
    m.items.push_back(late);
    bms::Entity soldier{};
    soldier.type = bms::ItemType::Organic;
    soldier.type_id = 2072;
    m.organics.push_back(soldier);
    mission::PromoteOptions opts;
    mission::ItemSeatSpec spec;
    spec.type_id = kTruck;
    Seat ctrl;
    ctrl.type = SeatType::Controller;
    spec.seats.push_back(ctrl);
    opts.item_seat_specs.push_back(spec);
    // A brain exists only for a def with the AI-class attrib and a brain-class
    // ai_function row [orig: Entity_SpawnFromBMSRecord @0x40ED4E; the class
    // inits' slot gates @0x46848E / @0x46878D].
    opts.ai_profile_defaults = [kTruck](int32_t type) {
        mission::PromoteOptions::AiProfileDefaults d;
        d.known = type == kTruck;
        return d;
    };
    opts.item_attributes = [kTruck](int32_t type) { return type == kTruck ? kItemAttribAIData : 0u; };
    auto wp = std::make_unique<World>();
    World &w = *wp;
    w.ai.is_authority = true;
    mission::promote_mission(m, w, opts);
    const AiEntity *first = w.ai.for_handle(EntityHandle::make(1, 0));
    const AiEntity *second = w.ai.for_handle(EntityHandle::make(1, 1));
    CHECK(first != nullptr && second != nullptr);
    if (first == nullptr || second == nullptr) return;
    CHECK(first->brain.f[AiBrain::kWpType] == 1);
    CHECK(first->brain.f[AiBrain::kWpChannel] == 26);
    CHECK(first->brain.f[AiBrain::kWpNode] == 7);
    CHECK(second->brain.f[AiBrain::kWpChannel] == 2);
    CHECK(second->brain.f[AiBrain::kWpNode] == 9);
    const AiEntity *body = w.ai.for_handle(EntityHandle::make(0, 0));
    CHECK(body != nullptr && body->heading == 0x40000000);
}

// A flat terrain column `units` high (raw16 = units * 256).
struct FlatField {
    static constexpr int kDim = 512;
    std::vector<uint16_t> heightmap;
    std::vector<int> sector_grid;
    terrain::TerrainHeightField field;
    explicit FlatField(int units)
        : heightmap(kDim * kDim, static_cast<uint16_t>(units * 256)), sector_grid(256, 1) {
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
    }
};

// An uncrewed hull still airborne more than 2 u over its cached ground (Z less
// the probe offset) yaws 2886390 BAM per visit and pins the turned heading;
// grounded, or within 2 u, it holds its yaw.
// [orig: Entity_UpdateAircraftPhysics `test dword ptr [esi+24h],2000h`
//  @0x491C09, the 2 u test @0x491C18..0x491C28, `add dword ptr [esi+10h],
//  0FFD3F50Ah` @0x491C2A, brain+0x210 = Yaw @0x491C54]
void test_parked_airborne_hull_spins() {
    struct Case { bool in_air; int32_t z, probe_off; bool spins; };
    const Case cases[] = {
        {true, 50 << 16, 0, true},
        {false, 50 << 16, 0, false},
        {true, 12 << 16, 0, false},       // exactly 2 u over the ground
        {true, (12 << 16) + 1, 0, true},
        {true, (12 << 16) + 1, 1, false}, // the probe offset comes off Z
    };
    for (const Case &c : cases) {
        AirRig r;
        Entity &helo = r.helo();
        helo.veh.yaw_bam = 0x10000000;
        helo.veh.yaw_seeded = true;
        helo.veh.ground_cache = 10 << 16;
        helo.veh.air_probe_z_off = c.probe_off;
        if (c.in_air) helo.flags |= kEntityFlagInAir;
        r.w.ai.for_handle(r.helo_h)->pos[2] = c.z;
        r.w.ai.chel_ai_drive(r.w, helo, nullptr, r.traits);
        const int32_t want = c.spins ? 0x10000000 - 2886390 : 0x10000000;
        CHECK(helo.veh.yaw_bam == want);
        CHECK(helo.veh.steer_target_bam == want);
        CHECK(r.brain().f[AiBrain::kCurState] == 14);
    }
}

// The flight block's floor (ground + bound/4) is a FRESH radius-0 sample of the
// hull's own ground at its current Z; the 8-tick cached ground feeds only tz,
// the climb and the landing target. Over 10 u terrain with a stale 5 u cache, a
// hull at 11.5 u with bound 8 u is under its 12 u floor and lands onto the node.
// [orig: Entity_UpdateAircraftPhysics — the self sample
//  Entity_CalcAverageGroundHeight @0x491845 into ebp @0x491853, the floor
//  `sar eax,2; add eax,ebp` @0x491874, the landing leg @0x4918B0..0x4918DA]
void test_flight_floor_is_the_fresh_sample() {
    FlatField terrain(10);
    AirRig r;
    r.w.tables.terrain = &terrain.field;
    r.seat_pilot();
    Entity &helo = r.helo();
    const int32_t z = (23 << 16) / 2; // 11.5 u
    helo.position = {100.0f, 20.0f, 11.5f};
    helo.bound_radius = 8.0f;
    helo.veh.yaw_bam = 0;
    helo.veh.yaw_seeded = true;
    helo.veh.ground_cache = 5 << 16;
    AiEntity &ai = *r.w.ai.for_handle(r.helo_h);
    ai.pos[0] = 100 << 16;
    ai.pos[1] = 20 << 16;
    ai.pos[2] = z;
    NavNodeTable &nav = r.w.ai.nav;
    nav.nodes.resize(1);
    nav.nodes[0].f[1] = 102 << 16;
    nav.nodes[0].f[2] = 20 << 16;
    nav.nodes[0].f[3] = 10 << 16;
    nav.channels.resize(2);
    nav.channels[1].count = 1;
    AiBrain &b = r.brain();
    b.f[AiBrain::kCurState] = 7;
    b.f[AiBrain::kWpType] = 1;
    b.f[AiBrain::kWpChannel] = 1;
    b.f[AiBrain::kWpNode] = 0;
    b.f[AiBrain::kOutSpeed] = 65536;
    r.w.ai.chel_ai_drive(r.w, helo, &r.pilot(), r.traits);
    CHECK(helo.veh.cmd_speed == 8192);                        // an eighth
    CHECK(ai.pos[0] == (100 << 16) + ((2 << 16) >> 6));       // a 64th onto the node
    CHECK(helo.veh.net_alt_target == (5 << 16) - 0x2000);     // parked on the CACHE
}

// Every Entity_CalcAverageGroundHeight sample refreshes the hull's ground link:
// the east and centre taps are the AndObject ray kind, which stores its hit
// entity (null on a miss), so the centre hit is the link that survives.
// [orig: Entity_CalcAverageGroundHeight taps @0x4572A1 (east) and @0x4572E0
//  (centre), the radius-0 arm @0x45735D; Entity_RaycastGroundHeightAndObject
//  `mov [esi+28h],eax` @0x414370]
void test_ground_samples_refresh_the_link() {
    FlatField terrain(0);
    CollisionWorld cw;
    cw.terrain = &terrain.field;
    AirRig r;
    r.w.registry.configure_pool(2, 4);
    // Pool-2 slot 0 packs to handle 0, the "no hit" value: keep the deck off it.
    Entity filler{};
    filler.kind = EntityKind::Marker;
    r.w.registry.spawn(2, filler);
    Entity deck{};
    deck.kind = EntityKind::Building;
    deck.position = {10.0f, 10.0f, 0.0f};
    deck.yaw = 90; // engine heading 0
    deck.alive = true;
    const EntityHandle deck_h = r.w.registry.spawn(2, deck);
    // A 3 u high, 4 u square type-1 box.
    CollisionModel box;
    const auto plane = [&](int nx, int ny, int nz, int32_t d) {
        CollisionPlane p;
        p.nx = static_cast<int16_t>(nx);
        p.ny = static_cast<int16_t>(ny);
        p.nz = static_cast<int16_t>(nz);
        p.dist = d;
        box.planes.push_back(p);
    };
    plane(16384, 0, 0, -(2 << 16));
    plane(-16384, 0, 0, -(2 << 16));
    plane(0, 16384, 0, -(2 << 16));
    plane(0, -16384, 0, -(2 << 16));
    plane(0, 0, 16384, -(3 << 16));
    plane(0, 0, -16384, 0);
    CollisionVolume v;
    v.type = 1;
    v.min_x = v.min_y = -(2 << 16);
    v.max_x = v.max_y = 2 << 16;
    v.max_z = 3 << 16;
    v.plane_count = 6;
    box.volumes.push_back(v);
    CollisionSection s;
    s.volume_count = 1;
    box.sections.push_back(s);
    cw.assign_entity(deck_h, cw.add_model(std::move(box)));
    r.w.ai.collision = &cw;

    Entity &helo = r.helo();
    helo.has_item_def = true; // a pool-1 candidate source needs its ItemDef
    helo.position = {10.0f, 10.0f, 5.0f};
    helo.bound_radius = 1.0f;
    for (int i = 0; i < 17; ++i) cw.build_tick_tables(r.w);
    AiEntity &ai = *r.w.ai.for_handle(r.helo_h);
    CHECK(r.w.ai.aircraft_ground_height(r.w, ai, 0) == (3 << 16));
    CHECK(helo.ground_target == deck_h);
    helo.ground_target = {};
    CHECK(r.w.ai.aircraft_ground_height(r.w, ai, 0x10000) == (3 << 16));
    CHECK(helo.ground_target == deck_h);
    // Off the deck the centre ray finds terrain only: the link clears.
    helo.position = {30.0f, 10.0f, 5.0f};
    for (int i = 0; i < 17; ++i) cw.build_tick_tables(r.w);
    r.w.ai.aircraft_ground_height(r.w, ai, 0);
    CHECK(!helo.ground_target.valid());
    r.w.ai.collision = nullptr;
}

// The boat's slip bearing truncates through _ftol2_sse. Velocity (1.0, 0.188)
// u/tick puts it at 127271671.84 BAM; with the hull 10431 BAM ahead of the
// truncated bearing, sin22 is 64 and the counter-steer adds one 1 << 14 step,
// where the rounded bearing leaves sin22 at 63 and no step.
// [orig: Entity_UpdateWatercraftPhysics fpatan / fmul dbl_7C19D8 /
//  `call _ftol2_sse` @0x48E417..0x48E423, fsin @0x48E43B, `shl edx,0Eh`
//  @0x48E49A]
void test_boat_slip_bearing_truncates() {
    AirRig r;
    r.seat_pilot();
    Entity &hull = r.helo();
    constexpr int32_t kHeading = 127271671 + 10431;
    hull.veh.yaw_bam = kHeading;
    hull.veh.yaw_seeded = true;
    hull.veh.vel_x = 65536;
    hull.veh.vel_y = 12345;
    AiBrain &b = r.brain();
    b.f[AiBrain::kCurState] = 16;
    b.f[AiBrain::kOutSpeed] = 65536;
    VehicleTraits t;
    t.water_speed = 65536;
    t.player_control = true;
    VehicleDriveCmd cmd;
    r.w.ai.watercraft_ai_drive(r.w, hull, &r.pilot(), t, cmd);
    CHECK(cmd.ai_drive);
    CHECK(cmd.steer_target_bam == kHeading + (1 << 14));
}

} // namespace

int main() {
    test_single_node_lists_are_one_shot();
    test_air_leg_stamps_current_state_only();
    test_player_pilot_holds_pretty();
    test_item_type_index_is_the_first_row_ordinal();
    test_avoid_brake_item_index_gates();
    test_nav_reads_are_unbounded();
    test_marker_fields_follow_the_marker_type();
    test_route_seed_is_the_slot_words();
    test_parked_airborne_hull_spins();
    test_flight_floor_is_the_fresh_sample();
    test_ground_samples_refresh_the_link();
    test_boat_slip_bearing_truncates();
    if (failures == 0) std::printf("route_parity: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
