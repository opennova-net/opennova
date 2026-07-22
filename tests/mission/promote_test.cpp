// Mission -> world promotion: a synthetic BMS mission is promoted into a live world +
// AI system, then the AI is ticked to prove the brains/nav are wired to the real mission
// data (entities patrol their authored routes). See libs/mission/src/promote.cpp.
#include <cstdio>
#include <memory>

#include "mission/promote.h"
#include "world/ai.h"
#include "world/world.h"

using namespace opennova;
using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static bms::Entity organic(int32_t x, int32_t y, int32_t z, uint8_t team, uint8_t wp_id,
                           int32_t wp_num) {
    bms::Entity e{};
    e.type = bms::ItemType::Organic;
    e.x = x; e.y = y; e.z = z;
    e.yaw = 90;
    e.team = team;
    e.waypoint_id = wp_id;
    e.wp_number = wp_num;
    e.min_engagement_distance = 50 << 16;
    e.max_engagement_distance = 500 << 16;
    return e;
}

static bms::Entity marker(int32_t x, int32_t y, int32_t z) {
    bms::Entity e{};
    e.type = bms::ItemType::Marker;
    e.x = x; e.y = y; e.z = z;
    return e;
}

static bms::Entity item(int32_t type_id, int32_t x, int32_t y, int32_t z) {
    bms::Entity e{};
    e.type = bms::ItemType::Item;
    e.type_id = type_id;
    e.x = x; e.y = y; e.z = z;
    return e;
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
static void test_emplacement_attachments() {
    bms::File cm{};
    cm.items.push_back(item(/*type_id=*/164, 10 << 16, 20 << 16, 3 << 16));
    cm.items[0].id = 11;

    mission::PromoteOptions co{};
    mission::ItemSeatSpec carrier{};
    carrier.type_id = 164;
    mission::ItemEmplacementAttachmentSpec plain{};
    plain.child_type_id = 166;
    plain.kind = mission::EmplacementAttachmentKind::Standard;
    plain.stored_slot = 1;
    plain.anchor.type = SeatType::Gunner;
    plain.anchor.bone_index = 4;
    plain.anchor.seat_local = {2.f, 0.f, 1.f};
    plain.anchor_found = true;
    carrier.emplacement_attachments.push_back(plain);
    mission::ItemEmplacementAttachmentSpec gun{};
    gun.child_type_id = 183;
    gun.kind = mission::EmplacementAttachmentKind::G;
    gun.stored_slot = 2;
    gun.attachment_flags = 2;
    gun.anchor.type = SeatType::Gunner;
    gun.angle_count = 4;
    gun.down_limit_bam = 70 * 11930464;
    carrier.emplacement_attachments.push_back(gun);
    mission::ItemEmplacementAttachmentSpec crosshair{};
    crosshair.child_type_id = 182;
    crosshair.kind = mission::EmplacementAttachmentKind::C;
    crosshair.stored_slot = 3;
    crosshair.attachment_flags = 1;
    crosshair.anchor.type = SeatType::Gunner;
    carrier.emplacement_attachments.push_back(crosshair);
    co.item_seat_specs.push_back(carrier);

    mission::ItemSeatSpec gun_item{};
    gun_item.type_id = 183;
    Seat usegun{};
    usegun.type = SeatType::Gunner;
    gun_item.seats.push_back(usegun);
    gun_item.primary_weapon = "WPN_HELOGUN";
    co.item_seat_specs.push_back(gun_item);

    auto cw = std::make_unique<World>();
    auto cai = std::make_unique<AiSystem>();
    const mission::PromoteResult cr =
            mission::promote_mission(cm, *cw, *cai, co);
    CHECK(cr.spawned == 4);
    CHECK(cw->registry.live_count() == 4);
    const EntityHandle parent_h = cw->registry.find_by_net_id(11);
    EntityHandle plain_h{}, gun_h{}, crosshair_h{};
    cw->registry.for_each([&](const Entity &e) {
        if (e.item_id == 166) plain_h = e.handle;
        if (e.item_id == 183) gun_h = e.handle;
        if (e.item_id == 182) crosshair_h = e.handle;
    });
    Entity *parent = cw->registry.get(parent_h);
    Entity *plain_child = cw->registry.get(plain_h);
    Entity *gun_child = cw->registry.get(gun_h);
    Entity *crosshair_child = cw->registry.get(crosshair_h);
    CHECK(parent != nullptr && plain_child != nullptr);
    CHECK(gun_child != nullptr && crosshair_child != nullptr);
    if (parent == nullptr || plain_child == nullptr ||
        gun_child == nullptr || crosshair_child == nullptr)
        return;
    CHECK(plain_child->emplacement_parent == parent_h);
    CHECK(plain_child->emplacement_kind == 0);
    CHECK(gun_child->emplacement_kind == 1);
    CHECK(gun_child->emplacement_attachment_flags == 2);
    CHECK(gun_child->emplacement_angle_count == 4);
    CHECK(gun_child->emplacement_down_limit_bam == 70 * 11930464);
    CHECK(gun_child->seats.size() == 1);
    CHECK(gun_child->primary_weapon == "WPN_HELOGUN");
    CHECK(crosshair_child->emplacement_kind == 2);
    CHECK(crosshair_child->emplacement_attachment_flags == 1);
    CHECK(plain_child->position.x == 12.f && plain_child->position.y == 20.f &&
          plain_child->position.z == 4.f);
    CHECK(gun_child->position.x == 10.f && gun_child->position.y == 20.f &&
          gun_child->position.z == 3.f);

    parent->position = {30.f, 40.f, 5.f};
    parent->yaw = 90;
    cw->run_logic_tick();
    plain_child = cw->registry.get(plain_h);
    CHECK(plain_child->position.x == 30.f && plain_child->position.y == 38.f &&
          plain_child->position.z == 6.f);
    Entity rider_seed{};
    rider_seed.kind = EntityKind::Organic;
    rider_seed.health = 100;
    const EntityHandle rider_h = cw->registry.spawn(0, rider_seed);
    Entity *rider = cw->registry.get(rider_h);
    rider->mounted = true;
    rider->mount_target = gun_h;
    rider->mount_seat = 0;
    rider->mount_type = SeatType::Gunner;
    gun_child->seats[0].occupant = rider_h;
    const uint64_t old_parent_spawn_id = parent->registry_spawn_id;
    cw->registry.despawn(parent_h);
    Entity replacement_seed{};
    replacement_seed.kind = EntityKind::Item;
    replacement_seed.item_id = 999;
    const EntityHandle replacement_h =
            cw->registry.spawn(1, replacement_seed);
    CHECK(replacement_h == parent_h);
    CHECK(cw->registry.get(replacement_h)->registry_spawn_id !=
          old_parent_spawn_id);
    cw->run_logic_tick();
    CHECK(cw->registry.get(plain_h) == nullptr);
    CHECK(cw->registry.get(gun_h) == nullptr);
    CHECK(cw->registry.get(crosshair_h) == nullptr);
    rider = cw->registry.get(rider_h);
    CHECK(rider != nullptr && !rider->mounted);
    CHECK(cw->registry.get(replacement_h) != nullptr);
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
static void test_emplacement_parent_death_cascades() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 4);
    world->registry.configure_pool(1, 8);

    Entity parent_seed{};
    parent_seed.kind = EntityKind::Item;
    parent_seed.item_id = 164;
    const EntityHandle parent_h = world->registry.spawn(1, parent_seed);
    Entity *parent = world->registry.get(parent_h);
    CHECK(parent != nullptr);
    if (parent == nullptr) return;

    Entity child_seed{};
    child_seed.kind = EntityKind::Item;
    child_seed.item_id = 166;
    child_seed.emplacement_parent = parent_h;
    child_seed.emplacement_parent_spawn_id = parent->registry_spawn_id;
    const EntityHandle child_h = world->registry.spawn(1, child_seed);
    Entity *child = world->registry.get(child_h);
    CHECK(child != nullptr);
    if (child == nullptr) return;

    Entity grandchild_seed{};
    grandchild_seed.kind = EntityKind::Item;
    grandchild_seed.item_id = 183;
    grandchild_seed.emplacement_parent = child_h;
    grandchild_seed.emplacement_parent_spawn_id = child->registry_spawn_id;
    const EntityHandle grandchild_h = world->registry.spawn(1, grandchild_seed);
    Entity *grandchild = world->registry.get(grandchild_h);
    CHECK(grandchild != nullptr);
    if (grandchild == nullptr) return;

    Entity rider_seed{};
    rider_seed.kind = EntityKind::Organic;
    const EntityHandle rider_h = world->registry.spawn(0, rider_seed);
    Entity *rider = world->registry.get(rider_h);
    CHECK(rider != nullptr);
    if (rider == nullptr) return;
    Seat usegun{};
    usegun.type = SeatType::Gunner;
    usegun.occupant = rider_h;
    grandchild->seats.push_back(usegun);
    rider->mounted = true;
    rider->mount_target = grandchild_h;
    rider->mount_seat = 0;
    rider->mount_type = SeatType::Gunner;

    // The ordinary item-death path keeps the carrier entity resident as a husk.
    // Its implicit attachment ownership must still end immediately.
    parent->health = 0;
    destruction_notify_item_damage(*world, *parent, 2);
    CHECK(world->registry.get(parent_h) != nullptr);
    CHECK(!parent->alive);

    world->run_logic_tick();
    CHECK(world->registry.get(parent_h) != nullptr);
    CHECK(world->registry.get(child_h) == nullptr);
    CHECK(world->registry.get(grandchild_h) == nullptr);
    rider = world->registry.get(rider_h);
    CHECK(rider != nullptr && !rider->mounted);
    CHECK(rider != nullptr && !rider->mount_target.valid());
}

int main() {
    // A synthetic mission: 3 markers forming a path, 1 looping waypoint record (channel 0),
    // 2 organics on that route (teams 1/2), 1 building.
    bms::File m{};
    m.markers.push_back(marker(100 << 16, 0, 0));
    m.markers.push_back(marker(200 << 16, 0, 0));
    m.markers.push_back(marker(300 << 16, 0, 0));

    bms::WaypointRecord wr{};
    wr.flags = bms::WaypointFlags::None; // loops
    wr.marker_count = 3;
    wr.waypoint_numbers = {0, 1, 2};
    m.waypoint_records.push_back(wr);

    // waypoint_id is 1-based (channel 0 = the AI "no route" sentinel); these patrol channel 1.
    m.organics.push_back(organic(0, 0, 0, /*team=*/1, /*wp_id=*/1, /*wp_num=*/0));
    m.organics.push_back(organic(50 << 16, 0, 0, /*team=*/2, /*wp_id=*/1, /*wp_num=*/0));
    m.organics[0].bmsi_attributes =
            static_cast<uint32_t>(bms::BmsiAttributeFlags::Blind) |
            static_cast<uint32_t>(bms::BmsiAttributeFlags::Guarding) |
            static_cast<uint32_t>(bms::BmsiAttributeFlags::Berserk) |
            static_cast<uint32_t>(bms::BmsiAttributeFlags::Coward);

    bms::Entity bldg{};
    bldg.type = bms::ItemType::Building;
    bldg.x = 999 << 16;
    m.buildings.push_back(bldg);

    // The SSN every trigger/action references is AUTHORED in the record (the editor
    // assigns it); promotion copies it verbatim. [orig: Entity_SpawnFromBMSRecord
    // @0x40e9f0 -> entity+124 = record dword @+8]
    m.organics[0].id = 1;
    m.organics[1].id = 2;
    m.buildings[0].id = 3;
    m.markers[0].id = 10;
    m.markers[1].id = 11;
    m.markers[2].id = 12;

    World world;
    AiSystem ai;
    ai.is_authority = true;

    mission::PromoteOptions opts;
    opts.arrival_radius = 1000;
    opts.default_speed = 20;
    mission::PromoteResult r = mission::promote_mission(m, world, ai, opts);

    // ---- promotion populated entities + brains + nav ----
    CHECK(r.nav_nodes == 3);
    CHECK(r.nav_channels == 1);
    CHECK(r.brains == 2);   // the 2 organics
    CHECK(r.spawned == 6);  // 1 building + 3 markers (pool 3, as the original spawns them) + 2 organics
    CHECK(r.dropped == 0);
    CHECK(ai.count() == 2);

    // nav table built from markers + the waypoint record.
    CHECK(ai.nav.nodes.size() == 3);
    CHECK(ai.nav.nodes[0].f[1] == (100 << 16)); // marker 0 X
    CHECK(ai.nav.nodes[0].f[0] == 1000);        // arrival radius (payload0)
    CHECK(ai.nav.channel(0) != nullptr);
    CHECK(ai.nav.channel(0)->count == 0);       // channel 0 = empty "no route" sentinel
    const NavChannel *ch = ai.nav.channel(1);   // the record populates channel 1 (1-based)
    CHECK(ch != nullptr);
    CHECK(ch->count == 3);
    CHECK(ch->loopflag == 0);             // WaypointFlags::None -> loops
    CHECK(ch->entries[2] == 2);

    // organic 0 is in GROUND_FOLLOWWP with its route + spawn transform.
    AiEntity *e0 = ai.at(0);
    CHECK(e0 != nullptr);
    CHECK(e0->brain.f[AiBrain::kCurState] == 16); // GROUND_FOLLOWWP (patrol_on_spawn)
    CHECK(e0->brain.f[AiBrain::kWpType] == 1);
    CHECK(e0->brain.f[AiBrain::kWpChannel] == 1); // 1-based channel
    CHECK(e0->brain.f[AiBrain::kWpNode] == 0);
    CHECK(e0->brain.f[AiBrain::kSpeedB] == 20);
    CHECK(e0->team == 1);
    CHECK(e0->pos[0] == 0);               // spawned at origin
    CHECK(e0->net_id == 1);               // the AUTHORED record id, copied verbatim
    CHECK((e0->slot.f[1] & 0x209) == 0x209);
    CHECK(e0->see_all);
    CHECK((world.registry.get(world.registry.find_by_net_id(1))->engine_flags &
           0x40u) != 0);
    CHECK((world.registry.get(world.registry.find_by_net_id(1))->flags &
           0x40u) != 0);

    // entities carry their authored net ids; the registry resolves them (faithful
    // find_by_net_id over pools 0..3, markers included).
    CHECK(world.registry.find_by_net_id(1).valid());
    CHECK(world.registry.find_by_net_id(2).valid());  // second organic
    CHECK(world.registry.find_by_net_id(3).valid());  // building
    CHECK(world.registry.find_by_net_id(11).valid()); // marker 1 (pool 3)

    // ---- area-trigger zones are promoted into the registry's area table (array order) ----
    {
        bms::File ma{};
        bms::AreaTrigger zone{};
        zone.id = 5;                  // designer zone id (1..99); registered at array index 0
        zone.x_min = -10 << 16; zone.x_max = 10 << 16;
        zone.y_min = -10 << 16; zone.y_max = 10 << 16;
        zone.flags = 0;               // Z unbounded
        ma.area_triggers.push_back(zone);
        ma.organics.push_back(organic(0, 0, 0, 1, 0, 0));         // SSN 1, inside the zone
        ma.organics.push_back(organic(100 << 16, 0, 0, 1, 0, 0)); // SSN 2, outside
        ma.organics[0].id = 1;
        ma.organics[1].id = 2;
        World wz;
        AiSystem aiz;
        mission::promote_mission(ma, wz, aiz);
        CHECK(wz.registry.area(0) != nullptr);  // the zone populated the table (was empty before)
        CHECK(wz.commands.ssn_in_area(1, 0));    // organic at origin is inside zone 0
        CHECK(!wz.commands.ssn_in_area(2, 0));   // organic at x=100 is outside
    }

    // ---- the player waypoint track: the BLUE-flagged route + the marker fields ----
    // [orig: NetPacket_WriteWorldStateLoad0x0F @0x502e41 picks the first flags&2
    //  channel; Entity_SpawnFromBMSRecord @0x40f0aa seeds radius/name/link/chain]
    {
        bms::File wm{};
        wm.markers.push_back(marker(100 << 16, 0, 0));
        wm.markers.push_back(marker(200 << 16, 0, 0));
        wm.markers.push_back(marker(300 << 16, 0, 0));
        wm.markers[0].wp_distance = 25;             // authored radius -> 25<<16
        wm.markers[0].ttool_index = 4;              // STRWPNAME004
        wm.markers[1].wp_adv_trigger = 3;           // completes when event 3 fires
        wm.markers[1].bmsi_attributes = 1u << 22;   // chain-back
        // Record 0: an AI patrol route (unflagged) — must NOT become the track.
        bms::WaypointRecord ai_route{};
        ai_route.flags = bms::WaypointFlags::None;
        ai_route.marker_count = 1;
        ai_route.waypoint_numbers = {2};
        wm.waypoint_records.push_back(ai_route);
        // Record 1: the blue player route.
        bms::WaypointRecord blue{};
        blue.flags = bms::WaypointFlags::BlueTeam;
        blue.marker_count = 2;
        blue.waypoint_numbers = {0, 1};
        wm.waypoint_records.push_back(blue);

        World ww;
        AiSystem wai;
        mission::promote_mission(wm, ww, wai);
        CHECK(ww.waypoints.entries.size() == 2);    // the blue route only
        CHECK(ww.waypoints.show);                    // visible by default
        CHECK(ww.waypoints.current == -1);           // no selection until the tick
        CHECK(ww.waypoints.entries[0].x == (100 << 16));
        CHECK(ww.waypoints.entries[0].radius == (25 << 16)); // authored wp_distance
        CHECK(ww.waypoints.entries[0].name_id == 4);
        CHECK(ww.waypoints.entries[1].radius == 0x8000);     // default 0.5 u
        CHECK(ww.waypoints.entries[1].linked_event == 3);
        CHECK(ww.waypoints.entries[1].chain_back);
        // The raw route-flags word rides the nav channel (bit1 = the blue mark).
        CHECK((wai.nav.channel(2)->loopflag &
               static_cast<int32_t>(bms::WaypointFlags::BlueTeam)) != 0);
    }

    // a non-routed entity option: with patrol_on_spawn=false the brain stays in state 0.
    {
        World w2;
        AiSystem ai2;
        mission::PromoteOptions o2;
        o2.patrol_on_spawn = false;
        mission::promote_mission(m, w2, ai2, o2);
        CHECK(ai2.at(0)->brain.f[AiBrain::kCurState] == 0); // faithful init, no auto-patrol
    }

    // ---- command 125: BMS wp_number is a target entity serial, not a path node ----
    // IDA proof: Entity_SpawnFromBMSRecord @0x40F02F copies record byte 0x4F to slot+148
    // and record dword 0x30 to slot+152; Entity_UpdateInfantryAI @0x4B9910 resolves
    // slot+152 against entity+124 and then uses Entity_FindBestSeatSlot @0x4351F0.
    {
        bms::File cm{};
        cm.items.push_back(item(/*type_id=*/1294, 10 << 16, 0, 0));
        cm.items[0].id = 11;
        cm.organics.push_back(organic(11 << 16, 0, 0, 1, /*wp_id=*/125, /*wp_num=*/11));
        cm.organics[0].id = 1;

        mission::PromoteOptions co{};
        mission::ItemSeatSpec seats{};
        seats.type_id = 1294;
        Seat s{};
        s.type = SeatType::Passenger;
        s.seat_local = {1.f, 2.f, 3.f};
        seats.seats.push_back(s);
        co.item_seat_specs.push_back(seats);

        World cw;
        AiSystem cai;
        mission::promote_mission(cm, cw, cai, co);

        const EntityHandle vh = cw.registry.find_by_net_id(11);
        const EntityHandle oh = cw.registry.find_by_net_id(1);
        Entity *veh = cw.registry.get(vh);
        Entity *occ = cw.registry.get(oh);
        CHECK(veh != nullptr);
        CHECK(occ != nullptr);
        CHECK(veh->seats.size() == 1);
        CHECK(occ->mounted);
        CHECK(occ->mount_target == vh);
        CHECK(veh->seats[0].occupant == oh);
        CHECK(occ->position.x == 11.f && occ->position.y == 2.f && occ->position.z == 3.f);
        CHECK(cai.at(0)->slot.f[37] == 125);
        CHECK(cai.at(0)->slot.f[38] == 11);
        CHECK(cai.at(0)->pos[0] == to_fixed(11.0));
        CHECK(cai.at(0)->pos[1] == to_fixed(2.0));
        CHECK(cai.at(0)->pos[2] == to_fixed(3.0));
    }

    {
        bms::File cm{};
        cm.items.push_back(item(/*type_id=*/1294, 100 << 16, 0, 0));
        cm.items[0].id = 11;
        cm.organics.push_back(organic(0, 0, 0, 1, /*wp_id=*/125, /*wp_num=*/11));
        cm.organics[0].id = 1;

        mission::PromoteOptions co{};
        mission::ItemSeatSpec seats{};
        seats.type_id = 1294;
        Seat s{};
        s.type = SeatType::Passenger;
        seats.seats.push_back(s);
        co.item_seat_specs.push_back(seats);

        World cw;
        AiSystem cai;
        mission::promote_mission(cm, cw, cai, co);

        Entity *veh = cw.registry.get(cw.registry.find_by_net_id(11));
        Entity *occ = cw.registry.get(cw.registry.find_by_net_id(1));
        CHECK(veh != nullptr);
        CHECK(occ != nullptr);
        CHECK(!occ->mounted);
        CHECK(!veh->seats.empty());
        CHECK(!veh->seats[0].occupant.valid());
        CHECK(cai.at(0)->pos[0] == 0);
    }

    {
        bms::File cm{};
        cm.items.push_back(item(/*type_id=*/1420, 10 << 16, 0, 0));
        cm.items[0].id = 11;
        cm.organics.push_back(organic(11 << 16, 0, 0, 1, /*wp_id=*/125, /*wp_num=*/11));
        cm.organics[0].id = 1;
        cm.organics.push_back(organic(12 << 16, 0, 0, 1, /*wp_id=*/125, /*wp_num=*/11));
        cm.organics[1].id = 2;

        mission::PromoteOptions co{};
        mission::ItemSeatSpec seats{};
        seats.type_id = 1420;
        Seat s0{};
        s0.type = SeatType::Passenger;
        Seat s1{};
        s1.type = SeatType::Passenger;
        s1.seat_local = {1.f, 0.f, 0.f};
        seats.seats.push_back(s0);
        seats.seats.push_back(s1);
        co.item_seat_specs.push_back(seats);

        World cw;
        AiSystem cai;
        mission::promote_mission(cm, cw, cai, co);

        Entity *veh = cw.registry.get(cw.registry.find_by_net_id(11));
        Entity *o0 = cw.registry.get(cw.registry.find_by_net_id(1));
        Entity *o1 = cw.registry.get(cw.registry.find_by_net_id(2));
        CHECK(veh != nullptr);
        CHECK(o0 != nullptr && o1 != nullptr);
        CHECK(veh->seats.size() == 2);
        CHECK(o0->mounted);
        CHECK(o1->mounted);
        CHECK(veh->seats[0].occupant.valid());
        CHECK(veh->seats[1].occupant.valid());
        CHECK(veh->seats[0].occupant != veh->seats[1].occupant);
    }

    // ---- command 123/124/125 seat restrictions: sitex-only / no-ctrlx / any ----
    {
        bms::File cm{};
        cm.items.push_back(item(/*type_id=*/1294, 10 << 16, 0, 0));
        cm.items[0].id = 11;
        cm.organics.push_back(organic(10 << 16, 0, 0, 1, /*wp_id=*/123, /*wp_num=*/11));
        cm.organics[0].id = 1;

        mission::PromoteOptions co{};
        mission::ItemSeatSpec seats{};
        seats.type_id = 1294;
        Seat passenger{};
        passenger.type = SeatType::Passenger;
        passenger.seat_local = {4.f, 0.f, 0.f};
        Seat driver{};
        driver.type = SeatType::Driver;
        driver.seat_local = {1.f, 0.f, 0.f};
        seats.seats.push_back(passenger);
        seats.seats.push_back(driver);
        co.item_seat_specs.push_back(seats);

        World cw;
        AiSystem cai;
        mission::promote_mission(cm, cw, cai, co);

        Entity *occ = cw.registry.get(cw.registry.find_by_net_id(1));
        CHECK(occ != nullptr);
        CHECK(occ->mounted);
        CHECK(occ->mount_type == SeatType::Passenger);
        CHECK(occ->position.x == 14.f);
    }

    {
        bms::File cm{};
        cm.items.push_back(item(/*type_id=*/1294, 10 << 16, 0, 0));
        cm.items[0].id = 11;
        cm.organics.push_back(organic(10 << 16, 0, 0, 1, /*wp_id=*/124, /*wp_num=*/11));
        cm.organics[0].id = 1;

        mission::PromoteOptions co{};
        mission::ItemSeatSpec seats{};
        seats.type_id = 1294;
        Seat controller{};
        controller.type = SeatType::Controller;
        controller.seat_local = {1.f, 0.f, 0.f};
        Seat driver{};
        driver.type = SeatType::Driver;
        driver.seat_local = {5.f, 0.f, 0.f};
        seats.seats.push_back(controller);
        seats.seats.push_back(driver);
        co.item_seat_specs.push_back(seats);

        World cw;
        AiSystem cai;
        mission::promote_mission(cm, cw, cai, co);

        Entity *occ = cw.registry.get(cw.registry.find_by_net_id(1));
        CHECK(occ != nullptr);
        CHECK(occ->mounted);
        CHECK(occ->mount_type == SeatType::Driver);
        CHECK(occ->position.x == 15.f);
    }

    {
        bms::File cm{};
        cm.items.push_back(item(/*type_id=*/1294, 10 << 16, 0, 0));
        cm.items[0].id = 11;
        cm.organics.push_back(organic(10 << 16, 0, 0, 1, /*wp_id=*/125, /*wp_num=*/11));
        cm.organics[0].id = 1;

        mission::PromoteOptions co{};
        mission::ItemSeatSpec seats{};
        seats.type_id = 1294;
        Seat passenger{};
        passenger.type = SeatType::Passenger;
        passenger.seat_local = {5.f, 0.f, 0.f};
        Seat driver{};
        driver.type = SeatType::Driver;
        driver.seat_local = {1.f, 0.f, 0.f};
        seats.seats.push_back(passenger);
        seats.seats.push_back(driver);
        co.item_seat_specs.push_back(seats);

        World cw;
        AiSystem cai;
        mission::promote_mission(cm, cw, cai, co);

        Entity *occ = cw.registry.get(cw.registry.find_by_net_id(1));
        CHECK(occ != nullptr);
        CHECK(occ->mounted);
        CHECK(occ->mount_type == SeatType::Driver);
        CHECK(occ->position.x == 11.f);
    }

    // ---- items.def addeweap*: spawn every child and carry it on the parent frame ----
    test_emplacement_attachments();
    test_emplacement_parent_death_cascades();

    // ---- organics are routed through the INFANTRY motor with seeded slots ----
    // [orig: g_EntityClassPhysicsTable "org1" -> Entity_UpdateInfantryAI @0x4b9910;
    //  slot map from Entity_SpawnFromBMSRecord @0x40e9f0]
    CHECK(e0->inf.active);
    CHECK(e0->slot.f[35] == 1);   // has-route flag (slot+140)
    CHECK(e0->slot.f[37] == 1);   // channel = waypoint_id (slot+148)
    CHECK(e0->slot.f[38] == 0);   // start node = wp_number (slot+152)
    CHECK(e0->inf.body_heading == e0->heading); // spawns facing its authored heading

    // ---- end-to-end: the soldier WALKS its route on anim root motion ----
    // Synthetic clip source: gaits move 0.25u forward per tick, idles don't move.
    struct TestSource : IRootMotionSource {
        int32_t step;
        explicit TestSource(int32_t s) : step(s) {}
        static bool gait(int id) {
            return id == anim_state::kWalkForward || id == anim_state::kRunForward ||
                   id == anim_state::kJogForward;
        }
        bool has_clip(int /*adm_id*/, int id) const override {
            return gait(id) || id == anim_state::kIdle || id == anim_state::kStop;
        }
        int32_t clip_length_ticks(int, int) const override { return -1; }
        bool advance(int /*adm_id*/, int id, int32_t &phase, RootMotionFrame &out) override {
            if (!has_clip(0, id)) return false;
            ++phase;
            out = RootMotionFrame{};
            if (gait(id)) out.dx = step;
            return true;
        }
    };
    {
        TestSource src(0x4000); // 0.25u/tick forward
        ai.root_motion = &src;
        for (auto &n : ai.nav.nodes) n.f[0] = 5 << 16; // robust arrival window for the walk
        ai.nav.nodes[1].wait_ticks = 62;               // 1s authored hold at marker 1

        TickContext ctx;
        ctx.world = &world;
        ctx.is_authority = true;

        int max_node = 0;
        bool held = false, walked = false;
        for (int t = 0; t < 2600; ++t) {
            ctx.logic_tick = static_cast<uint32_t>(t); // the cadence gates key off this
            ai.tick(world, ctx);
            if (e0->slot.f[38] > max_node) max_node = e0->slot.f[38];
            if (e0->inf.wait_cooldown > 0) held = true;
            if (e0->inf.anim_state == anim_state::kWalkForward) walked = true;
        }
        CHECK(walked);                       // unalerted patrol uses the walk gait
        CHECK(max_node >= 2);                // arrived at markers and advanced the route
        CHECK(e0->pos[0] > (100 << 16));     // physically walked past marker 0
        CHECK(e0->pos[0] < (300 << 16));     // still on the route, not teleported
        CHECK(held);                         // honored marker 1's movetimer hold
        CHECK(!ai.relmat_calls.empty());     // arrival recorded the relation-matrix marks
        ai.root_motion = nullptr;
    }

    if (failures == 0) std::printf("promote: all tests passed\n");
    return failures ? 1 : 0;
}
