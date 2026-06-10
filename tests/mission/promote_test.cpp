// Mission -> world promotion: a synthetic BMS mission is promoted into a live world +
// AI system, then the AI is ticked to prove the brains/nav are wired to the real mission
// data (entities patrol their authored routes). See libs/mission/src/promote.cpp.
#include <cstdio>

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

    // a non-routed entity option: with patrol_on_spawn=false the brain stays in state 0.
    {
        World w2;
        AiSystem ai2;
        mission::PromoteOptions o2;
        o2.patrol_on_spawn = false;
        mission::promote_mission(m, w2, ai2, o2);
        CHECK(ai2.at(0)->brain.f[AiBrain::kCurState] == 0); // faithful init, no auto-patrol
    }

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
        bool has_clip(int id) const override {
            return gait(id) || id == anim_state::kIdle || id == anim_state::kStop;
        }
        bool advance(int id, int32_t &phase, RootMotionFrame &out) override {
            if (!has_clip(id)) return false;
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
