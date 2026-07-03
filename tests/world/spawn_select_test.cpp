// Player spawn-point selection (net-re §5.2c): the player-start is chosen from the mission's 60xx
// start-marker family (first present type wins, farthest from enemy soldiers) — never an NPC's
// position. Guards the "player spawns on top of NPC #0" + the 00TRa "spawns at origin" regressions.
// [orig: Server_PositionPlayerForSpawn @0x50cf60 -> Entity_FindBestSpawnPoint @0x50ccc0]
#include "world/entity.h"
#include "world/spawn_select.h"
#include "world/world.h"

#include <cmath>
#include <cstdio>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

void spawn_marker(World &w, int32_t item_id, Vec3 pos, int16_t yaw = 0) {
    Entity e;
    e.kind = EntityKind::Marker; // markers promote into pool 3
    e.item_id = item_id;
    e.position = pos;
    e.yaw = yaw;
    w.registry.spawn(3, e);
}

void spawn_npc(World &w, Vec3 pos) {
    Entity e;
    e.kind = EntityKind::Organic; // soldiers promote into pool 0
    e.item_id = 0x14B9;
    e.position = pos;
    e.alive = true;
    w.registry.spawn(0, e);
}

bool approx(float a, float b) { return std::fabs(a - b) < 1e-3f; }

} // namespace

int main() {
    // --- farthest-from-enemy: among the 6002 starts, the one with the greatest min distance to any
    //     enemy soldier wins, and it is NEVER an NPC's position.
    {
        World w;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(3, 16);

        spawn_npc(w, {0.0f, 0.0f, 0.0f});
        spawn_npc(w, {5.0f, 0.0f, 0.0f});

        spawn_marker(w, 6002, {2.0f, 0.0f, 0.0f}, 30);    // on the enemies
        spawn_marker(w, 6002, {50.0f, 0.0f, 0.0f}, 60);   // mid
        spawn_marker(w, 6002, {200.0f, 10.0f, 3.0f}, 90); // farthest -> winner

        const SpawnPointResult r = select_player_spawn(w);
        CHECK(r.found);
        CHECK(approx(r.position.x, 200.0f));
        CHECK(approx(r.position.y, 10.0f));
        CHECK(approx(r.position.z, 3.0f));
        CHECK(r.yaw == 90);
        CHECK(!(approx(r.position.x, 0.0f) && approx(r.position.y, 0.0f)));
        CHECK(!(approx(r.position.x, 5.0f) && approx(r.position.y, 0.0f)));
    }

    // --- the 00TRa shape: an SP mission whose ONLY start marker is a single 6001 (no 6002), plus
    //     waypoint markers — the family scan finds the 6001, NOT found=false / origin. The A.2 fix.
    {
        World w;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(3, 16);
        spawn_npc(w, {248.0f, -363.0f, 27.0f});
        spawn_npc(w, {311.0f, -398.0f, 27.0f});
        spawn_marker(w, 6005, {300.0f, -377.0f, 27.0f});       // a waypoint marker (NOT a start)
        spawn_marker(w, 6001, {297.81f, -409.12f, 27.14f}, 0); // the lone coop/start marker

        const SpawnPointResult r = select_player_spawn(w);
        CHECK(r.found);
        CHECK(approx(r.position.x, 297.81f));
        CHECK(approx(r.position.y, -409.12f));
        CHECK(approx(r.position.z, 27.14f));
    }

    // --- priority: 6002 (SP/DM) outranks 6001 (coop fallback) when both are present.
    {
        World w;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(3, 16);
        spawn_marker(w, 6001, {10.0f, 10.0f, 0.0f}, 11);
        spawn_marker(w, 6002, {20.0f, 20.0f, 0.0f}, 22);
        const SpawnPointResult r = select_player_spawn(w);
        CHECK(r.found);
        CHECK(approx(r.position.x, 20.0f)); // the 6002, not the 6001
        CHECK(r.yaw == 22);
    }

    // --- no start-family marker at all: found=false (caller falls back, never to an NPC). A
    //     waypoint (6005) and a high mission marker type are NOT start markers.
    {
        World w;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(3, 16);
        spawn_npc(w, {7.0f, 8.0f, 0.0f});
        spawn_marker(w, 6005, {1.0f, 1.0f, 0.0f});
        spawn_marker(w, 6213, {2.0f, 2.0f, 0.0f});
        const SpawnPointResult r = select_player_spawn(w);
        CHECK(!r.found);
    }

    // --- no enemies: any start is valid; the first marker of the winning type is chosen.
    {
        World w;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(3, 16);
        spawn_marker(w, 6002, {11.0f, 22.0f, 1.0f}, 45);
        spawn_marker(w, 6002, {99.0f, 0.0f, 0.0f}, 0);
        const SpawnPointResult r = select_player_spawn(w);
        CHECK(r.found);
        CHECK(approx(r.position.x, 11.0f));
        CHECK(approx(r.position.y, 22.0f));
        CHECK(r.yaw == 45);
    }

    if (failures == 0) std::printf("OK spawn_select\n");
    return failures == 0 ? 0 : 1;
}
