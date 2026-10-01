// BMS event runtime + cross-system tests. The headline assertion: the BMS event
// evaluator and the WAC VM share ONE world and ONE variable store (the original's
// dword_C6B240) -- they are two interfaces to the same thing.
//
// Cadence model under test (grilled 2026-06-10, docs/mission/bms-event-runtime-re.md):
// normal events are processed in QUARTER-LIST slices every 16th tick
// [orig: Server_TickUpdate @0x51d7e0 -> @0x454d50], so an event at index i of n is
// touched once per 64 ticks, on pass p where (p-1)&3 == i/ceil(n/4); the WAC VM
// executes every 62nd tick [orig: WacScript_AdvanceTick @0x4f81b1].
#include <cstdio>
#include <array>

#include <runtime/mission/event_runtime.h>
#include <runtime/wac/compiler.h>
#include <runtime/wac/wac_system.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/world.h>

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
    // A mission only advances while a human is in the world - retail holds the
    // WAC tick and the BMS event pump on `g_WacVarHumans || !g_WacVarTicks`
    // (World::script_may_advance). These harnesses model a mission IN PROGRESS,
    // so they stand a player up; the empty-server hold has its own test.
    w.cached.humans = 1;
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
    CHECK(w.script.vars.get_mission(5) == 7); // BMS fired on its first processing pass
    CHECK(w.script.vars.get_mission(6) == 0); // WAC has not executed yet (62-tick divider)
    tick_n(w, 62 - kPass);
    CHECK(w.script.vars.get_mission(6) == 1); // WAC eq(v5,7) saw the same var and fired
}

// WAC sets V1=1; a BMS event gated by MissionVariable(V1==1) kills an entity.
// Proves the WAC->BMS direction across the shared var + shared entity commands.
static void test_wac_to_bms_shared_var() {
    World w;
    w.cached.humans = 1;
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
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK(!w.commands.ssn_dead(100)); // the event's quarter is next touched at tick 80
    tick_n(w, 80 - 62);
    CHECK(w.commands.ssn_dead(100));
}

// A mission WAC script can tune the global infantry aim spread, and the AI pass
// consumes the new value. Retail resolves `accuracyspread` through the writable
// named-value table, then reads that same dword in the sawtooth aim-error formula.
// [orig: WacScript_ResolveParameter @0x4f2920 -> g_WacVarAccuracySpread
// @0xC6EAE8; Entity_UpdateInfantryAI @0x4bc5ea]
static void test_wac_accuracyspread_drives_npc_aim() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);

    world::Entity target{};
    target.kind = world::EntityKind::Organic;
    target.team = 2;
    target.health = 100;
    target.position = {20.0f, 0.0f, 0.0f};
    const world::EntityHandle target_h = w.registry.spawn(0, target);

    world::Entity npc_seed{};
    npc_seed.kind = world::EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    const world::EntityHandle npc_h = w.registry.spawn(0, npc_seed);

    world::AiSystem ai;
    const int ai_index = ai.attach(npc_h);
    world::AiEntity &npc = *ai.at(ai_index);
    npc.team = 1;
    npc.health = 100;
    npc.inf.combat_target = target_h;
    npc.inf.anim_state = world::anim_state::kAttack;
    // No lead: the target stood still over its last tick, so its mover's
    // savedLivePose stamp equals its position [orig: the lead's
    // `sub ecx,[ebp+80h]` @0x4BCB2D].
    world::stamp_saved_live_pose(*w.registry.get(target_h));
    npc.slot.f[11] = 1;              // fresh-target accuracy parameter

    // Spread 1 (the mission-load default is 10 [orig: WacScript_FreeAll @0x4f6395];
    // pinned to 1 here so the WAC write below is an exact x3): target is due east,
    // so the heading is only the witnessed sawtooth error term.
    w.script.wac_values.accuracy_spread = 1;
    ai.infantry_combat_think(npc, w, /*key=*/1);
    CHECK(npc.inf.aim_valid);
    const int32_t default_heading = npc.inf.aim_heading;
    CHECK(default_heading != 0);

    wac::WacSystem wac_sys;
    wac::CompileEnv env;
    wac_sys.set_program(wac::compile_source(
        "if never() then set(AcCuRaCySpReAd,3) endif\n"
        "if eq(accuracyspread,3) then set(v9,1) endif\n", env));
    w.add_system(&wac_sys);
    w.load_systems();
    tick_n(w, wac::WacSystem::kTicksPerExecution);
    CHECK(w.script.vars.get_mission(0) == 0); // the named lvalue must not alias V0
    CHECK(w.script.vars.get_mission(9) == 1); // WAC can read its write back

    ai.infantry_combat_think(npc, w, /*key=*/1);
    CHECK(npc.inf.aim_heading == default_heading * 3);
}

// WAC's writable seatbelt value locks local USE, including seat swaps, while
// script-forced detach remains available. [orig: @0xC6EADC / @0x43698B]
static void test_wac_seatbelt_controls_local_use() {
	World w;
	w.cached.humans = 1;
	w.registry.configure_pool(0, 4);
	w.registry.configure_pool(1, 4);
	world::Entity player;
	player.kind = world::EntityKind::Organic;
	player.health = 100;
	const auto ph = w.registry.spawn(0, player);
	w.cached.local_player = ph;
	world::Entity carrier;
	carrier.has_item_def = true;
	carrier.item_type = 1;
	carrier.item_attrib = world::kItemAttribPlayerControl;
	carrier.health = 100;
	world::Seat seat;
	seat.type = world::SeatType::Controller;
	seat.bone_index = 1;
	carrier.seats.push_back(seat);
	const auto vh = w.registry.spawn(1, carrier);
	CHECK(w.vehicles.process_attach(ph, vh, 1));
	wac::WacSystem ws;
	wac::CompileEnv env;
	auto program = wac::compile_source("if never() then set(SeAtBeLt,1) endif\n"
									   "if eq(seatbelt,1) then set(v9,1) endif\n"
									   "if eq(v10,1) then set(seatbelt,0) endif\n",
			env);
	CHECK(program.diagnostics.empty());
	ws.set_program(std::move(program));
	w.add_system(&ws);
	w.load_systems();
	tick_n(w, wac::WacSystem::kTicksPerExecution);
	CHECK(w.script.vars.get_mission(0) == 0);
	CHECK(w.script.vars.get_mission(9) == 1);
	CHECK(!w.vehicles.player_toggle_mount(ph));
	CHECK(w.registry.get(ph)->mounted);
	CHECK(w.vehicles.detach(ph)); // forced script/API detach bypasses USE
	CHECK(w.vehicles.process_attach(ph, vh, 1));
	w.script.vars.set_mission(10, 1);
	tick_n(w, wac::WacSystem::kTicksPerExecution);
	CHECK(w.script.wac_values.seatbelt == 0);
	CHECK(w.vehicles.player_toggle_mount(ph));
	CHECK(!w.registry.get(ph)->mounted);
}

// Pure BMS: var counter via Increment, trigger threshold, fire-once vs ResetAfter.
// Two events: index 0 is touched on passes 1,5,9,.. (ticks 16,80,144,208) and index 1
// on passes 2,6,10,.. (ticks 32,96,160,224).
static void test_bms_increment_and_threshold() {
    World w;
    w.cached.humans = 1;
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
    CHECK(w.script.vars.get_mission(2) == 4); // A fired at ticks 16/80/144/208
    CHECK(w.script.vars.get_mission(3) == 1); // B saw V2==3 at tick 160, fired once
}

// Activation delay [orig: event +18 reload -> +16 countdown, -64 per processing pass].
// delay=2 (authored units): armed on the pass that passes the chain, counts 128->64->0
// across the next two passes, fires on the third. event_fired() (the cat-3 read:
// active && countdown==0) stays false while the delay counts.
static void test_activation_delay() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    bms::Event e = simple_event(bms::EventFlags::None, 0);
    e.delay = 2;
    mission::BmsEventSystem sys;
    sys.load({e}, {}, {output_text(7)});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kPass); // pass 1: chain true -> latch + arm (no fire)
    CHECK(w.out.effects.count("text") == 0);
    CHECK(!sys.event_fired(0)); // active but still counting
    tick_n(w, kCycle); // pass @tick 80: 128 -> 64
    CHECK(w.out.effects.count("text") == 0);
    tick_n(w, kCycle); // pass @tick 144: 64 -> 0 -> fire
    CHECK(w.out.effects.count("text") == 1);
    CHECK(sys.event_fired(0));
}

// 16-bit signedness quirk [orig: unsigned word load, SIGNED 16-bit <=0 test @0x454cef]:
// delay values >= 513 wrap negative on the first decrement and fire one pass after arming.
static void test_activation_delay_signed_wrap() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    bms::Event e = simple_event(bms::EventFlags::None, 0);
    e.delay = 1023; // reload 0xFFC0; first decrement -> 0xFF80 = -128 as int16 -> fires
    mission::BmsEventSystem sys;
    sys.load({e}, {}, {output_text(7)});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kPass);          // arm
    CHECK(w.out.effects.count("text") == 0);
    tick_n(w, kCycle);         // first decrement: wraps negative -> fires
    CHECK(w.out.effects.count("text") == 1);
}

// Repeat cooldown [orig: event +14 reload -> +12 countdown; +20 cleared on expiry].
// reset_after=2: fire, then two passes of cooldown, then re-evaluate and re-fire.
// event_fired() is a WINDOW: true from the fire until the cooldown expires.
static void test_repeat_cooldown() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    bms::Event e = simple_event(bms::EventFlags::ResetAfter, 0);
    e.reset_after = 2;
    mission::BmsEventSystem sys;
    sys.load({e}, {}, {output_text(9)});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kPass); // pass 1 (tick 16): fire + arm cooldown 128
    CHECK(w.out.effects.count("text") == 1);
    CHECK(sys.event_fired(0)); // window open
    tick_n(w, kCycle); // tick 80: cooldown 128 -> 64
    CHECK(w.out.effects.count("text") == 1);
    CHECK(sys.event_fired(0));
    tick_n(w, kCycle); // tick 144: cooldown 64 -> 0, latch cleared
    CHECK(w.out.effects.count("text") == 1);
    CHECK(!sys.event_fired(0)); // window closed
    tick_n(w, kCycle); // tick 208: re-evaluated -> fires again
    CHECK(w.out.effects.count("text") == 2);
}

// The file preserves an unsigned byte, but BOTH dispatch loops read it as a
// signed char. Skipped actions do not skip the input consume, linked-spawn
// hook, active latch, or repeat cooldown. Exercise the public World tick path.
// [orig: EventTrigger_UpdateEntry @0x454C92/@0x454CAB and @0x454D01/@0x454D13]
static void test_signed_action_count_immediate_and_delayed() {
    struct Case { uint8_t count; int available; int executed; };
    const Case cases[] = {
        {0, 255, 0}, {1, 255, 1}, {127, 255, 127},
        {128, 255, 0}, {255, 255, 0},
        {127, 1, 1}, // bounded portable slice: never copy retail's invalid reads
    };
    for (const Case &c : cases) {
        for (int delay : {0, 1}) {
            World w;
            w.cached.humans = 1;
            w.registry.configure_pool(0, 4);
            world::WaypointEntry marker;
            marker.linked_event = 1; // retail reserves event index zero as no link
            w.script.waypoints.entries = {{}, marker};
            w.script.waypoints.current = 1;
            w.script.input_action_bits = 0x40;

            bms::Event filler = simple_event(bms::EventFlags::PreMission, 0);
            filler.action_count = 0;
            bms::Event event = simple_event(bms::EventFlags::ResetAfter, 1);
            event.action_count = c.count;
            event.delay = delay;
            event.reset_after = 2;
            std::vector<bms::Action> actions(static_cast<size_t>(c.available) + 1,
                    misvar(bms::MissionVariableActionSubType::Increment, 7, 0));
            actions[0] = misvar(bms::MissionVariableActionSubType::Set, 8, 99);
            mission::BmsEventSystem sys;
            sys.load({filler, event}, {}, actions);
            w.add_system(&sys);
            w.load_systems();
            CHECK(sys.events()[1].event.action_count == c.count);

            tick_n(w, 2 * kPass); // event 1 is the second quarter's first entry
            CHECK(sys.is_active(1));
            CHECK(sys.events()[1].repeat_countdown == 128);
            if (delay != 0) {
                CHECK(sys.events()[1].activate_countdown == 64);
                CHECK(!sys.event_fired(1));
                CHECK(w.script.vars.get_mission(7) == 0);
                CHECK(!w.script.waypoints.entries[1].done);
                w.script.input_action_bits = 0x80; // expiry must commit the old mirror
                tick_n(w, kCycle);
                CHECK(sys.events()[1].repeat_countdown == 64);
            }
            CHECK(sys.event_fired(1));
            CHECK(sys.events()[1].activate_countdown == 0);
            CHECK(w.script.vars.get_mission(7) == c.executed);
            CHECK(w.script.vars.get_mission(8) == 0); // honor the action slice offset
            CHECK(w.script.input_action_bits == 0x40);
            CHECK(w.script.waypoints.entries[1].done);
            CHECK(!w.script.waypoints.entries[0].done);
            CHECK(w.script.waypoints.current == 0);

            tick_n(w, (delay == 0 ? 2 : 1) * kCycle);
            CHECK(!sys.is_active(1));
            CHECK(!sys.event_fired(1));
            CHECK(sys.events()[1].repeat_countdown == 0);
            CHECK(w.script.vars.get_mission(7) == c.executed);
        }
    }
}

// The chain reads the trigger-count byte SIGNED after its zero test: 128..255
// evaluate trigger 0 alone, so a chain whose later entries would reject it
// fires on trigger 0. [orig: EventTrigger_EvaluateChain @0x45405f (zero),
// @0x45408c (`cmp [edi+15h],bl; jle`), @0x4540c7 (movsx loop bound)]
static void test_trigger_count_is_signed_in_the_chain() {
    for (uint8_t count : {uint8_t{128}, uint8_t{255}}) {
        World w;
        w.cached.humans = 1;
        mission::BmsEventSystem sys;
        bms::Event event = simple_event(bms::EventFlags::None, 0);
        event.trigger_count = count;
        bms::Trigger condition{};
        condition.main_type = bms::TriggerMainType::MissionVariable;
        condition.sub_type = static_cast<int32_t>(
                bms::MissionVariableTriggerType::MissionVariableIsEqual);
        condition.param1 = 1;
        std::vector<bms::Trigger> triggers(count, condition); // V1 == 0 throughout
        triggers.back().param2 = 1; // an entry the signed count never reaches
        sys.load({event}, triggers,
                {misvar(bms::MissionVariableActionSubType::Set, 7, 1)});
        w.add_system(&sys);
        w.load_systems();
        tick_n(w, kPass);
        CHECK(sys.is_active(0));
        CHECK(w.script.vars.get_mission(7) == 1);
    }
}

// reset_after == 0: a repeat event re-fires on EVERY processing pass while its chain
// holds: the zero reload word clears the +20 latch in the same call
// [orig: EventTrigger_UpdateEntry @0x454C30 — the ResetAfter test @0x454CC5, the
//  zero-reload jump @0x454CD5 to the latch clear @0x454D46].
static void test_repeat_zero_refires_every_pass() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    mission::BmsEventSystem sys;
    sys.load({simple_event(bms::EventFlags::ResetAfter, 0)}, {}, {output_text(3)});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kPass + 2 * kCycle); // passes at ticks 16, 80, 144
    CHECK(w.out.effects.count("text") == 3);
}

// PreMission events run on the pre-mission pass (whole list per call, no 16-tick gate)
// and are EXCLUDED from the normal quarter pass; normal events ignore the pre pass.
// [orig: UpdateAllWithFlag2 @0x454dc0 (flags&2) vs the (flags&6)==0 quarter pass]
static void test_pre_mission_pass() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    mission::BmsEventSystem sys;
    sys.load({simple_event(bms::EventFlags::PreMission, 0),
              simple_event(bms::EventFlags::None, 1)},
             {}, {output_text(1), output_text(2)});
    w.add_system(&sys);
    w.load_systems();

    w.run_logic_tick(true, opennova::world::TickPhase::PreMission); // one pre pass
    CHECK(w.out.effects.count("text") == 1); // only the PreMission event fired
    tick_n(w, kPass + kCycle); // normal passes touch only the normal event
    CHECK(w.out.effects.count("text") == 2); // +1 from the normal event, pre event excluded
}

// A ChangeSingleAI/PLAYPARTANIM action mutates the target's AI brain IN-ENGINE (no embedder
// effect), faithful to Entity_ApplyCommand @0x43ab60 case 0x22: it stores the channel
// direction and rate only. Retail's sweep integrator (Entity_UpdateSuspensionBounce
// @0x456710) has no caller, so later world ticks hold the phase dword. Proves the
// in-engine action-dispatch path end to end.
static void test_playpartanim_mutates_brain() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 8);
    world::Entity org;
    org.net_id = 42;
    org.alive = true;
    world::EntityHandle h = w.registry.spawn(0, org);

    world::AiSystem &ai = w.ai; // the AI-change family reaches the brain through World::ai
    ai.attach(h);

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
        CHECK(ae->brain.f[world::AiBrain::kPartAnimPhase0] == 0);
    }
    tick_n(w, kPass); // a further pass leaves the stored channel untouched
    ae = ai.for_handle(h);
    CHECK(ae != nullptr);
    if (ae) {
        CHECK(ae->brain.f[world::AiBrain::kPartAnimDir0] == 1);
        CHECK(ae->brain.f[world::AiBrain::kPartAnimRate0] == 1048);
        CHECK(ae->brain.f[world::AiBrain::kPartAnimPhase0] == 0);
    }
    // In-engine mutation, NOT an embedder effect.
    CHECK(w.out.effects.count("unported_action") == 0);
}

// RedirectGroupTo/RedirectSingleTo carry an authored waypoint node in param3.
// The two-argument WAC form uses -1/nearest, but BMS must preserve its explicit
// node through dispatch and the shared EntityCommands seam.
// [orig: Entity_SetWaypointByTeam @0x43cdb4]
static void test_redirect_actions_preserve_authored_node() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 8);

    world::Entity first{};
    first.net_id = 42;
    first.group_id = 3;
    first.alive = true;
    first.position = {90.0f, 0.0f, 0.0f}; // nearest node is 1
    const world::EntityHandle first_h = w.registry.spawn(0, first);

    world::Entity second = first;
    second.net_id = 43;
    second.position = {10.0f, 0.0f, 0.0f}; // nearest node is 0
    const world::EntityHandle second_h = w.registry.spawn(0, second);

    world::AiSystem &ai = w.ai;
    const int first_ai_index = ai.attach(first_h);
    const int second_ai_index = ai.attach(second_h);
    world::AiEntity &first_ai = *ai.at(first_ai_index);
    world::AiEntity &second_ai = *ai.at(second_ai_index);
    first_ai.pos[0] = 90 << 16;
    second_ai.pos[0] = 10 << 16;
    ai.nav.channels.resize(3);
    ai.nav.channels[2].count = 2;
    ai.nav.channels[2].entries[0] = 0;
    ai.nav.channels[2].entries[1] = 1;
    ai.nav.nodes.resize(2);
    ai.nav.nodes[0] = world::NavEntry{{1 << 16, 0, 0, 0, 0}};
    ai.nav.nodes[1] = world::NavEntry{{1 << 16, 100 << 16, 0, 0, 0}};

    bms::Event e = simple_event(bms::EventFlags::None, 0);
    e.action_count = 2;
    bms::Action group{};
    group.action_type = bms::ActionType::RedirectGroupTo;
    group.param1 = 3;
    group.param2 = 2;
    group.param3 = 0; // explicit node 0, despite first being nearer node 1
    bms::Action single{};
    single.action_type = bms::ActionType::RedirectSingleTo;
    single.param1 = 43;
    single.param2 = 2;
    single.param3 = 1; // explicit node 1, despite second being nearer node 0

    mission::BmsEventSystem sys;
    sys.load({e}, {}, {group, single});
    w.add_system(&sys);
    w.load_systems();
    tick_n(w, kPass);

    CHECK(first_ai.brain.f[world::AiBrain::kWpNode] == 0);
    CHECK(second_ai.brain.f[world::AiBrain::kWpNode] == 1);
    CHECK(w.registry.get(first_h)->wp_number == 0);
    CHECK(w.registry.get(second_h)->wp_number == 1);

    // Arrival closes the mission loop: the mover must use the live registry's
    // script-facing group/SSN keys, not the provisional AiEntity mirrors.
    first_ai.pos[0] = 0;
    second_ai.pos[0] = 100 << 16;
    ai.update_waypoint_movement(first_ai, w);
    ai.update_waypoint_movement(second_ai, w);
    CHECK(w.script.relations.group_visited(3, 2, 0));
    CHECK(w.script.relations.group_visited(3, 2, 1));
    CHECK(w.script.relations.single_visited(42, 2, 0));
    CHECK(w.script.relations.single_visited(43, 2, 1));
}

// 09TR redirects boarding groups to command 123 (node is the carrier SSN),
// and stops the free-standing group with command 0. Neither requires a route.
// [orig: Entity_SetWaypointByTeam @0x43CDB4]
static void test_redirect_actions_accept_boarding_and_stop_commands() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 8);
    world::Entity soldier{};
    soldier.net_id = 42;
    soldier.group_id = 3;
    soldier.alive = true;
    const auto boarder = w.registry.spawn(0, soldier);
    soldier.net_id = 43;
    soldier.group_id = 4;
    const auto standing = w.registry.spawn(0, soldier);
    for (const auto h : {boarder, standing}) {
        auto &a = *w.ai.at(w.ai.attach(h));
        a.slot.f[35] = a.brain.f[world::AiBrain::kWpType] = 1;
        a.slot.f[37] = a.brain.f[world::AiBrain::kWpChannel] = 2;
        a.slot.f[38] = a.brain.f[world::AiBrain::kWpNode] = 1;
        a.inf.wait_cooldown = 30;
    }
    bms::Event e = simple_event(bms::EventFlags::None, 0);
    e.action_count = 2;
    bms::Action board{};
    board.action_type = bms::ActionType::RedirectGroupTo;
    board.param1 = 3;
    board.param2 = 123;
    board.param3 = 4572;
    bms::Action stop{};
    stop.action_type = bms::ActionType::RedirectSingleTo;
    stop.param1 = 43;
    stop.param2 = 0;
    stop.param3 = 0;
    mission::BmsEventSystem sys;
    sys.load({e}, {}, {board, stop});
    w.add_system(&sys);
    w.load_systems();
    tick_n(w, kPass);
    const auto &boarding_ai = *w.ai.for_handle(boarder);
    const auto &standing_ai = *w.ai.for_handle(standing);
    CHECK(boarding_ai.slot.f[37] == 123);
    CHECK(boarding_ai.slot.f[38] == 4572);
    CHECK(boarding_ai.brain.f[world::AiBrain::kWpChannel] == 123);
    CHECK(boarding_ai.brain.f[world::AiBrain::kWpNode] == 4572);
    CHECK(standing_ai.slot.f[37] == 0);
    CHECK(standing_ai.slot.f[38] == 0);
    CHECK(standing_ai.brain.f[world::AiBrain::kWpChannel] == 0);
    CHECK(standing_ai.brain.f[world::AiBrain::kWpNode] == 0);
    CHECK(boarding_ai.inf.wait_cooldown == 0);
    CHECK(standing_ai.inf.wait_cooldown == 0);
    CHECK(w.registry.get(boarder)->wp_number == 4572);
}

// Presentation actions surface as presentation-only EffectLog entries; an unmodelled
// action records as "unported_action" (diagnostic), never as a real effect.
static void test_presentation_effects() {
    World w;
    w.cached.humans = 1;
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
    CHECK(w.out.effects.count("dialog") == 1);
    CHECK(w.out.effects.count("show_waypoints") == 1);
    CHECK(w.out.effects.count("unported_action") == 0);
    // ShowWaypoints(1) with param1=1 keeps the flag on; the engine-side gate
    // lives on the world track. [orig: Game_SetShowWaypoints @0x58fb50]
    CHECK(w.script.waypoints.show);
}

// A fired event completes the waypoint-track entries linked to its index (the HUD
// waypoint chain), and ShowWaypoints(0) drops the track's show gate. Linked
// indices are > 0 only — the original's `linkedTrigger > 0` gate reserves 0 as
// "no link" (BMS wp_adv_trigger 0), so event 0 can never complete a waypoint.
// [orig: EventTrigger_MarkLinkedSpawnPoints @0x452ce0 (the @0x452d34 > 0 gate)
//  after the dispatch loops @0x454cbd; Game_SetShowWaypoints @0x58fb50]
static void test_waypoint_track_integration() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    // The mission's waypoint track: entry 1 completes when EVENT 1 fires.
    world::WaypointEntry a;
    a.x = 0; a.y = 0;
    world::WaypointEntry b;
    b.x = 100 << 16; b.y = 0;
    b.linked_event = 1;
    w.script.waypoints.entries = {a, b};
    w.script.waypoints.current = 1;

    // Event 0: inert filler (its OutputText is irrelevant); event 1: the linked
    // one, whose action also hides the waypoints.
    bms::Event filler = simple_event(bms::EventFlags::None, 0);
    bms::Event linked = simple_event(bms::EventFlags::None, 1);
    bms::Action txt{};
    txt.action_type = bms::ActionType::OutputText;
    txt.param1 = 1;
    bms::Action wps{};
    wps.action_type = bms::ActionType::ShowWaypoints;
    wps.param1 = 0; // hide
    mission::BmsEventSystem sys;
    sys.load({filler, linked}, {}, {txt, wps});
    w.add_system(&sys);
    w.load_systems();

    CHECK(w.script.waypoints.show);
    tick_n(w, kCycle); // a full quarter cycle: both events process + fire
    CHECK(!w.script.waypoints.show);            // ShowWaypoints(0) applied
    CHECK(w.script.waypoints.entries[1].done);  // the linked entry completed
    CHECK(w.script.waypoints.current == 0);     // skip_done cycled off the done entry
    CHECK(!w.script.waypoints.entries[0].done); // no chain_back: entry 0 untouched
}

// The subgoal actions drive the objectives-panel state: ShowWinSubgoal sets the
// RAW-slot show bit, SubGoalWon sets the won bit once (the already-won guard
// silences a refire), SubGoalLost accumulates without a guard. Effects carry
// the header text id + the round-running announce flag.
// [orig: EventAction_Dispatch cases 14 @0x454500 / 15 @0x4545e0 / 35 @0x4546af]
static void test_subgoal_state() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    w.script.subgoals.win_text_ids[2] = 7;   // header WinConditions slot 2 -> STRWINCOND007
    w.script.subgoals.lose_text_ids[3] = 4;

    bms::Event show_e = simple_event(bms::EventFlags::None, 0);
    bms::Event won_e = simple_event(bms::EventFlags::None, 1);
    // Refire the SAME SubGoalWon from a later event: the guard must silence it.
    bms::Event won_again_e = simple_event(bms::EventFlags::None, 2);
    bms::Event lost_e = simple_event(bms::EventFlags::None, 3);
    bms::Action show{};
    show.action_type = bms::ActionType::ShowWinSubgoal;
    show.param1 = 2;
    show.param2 = 1;
    bms::Action won{};
    won.action_type = bms::ActionType::SubGoalWon;
    won.param1 = 2;
    bms::Action lost{};
    lost.action_type = bms::ActionType::SubGoalLost;
    lost.param1 = 3;
    mission::BmsEventSystem sys;
    sys.load({show_e, won_e, won_again_e, lost_e}, {}, {show, won, won, lost});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kCycle);
    CHECK((w.script.subgoals.show_win & (1u << 2)) != 0);   // raw-slot bit
    CHECK((w.script.subgoals.won & (1u << 2)) != 0);
    CHECK((w.script.subgoals.lost & (1u << 3)) != 0);
    CHECK(w.out.effects.count("subgoal_won") == 1);       // the refire was guarded
    CHECK(w.out.effects.count("subgoal_lost") == 1);
    bool saw_won = false;
    for (const auto &e : w.out.effects.entries()) {
        if (e.kind != "subgoal_won") continue;
        saw_won = true;
        CHECK(e.a == 2);
        CHECK(e.b == 7); // the header text id resolved into the effect
        CHECK(e.c == 1); // round still running -> announce
    }
    CHECK(saw_won);
    // The score tally counts the first win of the slot once (the guarded
    // refire adds nothing). [orig: the Score_TallySubGoalWon call @0x454532 ->
    // the count @0x4fd117]
    CHECK(w.kill_stats.subgoals_won == 1);
}

// The SubGoalWon tally rides the SP gate: inside a session the mask and the
// announcement still land, the count does not; outside it every first win of
// a slot counts once, raw slot bits included.
// [orig: Score_TallySubGoalWon @0x4fd100 — is_authority && !is_in_session
//  @0x4fd110, ++g_SubGoalsWonCount @0x4fd117]
static void test_subgoal_tally_gate() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();
    bms::Action won{};
    won.action_type = bms::ActionType::SubGoalWon;
    won.param1 = 1;
    w.rules.mp_session = true;
    sys.dispatch_action_for_test(w, won);
    CHECK(w.script.subgoals.won == (1u << 1));
    CHECK(w.kill_stats.subgoals_won == 0); // a session never tallies
    w.rules.mp_session = false;
    sys.dispatch_action_for_test(w, won);
    CHECK(w.kill_stats.subgoals_won == 0); // the already-won guard skips the tally
    won.param1 = 3;
    sys.dispatch_action_for_test(w, won);
    won.param1 = 33; // the masked shift: bit 1 again, already won
    sys.dispatch_action_for_test(w, won);
    won.param1 = 4;
    sys.dispatch_action_for_test(w, won);
    CHECK(w.kill_stats.subgoals_won == 2);
    CHECK(w.script.subgoals.won == ((1u << 1) | (1u << 3) | (1u << 4)));
}

// OutputText surfaces a "text" effect; ResetEvent clears the target's active latch
// [orig: EventAction_Dispatch case 0x22 @0x454974 -- ONLY event +20] so a fired
// fire-once event becomes evaluable again on its next processing pass.
static void test_output_text_and_reset_event() {
    World w;
    w.cached.humans = 1;
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
    CHECK(w.out.effects.count("text") == 1); // event 0 fired OutputText(7) at tick 16
    w.out.effects.clear();
    // tick 32: event 1 fires ResetEvent -> clears event 0's latch.
    // tick 80: event 0 re-evaluates and fires its OutputText again.
    tick_n(w, 80 - kPass);
    CHECK(w.out.effects.count("text") == 1);
}

// The Event trigger category reads the live latch window (active && delay elapsed),
// not a sticky has-fired bit [orig: EvaluateCondition case 3 @0x453a75]. Event 1,
// gated on Event(0), fires while event 0's window is open.
static void test_event_trigger_reads_window() {
    World w;
    w.cached.humans = 1;
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
    CHECK(w.out.effects.count("text") == 2);
}

// PLAYPARTANIM with ANIMTIME=0 gets INT_MIN from x87 ftol(+inf). The sweep
// step (the editor preview's integrator; retail's Entity_UpdateSuspensionBounce
// has no caller) uses wrapping 32-bit ADD/SUB with asymmetric strict clamps.
static void test_playpartanim_zero_time_wraps_like_retail() {
    world::AiBrain b;
    world::ai_apply_command(b, 0x22, /*channel=*/1, /*play_type=*/1, /*time=*/0);
    CHECK(b.f[world::AiBrain::kPartAnimDir0] == 1);
    CHECK(b.f[world::AiBrain::kPartAnimRate0] == static_cast<int32_t>(0x80000000)); // INT_MIN
    const int32_t zero_time_rate = b.f[world::AiBrain::kPartAnimRate0];
    int32_t phase = 0;
    CHECK(!world::part_anim_step(phase, 1, zero_time_rate));
    CHECK(phase == static_cast<int32_t>(0x80000000));
    CHECK(!world::part_anim_step(phase, 1, zero_time_rate));
    CHECK(phase == 0);

    world::AiBrain reverse;
    world::ai_apply_command(
            reverse, 0x22, /*channel=*/1, /*play_type=*/-1, /*time=*/0);
    CHECK(reverse.f[world::AiBrain::kPartAnimDir0] == -1);
    phase = 0;
    CHECK(world::part_anim_step(phase, -1, reverse.f[world::AiBrain::kPartAnimRate0]));
    CHECK(phase == 0);

    // Exact endpoints do not stop; only the following strict overshoot does.
    phase = 0x10000 - 1048;
    CHECK(!world::part_anim_step(phase, 1, 1048));
    CHECK(phase == 0x10000);
    CHECK(world::part_anim_step(phase, 1, 1048));
    CHECK(phase == 0x10000);

    phase = 1048;
    CHECK(!world::part_anim_step(phase, -1, 1048));
    CHECK(phase == 0);
    CHECK(world::part_anim_step(phase, -1, 1048));
    CHECK(phase == 0);
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
        w.cached.humans = 1;
        w.registry.configure_pool(0, 4);
        mission::BmsEventSystem sys;
        load_probe(sys, make_trigger(bms::TriggerMainType::SecondTimeThrough, 0));
        const bool expected = (round == 0) ? !before : before;
        CHECK(mission::BmsEventSystem::second_time_through() == expected);
        w.add_system(&sys);
        w.load_systems();
        tick_n(w, kPass);
        CHECK((w.script.vars.get_mission(9) == 1) == expected);
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
        w.cached.humans = 1;
        w.registry.configure_pool(0, 4);
        w.rules.mp_session = c.mp;
        w.rules.teammates_disabled = c.disabled;
        mission::BmsEventSystem sys;
        load_probe(sys, make_trigger(bms::TriggerMainType::Teammate, c.sub));
        w.add_system(&sys);
        w.load_systems();
        // Inject the query fixture ahead of each tick's script pass: the entity
        // update's HeliLift pass republishes its own (empty) slot count.
        // [orig: HeliLift_UpdateAll @0x451FA0 from Entity_UpdateAllEntities @0x4C21F6]
        for (int i = 0; i < kPass; ++i) {
            w.script.heli_lift_active_count = c.lifts;
            tick_n(w, 1);
        }
        CHECK((w.script.vars.get_mission(9) == 1) == c.fires);
    }
}

// The AWOL counter: +1 per full quarter cycle (64 ticks) while the local player
// is outside every ACTIVE zone (X/Y only), reset when back inside; PlayerAwol
// compares it to the authored threshold. [orig: @0x454d50 cursor==0 ->
// Entity_UpdatePlayerAwolCounter @0x439dc0 / probe @0x439d40; trigger @0x453d40]
static void test_player_awol_counter_and_trigger() {
    World w;
    w.cached.humans = 1;
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
    CHECK(w.script.vars.get_mission(9) == 0); // threshold 2 not reached
    tick_n(w, kCycle);
    CHECK(sys.awol_count() == 2);
    CHECK(w.script.vars.get_mission(9) == 1); // PlayerAwol fires at >= threshold

    w.registry.get(player)->position = {50.0f, 50.0f, 0.0f};
    tick_n(w, kCycle);
    CHECK(sys.awol_count() == 0); // back in bounds resets

    // An inactive-only zone world never counts as out of bounds.
    World w2;
    w2.cached.humans = 1;
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

// The cat-7 mount-sub dispatch must discriminate the four predicates — the shipped
// IDB carried PERMUTED names for exactly these, so a permuted case map is the
// realistic defect and every row below catches one permutation.
// [orig: EventTrigger_EvaluateCondition cat-7 subs 38-41
//  -> @0x4f10d0/0x4f1260/0x4f1150/0x4f11e0]
static void test_player_mount_trigger_dispatch() {
    const int kAttached = static_cast<int>(bms::PlayerTriggerType::PlayerAttachedToSsn);
    const int kOn = static_cast<int>(bms::PlayerTriggerType::PlayerOnSsn);
    const int kDriving = static_cast<int>(bms::PlayerTriggerType::PlayerDrivingSsn);
    const int kOnGun = static_cast<int>(bms::PlayerTriggerType::PlayerOnGun);
    enum Pose { kSeatCtrl, kSeatGun, kStanding };
    struct Case { int sub; Pose pose; bool fires; };
    const Case cases[] = {
        {kAttached, kSeatCtrl, true},  {kAttached, kSeatGun, true},
        {kAttached, kStanding, false},
        {kOn, kStanding, true},        {kOn, kSeatCtrl, false},
        {kDriving, kSeatCtrl, true},   {kDriving, kSeatGun, false},
        {kDriving, kStanding, false},
        {kOnGun, kSeatGun, true},      {kOnGun, kSeatCtrl, false},
    };
    for (const Case &c : cases) {
        World w;
        w.cached.humans = 1;
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 4);
        world::Entity veh{};
        veh.kind = world::EntityKind::Item;
        veh.alive = true;
        veh.health = 1000;
        veh.net_id = 11;
        veh.bms_id = 11;
        veh.item_id = 1; veh.item_type_index = 7; // Mount predicates require the target's item definition.
        world::EntityHandle vh = w.registry.spawn(1, veh);
        world::Entity pl{};
        pl.alive = true;
        pl.health = 100;
        pl.player_class = 8;
        world::EntityHandle ph = w.registry.spawn(0, pl);
        w.cached.local_player = ph;
        world::Entity *p = w.registry.get(ph);
        if (c.pose == kStanding) {
            p->ground_target = vh;
        } else {
            p->mounted = true;
            p->mount_target = vh;
            p->mount_type = c.pose == kSeatCtrl ? world::SeatType::Controller
                                                : world::SeatType::Gunner;
        }
        mission::BmsEventSystem sys;
        load_probe(sys, make_trigger(bms::TriggerMainType::Player, c.sub, /*p1=*/11));
        w.add_system(&sys);
        w.load_systems();
        tick_n(w, kPass);
        CHECK((w.script.vars.get_mission(9) == 1) == c.fires);
    }
}

// Cat-7 input subs [orig: EventTrigger_EvaluateCondition @0x453b7e, the shared
// leg @0x453ba5..0x453bb1]: the LIVE word is tested, the matched bit toggles in
// the chain mirror (seeded from the live word at every chain entry, EvaluateChain
// @0x45405a) and the mirror is committed back to the live word only when the
// event fires (UpdateEntry @0x454c8b / @0x454cfa).
static void test_player_input_bit_triggers() {
    struct Case { int sub; uint32_t bit; };
    const Case view_bits[] = {
        {static_cast<int>(bms::PlayerTriggerType::PlayerFirstPerson), 0x4000000u},
        {static_cast<int>(bms::PlayerTriggerType::PlayerThirdPerson), 0x8000000u},
        {static_cast<int>(bms::PlayerTriggerType::PlayerCockpitView), 0x10000000u},
        {static_cast<int>(bms::PlayerTriggerType::PlayerInputBitIndex), 0x4000000u}, // p1 = 26
        {static_cast<int>(bms::PlayerTriggerType::PlayerInputBitIndexPlus15), 0x4000000u}, // p1 = 11
    };
    for (const Case &c : view_bits) {
        World w;
        w.cached.humans = 1;
        w.registry.configure_pool(0, 4);
        mission::BmsEventSystem sys;
        const int p1 = c.sub == static_cast<int>(bms::PlayerTriggerType::PlayerInputBitIndex) ? 26
                     : c.sub == static_cast<int>(bms::PlayerTriggerType::PlayerInputBitIndexPlus15) ? 11 : 0;
        load_probe(sys, make_trigger(bms::TriggerMainType::Player, c.sub, p1));
        w.add_system(&sys);
        w.load_systems();
        w.script.input_action_bits = 0x10u; // an unrelated bit only
        tick_n(w, kPass);
        CHECK(w.script.vars.get_mission(9) == 0);
        CHECK(w.script.input_action_bits == 0x10u);
        w.script.input_action_bits |= c.bit;
        tick_n(w, kCycle); // the next evaluation of this entry
        CHECK(w.script.vars.get_mission(9) == 1);
        CHECK(w.script.input_action_bits == 0x10u); // consumed on fire; the other bit stays
    }

    // Evaluation alone toggles the MIRROR; the live word keeps the bit until a
    // fire commits. A second predicate on the same bit in one chain toggles it
    // back (the retail xor), so that bit is NOT consumed.
    {
        World w;
        w.cached.humans = 1;
        w.registry.configure_pool(0, 4);
        mission::BmsEventSystem sys;
        sys.load({}, {}, {});
        w.add_system(&sys);
        w.load_systems();
        const bms::Trigger first = make_trigger(bms::TriggerMainType::Player,
                static_cast<int>(bms::PlayerTriggerType::PlayerFirstPerson));
        w.script.input_action_bits = 0x4000000u;
        w.script.input_action_mirror = w.script.input_action_bits;
        CHECK(sys.evaluate_trigger_for_test(w, first));
        CHECK(w.script.input_action_bits == 0x4000000u);
        CHECK(w.script.input_action_mirror == 0);
        CHECK(sys.evaluate_trigger_for_test(w, first));
        CHECK(w.script.input_action_mirror == 0x4000000u);
    }

    // No short-circuit: an OR chain whose first term already decides still
    // runs the second predicate, so BOTH bits are consumed when it fires.
    {
        World w;
        w.cached.humans = 1;
        w.registry.configure_pool(0, 4);
        bms::Trigger a = make_trigger(bms::TriggerMainType::Player,
                static_cast<int>(bms::PlayerTriggerType::PlayerFirstPerson));
        a.condition_flags = bms::Trigger::kConditionOr; // joins the NEXT trigger with OR
        const bms::Trigger b = make_trigger(bms::TriggerMainType::Player,
                static_cast<int>(bms::PlayerTriggerType::PlayerThirdPerson));
        bms::Event e{};
        e.trigger_index = 0;
        e.trigger_count = 2;
        e.action_index = 0;
        e.action_count = 1;
        mission::BmsEventSystem sys;
        sys.load({e}, {a, b}, {misvar(bms::MissionVariableActionSubType::Set, 9, 1)});
        w.add_system(&sys);
        w.load_systems();
        w.script.input_action_bits = 0x4000000u | 0x8000000u;
        tick_n(w, kPass);
        CHECK(w.script.vars.get_mission(9) == 1);
        CHECK(w.script.input_action_bits == 0);
    }
    // An AND chain that fails leaves the live word untouched even though its
    // matched predicate toggled the mirror.
    {
        World w;
        w.cached.humans = 1;
        w.registry.configure_pool(0, 4);
        const bms::Trigger a = make_trigger(bms::TriggerMainType::Player,
                static_cast<int>(bms::PlayerTriggerType::PlayerFirstPerson));
        const bms::Trigger b = make_trigger(bms::TriggerMainType::Player,
                static_cast<int>(bms::PlayerTriggerType::PlayerThirdPerson));
        bms::Event e{};
        e.trigger_index = 0;
        e.trigger_count = 2;
        e.action_index = 0;
        e.action_count = 1;
        mission::BmsEventSystem sys;
        sys.load({e}, {a, b}, {misvar(bms::MissionVariableActionSubType::Set, 9, 1)});
        w.add_system(&sys);
        w.load_systems();
        w.script.input_action_bits = 0x8000000u; // only the second term's bit
        tick_n(w, kPass);
        CHECK(w.script.vars.get_mission(9) == 0);
        CHECK(w.script.input_action_bits == 0x8000000u);
        CHECK(w.script.input_action_mirror == 0);
    }

    // The delayed fire commits at expiry: the mirror captured when the chain
    // passed, so a bit that arrived meanwhile is dropped by the commit
    // [orig: @0x454cfa].
    {
        World w;
        w.cached.humans = 1;
        w.registry.configure_pool(0, 4);
        bms::Event e{};
        e.trigger_index = 0;
        e.trigger_count = 1;
        e.action_index = 0;
        e.action_count = 1;
        e.delay = 1; // 64 units: one processing pass of this entry
        mission::BmsEventSystem sys;
        sys.load({e}, {make_trigger(bms::TriggerMainType::Player,
                static_cast<int>(bms::PlayerTriggerType::PlayerFirstPerson))},
                {misvar(bms::MissionVariableActionSubType::Set, 9, 1)});
        w.add_system(&sys);
        w.load_systems();
        w.script.input_action_bits = 0x4000000u | 0x10u;
        tick_n(w, kPass); // the chain passes and arms the delay
        CHECK(w.script.vars.get_mission(9) == 0);
        CHECK(w.script.input_action_bits == (0x4000000u | 0x10u)); // not committed yet
        w.script.input_action_bits |= 0x20u; // arrives before expiry
        tick_n(w, kCycle);
        CHECK(w.script.vars.get_mission(9) == 1);
        CHECK(w.script.input_action_bits == 0x10u); // the captured mirror, 0x20 dropped
    }

    // The never-set masks (22-25, 28-30) follow the same leg, but nothing in the
    // image sets those bits: with every producer-reachable bit set they read
    // false, and only a forced word makes them true.
    {
        World w;
        w.cached.humans = 1;
        w.registry.configure_pool(0, 4);
        mission::BmsEventSystem sys;
        sys.load({}, {}, {});
        w.add_system(&sys);
        w.load_systems();
        const uint32_t settable = 0x4u | 0x10u | 0x40u | 0x80u | 0x100u | 0x200u |
                                  0x4000000u | 0x8000000u | 0x10000000u;
        for (int sub : {22, 23, 24, 25, 28, 29, 30}) {
            w.script.input_action_bits = settable;
            w.script.input_action_mirror = settable;
            CHECK(!sys.evaluate_trigger_for_test(w, make_trigger(bms::TriggerMainType::Player, sub)));
            w.script.input_action_bits = 0xFFFFFFFFu;
            w.script.input_action_mirror = 0xFFFFFFFFu;
            CHECK(sys.evaluate_trigger_for_test(w, make_trigger(bms::TriggerMainType::Player, sub)));
        }
        // The look byte's bit 0 is never set: its only store writes 0, so sub
        // 26 reads true and sub 27 false. [orig: EventTrigger_EvaluateCondition
        // @0x453ca5 / @0x453cb6; HUD_BuildEntityInfo @0x4b84d9]
        CHECK(sys.evaluate_trigger_for_test(w, make_trigger(bms::TriggerMainType::Player, 26)) == 1);
        CHECK(sys.evaluate_trigger_for_test(w, make_trigger(bms::TriggerMainType::Player, 27)) == 0);
        // BMS action 28 sub 38 zeroes the word [orig: EventAction_HandleSpecialTypes @0x4535c2].
        bms::Action clear{};
        clear.action_type = bms::ActionType::SpecialSubType;
        clear.action_sub_type = 38;
        sys.dispatch_action_for_test(w, clear);
        CHECK(w.script.input_action_bits == 0);
        CHECK(w.diagnostics.empty());
    }
}

// Sub 18 reads the local player's AiSlot behavior word bit 0x200 (Berserk)
// [orig: @0x453b99].
static void test_player_berserk_trigger() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    world::Entity pl{};
    pl.alive = true;
    world::EntityHandle ph = w.registry.spawn(0, pl);
    w.cached.local_player = ph;
    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();
    const bms::Trigger berserk = make_trigger(bms::TriggerMainType::Player,
            static_cast<int>(bms::PlayerTriggerType::PlayerBerserk));
    CHECK(!sys.evaluate_trigger_for_test(w, berserk)); // no brain
    w.ai.attach(ph);
    CHECK(!sys.evaluate_trigger_for_test(w, berserk));
    w.ai.for_handle(ph)->slot.f[world::AiSlot::kBehaviorFlags] |= 0x200;
    // The read is the raw masked word, not a normalized bool.
    CHECK(sys.evaluate_trigger_for_test(w, berserk) == 0x200);
    w.ai.for_handle(ph)->slot.f[world::AiSlot::kBehaviorFlags] &= ~0x200;
    CHECK(!sys.evaluate_trigger_for_test(w, berserk));
}

// Subs 34/35 over the dialog registry: DONE = absent from the active table
// [orig: Dialog_ExistsByIndex @0x44e170 == 0], FINISHED = in the registered
// history AND absent from the active table [orig: sub_44E220 @0x44e220].
static void test_player_dialog_triggers() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();
    const bms::Trigger done = make_trigger(bms::TriggerMainType::Player,
            static_cast<int>(bms::PlayerTriggerType::PlayerDialogDone), 7);
    const bms::Trigger finished = make_trigger(bms::TriggerMainType::Player,
            static_cast<int>(bms::PlayerTriggerType::PlayerDialogFinished), 7);
    CHECK(sys.evaluate_trigger_for_test(w, done));      // never played: not active
    CHECK(!sys.evaluate_trigger_for_test(w, finished)); // never registered
    w.script.dialog.register_started(7);                // Dialog_Register
    CHECK(!sys.evaluate_trigger_for_test(w, done));
    CHECK(!sys.evaluate_trigger_for_test(w, finished));
    w.script.dialog.register_started(8);                // another dialog behind it
    w.script.dialog.finished(7);                        // Dialog_FreeByName
    CHECK(sys.evaluate_trigger_for_test(w, done));
    CHECK(sys.evaluate_trigger_for_test(w, finished));
    CHECK(w.script.dialog.active_exists(8));            // compaction kept the other
    w.script.dialog.reset();                            // Dialog_ResetAll
    CHECK(sys.evaluate_trigger_for_test(w, done));
    CHECK(!sys.evaluate_trigger_for_test(w, finished));
    // The history count saturates at 255: registration 256 lands in slot 255,
    // which the scan never reaches [orig: @0x44d99c/@0x44d9a3, scan @0x44e28e].
    for (int i = 1; i <= 255; ++i) w.script.dialog.register_started(1000 + i);
    CHECK(w.script.dialog.registered(1255));
    w.script.dialog.register_started(2000);
    CHECK(!w.script.dialog.registered(2000));
    CHECK(w.script.dialog.active_count == world::ScriptDialogRegistry::kActiveCapacity);
    // A mission load clears both tables [orig: Game_InitNewRound -> Dialog_ResetAll].
    w.load_systems();
    CHECK(!sys.evaluate_trigger_for_test(w, finished));
    CHECK(w.script.dialog.history_count == 0 && w.script.dialog.active_count == 0);
}

// Sub 37: any placed device with satchel ammo whose registry position lies
// inside the area's x/y box and, when the record constrains z, its z range
// [orig: EventTrigger_AnySatchelInArea @0x547160].
static void test_player_satchel_in_area_trigger() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 8);
    w.tables.ammo.entries.resize(4);
    w.tables.ammo.entries[1].name = "grenadehe";
    w.tables.ammo.entries[1].valid = true;
    w.tables.ammo.entries[3].name = "satchel";
    w.tables.ammo.entries[3].valid = true;
    world::Aabb any_z;
    any_z.min = {0.0f, 0.0f, bms::AreaTrigger::kUnboundedZMin};
    any_z.max = {100.0f, 100.0f, bms::AreaTrigger::kUnboundedZMax};
    world::Aabb constrained_z = any_z;
    constrained_z.min.z = 0.0f;
    constrained_z.max.z = 10.0f;
    const int area_any = w.registry.register_area("", any_z, true);
    const int area_z = w.registry.register_area("", constrained_z, true);
    world::Entity dev{};
    dev.alive = true;
    dev.position = {50.0f, 50.0f, 500.0f};
    world::EntityHandle dh = w.registry.spawn(1, dev);
    world::PlacedDevice placed;
    placed.active = true;
    placed.entity = dh;
    placed.entity_spawn_id = w.registry.get(dh)->registry_spawn_id;
    placed.ammo_index = 3;
    w.throwables.devices.push_back(placed);
    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();
    const bms::Trigger in_any = make_trigger(bms::TriggerMainType::Player,
            static_cast<int>(bms::PlayerTriggerType::PlayerSatchel), area_any);
    const bms::Trigger in_z = make_trigger(bms::TriggerMainType::Player,
            static_cast<int>(bms::PlayerTriggerType::PlayerSatchel), area_z);
    CHECK(sys.evaluate_trigger_for_test(w, in_any));
    CHECK(!sys.evaluate_trigger_for_test(w, in_z)); // z 500 outside 0..10
    w.registry.get(dh)->position.z = 5.0f;
    CHECK(sys.evaluate_trigger_for_test(w, in_z));
    w.registry.get(dh)->position.x = 100.0f; // inclusive edge
    CHECK(sys.evaluate_trigger_for_test(w, in_z));
    w.registry.get(dh)->position.x = 100.5f;
    CHECK(!sys.evaluate_trigger_for_test(w, in_any));
    w.registry.get(dh)->position.x = 50.0f;
    w.throwables.devices[0].ammo_index = 1; // a grenade is not a satchel
    CHECK(!sys.evaluate_trigger_for_test(w, in_any));
    w.throwables.devices[0].ammo_index = 3;
    w.throwables.devices[0].active = false;
    CHECK(!sys.evaluate_trigger_for_test(w, in_any));
    w.throwables.devices[0].active = true;
    w.registry.despawn(dh); // the row is gone: the populated-row gate
    CHECK(!sys.evaluate_trigger_for_test(w, in_any));
    CHECK(!sys.evaluate_trigger_for_test(w, make_trigger(bms::TriggerMainType::Player,
            static_cast<int>(bms::PlayerTriggerType::PlayerSatchel), 99))); // no such area
}

// The post pass is an embedder-called one-shot sweep, never periodic (D-EVT-4)
// [orig: UpdateAllWithFlag4 @0x454e00, one call per teardown/restart]. Normal
// ticks must never touch a PostMission-flag entry.
static void test_post_pass_is_a_one_shot() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    mission::BmsEventSystem sys;
    sys.load({simple_event(bms::EventFlags::PostMission, 0)}, {},
             {misvar(bms::MissionVariableActionSubType::Increment, 9, 0)});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kCycle * 2);
    CHECK(w.script.vars.get_mission(9) == 0); // normal ticking never runs post entries

    sys.run_post_mission_pass(w);
    CHECK(w.script.vars.get_mission(9) == 1); // exactly one sweep per transition call
    sys.run_post_mission_pass(w);
    CHECK(w.script.vars.get_mission(9) == 1); // fire-once latch holds across transitions
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
    w.cached.humans = 1;
    w.registry.configure_pool(0, 16);
    w.registry.configure_pool(4, 4);
    world::Entity seed{};
    seed.alive = true;
    seed.group_id = 3;
    for (int i = 0; i < 3; ++i) {
        seed.net_id = static_cast<uint16_t>(10 + i);
        w.registry.spawn(0, seed);
    }
    // The initial recount tallies EVERY populated row of pools 2/0/1 by its
    // group id with no dead/health test [orig: EntityPool_RecountByType
    // @0x40e7e0, tallies @0x40e834/@0x40e867/@0x40e89a]: a dead row and a
    // zero-health row count; a pool-4 row never does.
    seed.net_id = 13;
    seed.engine_flags |= world::kEntityFlagDead; // killed by the pre-mission pass
    w.registry.spawn(0, seed);
    seed.engine_flags = 0;
    seed.net_id = 14;
    seed.health = 0; // an authored hp-0 row
    w.registry.spawn(0, seed);
    seed.health = 100;
    seed.net_id = 15;
    w.registry.spawn(4, seed); // static-prop pool: outside the recount walk
    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();

    // Initial counts land on the pre-mission pass, ordered after the pre
    // sweep [orig: Game_StartMission @ 0x525b86 -> @ 0x525b8b]; live is the
    // copy of the unfiltered initial [orig: @0x40e8b1..0x40e8c4].
    w.run_logic_tick(true, opennova::world::TickPhase::PreMission);
    CHECK(w.script.relations.group(3).initial_count == 5);
    CHECK(w.script.relations.group(3).live_count == 5);

    // The server tick's first periodic second rescans (its timer starts at
    // zero, as retail's round init leaves it); this bare world has no server
    // tick, so the test stands in for it. Only rows that are not dead and hold
    // health > 0 count [orig: Server_TickUpdate -- the
    // EntityPool_RecountLiveByGroup call @0x51DC02; EntityPool_RecountLiveByGroup
    // @0x40e8d0, predicate @0x40e926/@0x40e96c/@0x40e9b6].
    w.run_logic_tick(true);
    w.recount_group_live();
    CHECK(w.script.relations.group(3).initial_count == 5);
    CHECK(w.script.relations.group(3).live_count == 3);
    CHECK(sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupHasLostMoreUnits, 3, 2)));
    CHECK(!sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupIntact, 3)));
    CHECK(sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupHasMoreUnits, 3, 3)));
    CHECK(sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupAlive, 3)));

    // A kill reads STALE until the next periodic second's live rescan —
    // retail cadence [orig: timer reload 0x3E @ 0x51db93 ->
    // EntityPool_RecountLiveByGroup]; the ticks alone never rescan.
    bms::Trigger lost = group_trigger(bms::GroupTriggerType::GroupHasLostMoreUnits, 3, 3);
    w.commands.kill_ssn(10);
    CHECK(!sys.evaluate_trigger_for_test(w, lost));
    tick_n(w, 62);
    CHECK(!sys.evaluate_trigger_for_test(w, lost));
    w.recount_group_live();
    CHECK(w.script.relations.group(3).live_count == 2);
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
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();
    using R = world::TriggerRelations;

    w.script.relations.set_group_group(R::kSees, 2, 5);
    CHECK(sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupSeesGroup, 2, 5)));
    CHECK(!sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupSeesGroup, 5, 2)));

    // The G->S table is transposed storage; the trigger still reads
    // (group, single) [orig: index [2b + (a >> 5)] at the GS bases].
    w.script.relations.set_group_single(R::kShot, 7, 40);
    CHECK(sys.evaluate_trigger_for_test(w, group_trigger(bms::GroupTriggerType::GroupHasShotSingle, 7, 40)));

    bms::Trigger sss{};
    sss.main_type = bms::TriggerMainType::Single;
    sss.sub_type = static_cast<int32_t>(bms::SingleTriggerType::SingleSeesSingle);
    sss.param1 = 100;
    sss.param2 = 101;
    w.script.relations.set_single_single(R::kSees, 100, 101);
    CHECK(sys.evaluate_trigger_for_test(w, sss));

    // Out-of-range rows: retail writes are guarded (< 0x80) and our reads are
    // sanitized to false instead of reproducing the unguarded OOB read
    // (record §3a, ADR 0003 class).
    w.script.relations.set_single_single(R::kSees, 200, 5); // no-op
    sss.param1 = 200;
    sss.param2 = 5;
    CHECK(!sys.evaluate_trigger_for_test(w, sss));

    // Visited matrices + the 32-list clear quirk: actions 32/33 memset only
    // 0x80 bytes = lists 0..31 of the row [orig: EventTrigger_ClearSlotB
    // @ 0x4535e0 / ClearSlotA @ 0x453600].
    w.script.relations.mark_waypoint_visited(9, 4, /*list=*/5, /*number=*/3);
    w.script.relations.mark_waypoint_visited(9, 4, /*list=*/40, /*number=*/3);
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

// The 2026-08-13 cat-2 close-out (record §3b): the single-state family.
static bms::Trigger single_trigger(bms::SingleTriggerType sub, int p1, int p2 = 0, int p3 = 0) {
    bms::Trigger t{};
    t.main_type = bms::TriggerMainType::Single;
    t.sub_type = static_cast<int32_t>(sub);
    t.param1 = p1;
    t.param2 = p2;
    t.param3 = p3;
    return t;
}

// Subs 3/14 read the per-entity controller alert byte; the ChangeAI command
// family writes it per-SSN and per-member [orig: Entity_IsSsnAtAlertLevel
// @0x43e780; Entity_ApplyCommand cases 5/22/6].
static void test_single_alert_triggers() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 8);
    world::Entity org;
    org.net_id = 42;
    org.alive = true;
    org.group_id = 3;
    world::EntityHandle h = w.registry.spawn(0, org);
    org.net_id = 43; // second member, no AI component
    w.registry.spawn(0, org);

    world::AiSystem &ai = w.ai;
    ai.attach(h);
    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();

    using S = bms::SingleTriggerType;
    // Spawn default = green: neither level satisfied.
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleAtRedAlert, 42)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleAtYellowAlert, 42)));

    // ChangeSingleAI red (sub 5) writes the byte per-SSN.
    w.commands.apply_ai_command(42, 5, 0, 0, 0);
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleAtRedAlert, 42)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleAtYellowAlert, 42)));

    // The group fan reaches members' bytes (and the group record) too.
    w.commands.apply_group_ai_command(3, 22, 0, 0, 0);
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleAtYellowAlert, 42)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleAtRedAlert, 42)));

    // Green (sub 6) clears both levels.
    w.commands.apply_ai_command(42, 6, 0, 0, 0);
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleAtRedAlert, 42)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleAtYellowAlert, 42)));

    // No AI component (retail aiRuntime null) and unresolved SSNs read false.
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleAtRedAlert, 43)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleAtRedAlert, 99)));
}

// Subs 6/9/12 are HEALTH thresholds, not unit counts [orig:
// Entity_HasDamageCapacity @0x43e3d0 / HasFullHealth @0x43e470 /
// HasHealthAboveThreshold @0x43e350 — pools 0-1, signed, no alive gate].
static void test_single_health_triggers() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 8);
    w.registry.configure_pool(2, 4);
    world::Entity org;
    org.net_id = 42;
    org.alive = true;
    org.health = 100;
    org.health_max = 100;
    org.has_item_def = true; // resolved def: SSNADDHP's ItemDef gate passes
    w.registry.spawn(0, org);
    org.net_id = 44; // a pool-2 sibling: outside the retail scan set
    w.registry.spawn(2, org);
    org.net_id = 45; // unresolved def: health_max 0
    org.health_max = 0;
    org.has_item_def = false;
    w.registry.spawn(0, org);

    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();

    using S = bms::SingleTriggerType;
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleIntact, 42)));
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasMoreUnits, 42, 100)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasMoreUnits, 42, 101)));
    // p2 = 0 is trivially satisfied (health <= max - 0); p2 = 1 is not.
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasLostMoreUnits, 42, 0)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasLostMoreUnits, 42, 1)));

    w.commands.add_ssn_hp(42, -30);
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleIntact, 42)));
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasLostMoreUnits, 42, 30)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasLostMoreUnits, 42, 31)));
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasMoreUnits, 42, 70)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasMoreUnits, 42, 71)));

    // A corpse keeps satisfying the damage form (no alive gate).
    w.commands.kill_ssn(42);
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasLostMoreUnits, 42, 100)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleIntact, 42)));

    // Unresolved def (health_max 0; retail null itemDef) fails Intact.
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleIntact, 45)));
    // Pool 2 sits outside the retail pools-0/1 scan; unresolved SSNs false.
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleIntact, 44)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasMoreUnits, 44, 1)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasLostMoreUnits, 99, 1)));

    // SSN 0 reads FALSE even though registry rows with net_id 0 exist (every
    // helper head-guards the zero SSN; the pool finder itself does not).
    // [orig: 'if (!netId) return 0' @0x43e787 / @0x43e2f7 and the heads of
    //  @0x43e3d0/@0x43e470/@0x43e350]
    world::Entity anon;
    anon.net_id = 0;
    anon.alive = true;
    anon.health = 100;
    anon.health_max = 100;
    w.registry.spawn(0, anon);
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleIntact, 0)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasMoreUnits, 0, 1)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasLostMoreUnits, 0, 0)));
}

// Sub 11 both cats over the carried-object link [orig:
// TriggerGroup_AnyMemberHoldingItemGroup @0x43c870 (pool-0 walk, ItemTypeIndex
// gate) / Entity_IsSsnHoldingItemGroup @0x43e2f0].
static void test_holding_triggers() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 8);
    w.registry.configure_pool(1, 8);
    world::Entity holder;
    holder.net_id = 50;
    holder.alive = true;
    holder.group_id = 2;
    holder.item_id = 1001; holder.item_type_index = 7;
    world::EntityHandle holder_h = w.registry.spawn(0, holder);
    world::Entity item;
    item.net_id = 60;
    item.alive = true;
    item.group_id = 9;
    item.item_id = 4095; item.item_type_index = 7;
    world::EntityHandle item_h = w.registry.spawn(1, item);
    // A pool-1 "holder" with the same group: outside the retail pool-0 walk.
    holder.net_id = 51;
    world::EntityHandle wrong_pool_h = w.registry.spawn(1, holder);
    // A pool-0 member with no resolved item (ItemTypeIndex 0): gated out.
    holder.net_id = 52;
    holder.item_id = 0; holder.item_type_index = 0;
    world::EntityHandle ungated_h = w.registry.spawn(0, holder);

    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();

    using S = bms::SingleTriggerType;
    using G = bms::GroupTriggerType;
    CHECK(!sys.evaluate_trigger_for_test(w, group_trigger(G::GroupHoldingGroup, 2, 9)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHoldingGroup, 50, 9)));

    w.registry.get(holder_h)->mounted_child = item_h;
    CHECK(sys.evaluate_trigger_for_test(w, group_trigger(G::GroupHoldingGroup, 2, 9)));
    CHECK(!sys.evaluate_trigger_for_test(w, group_trigger(G::GroupHoldingGroup, 2, 8)));
    CHECK(!sys.evaluate_trigger_for_test(w, group_trigger(G::GroupHoldingGroup, 3, 9)));
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHoldingGroup, 50, 9)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHoldingGroup, 50, 8)));

    // Clearing the link clears both conditions.
    w.registry.get(holder_h)->mounted_child = world::EntityHandle{};
    CHECK(!sys.evaluate_trigger_for_test(w, group_trigger(G::GroupHoldingGroup, 2, 9)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHoldingGroup, 50, 9)));

    // A wrong-pool member holding the item does NOT satisfy either walk
    // (retail scans pool 0 only), and the ItemTypeIndex gate holds.
    w.registry.get(wrong_pool_h)->mounted_child = item_h;
    CHECK(!sys.evaluate_trigger_for_test(w, group_trigger(G::GroupHoldingGroup, 2, 9)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHoldingGroup, 51, 9)));
    w.registry.get(wrong_pool_h)->mounted_child = world::EntityHandle{};
    w.registry.get(ungated_h)->mounted_child = item_h;
    CHECK(!sys.evaluate_trigger_for_test(w, group_trigger(G::GroupHoldingGroup, 2, 9)));
}

// Subs 42-45: the chain/distance/LOS family — RAW-positive senses; the
// authored negation bit is the flipper [orig: Entity_IsOnTopOfChain @0x4f19a0;
// Entity_CheckProximity @0x4f14c0; Entity_CheckLineOfSightInRange @0x4f15e0;
// Entity_CheckLineOfSight @0x4f17c0 (the ±30° facing cone)].
static void test_single_distance_los_chain() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 8);
    world::Entity org;
    org.alive = true;
    org.item_id = 1001; org.item_type_index = 7;
    org.net_id = 70;
    org.position = {0.0f, 0.0f, 0.0f};
    org.yaw = 90; // mission 90 deg = engine BAM 0 = facing +X
    world::EntityHandle a_h = w.registry.spawn(0, org);
    org.net_id = 71;
    org.position = {30.0f, 0.0f, 0.0f};
    world::EntityHandle b_h = w.registry.spawn(0, org);
    org.net_id = 72;
    org.position = {15.0f, 0.0f, 0.0f};
    world::EntityHandle mid_h = w.registry.spawn(0, org);

    // The system carries one authored NEGATED event from the start (the
    // add-a-system-mid-test route never ticks): a negated within-40 trigger
    // ("farther than 40") that sets V7 once the pair separates.
    bms::Event e{};
    e.trigger_index = 0;
    e.trigger_count = 1;
    e.action_index = 0;
    e.action_count = 1;
    bms::Trigger negated = single_trigger(bms::SingleTriggerType::SingleFartherThan, 70, 71, 40);
    negated.condition_flags = bms::Trigger::kConditionNegated;
    mission::BmsEventSystem sys;
    sys.load({e}, {negated}, {misvar(bms::MissionVariableActionSubType::Set, 7, 1)});
    w.add_system(&sys);
    w.load_systems();

    using S = bms::SingleTriggerType;
    // Sub 43 RAW = "within p3 meters" (inclusive).
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleFartherThan, 70, 71, 40)));
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleFartherThan, 70, 71, 30)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleFartherThan, 70, 71, 20)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleFartherThan, 70, 99, 40)));

    // Sub 44 RAW = "in range AND ray clear" — terrain-less default = clear,
    // so only the range gate discriminates here (the blocked-ray leg lives in
    // the ai ctest with a wired heightfield).
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasNoLOS, 70, 71, 40)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasNoLOS, 70, 71, 20)));

    // Sub 45 RAW = "sees": adds the ±30° facing cone over the shooter yaw.
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleDoesNotSeeOrFarther, 70, 71, 40)));
    // 71 faces +Y (mission yaw 0), 70 lies due -X of it: 90 deg off-cone.
    // (Exactly 180 deg astern would PASS via retail's abs INT_MIN overflow
    // quirk — carried in the port, so the test avoids the degenerate angle.)
    w.registry.get(b_h)->yaw = 0;
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleDoesNotSeeOrFarther, 71, 70, 40)));
    // Rotate the shooter 45 deg away: outside the 30-deg cone.
    w.registry.get(a_h)->yaw = 45;
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleDoesNotSeeOrFarther, 70, 71, 40)));
    // 20 deg off: inside the cone.
    w.registry.get(a_h)->yaw = 70;
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleDoesNotSeeOrFarther, 70, 71, 40)));

    // Sub 45's range AND bearing ride the +0x1FC bbox-center offset points
    // (position + the host-stamped model bbox center, added unrotated)
    // [orig: the deltas over the offset sums @0x4f18cd..0x4f18dd; center
    //  writer Entity_InitFromModel @0x40df1e..0x40df4a].
    w.registry.get(a_h)->yaw = 90;
    w.registry.get(b_h)->bbox_center = {15.0f, 0.0f, 0.0f};
    CHECK(!sys.evaluate_trigger_for_test(w,
            single_trigger(S::SingleDoesNotSeeOrFarther, 70, 71, 40)));
    // 45 u over the offsets > 40, though the centers sit 30 u apart.
    w.registry.get(b_h)->bbox_center = {0.0f, 20.0f, 0.0f};
    CHECK(!sys.evaluate_trigger_for_test(w,
            single_trigger(S::SingleDoesNotSeeOrFarther, 70, 71, 40)));
    // atan2(20, 30) = 33.7 deg: the offset point falls outside the cone.
    w.registry.get(b_h)->bbox_center = {0.0f, 10.0f, 0.0f};
    CHECK(sys.evaluate_trigger_for_test(w,
            single_trigger(S::SingleDoesNotSeeOrFarther, 70, 71, 40)));
    // atan2(10, 30) = 18.4 deg: inside again.
    // Sub 44's DISTANCE gate stays center-based: the same 15 u offset that
    // fails sub 45's range leaves sub 44 in range (its ray, not its range,
    // rides the offset points).
    w.registry.get(b_h)->bbox_center = {15.0f, 0.0f, 0.0f};
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleHasNoLOS, 70, 71, 40)));
    w.registry.get(b_h)->bbox_center = {};

    // Sub 42: the groundEntity chain, up to 3 hops, first-match.
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleOnTopOf, 70, 71)));
    w.registry.get(a_h)->ground_target = b_h;
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleOnTopOf, 70, 71)));
    CHECK(!sys.evaluate_trigger_for_test(w, single_trigger(S::SingleOnTopOf, 71, 70)));
    // Two hops through the middle carrier.
    w.registry.get(a_h)->ground_target = mid_h;
    w.registry.get(mid_h)->ground_target = b_h;
    CHECK(sys.evaluate_trigger_for_test(w, single_trigger(S::SingleOnTopOf, 70, 71)));

    // The authored negation bit flips the RAW sense end to end (the loaded
    // event from the fixture head): within 40 -> authored condition false,
    // separated -> "farther than 40" latches on its processing pass and fires
    // at the next 64-tick delay-expiry slot (the witnessed cadence).
    tick_n(w, kPass);
    CHECK(w.script.vars.get_mission(7) == 0);
    w.registry.get(b_h)->position = {80.0f, 0.0f, 0.0f};
    tick_n(w, kPass + kCycle);
    CHECK(w.script.vars.get_mission(7) == 1);
}

// Zone refs in the FILE carry the authored zone ID; the load-time resolver
// rewrites them to the zone-array INDEX (dangling/degenerate refs neuter the
// trigger). The 04TR shape: a NEGATED not-in-area trigger -> RedWin must stay
// quiet while the player stands inside the id-referenced zone.
// [orig: the mission-start resolvers @0x453000/@0x453100 over record[0]]
static void test_zone_refs_resolve_by_id() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    world::Aabb inner;
    inner.min = {0.0f, 0.0f, -16384.0f};
    inner.max = {100.0f, 100.0f, 16384.0f};
    // Zone IDs deliberately out of order and nowhere near the array indices.
    w.registry.register_area("", inner, true, /*zone_id=*/9);
    w.registry.register_area("", inner, true, /*zone_id=*/6);

    world::Entity seed{};
    seed.alive = true;
    seed.net_id = 10000; // resolve_ssn maps 10000 -> the local player anyway
    seed.position = {50.0f, 50.0f, 0.0f};
    world::EntityHandle player = w.registry.spawn(0, seed);
    w.cached.local_player = player;

    bms::Event e = simple_event(bms::EventFlags::None, 0);
    e.trigger_count = 1;
    e.action_count = 1;
    bms::Trigger not_in_area{};
    not_in_area.condition_flags = 1; // negated
    not_in_area.main_type = bms::TriggerMainType::Single;
    not_in_area.sub_type = static_cast<int32_t>(bms::SingleTriggerType::SingleIsWithinArea);
    not_in_area.param1 = 10000;   // the player SSN
    not_in_area.param2 = 6;       // zone ID 6 == array index 1
    bms::Action rw{};
    rw.action_type = bms::ActionType::RedWin;
    mission::BmsEventSystem sys;
    sys.load({e}, {not_in_area}, {rw});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kCycle);
    CHECK(!w.match.outcome().ended); // inside zone-id 6 -> the negated trigger is false

    w.registry.get(player)->position = {500.0f, 500.0f, 0.0f}; // leave the zone
    tick_n(w, kCycle);
    CHECK(w.match.outcome().ended); // out of bounds -> RedWin
    CHECK(w.match.outcome().winner_team == 2);
}

// BMS area helpers use their own pool scans, not the WAC SSN resolver.
// [orig: Entity_IsBmsRefInTriggerBounds @ 0x43e510;
// Entity_IsTeamInTriggerBounds @ 0x43c730]
static void test_area_trigger_pool_and_identity_rules() {
    World w;
    for (int pool = 0; pool < 4; ++pool) w.registry.configure_pool(pool, 4);
    world::Aabb box{{0.0f, 0.0f, -2.0f}, {10.0f, 10.0f, 2.0f}};
    w.registry.register_area("zone", box, true, 17);
    world::Entity outside{};
    outside.net_id = 71;
    outside.group_id = 3;
    outside.position = {11.0f, 5.0f, 0.0f};
    w.registry.spawn(0, outside);
    world::Entity inside = outside;
    inside.position = {10.0f, 10.0f, 2.0f}; // inclusive XYZ edges
    inside.alive = false;
    inside.flags |= 1u;
    const auto item = w.registry.spawn(1, inside);
    mission::BmsEventSystem sys;
    bms::Trigger single{};
    single.main_type = bms::TriggerMainType::Single;
    single.sub_type = static_cast<int32_t>(bms::SingleTriggerType::SingleIsWithinArea);
    single.param1 = 71;
    single.param2 = 0;
    CHECK(sys.evaluate_trigger_for_test(w, single)); // duplicate + dead item
    bms::Trigger group = single;
    group.main_type = bms::TriggerMainType::Group;
    group.sub_type = static_cast<int32_t>(bms::GroupTriggerType::GroupIsWithinArea);
    group.param1 = 3;
    CHECK(sys.evaluate_trigger_for_test(w, group)); // pool-1 death flag is ignored
    w.registry.despawn(item);
    const auto person = w.registry.spawn(0, inside);
    CHECK(sys.evaluate_trigger_for_test(w, single)); // singles ignore the flag in both pools
    CHECK(!sys.evaluate_trigger_for_test(w, group));
    w.registry.get(person)->flags &= ~1u;
    CHECK(sys.evaluate_trigger_for_test(w, group));
    w.registry.despawn(person);
    w.registry.spawn(2, inside);
    w.registry.spawn(3, inside);
    CHECK(!sys.evaluate_trigger_for_test(w, single));
    CHECK(!sys.evaluate_trigger_for_test(w, group));
    inside.net_id = 0;
    inside.group_id = 0;
    const auto local = w.registry.spawn(0, inside);
    w.cached.local_player = local;
    single.param1 = 0;
    group.param1 = 0;
    CHECK(!sys.evaluate_trigger_for_test(w, single));
    CHECK(!sys.evaluate_trigger_for_test(w, group));
    single.param1 = 10000;
    CHECK(sys.evaluate_trigger_for_test(w, single)); // SP representation alias
    w.rules.mp_session = true;
    CHECK(!sys.evaluate_trigger_for_test(w, single)); // no MP local-player substitution
    single.param1 = 0x10047; // must not wrap to SSN 71
    CHECK(!sys.evaluate_trigger_for_test(w, single));
    group.param1 = 259; // must not wrap to group 3
    CHECK(!sys.evaluate_trigger_for_test(w, group));
}

// A dangling zone id NEUTERS the trigger (main/sub zeroed, flags kept): the
// negated not-in-area chain then reads false->negate->true, firing the event —
// exactly the original's dangling-ref behavior. [orig: @0x45309e/@0x4530b9]
static void test_dangling_zone_ref_neuters_trigger() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    bms::Event e = simple_event(bms::EventFlags::None, 0);
    e.trigger_count = 1;
    e.action_count = 1;
    bms::Trigger t{};
    t.main_type = bms::TriggerMainType::Single;
    t.sub_type = static_cast<int32_t>(bms::SingleTriggerType::SingleIsWithinArea);
    t.param1 = 10000;
    t.param2 = 42; // no such zone id
    bms::Action act{};
    act.action_type = bms::ActionType::OutputText;
    act.param1 = 7;
    mission::BmsEventSystem sys;
    sys.load({e}, {t}, {act});
    w.add_system(&sys);
    w.load_systems();
    tick_n(w, kPass);
    CHECK(w.out.effects.count("text") == 0); // neutered trigger evaluates false
}

// The BMS win actions end the round through the SAME entry the WAC win/lose
// handlers use [orig: EventAction_Dispatch @0x4542e0, the BlueWin case @0x45447b ->
// Server_ProcessRoundEnd(1), one shared round end for both script front-ends].
static void test_bluewin_ends_round() {
    World w;
    w.cached.humans = 1;
    w.registry.configure_pool(0, 4);
    bms::Event e = simple_event(bms::EventFlags::None, 0);
    e.action_count = 1;
    bms::Action bw{};
    bw.action_type = bms::ActionType::BlueWin;
    mission::BmsEventSystem sys;
    sys.load({e}, {}, {bw});
    w.add_system(&sys);
    w.load_systems();

    tick_n(w, kPass);
    CHECK(w.match.outcome().ended);
    CHECK(w.match.outcome().winner_team == 1);
    CHECK(w.out.effects.count("win") == 1);
    CHECK(w.out.effects.count("round_end") == 1);
}

// An EMPTY host must not burn through its mission. Retail wraps the WAC tick,
// the idle sweep and the BMS event pump in one condition whose live half is
// `g_WacVarHumans || !g_WacVarTicks`: once the VM has run at all, the whole
// script HOLDS until a human is in the world.
//
// This is not academic. 05TRcoop's event 40 is "group 1 is NOT within area 6"
// (the negated bit) running a KillGroup, and group 1 is where players live - so
// on an empty host it is trivially true and kills ten AI a fifth of a second in,
// before any client can be there to be told. Retail never reaches it.
static void test_empty_host_holds_the_script() {
    world::World w;
    w.registry.configure_pool(0, 8);
    world::Entity victim;
    victim.net_id = 100;
    victim.group_id = 3;
    victim.alive = true;
    victim.health = 150;
    w.registry.spawn(0, victim);

    // One event, no triggers at all -> the chain is vacuously TRUE, so the only
    // thing that can hold it is the gate under test.
    std::vector<bms::Event> events(1);
    events[0].trigger_index = 0;
    events[0].trigger_count = 0;
    events[0].action_index = 0;
    events[0].action_count = 1;
    std::vector<bms::Trigger> triggers;
    std::vector<bms::Action> actions(1);
    actions[0].action_type = bms::ActionType::KillGroup;
    actions[0].param1 = 3;

    mission::BmsEventSystem bms_sys;
    bms_sys.load(events, triggers, actions);
    w.add_system(&bms_sys);
    w.load_systems();

    // No humans, and the VM has already run (wac_ticks != 0): the pump is held.
    w.cached.humans = 0;
    w.cached.wac_ticks = 1;
    for (int t = 0; t < 200; ++t) w.run_logic_tick(/*is_authority=*/true);
    CHECK(w.commands.ssn_alive(100));
    CHECK(w.round_sim.deaths.empty());

    // A human arrives and the same event goes through.
    w.cached.humans = 1;
    for (int t = 0; t < 200; ++t) w.run_logic_tick(/*is_authority=*/true);
    CHECK(w.commands.ssn_dead(100));

    // The other half of the condition: before the VM has ever run, the script
    // advances regardless of who is watching.
    world::World w2;
    w2.cached.humans = 0;
    w2.cached.wac_ticks = 0;
    CHECK(w2.script_may_advance());
}

// The retail outer condition admits both script systems before WAC executes.
// A mutable ticks word (including -1 + 1 wrap) is not the diagnostic run count.
static void test_wac_and_bms_share_admission_before_clock_or_human_writes() {
    for (bool change_clock : {false, true}) {
        World w;
        w.registry.configure_pool(0, 8);
        w.cached.humans = change_clock ? 0 : 1;
        wac::WacSystem script;
        const std::string source = change_clock
                ? "inc(v1) set(ticks,-1) if eq(v1,8) then set(ticks,4) endif\n"
                : "inc(v1) if eq(v1,8) then set(humans,0) endif\n";
        script.set_program(wac::compile_source(source, {}));
        mission::BmsEventSystem events;
        std::vector<bms::Event> rows(4, simple_event(bms::EventFlags::None, 0));
        for (bms::Event &row : rows) row.trigger_count = 1;
        bms::Trigger trigger{};
        trigger.main_type = bms::TriggerMainType::MissionVariable;
        trigger.sub_type = static_cast<int32_t>(
                bms::MissionVariableTriggerType::MissionVariableIsEqual);
        trigger.param1 = 1;
        trigger.param2 = 8;
        events.load(rows, {trigger}, {output_text(23)});
        w.add_system(&script);
        w.add_system(&events);
        w.load_systems();
        // At tick 496 the eighth WAC execution coincides with a BMS quarter.
        tick_n(w, 8 * wac::WacSystem::kTicksPerExecution - 1);
        CHECK(script.runs() == 7);
        CHECK(w.out.effects.count("text") == 0);
        if (change_clock) CHECK(script.vm().time() == 0 && w.cached.wac_ticks == 0);
        tick_n(w, 1);
        CHECK(script.runs() == 8);
        CHECK(w.cached.wac_ticks == (change_clock ? 5 : 8));
        CHECK(w.cached.humans == 0);
        CHECK(w.out.effects.count("text") == 1); // same admitted tick completes
        CHECK(!w.script_may_advance());
        tick_n(w, 128);
        CHECK(script.runs() == 8);
        CHECK(w.out.effects.count("text") == 1); // next admissions hold
    }
}

static void test_change_ai_command_family() {
    World w;
    w.registry.configure_pool(0, 8);

    world::Entity entity{};
    entity.net_id = 42;
    entity.item_id = 1001; entity.item_type_index = 7;
    entity.alive = true;
    const world::EntityHandle handle = w.registry.spawn(0, entity);

    world::AiSystem &ai = w.ai;
    world::AiEntity &ae = *ai.at(ai.attach(handle));
    ae.profile.type = 2; // GROUND
    ae.brain.f[world::AiBrain::kCurState] = world::kAiGroundFollowWp;
    ae.brain.f[world::AiBrain::kPendState] = world::kAiGroundFollowWp;

    mission::BmsEventSystem events;
    auto dispatch = [&](int subtype, int32_t p2 = 0, int32_t p3 = 0,
                        int32_t p4 = 0) {
        bms::Action action{};
        action.action_type = bms::ActionType::ChangeSingleAI;
        action.action_sub_type = subtype;
        action.param1 = 42;
        action.param2 = p2;
        action.param3 = p3;
        action.param4 = p4;
        events.dispatch_action_for_test(w, action);
    };

    dispatch(2, 1);
    CHECK((w.registry.get(handle)->flags & world::kEntityFlagMounted) != 0);
    CHECK((w.registry.get(handle)->engine_flags &
           world::kEntityFlagMounted) != 0);
    dispatch(8, 70);
    CHECK(ae.slot.f[world::AiSlot::kAimErrorPrimary] == 30);
    dispatch(15, 1);
    dispatch(16, 1);
    CHECK((static_cast<uint32_t>(
                   ae.slot.f[world::AiSlot::kBehaviorFlags]) &
           0x201u) == 0x201u);
    // Sub 17 is CLIMBER_BIT on the AI slot (behavior 0x400); the entity
    // Flags 0x80 climb-chase write is sub 23 [orig: Entity_ApplyCommand
    // cases 0x11 @0x43af25 / 0x17 @0x43afae].
    dispatch(17, 1);
    CHECK((static_cast<uint32_t>(
                   ae.slot.f[world::AiSlot::kBehaviorFlags]) &
           0x400u) != 0);
    CHECK((w.registry.get(handle)->flags & world::kEntityFlagAiClimb) == 0);
    dispatch(23, 1);
    CHECK((w.registry.get(handle)->flags & world::kEntityFlagAiClimb) != 0);
    CHECK((w.registry.get(handle)->engine_flags &
           world::kEntityFlagAiClimb) != 0);
    ae.slot.f[world::AiSlot::kBehaviorFlags] |= 0x20000;
    dispatch(21, 1);
    CHECK((static_cast<uint32_t>(
                   ae.slot.f[world::AiSlot::kBehaviorFlags]) &
           0x20000u) == 0);
    CHECK((static_cast<uint32_t>(
                   ae.slot.f[world::AiSlot::kBehaviorFlags]) &
           0x8u) != 0);
    dispatch(41, 12);
    CHECK(ae.slot.f[world::AiSlot::kAttackRange] == (12 << 16));
    dispatch(42, 3, 9);
    CHECK(ae.slot.f[world::AiSlot::kEngageMin] == (3 << 16));
    CHECK(ae.slot.f[world::AiSlot::kSightRange] == (9 << 16));
    dispatch(43, 1);
    // The Indestructible consumers (destruction, collision_resolve, round_sim)
    // read engine_flags — the retail Flags dword home.
    CHECK((w.registry.get(handle)->engine_flags &
           world::kEntityFlagIndestructible) != 0);
    // Sub 43 is the one arm with no aiRuntime gate: a brainless row (a
    // building, a parked vehicle) takes it too [orig: case 0x2B @0x43b20a].
    {
        world::Entity building{};
        building.net_id = 43;
        building.item_id = 2002; building.item_type_index = 7;
        building.alive = true;
        const world::EntityHandle bh = w.registry.spawn(0, building);
        CHECK(ai.for_handle(bh) == nullptr);
        bms::Action action{};
        action.action_type = bms::ActionType::ChangeSingleAI;
        action.action_sub_type = 43;
        action.param1 = 43;
        action.param2 = 1;
        events.dispatch_action_for_test(w, action);
        CHECK((w.registry.get(bh)->engine_flags &
               world::kEntityFlagIndestructible) != 0);
        action.param2 = 0;
        events.dispatch_action_for_test(w, action);
        CHECK((w.registry.get(bh)->engine_flags &
               world::kEntityFlagIndestructible) == 0);
    }
    // FIND_AND_USE with no matching model point leaves the prior relation alone.
    dispatch(31, 7);
    dispatch(41, 12);
    dispatch(32);
    CHECK(ae.brain.f[world::AiBrain::kUseWaypointZones] == 1);
    dispatch(33);
    CHECK(ae.brain.f[world::AiBrain::kUseWaypointZones] == 0);

    auto dispatch_queued = [&](int subtype, int32_t argument) {
        ae.brain.f[world::AiBrain::kCurState] =
                world::kAiGroundFollowWp;
        ae.brain.f[world::AiBrain::kPendState] =
                world::kAiGroundFollowWp;
        dispatch(subtype, argument);
        CHECK(ai.events.count() == 1);
        // The command-queued family stamps channel 0, not the combat sites'
        // 9 [orig: event_source = 0 @0x43b30e].
        CHECK(ai.events.at(0).channel() == 0);
        ai.events.process_timed(ai, w);
        CHECK(ai.events.count() == 0);
    };

    dispatch_queued(27, 3);
    CHECK(ae.brain.f[world::AiBrain::kAccuracy] == 3);
    dispatch_queued(26, 9);
    CHECK(ae.brain.f[world::AiBrain::kDriveSkill] == 4);
    dispatch_queued(29, 36);
    CHECK(ae.brain.f[world::AiBrain::kSpeedA] == 10485);
    dispatch_queued(30, 18);
    CHECK(ae.brain.f[world::AiBrain::kSpeedB] == 5242);
    dispatch_queued(28, 1);
    CHECK(ae.brain.f[world::AiBrain::kFallback] ==
          world::kAiGroundFormation);
    CHECK(ae.brain.f[world::AiBrain::kCurState] ==
          world::kAiGroundFormation);
    dispatch_queued(45, 1);
    CHECK(ae.brain.bytes()[world::AiBrain::kGuardFireByte] == 1);
    CHECK(ae.brain.f[world::AiBrain::kCurState] ==
          world::kAiGroundCombat);
    dispatch_queued(46, 15);
    CHECK(ae.brain.f[world::AiBrain::kElevationBias] ==
          15 * 11930464);
}

static void test_structural_bms_actions() {
    World w;
    for (int pool = 0; pool < 4; ++pool)
        w.registry.configure_pool(pool, 8);

    world::Entity marker{};
    marker.item_id = world::kParticleEffectMarkerTypeId;
    marker.has_item_def = true;
    marker.wp_number = 9;
    marker.position = {10.0f, 20.0f, 30.0f};
    marker.yaw = 45;
    marker.pitch = 5;
    marker.roll = -3;
    // The BMS spawn stamps the chute bit on engine_flags; the teleport read
    // merges both views.
    marker.engine_flags = world::kEntityFlagParachute;
    w.registry.spawn_from(3, 0, marker);

    auto member = [](uint16_t ssn) {
        world::Entity entity{};
        entity.net_id = ssn;
        entity.item_id = 1001;
        entity.has_item_def = true;
        entity.item_type_index = 7; // the def row's ordinal the +0x1C walk reads
        entity.group_id = 2;
        entity.team = 1;
        entity.alive = true;
        entity.flags = world::kEntityFlagMatrixBuilt;
        entity.engine_flags = world::kEntityFlagMatrixBuilt;
        entity.position = {-1.0f, -2.0f, -3.0f};
        return entity;
    };

    const world::EntityHandle pool0_h =
            w.registry.spawn_from(0, 0, member(100));
    const world::EntityHandle pool1_h =
            w.registry.spawn_from(1, 0, member(200));
    const world::EntityHandle pool2_h =
            w.registry.spawn_from(2, 0, member(300));

    world::Entity dead = member(101);
    dead.flags |= world::kEntityFlagDead;
    dead.alive = false;
    const world::EntityHandle dead_h =
            w.registry.spawn_from(0, 1, dead);

    world::Entity removable = member(400);
    removable.group_id = 4;
    const world::EntityHandle pool3_h =
            w.registry.spawn_from(3, 1, removable);

    world::AiSystem &ai = w.ai;
    world::AiEntity &pool1_ai = *ai.at(ai.attach(pool1_h));

    mission::BmsEventSystem events;
    auto dispatch = [&](bms::ActionType type, int32_t p1 = 0,
                        int32_t p2 = 0) {
        bms::Action action{};
        action.action_type = type;
        action.param1 = p1;
        action.param2 = p2;
        events.dispatch_action_for_test(w, action);
    };

    dispatch(bms::ActionType::Null);
    dispatch(bms::ActionType::SingleVelocity, 200, 99);
    CHECK(w.registry.get(pool1_h)->move_speed_kph == 0);
    CHECK(w.out.effects.count("unported_action") == 0);

    dispatch(bms::ActionType::GroupVelocity, 2, 36);
    CHECK(w.script.relations.group(2).move_speed_q16_per_sec == 655360);

    dispatch(bms::ActionType::ChangeGTeamAction, 2, 3);
    CHECK(w.registry.get(pool0_h)->team == 3);
    CHECK(w.registry.get(pool1_h)->team == 3);
    CHECK(w.registry.get(pool2_h)->team == 3);
    CHECK(w.registry.get(dead_h)->team == 3);
    CHECK(pool1_ai.team == 3);

    dispatch(bms::ActionType::ChangeGroupAction, 2, 4);
    CHECK(w.registry.get(pool0_h)->group_id == 4);
    CHECK(w.registry.get(pool1_h)->group_id == 4);
    CHECK(w.registry.get(pool2_h)->group_id == 4);
    CHECK(w.registry.get(dead_h)->group_id == 2);
    CHECK(pool1_ai.relmat_id == 4);

    dispatch(bms::ActionType::GroupTeleportAction, 4, 9);
    for (world::EntityHandle handle : {pool0_h, pool1_h, pool2_h}) {
        const world::Entity *entity = w.registry.get(handle);
        CHECK(entity->position.x == 10.0f);
        CHECK(entity->position.y == 20.0f);
        CHECK(entity->position.z == 30.0f);
        CHECK(entity->yaw == 45);
        CHECK(entity->pitch == 5);
        // Pool 0 enters Entity_ResetToSpawnState, which levels Roll.
        CHECK(entity->roll == (handle.pool() == 0 ? 0 : -3));
    }
    CHECK(w.registry.get(pool0_h)->spawn_position.x == 10.0f);
    CHECK((w.registry.get(pool0_h)->engine_flags &
           world::kEntityFlagMatrixBuilt) != 0);
    CHECK((w.registry.get(pool1_h)->engine_flags &
           world::kEntityFlagMatrixBuilt) == 0);
    CHECK((w.registry.get(pool2_h)->engine_flags &
           world::kEntityFlagMatrixBuilt) == 0);
    CHECK(pool1_ai.pos[0] == (10 << 16));
    CHECK(pool1_ai.pos[1] == (20 << 16));
    CHECK(pool1_ai.pos[2] == (30 << 16));
    CHECK(pool1_ai.heading ==
          world::bam_heading_from_mission_yaw_deg(45.0));

    dispatch(bms::ActionType::ChangeSteamAction, 100, 5);
    CHECK(w.registry.get(pool0_h)->team == 5);
    dispatch(bms::ActionType::SingleChangeGroup, 100, 7);
    CHECK(w.registry.get(pool0_h)->group_id == 7);

    w.registry.get(pool0_h)->position = {};
    w.registry.get(pool0_h)->engine_flags |= world::kEntityFlagMatrixBuilt;
    dispatch(bms::ActionType::SingleTeleportAction, 100, 9);
    CHECK(w.registry.get(pool0_h)->position.x == 10.0f);
    CHECK(w.registry.get(pool0_h)->spawn_position.z == 30.0f);
    CHECK((w.registry.get(pool0_h)->engine_flags &
           world::kEntityFlagMatrixBuilt) == 0);
    CHECK((w.registry.get(pool0_h)->flags & world::kEntityFlagParachute) != 0);
    CHECK((w.registry.get(pool0_h)->engine_flags &
           world::kEntityFlagParachute) != 0);

    dispatch(bms::ActionType::VaporizeGroup, 4);
    CHECK(w.registry.get(pool1_h) == nullptr);
    CHECK(w.registry.get(pool2_h) == nullptr);
    CHECK(w.registry.get(pool3_h) == nullptr);
    CHECK(w.registry.get(pool0_h) != nullptr);
    CHECK(w.registry.get(dead_h) != nullptr);
    CHECK(w.out.effects.count("unported_action") == 0);
}

static void test_bms_target_selectors_and_retail_noops() {
    World w;
    w.cached.humans = 1;
    for (int pool = 0; pool <= 2; ++pool) w.registry.configure_pool(pool, 4);
    world::Entity seed{};
    seed.net_id = 42;
    seed.group_id = 7;
    const auto first = w.registry.spawn(0, seed);
    const auto duplicate = w.registry.spawn(1, seed);
    seed.net_id = 43;
    seed.health = 0;
    seed.alive = false;
    const auto dead = w.registry.spawn(1, seed);
    const auto excluded_pool = w.registry.spawn(2, seed);
    mission::BmsEventSystem events;
    w.add_system(&events);
    const auto fire = [&](bms::ActionType type, int source, int target) {
        bms::Action action{};
        action.action_type = type;
        action.param1 = source;
        action.param2 = target;
        events.load({simple_event(bms::EventFlags::None, 0)}, {}, {action});
        w.load_systems();
        tick_n(w, kCycle);
    };
    using S = world::AiTargetSelector;
    const bms::ActionType singles[] = {bms::ActionType::SsnTargetSsnExc,
        bms::ActionType::SsnTargetSsnPri, bms::ActionType::SsnTargetGroupExc,
        bms::ActionType::SsnTargetGroupPri};
    const bms::ActionType groups[] = {bms::ActionType::GroupTargetSsnExc,
        bms::ActionType::GroupTargetSsnPri, bms::ActionType::GroupTargetGroupExc,
        bms::ActionType::GroupTargetGroupPri};
    const auto words = [](const world::Entity &entity) {
        const auto &s = entity.target_selectors;
        return std::array<uint16_t, 4>{s.exclusive_ssn, s.preferred_ssn, s.exclusive_group, s.preferred_group};
    };
    for (int field = 0; field < 4; ++field) {
        fire(singles[field], 42, 0x10055 + field);
        CHECK(words(*w.registry.get(first))[field] == 0x55 + field);
        CHECK(words(*w.registry.get(duplicate))[field] == 0);
        fire(groups[field], 7, 0xFFFF);
        CHECK(words(*w.registry.get(first))[field] == 0xFFFF);
        CHECK(words(*w.registry.get(duplicate))[field] == 0xFFFF);
        CHECK(words(*w.registry.get(dead))[field] == 0xFFFF);
        CHECK(words(*w.registry.get(excluded_pool))[field] == 0);
        fire(groups[field], 0, 1);
        CHECK(words(*w.registry.get(first))[field] == 0xFFFF);
        fire(singles[field], 0x1002A, 1); // source is not narrowed to 16 bits
        CHECK(words(*w.registry.get(first))[field] == 0xFFFF);
        fire(groups[field], 7, 0); // clear each selector independently
        CHECK(words(*w.registry.get(dead))[field] == 0);
    }
    fire(bms::ActionType::ExecuteWac, 1, 0);
    CHECK(w.out.effects.count("execute_wac") == 0);
    CHECK(w.out.effects.count("unported_action") == 0);

    w.ai.attach(first);
    auto &brain = w.ai.for_handle(first)->brain;
    const auto command = [&](int sub, int target) {
        bms::Action action{};
        action.action_type = bms::ActionType::ChangeSingleAI;
        action.action_sub_type = sub;
        action.param1 = 42;
        action.param2 = target;
        events.load({simple_event(bms::EventFlags::None, 0)}, {}, {action});
        tick_n(w, kCycle);
    };
    brain.f[world::AiBrain::kPriorityTarget] = int(duplicate.packed) + 1;
    command(world::EntityCommands::kTargetSsn, 42);
    CHECK(brain.f[world::AiBrain::kPriorityTarget] == int(duplicate.packed) + 1);
    command(world::EntityCommands::kTargetSsn, 43); // only a pool-1 match -> clear
    CHECK(brain.f[world::AiBrain::kPriorityTarget] == 0);
    command(world::EntityCommands::kHudItem, 1);
    command(world::EntityCommands::kTmateStatus, 1);
}

int main() {
    test_wac_and_bms_share_admission_before_clock_or_human_writes();
    test_bms_target_selectors_and_retail_noops();
    test_change_ai_command_family();
    test_structural_bms_actions();
    test_empty_host_holds_the_script();
    test_bms_to_wac_shared_var();
    test_wac_to_bms_shared_var();
    test_wac_accuracyspread_drives_npc_aim();
	test_wac_seatbelt_controls_local_use();
	test_bms_increment_and_threshold();
    test_activation_delay();
    test_activation_delay_signed_wrap();
    test_repeat_cooldown();
    test_signed_action_count_immediate_and_delayed();
    test_trigger_count_is_signed_in_the_chain();
    test_repeat_zero_refires_every_pass();
    test_pre_mission_pass();
    test_playpartanim_mutates_brain();
    test_redirect_actions_preserve_authored_node();
    test_redirect_actions_accept_boarding_and_stop_commands();
    test_presentation_effects();
    test_waypoint_track_integration();
    test_subgoal_state();
    test_subgoal_tally_gate();
    test_output_text_and_reset_event();
    test_event_trigger_reads_window();
    test_playpartanim_zero_time_wraps_like_retail();
    test_second_time_through_parity();
    test_teammate_triggers();
    test_player_awol_counter_and_trigger();
    test_player_mount_trigger_dispatch();
    test_player_input_bit_triggers();
    test_player_berserk_trigger();
    test_player_dialog_triggers();
    test_player_satchel_in_area_trigger();
    test_post_pass_is_a_one_shot();
    test_trigger_relations_group_records();
    test_trigger_relations_matrices_and_visited();
    test_single_alert_triggers();
    test_single_health_triggers();
    test_holding_triggers();
    test_single_distance_los_chain();
    test_zone_refs_resolve_by_id();
    test_area_trigger_pool_and_identity_rules();
    test_dangling_zone_ref_neuters_trigger();
    test_bluewin_ends_round();
    std::printf(failures ? "EVENT RUNTIME TESTS FAILED (%d)\n" : "event runtime tests passed\n", failures);
    // --- BMS action 27, PARTICLE_EFFECT [orig: EventAction_Dispatch case 0x1B
    // @0x4542e0 -> sub_4540E0]. Selects pool-3 def-type-6088 markers by WP_NUMBER.
    // The kong banner calls entity[167] a "team", but 00TRg's four 6088 markers carry
    // no team byte at all while their wp_numbers are 1/2/3/4 -- matching on team would
    // fire nothing, so this pins the wp_number match specifically.
    {
        World pw;
        pw.registry.configure_pool(3, 8);
        auto put_marker = [&](int slot, int32_t type_id, int32_t wpn) {
            world::Entity m;
            m.item_id = type_id;
            m.has_item_def = true;
            m.script_effect_name = "authored_effect";
            m.wp_number = wpn;
            m.alive = true;
            m.position = {static_cast<float>(10 * slot), 20.0f, 30.0f};
            pw.registry.spawn_from(3, slot, m);
        };
        put_marker(0, 6088, 1);
        put_marker(1, 6088, 2);
        put_marker(2, 6088, 2);   // two markers share wp_number 2
        put_marker(3, 6087, 2);   // right wp_number, WRONG def type
        put_marker(4, 6088, 0);   // no wp_number

        CHECK(pw.commands.spawn_marker_particle_effects(1) == 1);
        CHECK(pw.commands.spawn_marker_particle_effects(2) == 2);
        CHECK(pw.commands.spawn_marker_particle_effects(3) == 0);
        CHECK(pw.out.script_effects.size() == 3);
        CHECK(pw.out.script_effects[0].name == "authored_effect");
        CHECK(pw.out.script_effects[0].lookup_by_name);
    }

    return failures ? 1 : 0;
}
