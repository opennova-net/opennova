// BMS event runtime + cross-system tests. The headline assertion: the BMS event
// evaluator and the WAC VM share ONE world and ONE variable store (the original's
// dword_C6B240) -- they are two interfaces to the same thing.
#include <cstdio>

#include "mission/event_runtime.h"
#include "wac/compiler.h"
#include "wac/wac_system.h"
#include "world/ai.h"
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

// A ChangeSingleAI/PLAYPARTANIM action mutates the target's AI brain IN-ENGINE (no host
// effect), faithful to Entity_ApplyCommand @0x43ab60 case 0x22; the AI integrator then
// advances the channel phase. Proves the in-engine action-dispatch path end to end.
static void test_playpartanim_mutates_brain() {
    World w;
    w.registry.configure_pool(0, 8);
    world::Entity org;
    org.net_id = 42;
    org.alive = true;
    world::EntityHandle h = w.registry.spawn(0, org);

    world::AiSystem ai;
    ai.attach(h);
    w.ai = &ai; // the AI-change family reaches the brain through World::ai

    bms::Event e{};
    e.flags = bms::EventFlags::None;
    e.trigger_count = 0; // unconditional -> fires
    e.action_index = 0;
    e.action_count = 1;
    bms::Action act{};
    act.action_type = bms::ActionType::ChangeSingleAI;
    act.action_sub_type = static_cast<int32_t>(bms::AIActionSubType::PlayPartAnim); // 34 / 0x22
    act.param1 = 42;       // target ssn
    act.param2 = 1;        // ANIMNUM channel 1 -> slot 0
    act.param3 = 1;        // ANIMPLAYTYPE = play (+1)
    act.param4 = 1 << 16;  // ANIMTIME = 1.0 s (16.16)
    mission::BmsEventSystem sys;
    sys.load({e}, {}, {act});
    w.add_system(&sys);
    w.load_systems();

    w.run_logic_tick(true);
    world::AiEntity *ae = ai.for_handle(h);
    CHECK(ae != nullptr);
    if (ae) {
        // direction = play_type; rate = trunc(0.016/1.0 * 65536) = 1048.
        CHECK(ae->brain.f[world::AiBrain::kPartAnimDir0] == 1);
        CHECK(ae->brain.f[world::AiBrain::kPartAnimRate0] == 1048);
        // The integrator advances the phase by rate*dir each tick (clamped [0,65535]).
        ai.advance_part_anim(*ae);
        ai.advance_part_anim(*ae);
        CHECK(ae->brain.f[world::AiBrain::kPartAnimPhase0] == 2096);
    }
    // In-engine mutation, NOT a host effect.
    CHECK(w.effects.count("unported_action") == 0);
}

// Host-presentation actions surface as presentation-only EffectLog entries; an unmodelled
// action records as "unported_action" (diagnostic), never as a real effect.
static void test_presentation_effects() {
    World w;
    w.registry.configure_pool(0, 4);
    bms::Event e{};
    e.flags = bms::EventFlags::None;
    e.trigger_count = 0;
    e.action_index = 0;
    e.action_count = 2;
    bms::Action dlg{};
    dlg.action_type = bms::ActionType::PlayWavList;
    dlg.param1 = 7; // dialog id
    dlg.param2 = 1; // always play
    bms::Action wps{};
    wps.action_type = bms::ActionType::ShowWaypoints;
    wps.param1 = 1;
    mission::BmsEventSystem sys;
    sys.load({e}, {}, {dlg, wps});
    w.add_system(&sys);
    w.load_systems();

    w.run_logic_tick(true);
    CHECK(w.effects.count("dialog") == 1);
    CHECK(w.effects.count("show_waypoints") == 1);
    CHECK(w.effects.count("unported_action") == 0);
}

// OutputText surfaces a "text" effect; ResetEvent re-arms a fired event so it fires again.
// (Coverage carried over from the retired MissionRuntime evaluator test.)
static void test_output_text_and_reset_event() {
    World w;
    w.registry.configure_pool(0, 4);
    // Event 0: fire-once, unconditional, OutputText(7).
    bms::Event e0{};
    e0.flags = bms::EventFlags::None;
    e0.trigger_count = 0;
    e0.action_index = 0;
    e0.action_count = 1;
    // Event 1: fire-once, unconditional, ResetEvent(0) -> re-arms event 0.
    bms::Event e1{};
    e1.flags = bms::EventFlags::None;
    e1.trigger_count = 0;
    e1.action_index = 1;
    e1.action_count = 1;
    bms::Action text{};
    text.action_type = bms::ActionType::OutputText;
    text.param1 = 7;
    bms::Action reset{};
    reset.action_type = bms::ActionType::ResetEvent;
    reset.param1 = 0; // re-arm event 0
    mission::BmsEventSystem sys;
    sys.load({e0, e1}, {}, {text, reset});
    w.add_system(&sys);
    w.load_systems();

    w.run_logic_tick(true);
    CHECK(w.effects.count("text") == 1); // event 0 fired OutputText(7)
    // event 1's ResetEvent re-armed event 0, so it fires its OutputText again next tick.
    w.effects.clear();
    w.run_logic_tick(true);
    CHECK(w.effects.count("text") == 1);
}

// PLAYPARTANIM with ANIMTIME=0: rate = INT_MIN (ftol of +inf), so advance_part_anim saturates the
// channel to a clamp endpoint on the first tick (0 forward / 65535 reverse), matching the original.
static void test_playpartanim_zero_time_saturates() {
    world::AiBrain b;
    world::ai_apply_command(b, 0x22, /*channel=*/1, /*play_type=*/1, /*time=*/0);
    CHECK(b.f[world::AiBrain::kPartAnimDir0] == 1);
    CHECK(b.f[world::AiBrain::kPartAnimRate0] == static_cast<int32_t>(0x80000000)); // INT_MIN
    world::AiSystem ai;
    // advance integrates phase += rate*dir (int64), clamped: INT_MIN * +1 -> 0.
    world::AiEntity tmp;
    tmp.brain = b;
    ai.advance_part_anim(tmp);
    CHECK(tmp.brain.f[world::AiBrain::kPartAnimPhase0] == 0);
}

int main() {
    test_bms_to_wac_shared_var();
    test_wac_to_bms_shared_var();
    test_bms_increment_and_threshold();
    test_playpartanim_mutates_brain();
    test_presentation_effects();
    test_output_text_and_reset_event();
    test_playpartanim_zero_time_saturates();
    std::printf(failures ? "EVENT RUNTIME TESTS FAILED (%d)\n" : "event runtime tests passed\n", failures);
    return failures ? 1 : 0;
}
