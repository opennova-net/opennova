// BMS event parity: retail-witnessed trigger and action semantics driven through
// the public BmsEventSystem / EntityCommands seams. Each block names the retail
// function it pins; the synthetic worlds are the smallest that exercise the leg.
#include <cstdint>
#include <cstdio>
#include <string>
#include <variant>
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

static world::EntityHandle spawn_row(World &w, int pool, uint16_t ssn, uint8_t group,
                                     uint8_t team = 0) {
    world::Entity e{};
    e.net_id = ssn;
    e.item_id = 1213;
    e.health = 100;
    e.group_id = group;
    e.team = team;
    return w.registry.spawn(pool, e);
}

static bool indestructible(const World &w, world::EntityHandle h) {
    const world::Entity *e = w.registry.get(h);
    return e != nullptr && (e->engine_flags & world::kEntityFlagIndestructible) != 0;
}

static bms::Action action(bms::ActionType type, int32_t sub, int32_t p1, int32_t p2 = 0,
                          int32_t p3 = 0, int32_t p4 = 0) {
    bms::Action a{};
    a.action_type = type;
    a.action_sub_type = sub;
    a.param1 = p1;
    a.param2 = p2;
    a.param3 = p3;
    a.param4 = p4;
    return a;
}

// ChangeGroupAI fans over pool 2 rows WITH an AI component, then pools 0 and 1
// unconditionally; pool 3 is never walked. Sub 0 and group 0 do nothing. The
// run_group_fans Unicorn probe reached the brained pool-2 row and the
// brainless pool-0/1 rows, never the brainless pool-2 building or the pool-3
// marker. 01TR's PreMission ChangeGroupAI(6/7, INDESTRUCTABLE) therefore leaves
// its brainless pool-2 crates destructible.
// [orig: Entity_HandleAlertCommand @0x43cf10 — sub 0 @0x43cf1e, group 0
//  @0x43cf2c, the pool-2 aiRuntime gate @0x43cf64, pools 0/1 @0x43cf97/@0x43cfcd]
static void test_change_group_ai_fans_pools_2_0_1() {
    World w;
    w.cached.humans = 1;
    configure_pools(w);
    const world::EntityHandle brained2 = spawn_row(w, 2, 301, 6);
    const world::EntityHandle crate2 = spawn_row(w, 2, 302, 6);
    const world::EntityHandle row0 = spawn_row(w, 0, 303, 6);
    const world::EntityHandle row1 = spawn_row(w, 1, 304, 6);
    const world::EntityHandle marker3 = spawn_row(w, 3, 305, 6);
    const world::EntityHandle group0 = spawn_row(w, 0, 306, 0);
    w.ai.attach(brained2);

    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();

    sys.dispatch_action_for_test(w, action(bms::ActionType::ChangeGroupAI, 43, 6, 1));
    CHECK(indestructible(w, brained2));
    CHECK(!indestructible(w, crate2));
    CHECK(indestructible(w, row0));
    CHECK(indestructible(w, row1));
    CHECK(!indestructible(w, marker3));

    CHECK(w.commands.apply_group_ai_command(0, 43, 1, 0, 0) == 0);
    CHECK(!indestructible(w, group0));
    // Sub 0 returns before the fan: the brained row takes nothing.
    CHECK(w.commands.apply_group_ai_command(6, 0, 1, 0, 0) == 0);
    // The alert stamp still follows the fan.
    CHECK(w.commands.apply_group_ai_command(6, 5, 0, 0, 0) == 1);
    CHECK(w.script.relations.group(6).alert == world::TriggerRelations::kAlertRed);
}

// VaporizeSingle and VaporizeGroup remove through Server_RemoveEntityAndNotify:
// every removed row queues the S2C 0x12 removal for the joiners and runs the
// shared destroy (a brain holding the row as its priority target lets go).
// SSN 0, group 0 and a non-authority peer do nothing, and no live recount
// follows the group removal.
// [orig: find_entity_by_parent_and_dispatch @0x43e210 — SSN 0 @0x43e214,
//  authority @0x43e21c; Entity_TeleportAllByNetId @0x43d5d0 — group 0
//  @0x43d5d5, authority @0x43d5dd, pools 2,0,1,3 @0x43d615/@0x43d646/
//  @0x43d677/@0x43d6a8; Server_RemoveEntityAndNotify @0x50a270 — the 0x12 send
//  @0x50a2ac, Entity_Destroy @0x50a2c4]
static void test_vaporize_notifies_and_destroys() {
    World w;
    w.cached.humans = 1;
    configure_pools(w);
    // Our local player row carries net_id 0; SSN 0 must not reach it.
    const world::EntityHandle local = spawn_row(w, 0, 0, 0);
    const world::EntityHandle single = spawn_row(w, 1, 401, 0);
    const world::EntityHandle g3 = spawn_row(w, 3, 402, 4);
    const world::EntityHandle g1 = spawn_row(w, 1, 403, 4);
    const world::EntityHandle g0 = spawn_row(w, 0, 404, 4);
    const world::EntityHandle g2 = spawn_row(w, 2, 405, 4);
    const world::EntityHandle watcher = spawn_row(w, 1, 406, 9);
    w.ai.attach(watcher);
    w.ai.for_handle(watcher)->brain.f[world::AiBrain::kPriorityTarget] =
            int32_t(g0.packed) + 1;

    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();
    w.recount_group_live();
    const int32_t live_before = w.script.relations.group(4).live_count;
    CHECK(live_before == 3); // pools 2, 0, 1 count; the pool-3 row does not

    const auto removals = [&]() {
        std::vector<uint16_t> out;
        for (const auto &event : w.out.entity_events)
            if (const auto *r = std::get_if<world::EntityRemoveEvent>(&event))
                out.push_back(r->handle);
        return out;
    };

    w.out.entity_events.clear();
    sys.dispatch_action_for_test(w, action(bms::ActionType::VaporizeSingle, 0, 0));
    CHECK(w.registry.get(local) != nullptr);
    CHECK(removals().empty());

    w.rules.logic_authority = false;
    sys.dispatch_action_for_test(w, action(bms::ActionType::VaporizeSingle, 0, 401));
    sys.dispatch_action_for_test(w, action(bms::ActionType::VaporizeGroup, 0, 4));
    CHECK(w.registry.get(single) != nullptr);
    CHECK(w.registry.get(g0) != nullptr);
    CHECK(removals().empty());
    w.rules.logic_authority = true;

    sys.dispatch_action_for_test(w, action(bms::ActionType::VaporizeSingle, 0, 401));
    CHECK(w.registry.get(single) == nullptr);
    CHECK(removals() == std::vector<uint16_t>{single.packed});

    w.out.entity_events.clear();
    sys.dispatch_action_for_test(w, action(bms::ActionType::VaporizeGroup, 0, 0));
    CHECK(w.registry.get(local) != nullptr);
    CHECK(removals().empty());
    sys.dispatch_action_for_test(w, action(bms::ActionType::VaporizeGroup, 0, 4));
    CHECK(w.registry.get(g0) == nullptr);
    CHECK(w.registry.get(g1) == nullptr);
    CHECK(w.registry.get(g2) == nullptr);
    CHECK(w.registry.get(g3) == nullptr);
    CHECK((removals() == std::vector<uint16_t>{g2.packed, g0.packed, g1.packed, g3.packed}));
    CHECK(w.ai.for_handle(watcher)->brain.f[world::AiBrain::kPriorityTarget] == 0);
    CHECK(w.script.relations.group(4).live_count == live_before);
}

// AreaAiRed/Blue: the load-time resolver overwrites the zone id with the zone
// box in the record's order (p1 = x_min, p3 = y_min, p4 = x_max, reserved1 =
// y_max), and the dispatch walks pool 0 rows of the team with X in [p1, p3]
// and Y in [p4, reserved1], no Z test, handing p2/p3/p4 to the command. The
// Unicorn probe f saw the resolved record [.., 12, 22, x0, 11, y0, x1, y1] and
// the command reach rows outside the authored zone.
// [orig: EventTrigger_ResolveZoneActionRefs @0x453100 — @0x453183..0x4531ab;
//  Entity_KillTeamInBounds @0x43d030 — sub 0 @0x43d05b, team @0x43d099, X
//  @0x43d0a8..0x43d0b5, Y @0x43d0b7..0x43d0c4]
static void test_area_ai_reads_the_resolved_record() {
    World w;
    w.cached.humans = 1;
    configure_pools(w);
    world::Aabb zone{};
    zone.min = {100.0f, 300.5f, -16384.0f};
    zone.max = {200.0f, 400.0f, 16384.0f};
    w.registry.register_area(std::string(), zone, true, 11);
    const auto place = [&](int pool, uint16_t ssn, uint8_t team, float x, float y, float z) {
        const world::EntityHandle h = spawn_row(w, pool, ssn, 0, team);
        w.registry.get(h)->position = {x, y, z};
        return h;
    };
    const world::EntityHandle inside = place(0, 501, 2, 150.0f, 350.0f, 9000.0f);
    const world::EntityHandle past_x = place(0, 502, 2, 250.0f, 350.0f, 0.0f);
    const world::EntityHandle below_y = place(0, 503, 2, 150.0f, 250.0f, 0.0f);
    const world::EntityHandle outside = place(0, 504, 2, 150.0f, 450.0f, 0.0f);
    const world::EntityHandle blue = place(0, 505, 1, 150.0f, 350.0f, 0.0f);
    const world::EntityHandle pool1 = place(1, 506, 2, 150.0f, 350.0f, 0.0f);
    const world::EntityHandle brained = place(0, 507, 2, 150.0f, 350.0f, 0.0f);
    w.ai.attach(brained);

    bms::Event e{};
    e.action_count = 2;
    mission::BmsEventSystem sys;
    sys.load({e}, {},
             {action(bms::ActionType::AreaAiRed, 43, 11, 1, 7, 9),
              action(bms::ActionType::AreaAiRed, 42, 11, 3, 7, 9)});
    w.add_system(&sys);
    w.load_systems();

    const bms::Action &resolved = sys.events()[0].actions[0];
    CHECK(resolved.action_type == bms::ActionType::AreaAiRed);
    CHECK(resolved.param1 == 100 * 65536);
    CHECK(resolved.param2 == 1);
    CHECK(resolved.param3 == 300 * 65536 + 32768);
    CHECK(resolved.param4 == 200 * 65536);
    CHECK(resolved.reserved1 == 400 * 65536);

    tick_n(w, 16); // the event's first processing pass
    CHECK(sys.event_fired(0));
    CHECK(indestructible(w, inside));
    CHECK(indestructible(w, past_x));
    CHECK(indestructible(w, below_y));
    CHECK(!indestructible(w, outside));
    CHECK(!indestructible(w, blue));
    CHECK(!indestructible(w, pool1));
    // ENGAGEDISTANCE min/max = p2 << 16 / p3 << 16, and p3 is y_min.
    const world::AiEntity *ae = w.ai.for_handle(brained);
    CHECK(ae->slot.f[world::AiSlot::kEngageMin] == (3 << 16));
    CHECK(ae->slot.f[world::AiSlot::kSightRange] ==
          static_cast<int32_t>(static_cast<uint32_t>(300 * 65536 + 32768) << 16));
}

// The group team/regroup writers return on group 0; ChangeSingleAI returns on
// sub 0 or SSN 0 and walks pools 0, 1, 2 only (first match).
// [orig: Entity_SetTeamByNetId @0x43c680 — group 0 @0x43c685;
//  Entity_UpdateNetIdReferences @0x43c5b0 — @0x43c5b5;
//  Entity_HandleAlertStateEvent @0x43dee0 — sub 0 @0x43deef, SSN 0 @0x43defc,
//  pools 0/1/2 @0x43df20/@0x43df41/@0x43df69]
static void test_group_zero_and_single_ai_guards() {
    World w;
    w.cached.humans = 1;
    configure_pools(w);
    const world::EntityHandle ungrouped = spawn_row(w, 0, 601, 0, 1);
    const world::EntityHandle local = spawn_row(w, 0, 0, 3);
    const world::EntityHandle marker = spawn_row(w, 3, 602, 0);
    const world::EntityHandle brained = spawn_row(w, 1, 603, 5);
    w.ai.attach(brained);

    CHECK(w.commands.set_group_team(0, 2) == 0);
    CHECK(w.registry.get(ungrouped)->team == 1);
    CHECK(w.commands.change_group(0, 7) == 0);
    CHECK(w.registry.get(ungrouped)->group_id == 0);

    // Neither writer tests the item word: a grouped row without an item def
    // takes the team and the regroup like any other.
    // [orig: Entity_SetTeamByNetId @0x43c6ad..0x43c6ba;
    //  Entity_UpdateNetIdReferences @0x43c61b..0x43c631]
    world::Entity bare{};
    bare.net_id = 604;
    bare.group_id = 4;
    const world::EntityHandle itemless = w.registry.spawn(0, bare);
    CHECK(w.registry.get(itemless)->item_id == 0);
    CHECK(w.commands.set_group_team(4, 2) == 1);
    CHECK(w.registry.get(itemless)->team == 2);
    CHECK(w.commands.change_group(4, 6) == 1);
    CHECK(w.registry.get(itemless)->group_id == 6);

    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();
    sys.dispatch_action_for_test(w, action(bms::ActionType::ChangeSingleAI, 43, 0, 1));
    CHECK(!indestructible(w, local));
    sys.dispatch_action_for_test(w, action(bms::ActionType::ChangeSingleAI, 43, 602, 1));
    CHECK(!indestructible(w, marker));
    sys.dispatch_action_for_test(w, action(bms::ActionType::ChangeSingleAI, 43, 603, 1));
    CHECK(indestructible(w, brained));
}

// MisvarChange is the dword add/sub, so it wraps; the subgoal masks shift by
// the count's low 5 bits. [orig: EventAction_Dispatch case 5 subs 2/4
// @0x4543b1/@0x4543eb; case 14 shl @0x454508]
static void test_misvar_wraps_and_subgoal_shift_masks() {
    World w;
    w.cached.humans = 1;
    configure_pools(w);
    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();
    w.script.vars.set_mission(3, INT32_MAX);
    sys.dispatch_action_for_test(w, action(bms::ActionType::MisvarChange,
            static_cast<int32_t>(bms::MissionVariableActionSubType::Add), 3, 1));
    CHECK(w.script.vars.get_mission(3) == INT32_MIN);
    sys.dispatch_action_for_test(w, action(bms::ActionType::MisvarChange,
            static_cast<int32_t>(bms::MissionVariableActionSubType::Decrement), 3));
    CHECK(w.script.vars.get_mission(3) == INT32_MAX);
    sys.dispatch_action_for_test(w, action(bms::ActionType::SubGoalWon, 0, 33));
    CHECK(w.script.subgoals.won == (1u << 1));
}

// PlayWavList plays only on a client, and after the round is over only when
// param2 == 1 forces it. [orig: EventAction_Dispatch case 7 @0x45443d —
// @0x45444a, @0x454450]
static void test_play_wav_list_gate() {
    World w;
    w.cached.humans = 1;
    configure_pools(w);
    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();
    sys.dispatch_action_for_test(w, action(bms::ActionType::PlayWavList, 0, 7, 0));
    CHECK(w.out.effects.count("dialog") == 1);
    w.process_round_end(1);
    sys.dispatch_action_for_test(w, action(bms::ActionType::PlayWavList, 0, 8, 0));
    CHECK(w.out.effects.count("dialog") == 1);
    sys.dispatch_action_for_test(w, action(bms::ActionType::PlayWavList, 0, 9, 1));
    CHECK(w.out.effects.count("dialog") == 2);
    w.rules.mp_session_peer = false;
    sys.dispatch_action_for_test(w, action(bms::ActionType::PlayWavList, 0, 10, 1));
    CHECK(w.out.effects.count("dialog") == 2);
}

// Action 28: sub 37 hands the HUD item flash (timer, value) to the HUD, sub 38
// clears the input word, sub 39's store has no reader, and every other sub is
// a no-op; none records a gap. [orig: EventAction_HandleSpecialTypes
// @0x4535a0 — @0x4535cd, @0x4535c2, @0x4535b6..0x4535bc, @0x4535b4]
static void test_special_subtypes() {
    World w;
    w.cached.humans = 1;
    configure_pools(w);
    mission::BmsEventSystem sys;
    sys.load({}, {}, {});
    w.add_system(&sys);
    w.load_systems();
    w.script.input_action_bits = 0xFFFFFFFFu;
    sys.dispatch_action_for_test(w, action(bms::ActionType::SpecialSubType, 37, 3, 5));
    CHECK(w.out.effects.count("hud_item_flash") == 1);
    sys.dispatch_action_for_test(w, action(bms::ActionType::SpecialSubType, 39, 0));
    sys.dispatch_action_for_test(w, action(bms::ActionType::SpecialSubType, 40, 1, 1));
    CHECK(w.script.input_action_bits == 0xFFFFFFFFu);
    sys.dispatch_action_for_test(w, action(bms::ActionType::SpecialSubType, 38, 0));
    CHECK(w.script.input_action_bits == 0);
    CHECK(w.out.effects.count("unported_action") == 0);
    CHECK(w.diagnostics.empty());
}

int main() {
    test_single_alive_reads_the_bms_ref_rows();
    test_01tr_event34_chain_fires_when_the_placed_item_dies();
    test_change_group_ai_fans_pools_2_0_1();
    test_vaporize_notifies_and_destroys();
    test_area_ai_reads_the_resolved_record();
    test_group_zero_and_single_ai_guards();
    test_misvar_wraps_and_subgoal_shift_masks();
    test_play_wav_list_gate();
    test_special_subtypes();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("bms_event_parity: all passed\n");
    return 0;
}
