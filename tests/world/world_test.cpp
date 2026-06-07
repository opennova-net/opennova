// libs/world substrate tests: addressable entities (faithful find_by_net_id),
// shared var store, entity commands, tick cadence, snapshot/restore.
#include <cstdio>

#include "world/world.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

int main() {
    World w;
    w.registry.configure_pool(0, 16); // actor pool
    w.registry.configure_pool(1, 16); // actor pool
    w.registry.configure_pool(4, 8);  // static pool (not searched by find_by_net_id)

    Entity e;
    e.net_id = 100;
    e.kind = EntityKind::Organic;
    e.team = 2;
    e.group_id = 5;
    e.health = 100;
    e.alive = true;
    EntityHandle h = w.registry.spawn(0, e);
    CHECK(h.valid());
    CHECK(h.pool() == 0);
    CHECK(w.registry.get(h) != nullptr);

    // find_by_net_id contract.
    CHECK(w.registry.find_by_net_id(100) == h);
    CHECK(!w.registry.find_by_net_id(999).valid());

    // pool-0-first ordering: a duplicate net id in pool 1 must lose to pool 0.
    Entity dup;
    dup.net_id = 100;
    w.registry.spawn(1, dup);
    CHECK(w.registry.find_by_net_id(100).pool() == 0);

    // pool 4 is NOT searched (actor mask &0xF) — a net id only in pool 4 is absent.
    Entity p4;
    p4.net_id = 400;
    w.registry.spawn(4, p4);
    CHECK(!w.registry.find_by_net_id(400).valid());

    // commands operate on the clean model.
    CHECK(w.commands.ssn_alive(100));
    CHECK(w.commands.kill_ssn(100));
    CHECK(w.commands.ssn_dead(100));

    // group fan-out.
    Entity g1; g1.net_id = 201; g1.group_id = 7; g1.alive = true; w.registry.spawn(0, g1);
    Entity g2; g2.net_id = 202; g2.group_id = 7; g2.alive = true; w.registry.spawn(0, g2);
    CHECK(w.commands.group_alive(7));
    CHECK(w.commands.kill_group(7) == 2);
    CHECK(w.commands.group_dead(7));

    // shared var store.
    w.vars.set_mission(5, 42);
    CHECK(w.vars.get_mission(5) == 42);
    w.vars.set_global(3, -7);
    CHECK(w.vars.get_global(3) == -7);
    w.vars.clear_mission();
    CHECK(w.vars.get_mission(5) == 0);
    CHECK(w.vars.get_global(3) == -7); // globals survive mission clear

    // tick service: 62-frame divider.
    TickService ts;
    for (int i = 0; i < TickService::kFramesPerLogicTick - 1; ++i) {
        CHECK(!ts.advance_frame(w));
    }
    CHECK(ts.advance_frame(w));
    CHECK(w.logic_tick == 1);

    // snapshot / restore (editor play).
    w.vars.set_mission(1, 7);
    World::Snapshot snap = w.snapshot();
    w.vars.set_mission(1, 99);
    w.commands.kill_ssn(201); // mutate after snapshot
    w.restore(snap);
    CHECK(w.vars.get_mission(1) == 7);

    std::printf(failures ? "WORLD TESTS FAILED (%d)\n" : "world tests passed\n", failures);
    return failures ? 1 : 0;
}
