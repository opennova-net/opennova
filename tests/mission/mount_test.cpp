// Emplacement / mount (Phase 2) tests: the seat model + EntityCommands mount primitives
// (mount/mount_best/dismount/find_best_seat/find_mounted_on), the per-tick AI seat-follow,
// the BMS AttachToEmplaced action path (emits no unported_action), and snapshot/restore
// rewinding the mount. [orig chain: EventAction_Dispatch case 0x25 @0x4542e0 ->
// WacScript_TryMountEntityToVehicle @0x4f70f0 -> Entity_FindBestSeatSlot @0x4351f0.]
#include <cstdio>

#include "mission/event_runtime.h"
#include "mission/bms.h"
#include "world/ai.h"
#include "world/world.h"

using namespace opennova;
using world::AiBrain;
using world::AiEntity;
using world::AiSystem;
using world::Entity;
using world::EntityHandle;
using world::EntityKind;
using world::Seat;
using world::SeatType;
using world::TickContext;
using world::World;
using world::to_fixed;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static Entity make_gun(uint16_t ssn, float x, float y, float z, int16_t yaw) {
    Entity e;
    e.net_id = ssn;
    e.kind = EntityKind::Item;
    e.position = {x, y, z};
    e.yaw = yaw;
    Seat s;
    s.type = SeatType::Gunner;
    e.seats.push_back(s);
    return e;
}

static Entity make_soldier(uint16_t ssn, float x, float y, float z) {
    Entity e;
    e.net_id = ssn;
    e.kind = EntityKind::Organic;
    e.position = {x, y, z};
    return e;
}

int main() {
    // ---- mount sets both sides + poses onto the seat ----
    {
        World w;
        w.registry.configure_pool(0, 16); // organics
        w.registry.configure_pool(1, 16); // items
        const EntityHandle gh = w.registry.spawn(1, make_gun(200, 10.f, 20.f, 5.f, 0));
        const EntityHandle sh = w.registry.spawn(0, make_soldier(100, 100.f, 100.f, 0.f));

        CHECK(w.commands.mount(100, 200));
        Entity *occ = w.registry.get(sh);
        Entity *gun = w.registry.get(gh);
        CHECK(occ->mounted);
        CHECK(occ->mount_target == gh);
        CHECK(occ->mount_seat == 0);
        CHECK(occ->mount_type == SeatType::Gunner);
        CHECK(gun->seats[0].occupant == sh);
        // seat_local default 0 -> occupant snaps to the gun origin.
        CHECK(occ->position.x == 10.f && occ->position.y == 20.f && occ->position.z == 5.f);
        CHECK(w.commands.find_mounted_on(200) == 100);

        // dismount frees the seat + clears the ref.
        CHECK(w.commands.dismount(100));
        CHECK(!occ->mounted);
        CHECK(occ->mount_seat == -1);
        CHECK(!gun->seats[0].occupant.valid());
        CHECK(w.commands.find_mounted_on(200) == 0);
        CHECK(!w.commands.dismount(100)); // not mounted -> false
    }

    // ---- edges: full seat, already-mounted, seatless target ----
    {
        World w;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        w.registry.spawn(1, make_gun(200, 0.f, 0.f, 0.f, 0));
        w.registry.spawn(1, make_gun(201, 50.f, 50.f, 0.f, 0));
        Entity seatless;
        seatless.net_id = 202;
        seatless.kind = EntityKind::Item;
        w.registry.spawn(1, seatless);
        w.registry.spawn(0, make_soldier(100, 1.f, 0.f, 0.f));
        w.registry.spawn(0, make_soldier(101, 2.f, 0.f, 0.f));

        CHECK(w.commands.mount(100, 200));   // takes the only seat
        CHECK(!w.commands.mount(101, 200));  // gun full
        CHECK(!w.commands.mount(100, 201));  // 100 already mounted
        CHECK(!w.commands.mount(101, 202));  // target has no seats
        CHECK(!w.commands.mount(101, 999));  // missing target
        CHECK(w.commands.mount(101, 201));   // a free gun works
    }

    // ---- AI tick seat-follow: occupant tracks the seat, never path-follows ----
    {
        World w;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        const EntityHandle gh = w.registry.spawn(1, make_gun(200, 10.f, 20.f, 5.f, 0));
        const EntityHandle sh = w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));
        AiSystem ai;
        w.ai = &ai;
        const int idx = ai.attach(sh);
        ai.at(idx)->net_id = 100;
        CHECK(w.commands.mount(100, 200));

        TickContext ctx{};
        ctx.is_authority = true;
        ai.tick(w, ctx);
        AiEntity *ae = ai.at(idx);
        CHECK(ae->pos[0] == to_fixed(10.0));
        CHECK(ae->pos[1] == to_fixed(20.0));
        CHECK(ae->pos[2] == to_fixed(5.0));

        // Move the gun -> the gunner follows next tick.
        w.registry.get(gh)->position = {30.f, 40.f, 5.f};
        ai.tick(w, ctx);
        CHECK(ae->pos[0] == to_fixed(30.0));
        CHECK(ae->pos[1] == to_fixed(40.0));

        // Even with a locomotion target set, a mounted gunner does NOT path-follow.
        ae->brain.f[AiBrain::kOutSpeed] = 9999;
        ae->brain.f[AiBrain::kWorkPosX] = to_fixed(999.0);
        ai.tick(w, ctx);
        CHECK(ae->pos[0] == to_fixed(30.0)); // still seated, not moved toward 999

        // Vehicle gone -> auto-dismount, occupant resumes normal AI.
        w.registry.despawn(gh);
        ai.tick(w, ctx);
        CHECK(!w.registry.get(sh)->mounted);
    }

    // ---- BMS AttachToEmplaced: emits no unported_action + mounts via proximity ----
    {
        World w;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        w.registry.spawn(1, make_gun(200, 1.f, 1.f, 0.f, 0));
        w.registry.spawn(0, make_soldier(100, 2.f, 1.f, 0.f)); // within kMountRadius

        bms::Event e{};
        e.flags = bms::EventFlags::None;
        e.trigger_count = 0; // unconditional -> fires
        e.action_index = 0;
        e.action_count = 1;
        bms::Action act{};
        act.action_type = bms::ActionType::AttachToEmplaced;
        act.param1 = 100; // occupant SSN (the only param the action carries)
        mission::BmsEventSystem bms_sys;
        bms_sys.load({e}, {}, {act});
        w.add_system(&bms_sys);
        w.load_systems();
        // A normal event's first processing pass is the 16th tick (quarter-list
        // round-robin; see event_runtime_test).
        for (int i = 0; i < 16; ++i) w.run_logic_tick(true);

        CHECK(w.effects.count("unported_action") == 0);
        CHECK(w.commands.find_mounted_on(200) == 100);
    }

    // ---- snapshot/restore rewinds the mount (seats ride the registry value-copy) ----
    {
        World w;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        const EntityHandle gh = w.registry.spawn(1, make_gun(200, 1.f, 1.f, 0.f, 0));
        const EntityHandle sh = w.registry.spawn(0, make_soldier(100, 2.f, 1.f, 0.f));

        const World::Snapshot snap = w.snapshot(); // pre-mount
        CHECK(w.commands.mount(100, 200));
        CHECK(w.registry.get(sh)->mounted);
        CHECK(w.registry.get(gh)->seats[0].occupant.valid());

        w.restore(snap);
        CHECK(!w.registry.get(sh)->mounted);            // rewound
        CHECK(!w.registry.get(gh)->seats[0].occupant.valid()); // seat freed
    }

    if (failures == 0) std::printf("mount_test: OK\n");
    else std::printf("mount_test: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
