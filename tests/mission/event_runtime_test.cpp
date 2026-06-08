// BMS event runtime + cross-system tests. The headline assertion: the BMS event
// evaluator and the WAC VM share ONE world and ONE variable store (the original's
// dword_C6B240) -- they are two interfaces to the same thing.
#include <cstdio>

#include "mission/event_runtime.h"
#include "wac/compiler.h"
#include "wac/wac_system.h"
#include "world/world.h"

using namespace opennova;
using world::World;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static bms::Action misvar(bms::MissionVariableActionSubType sub, int var, int val) {
    bms::Action a{};
    a.action_type = bms::ActionType::MisvarChange;
    a.action_sub_type = static_cast<int32_t>(sub);
    a.param1 = var;
    a.param2 = val;
    return a;
}

// BMS sets V5=7; WAC reads the SAME V5 and reacts. Proves the shared var store.
static void test_bms_to_wac_shared_var() {
    World w;
    w.registry.configure_pool(0, 16);

    bms::Event e{};
    e.flags = bms::EventFlags::None;
    e.trigger_count = 0; // unconditional -> fires
    e.action_index = 0;
    e.action_count = 1;
    mission::BmsEventSystem bms_sys;
    bms_sys.load({e}, {}, {misvar(bms::MissionVariableActionSubType::Set, 5, 7)});

    wac::WacSystem wac_sys;
    wac::CompileEnv env;
    wac_sys.set_program(wac::compile_source("if eq(v5,7) then set(v6,1) endif\n", env));

    w.add_system(&bms_sys); // BMS ticks first, sets v5
    w.add_system(&wac_sys);  // WAC ticks next, reads v5
    w.load_systems();

    w.run_logic_tick(true);
    CHECK(w.vars.get_mission(5) == 7); // BMS MisvarChange wrote it
    CHECK(w.vars.get_mission(6) == 1); // WAC eq(v5,7) saw the same var and fired
}

// WAC sets V1=1; a BMS event gated by MissionVariable(V1==1) kills an entity.
// Proves the WAC->BMS direction across the shared var + shared entity commands.
static void test_wac_to_bms_shared_var() {
    World w;
    w.registry.configure_pool(0, 16);
    world::Entity tgt;
    tgt.net_id = 100;
    tgt.alive = true;
    w.registry.spawn(0, tgt);

    wac::WacSystem wac_sys;
    wac::CompileEnv env;
    wac_sys.set_program(wac::compile_source("if never() then set(v1,1) endif\n", env));

    bms::Event e{};
    e.flags = bms::EventFlags::None;
    e.trigger_index = 0;
    e.trigger_count = 1;
    e.action_index = 0;
    e.action_count = 1;
    bms::Trigger t{};
    t.condition_flags = 0;
    t.main_type = bms::TriggerMainType::MissionVariable;
    t.sub_type = static_cast<int32_t>(bms::MissionVariableTriggerType::MissionVariableIsEqual);
    t.param1 = 1; // V1
    t.param2 = 1; // == 1
    bms::Action kill{};
    kill.action_type = bms::ActionType::KillSingle;
    kill.param1 = 100;
    mission::BmsEventSystem bms_sys;
    bms_sys.load({e}, {t}, {kill});

    w.add_system(&wac_sys); // WAC sets v1=1
    w.add_system(&bms_sys);  // BMS sees v1==1 -> kills 100
    w.load_systems();

    w.run_logic_tick(true);
    CHECK(w.vars.get_mission(1) == 1);
    CHECK(w.commands.ssn_dead(100));
}

// Pure BMS: var counter via Increment, trigger threshold, fire-once vs ResetAfter.
static void test_bms_increment_and_threshold() {
    World w;
    w.registry.configure_pool(0, 8);

    // Event A: repeating, unconditional, increments V2 every tick.
    bms::Event a{};
    a.flags = bms::EventFlags::ResetAfter; // repeat
    a.trigger_count = 0;
    a.action_index = 0;
    a.action_count = 1;
    // Event B: fire-once, when V2 >= 3, set V3 = 1.
    bms::Event b{};
    b.flags = bms::EventFlags::None; // fire once
    b.trigger_index = 0;
    b.trigger_count = 1;
    b.action_index = 1;
    b.action_count = 1;

    bms::Trigger t{};
    t.main_type = bms::TriggerMainType::MissionVariable;
    t.sub_type = static_cast<int32_t>(bms::MissionVariableTriggerType::MissionVariableIsGreaterThanOrEqual);
    t.param1 = 2; // V2
    t.param2 = 3; // >= 3

    mission::BmsEventSystem sys;
    sys.load({a, b}, {t},
             {misvar(bms::MissionVariableActionSubType::Increment, 2, 0),
              misvar(bms::MissionVariableActionSubType::Set, 3, 1)});
    w.add_system(&sys);
    w.load_systems();

    for (int i = 0; i < 5; ++i) w.run_logic_tick(true);
    CHECK(w.vars.get_mission(2) == 5); // incremented every tick (ResetAfter)
    CHECK(w.vars.get_mission(3) == 1); // threshold event fired once V2 hit 3
}

int main() {
    test_bms_to_wac_shared_var();
    test_wac_to_bms_shared_var();
    test_bms_increment_and_threshold();
    std::printf(failures ? "EVENT RUNTIME TESTS FAILED (%d)\n" : "event runtime tests passed\n", failures);
    return failures ? 1 : 0;
}
