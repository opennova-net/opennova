// BMS event runtime + cross-system tests. The headline assertion: the BMS event
// evaluator and the WAC VM share ONE world and ONE variable store (the original's
// dword_C6B240) -- they are two interfaces to the same thing.
//
// Cadence model under test (grilled 2026-06-10, docs/mission/bms-event-runtime-re.md):
// normal events are processed in QUARTER-LIST slices every 16th tick
// [orig: Server_TickUpdate @0x51d7e0 -> @0x454d50], so an event at index i of n is
// touched once per 64 ticks, on pass p where (p-1)&3 == i/ceil(n/4); the WAC VM
// executes every 62nd tick [orig: sub_4F81A0 @0x4f81b1].
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

// One BMS event-processing pass happens every 16th tick; a single event is touched
// every 4th pass (64 ticks).
static constexpr int kPass = 16;
static constexpr int kCycle = 64;

static void tick_n(World &w, int n) {
    for (int i = 0; i < n; ++i) w.run_logic_tick(true);
}

static bms::Action misvar(bms::MissionVariableActionSubType sub, int var, int val) {
    bms::Action a{};
    a.action_type = bms::ActionType::MisvarChange;
    a.action_sub_type = static_cast<int32_t>(sub);
    a.param1 = var;
    a.param2 = val;
    return a;
}

static bms::Action output_text(int id) {
    bms::Action a{};
    a.action_type = bms::ActionType::OutputText;
    a.param1 = id;
    return a;
}

// An unconditional event (no triggers => chain true) with one action at `action_index`.
static bms::Event simple_event(bms::EventFlags flags, int action_index) {
    bms::Event e{};
    e.flags = flags;
    e.trigger_count = 0;
    e.action_index = action_index;
    e.action_count = 1;
    return e;
}

// BMS sets V5=7; WAC reads the SAME V5 and reacts. Proves the shared var store.
static void test_bms_to_wac_shared_var() {
    World w;
    w.registry.configure_pool(0, 16);

    mission::BmsEventSystem bms_sys;
    bms_sys.load({simple_event(bms::EventFlags::None, 0)}, {},
                 {misvar(bms::MissionVariableActionSubType::Set, 5, 7)});

    wac::WacSystem wac_sys;
    wac::CompileEnv env;
    wac_sys.set_program(wac::compile_source("if eq(v5,7) then set(v6,1) endif\n", env));

    w.add_system(&bms_sys); // BMS evaluator first, then WAC -- registration order
    w.add_system(&wac_sys);
    w.load_systems();

    tick_n(w, kPass);
    CHECK(w.vars.get_mission(5) == 7); // BMS fired on its first processing pass
    CHECK(w.vars.get_mission(6) == 0); // WAC has not executed yet (62-tick divider)
    tick_n(w, 62 - kPass);
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

    w.add_system(&wac_sys); // WAC sets v1=1 (tick 62)
    w.add_system(&bms_sys);  // BMS sees v1==1 on its next pass (tick 80) -> kills 100
    w.load_systems();

    tick_n(w, 62);
    CHECK(w.vars.get_mission(1) == 1);
    CHECK(!w.commands.ssn_dead(100)); // the event's quarter is next touched at tick 80
    tick_n(w, 80 - 62);
    CHECK(w.commands.ssn_dead(100));
}

// Pure BMS: var counter via Increment, trigger threshold, fire-once vs ResetAfter.
// Two events: index 0 is touched on passes 1,5,9,.. (ticks 16,80,144,208) and index 1
// on passes 2,6,10,.. (ticks 32,96,160,224).
static void test_bms_increment_and_threshold() {
    World w;
    w.registry.configure_pool(0, 8);

    // Event A: repeating (reset_after=0), unconditional, increments V2 each pass.
    bms::Event a = simple_event(bms::EventFlags::ResetAfter, 0);
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

    tick_n(w, 224);
    CHECK(w.vars.get_mission(2) == 4); // A fired at ticks 16/80/144/208
    CHECK(w.vars.get_mission(3) == 1); // B saw V2==3 at tick 160, fired once
}

// Activation delay [orig: event +18 reload -> +16 countdown, -64 per processing pass].
// delay=2 (authored units): armed on the pass that passes the chain, counts 128->64->0
// across the next two passes, fires on the third. event_fired() (the cat-3 read:
// active && countdown==0) stays false while the delay counts.
static void test_activation_delay() {
    World w;
    w.registry.configure_pool(0, 4);
    bms::Event e = simple_event(bms::EventFlags::None, 0);
    e.delay = 2;
    mission::BmsEventSystem sys;
    sys.load({e}, {}, {output_text(7)});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kPass); // pass 1: chain true -> latch + arm (no fire)
    CHECK(w.effects.count("text") == 0);
    CHECK(!sys.event_fired(0)); // active but still counting
    tick_n(w, kCycle); // pass @tick 80: 128 -> 64
    CHECK(w.effects.count("text") == 0);
    tick_n(w, kCycle); // pass @tick 144: 64 -> 0 -> fire
    CHECK(w.effects.count("text") == 1);
    CHECK(sys.event_fired(0));
}

// 16-bit signedness quirk [orig: unsigned word load, SIGNED 16-bit <=0 test @0x454cef]:
// delay values >= 513 wrap negative on the first decrement and fire one pass after arming.
static void test_activation_delay_signed_wrap() {
    World w;
    w.registry.configure_pool(0, 4);
    bms::Event e = simple_event(bms::EventFlags::None, 0);
    e.delay = 1023; // reload 0xFFC0; first decrement -> 0xFF80 = -128 as int16 -> fires
    mission::BmsEventSystem sys;
    sys.load({e}, {}, {output_text(7)});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kPass);          // arm
    CHECK(w.effects.count("text") == 0);
    tick_n(w, kCycle);         // first decrement: wraps negative -> fires
    CHECK(w.effects.count("text") == 1);
}

// Repeat cooldown [orig: event +14 reload -> +12 countdown; +20 cleared on expiry].
// reset_after=2: fire, then two passes of cooldown, then re-evaluate and re-fire.
// event_fired() is a WINDOW: true from the fire until the cooldown expires.
static void test_repeat_cooldown() {
    World w;
    w.registry.configure_pool(0, 4);
    bms::Event e = simple_event(bms::EventFlags::ResetAfter, 0);
    e.reset_after = 2;
    mission::BmsEventSystem sys;
    sys.load({e}, {}, {output_text(9)});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kPass); // pass 1 (tick 16): fire + arm cooldown 128
    CHECK(w.effects.count("text") == 1);
    CHECK(sys.event_fired(0)); // window open
    tick_n(w, kCycle); // tick 80: cooldown 128 -> 64
    CHECK(w.effects.count("text") == 1);
    CHECK(sys.event_fired(0));
    tick_n(w, kCycle); // tick 144: cooldown 64 -> 0, latch cleared
    CHECK(w.effects.count("text") == 1);
    CHECK(!sys.event_fired(0)); // window closed
    tick_n(w, kCycle); // tick 208: re-evaluated -> fires again
    CHECK(w.effects.count("text") == 2);
}

// reset_after == 0: a repeat event re-fires on EVERY processing pass while its chain
// holds [orig: the LABEL_24 path clears +20 in the same call].
static void test_repeat_zero_refires_every_pass() {
    World w;
    w.registry.configure_pool(0, 4);
    mission::BmsEventSystem sys;
    sys.load({simple_event(bms::EventFlags::ResetAfter, 0)}, {}, {output_text(3)});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kPass + 2 * kCycle); // passes at ticks 16, 80, 144
    CHECK(w.effects.count("text") == 3);
}

// PreMission events run on the pre-mission pass (whole list per call, no 16-tick gate)
// and are EXCLUDED from the normal quarter pass; normal events ignore the pre pass.
// [orig: UpdateAllWithFlag2 @0x454dc0 (flags&2) vs the (flags&6)==0 quarter pass]
static void test_pre_mission_pass() {
    World w;
    w.registry.configure_pool(0, 4);
    mission::BmsEventSystem sys;
    sys.load({simple_event(bms::EventFlags::PreMission, 0),
              simple_event(bms::EventFlags::None, 1)},
             {}, {output_text(1), output_text(2)});
    w.add_system(&sys);
    w.load_systems();

    w.run_logic_tick(true, /*pre_mission=*/true); // one pre pass
    CHECK(w.effects.count("text") == 1); // only the PreMission event fired
    tick_n(w, kPass + kCycle); // normal passes touch only the normal event
    CHECK(w.effects.count("text") == 2); // +1 from the normal event, pre event excluded
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

    bms::Event e = simple_event(bms::EventFlags::None, 0);
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

    tick_n(w, kPass); // first processing pass fires the event
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
    bms::Event e = simple_event(bms::EventFlags::None, 0);
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

    tick_n(w, kPass);
    CHECK(w.effects.count("dialog") == 1);
    CHECK(w.effects.count("show_waypoints") == 1);
    CHECK(w.effects.count("unported_action") == 0);
}

// OutputText surfaces a "text" effect; ResetEvent clears the target's active latch
// [orig: EventAction_Dispatch case 0x22 @0x454974 -- ONLY event +20] so a fired
// fire-once event becomes evaluable again on its next processing pass.
static void test_output_text_and_reset_event() {
    World w;
    w.registry.configure_pool(0, 4);
    // Event 0 (passes at ticks 16/80/144): fire-once, unconditional, OutputText(7).
    // Event 1 (passes at ticks 32/96): fire-once, unconditional, ResetEvent(0).
    bms::Action reset{};
    reset.action_type = bms::ActionType::ResetEvent;
    reset.param1 = 0; // re-arm event 0
    mission::BmsEventSystem sys;
    sys.load({simple_event(bms::EventFlags::None, 0),
              simple_event(bms::EventFlags::None, 1)},
             {}, {output_text(7), reset});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kPass);
    CHECK(w.effects.count("text") == 1); // event 0 fired OutputText(7) at tick 16
    w.effects.clear();
    // tick 32: event 1 fires ResetEvent -> clears event 0's latch.
    // tick 80: event 0 re-evaluates and fires its OutputText again.
    tick_n(w, 80 - kPass);
    CHECK(w.effects.count("text") == 1);
}

// The Event trigger category reads the live latch window (active && delay elapsed),
// not a sticky has-fired bit [orig: EvaluateCondition case 3 @0x453a75]. Event 1,
// gated on Event(0), fires while event 0's window is open.
static void test_event_trigger_reads_window() {
    World w;
    w.registry.configure_pool(0, 4);
    // Event 0: repeat, cooldown 2 units -> window open ticks 16..144.
    bms::Event e0 = simple_event(bms::EventFlags::ResetAfter, 0);
    e0.reset_after = 2;
    // Event 1: fire-once, trigger = Event(0) fired; processed at tick 32 (window open).
    bms::Event e1{};
    e1.flags = bms::EventFlags::None;
    e1.trigger_index = 0;
    e1.trigger_count = 1;
    e1.action_index = 1;
    e1.action_count = 1;
    bms::Trigger t{};
    t.main_type = bms::TriggerMainType::Event;
    t.param1 = 0;
    mission::BmsEventSystem sys;
    sys.load({e0, e1}, {t}, {output_text(1), output_text(2)});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kPass); // event 0 fires; window opens
    CHECK(sys.event_fired(0));
    tick_n(w, kPass); // event 1's pass: sees the open window -> fires
    CHECK(w.effects.count("text") == 2);
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
    test_activation_delay();
    test_activation_delay_signed_wrap();
    test_repeat_cooldown();
    test_repeat_zero_refires_every_pass();
    test_pre_mission_pass();
    test_playpartanim_mutates_brain();
    test_presentation_effects();
    test_output_text_and_reset_event();
    test_event_trigger_reads_window();
    test_playpartanim_zero_time_saturates();
    std::printf(failures ? "EVENT RUNTIME TESTS FAILED (%d)\n" : "event runtime tests passed\n", failures);
    return failures ? 1 : 0;
}
