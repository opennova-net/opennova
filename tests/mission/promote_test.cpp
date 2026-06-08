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
    CHECK(r.spawned == 3);  // 2 organics + 1 building (markers -> nav table, not the actor pools)
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
    CHECK(e0->net_id == 1);               // first SSN

    // entity got a net id; the registry resolves it (faithful find_by_net_id).
    CHECK(world.registry.find_by_net_id(1).valid());
    CHECK(world.registry.find_by_net_id(2).valid()); // second organic
    CHECK(world.registry.find_by_net_id(3).valid()); // building

    // a non-routed entity option: with patrol_on_spawn=false the brain stays in state 0.
    {
        World w2;
        AiSystem ai2;
        mission::PromoteOptions o2;
        o2.patrol_on_spawn = false;
        mission::promote_mission(m, w2, ai2, o2);
        CHECK(ai2.at(0)->brain.f[AiBrain::kCurState] == 0); // faithful init, no auto-patrol
    }

    // ---- ticking the AI drives the mover: it heads toward the first route marker ----
    e0->brain.f[AiBrain::kStep] = 64;
    TickContext ctx;
    ctx.world = &world;
    ctx.is_authority = true;
    ai.tick(world, ctx);

    // No enemies injected -> organic 0 walked its path; the mover wrote an out-speed and a
    // working target from the resolved node (marker 0 at X=100<<16).
    CHECK(ai.find_target_calls >= 1);                    // alive path attempted target acquisition
    CHECK(e0->brain.f[AiBrain::kOutSpeed] > 0);          // the mover produced a movement command
    CHECK(e0->brain.f[AiBrain::kWorkPosX] == (100 << 16)); // heading toward route marker 0

    if (failures == 0) std::printf("promote: all tests passed\n");
    return failures ? 1 : 0;
}
