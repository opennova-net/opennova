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

// A trigger record with just a main/sub type and params.
static bms::Trigger make_trigger(bms::TriggerMainType main, int sub, int p1 = 0, int p2 = 0) {
    bms::Trigger t{};
    t.main_type = main;
    t.sub_type = sub;
    t.param1 = p1;
    t.param2 = p2;
    return t;
}

// One-trigger event whose action sets V9=1 (the fired probe).
static void load_probe(mission::BmsEventSystem &sys, const bms::Trigger &t) {
    bms::Event e{};
    e.flags = bms::EventFlags::None;
    e.trigger_index = 0;
    e.trigger_count = 1;
    e.action_index = 0;
    e.action_count = 1;
    sys.load({e}, {t}, {misvar(bms::MissionVariableActionSubType::Set, 9, 1)});
}

// The session load-parity toggle: static image value 1, one XOR per BMS load
// [orig: dword_815174 ^= 1 at EventTrigger_LoadAllData @0x454029; read raw
// @0x453b24]. Assertions are RELATIVE to the current process state (every
// load() in this binary flips it), which is exactly the retail contract.
static void test_second_time_through_parity() {
    const bool before = mission::BmsEventSystem::second_time_through();
    for (int round = 0; round < 2; ++round) {
        World w;
        w.registry.configure_pool(0, 4);
        mission::BmsEventSystem sys;
        load_probe(sys, make_trigger(bms::TriggerMainType::SecondTimeThrough, 0));
        const bool expected = (round == 0) ? !before : before;
        CHECK(mission::BmsEventSystem::second_time_through() == expected);
        w.add_system(&sys);
        w.load_systems();
        tick_n(w, kPass);
        CHECK((w.vars.get_mission(9) == 1) == expected);
    }
}

// Teammate category [orig: EventTrigger_EvaluateCondition cat 6 @0x453b3c..0x453b67]:
// TeammateIsEnabled = !in-session && !(option bit); MedicAssisting/Evacuating both
// read the heli-lift active count.
static void test_teammate_triggers() {
    const int kEnabled = static_cast<int>(bms::TeammateTriggerType::TeammateIsEnabled);
    const int kMedic = static_cast<int>(bms::TeammateTriggerType::TeammateMedicAssisting);
    const int kEvac = static_cast<int>(bms::TeammateTriggerType::TeammateEvacuating);
    struct Case { int sub; bool mp; bool disabled; int lifts; bool fires; };
    const Case cases[] = {
        {kEnabled, false, false, 0, true},   // SP, option on -> enabled
        {kEnabled, false, true, 0, false},   // SP, teammates disabled
        {kEnabled, true, false, 0, false},   // MP session always disables
        {kMedic, false, false, 0, false},    // no lift op in flight
        {kMedic, false, false, 1, true},     // a lift op in flight
        {kEvac, false, false, 2, true},      // Evacuating reads the same count
    };
    for (const Case &c : cases) {
        World w;
        w.registry.configure_pool(0, 4);
        w.mp_session = c.mp;
        w.teammates_disabled = c.disabled;
        w.heli_lift_active_count = c.lifts;
        mission::BmsEventSystem sys;
        load_probe(sys, make_trigger(bms::TriggerMainType::Teammate, c.sub));
        w.add_system(&sys);
        w.load_systems();
        tick_n(w, kPass);
        CHECK((w.vars.get_mission(9) == 1) == c.fires);
    }
}

// The AWOL counter: +1 per full quarter cycle (64 ticks) while the local player
// is outside every ACTIVE zone (X/Y only), reset when back inside; PlayerAwol
// compares it to the authored threshold. [orig: @0x454d50 cursor==0 ->
// Entity_UpdateStuckCounter @0x439dc0 / probe @0x439d40; trigger @0x453d40]
static void test_player_awol_counter_and_trigger() {
    World w;
    w.registry.configure_pool(0, 4);
    world::Aabb zone;
    zone.min = {0.0f, 0.0f, -16384.0f};
    zone.max = {100.0f, 100.0f, 16384.0f};
    w.registry.register_area("", zone, /*active=*/true);
    world::Entity seed{};
    seed.alive = true;
    seed.position = {50.0f, 50.0f, 500.0f}; // inside in X/Y; Z is ignored
    world::EntityHandle player = w.registry.spawn(0, seed);
    w.cached.local_player = player;

    mission::BmsEventSystem sys;
    load_probe(sys, make_trigger(bms::TriggerMainType::Player,
                                 static_cast<int>(bms::PlayerTriggerType::PlayerAwol),
                                 /*threshold=*/2));
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kCycle);
    CHECK(sys.awol_count() == 0); // inside the active zone

    w.registry.get(player)->position = {500.0f, 500.0f, 0.0f}; // out of every zone
    tick_n(w, kCycle);
    CHECK(sys.awol_count() == 1);
    CHECK(w.vars.get_mission(9) == 0); // threshold 2 not reached
    tick_n(w, kCycle);
    CHECK(sys.awol_count() == 2);
    CHECK(w.vars.get_mission(9) == 1); // PlayerAwol fires at >= threshold

    w.registry.get(player)->position = {50.0f, 50.0f, 0.0f};
    tick_n(w, kCycle);
    CHECK(sys.awol_count() == 0); // back in bounds resets

    // An inactive-only zone world never counts as out of bounds.
    World w2;
    w2.registry.configure_pool(0, 4);
    w2.registry.register_area("", zone, /*active=*/false);
    world::EntityHandle p2 = w2.registry.spawn(0, seed);
    w2.registry.get(p2)->position = {500.0f, 500.0f, 0.0f};
    w2.cached.local_player = p2;
    mission::BmsEventSystem sys2;
    load_probe(sys2, make_trigger(bms::TriggerMainType::Player,
                                  static_cast<int>(bms::PlayerTriggerType::PlayerAwol), 1));
    w2.add_system(&sys2);
    w2.load_systems();
    tick_n(w2, kCycle);
    CHECK(sys2.awol_count() == 0);
}

// The post pass is a host-called one-shot sweep, never periodic (D-EVT-4)
// [orig: UpdateAllWithFlag4 @0x454e00, one call per teardown/restart]. Normal
// ticks must never touch a PostMission-flag entry.
static void test_post_pass_is_a_one_shot() {
    World w;
    w.registry.configure_pool(0, 4);
    mission::BmsEventSystem sys;
    sys.load({simple_event(bms::EventFlags::PostMission, 0)}, {},
             {misvar(bms::MissionVariableActionSubType::Increment, 9, 0)});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kCycle * 2);
    CHECK(w.vars.get_mission(9) == 0); // normal ticking never runs post entries

    sys.run_post_mission_pass(w);
    CHECK(w.vars.get_mission(9) == 1); // exactly one sweep per transition call
    sys.run_post_mission_pass(w);
    CHECK(w.vars.get_mission(9) == 1); // fire-once latch holds across transitions
}


// Slice B (TriggerRelations): the cat-1 group records + relation matrices +
// visited matrices behind the Group/Single trigger categories (record §3a).
static bms::Trigger group_trigger(bms::GroupTriggerType sub, int p1, int p2 = 0, int p3 = 0) {
    bms::Trigger t{};
    t.main_type = bms::TriggerMainType::Group;
    t.sub_type = static_cast<int32_t>(sub);
    t.param1 = p1;
    t.param2 = p2;
    t.param3 = p3;
    return t;
}

static void test_trigger_relations_group_records() {
    World w;
    w.registry.configure_pool(0, 16);
    world::Entity seed{};
    seed.alive = true;
    seed.group_id = 3;
    for (int i = 0; i < 3; ++i) {
        seed.net_id = static_cast<uint16_t>(10 + i);
        w.registry.spawn(0, seed);
    }
    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();

    // Initial counts land on the pre-mission pass, ordered after the pre
    // sweep [orig: Game_StartMission @ 0x525b86 -> @ 0x525b8b].
    w.run_logic_tick(true, /*pre_mission=*/true);
    CHECK(w.relations.group(3).initial_count == 3);
    CHECK(w.relations.group(3).live_count == 3);

    // A kill reads STALE until the 62-tick live rescan — retail cadence
    // [orig: timer reload 0x3E @ 0x51db93 -> EntityPool_RecountLiveByGroup].
    bms::Trigger lost = group_trigger(bms::GroupTriggerType::GroupHasLostMoreUnits, 3, 1);
    w.commands.kill_ssn(10);
    CHECK(!sys.evaluate_trigger_for_test(w, lost));
    tick_n(w, 62);
    CHECK(w.relations.group(3).live_count == 2);
    CHECK(sys.evaluate_trigger_for_test(w, lost));
    CHECK(!sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupIntact, 3)));
    CHECK(sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupHasMoreUnits, 3, 2)));
    CHECK(sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupAlive, 3)));
    CHECK(!sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupDestroyed, 3)));

    // Alert stamps via the ChangeGroupAI alert subs, brains or not
    // [orig: Entity_HandleAlertCommand @ 0x43cff7 — 5 red, 6 green, 22 yellow].
    w.commands.apply_group_ai_command(3, 5, 0, 0, 0);
    CHECK(sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupAtRedAlert, 3)));
    w.commands.apply_group_ai_command(3, 22, 0, 0, 0);
    CHECK(sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupAtYellowAlert, 3)));
    w.commands.apply_group_ai_command(3, 6, 0, 0, 0);
    CHECK(!sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupAtRedAlert, 3)));
}

static void test_trigger_relations_matrices_and_visited() {
    World w;
    w.registry.configure_pool(0, 4);
    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();
    using R = world::TriggerRelations;

    w.relations.set_group_group(R::kSees, 2, 5);
    CHECK(sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupSeesGroup, 2, 5)));
    CHECK(!sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupSeesGroup, 5, 2)));

    // The G->S table is transposed storage; the trigger still reads
    // (group, single) [orig: index [2b + (a >> 5)] at the GS bases].
    w.relations.set_group_single(R::kShot, 7, 40);
    CHECK(sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupHasShotSingle, 7, 40)));

    bms::Trigger sss{};
    sss.main_type = bms::TriggerMainType::Single;
    sss.sub_type = static_cast<int32_t>(bms::SingleTriggerType::SingleSeesSingle);
    sss.param1 = 100;
    sss.param2 = 101;
    w.relations.set_single_single(R::kSees, 100, 101);
    CHECK(sys.evaluate_trigger_for_test(w, sss));

    // Out-of-range rows: retail writes are guarded (< 0x80) and our reads are
    // sanitized to false instead of reproducing the unguarded OOB read
    // (record §3a, ADR 0003 class).
    w.relations.set_single_single(R::kSees, 200, 5); // no-op
    sss.param1 = 200;
    sss.param2 = 5;
    CHECK(!sys.evaluate_trigger_for_test(w, sss));

    // Visited matrices + the 32-list clear quirk: actions 32/33 memset only
    // 0x80 bytes = lists 0..31 of the row [orig: EventTrigger_ClearSlotB
    // @ 0x4535e0 / ClearSlotA @ 0x453600].
    w.relations.mark_waypoint_visited(9, 4, /*list=*/5, /*number=*/3);
    w.relations.mark_waypoint_visited(9, 4, /*list=*/40, /*number=*/3);
    CHECK(sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupAtWaypoint, 4, 5, 3)));
    CHECK(sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupAtWaypoint, 4, 40, 3)));
    bms::Action clear_group{};
    clear_group.action_type = bms::ActionType::GroupResetHasVisited;
    clear_group.param1 = 4;
    sys.dispatch_action_for_test(w, clear_group);
    CHECK(!sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupAtWaypoint, 4, 5, 3)));
    CHECK(sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupAtWaypoint, 4, 40, 3)));

    // Mission reload zeroes everything [orig: EventSystem_FreeAll @ 0x453210].
    sys.on_load(w);
    CHECK(!sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupAtWaypoint, 4, 40, 3)));
    CHECK(!sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupSeesGroup, 2, 5)));
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
    test_second_time_through_parity();
    test_teammate_triggers();
    test_player_awol_counter_and_trigger();
    test_post_pass_is_a_one_shot();
    test_trigger_relations_group_records();
    test_trigger_relations_matrices_and_visited();
    std::printf(failures ? "EVENT RUNTIME TESTS FAILED (%d)\n" : "event runtime tests passed\n", failures);
    return failures ? 1 : 0;
}
