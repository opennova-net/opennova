// BMS event parity: retail-witnessed trigger and action semantics driven through
// the public BmsEventSystem / EntityCommands seams. Each block names the retail
// function it pins; the synthetic worlds are the smallest that exercise the leg.
#include <cstdint>
#include <cstdio>
#include <vector>

#include <runtime/mission/event_runtime.h>
#include <runtime/world/world.h>

using namespace opennova;
using world::World;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

// One BMS processing pass every 16th tick; an event is touched every 64 ticks.
static constexpr int kCycle = 64;

static void tick_n(World &w, int n) {
    for (int i = 0; i < n; ++i) w.run_logic_tick(true);
}

static void configure_pools(World &w) {
    for (int pool = 0; pool <= 3; ++pool) w.registry.configure_pool(pool, 8);
}

static bms::Trigger single_trigger(bms::SingleTriggerType sub, int32_t ssn, int32_t flags = 0) {
    bms::Trigger t{};
    t.condition_flags = flags;
    t.main_type = bms::TriggerMainType::Single;
    t.sub_type = static_cast<int32_t>(sub);
    t.param1 = ssn;
    return t;
}

// ---------------------------------------------------------------------------
// SingleAlive / SingleDestroyed read the first pool 0/1/2 row carrying the
// SSN and answer with its dead flag; an SSN no row carries reads NOT alive,
// so SingleDestroyed is TRUE for it. Vectors from the retail condition under
// Unicorn: SSN 0 / absent / zeroed row -> sub 4 = 1, sub 5 = 0; a live row ->
// 0 / 1; Flags & 2 -> 1 / 0.
// [orig: EventTrigger_EvaluateCondition cat 2 sub 4 @0x453985 / sub 5
//  @0x45399d -> Entity_IsAliveByBmsRef @0x43e640; Entity_Destroy zeroes a
//  removed row @0x43ea69]
static void test_single_alive_reads_the_bms_ref_rows() {
    World w;
    w.cached.humans = 1;
    configure_pools(w);
    world::Entity live{};
    live.net_id = 102;
    live.item_id = 1213;
    const world::EntityHandle h102 = w.registry.spawn(1, live);
    world::Entity removed{};
    removed.net_id = 103;
    removed.item_id = 1213;
    const world::EntityHandle h103 = w.registry.spawn(1, removed);
    world::Entity marker{};
    marker.net_id = 104;
    marker.item_id = 6005;
    w.registry.spawn(3, marker);
    world::Entity building{};
    building.net_id = 105;
    building.item_id = 1595;
    w.registry.spawn(2, building);

    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();
    const auto destroyed = [&](int32_t ssn) {
        return sys.evaluate_trigger_for_test(w,
                single_trigger(bms::SingleTriggerType::SingleDestroyed, ssn));
    };
    const auto alive = [&](int32_t ssn) {
        return sys.evaluate_trigger_for_test(w,
                single_trigger(bms::SingleTriggerType::SingleAlive, ssn));
    };

    CHECK(destroyed(101));  // never placed
    CHECK(!alive(101));
    CHECK(destroyed(0));    // SSN 0
    CHECK(!alive(0));
    CHECK(!destroyed(102)); // a live row
    CHECK(alive(102));
    CHECK(!destroyed(105)); // pool 2 is walked
    CHECK(alive(105));
    CHECK(destroyed(104));  // pool 3 is not
    CHECK(!alive(104));

    // The kill latch (Flags |= 2) is what the read answers with.
    w.registry.get(h102)->engine_flags |= world::kEntityFlagDead;
    CHECK(destroyed(102));
    CHECK(!alive(102));

    // A removed SSN reads destroyed.
    CHECK(w.commands.remove_ssn(h103));
    CHECK(destroyed(103));
    CHECK(!alive(103));
}

// 01TR's event 34 is SingleDestroyed(101) AND SingleDestroyed(102) ->
// SubGoalWon(6), and the mission's only BlueWin (event 38) waits on it through
// an Event(34) trigger. SSN 101 is never placed in 01TR, so retail fires event
// 34 the pass after item 102 dies; the dependent event follows.
// [orig: Entity_IsAliveByBmsRef @0x43e640; EventTrigger_EvaluateCondition cat 3
//  @0x453a64]
static void test_01tr_event34_chain_fires_when_the_placed_item_dies() {
    World w;
    w.cached.humans = 1;
    configure_pools(w);
    world::Entity item{};
    item.net_id = 102;
    item.item_id = 1213;
    const world::EntityHandle h102 = w.registry.spawn(1, item);

    bms::Event e34{};
    e34.trigger_index = 0;
    e34.trigger_count = 2;
    e34.action_index = 0;
    e34.action_count = 1;
    bms::Event e38{};
    e38.trigger_index = 2;
    e38.trigger_count = 1;
    e38.action_index = 1;
    e38.action_count = 1;
    bms::Trigger fired{};
    fired.main_type = bms::TriggerMainType::Event;
    fired.param1 = 0;
    bms::Action won{};
    won.action_type = bms::ActionType::SubGoalWon;
    won.param1 = 6;
    bms::Action flag{};
    flag.action_type = bms::ActionType::MisvarChange;
    flag.action_sub_type = static_cast<int32_t>(bms::MissionVariableActionSubType::Set);
    flag.param1 = 9;
    flag.param2 = 1;
    mission::BmsEventSystem sys;
    sys.load({e34, e38},
             {single_trigger(bms::SingleTriggerType::SingleDestroyed, 101),
              single_trigger(bms::SingleTriggerType::SingleDestroyed, 102), fired},
             {won, flag});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kCycle);
    CHECK((w.script.subgoals.won & (1u << 6)) == 0); // item 102 still lives
    CHECK(w.script.vars.get_mission(9) == 0);

    w.registry.get(h102)->engine_flags |= world::kEntityFlagDead;
    tick_n(w, kCycle);
    CHECK(sys.event_fired(0));
    CHECK((w.script.subgoals.won & (1u << 6)) != 0);
    CHECK(sys.event_fired(1));
    CHECK(w.script.vars.get_mission(9) == 1);
}

int main() {
    test_single_alive_reads_the_bms_ref_rows();
    test_01tr_event34_chain_fires_when_the_placed_item_dies();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("bms_event_parity: all passed\n");
    return 0;
}
