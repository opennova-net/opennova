// WAC behavior tests: compile + run small scripts on a fake world across ticks
// and assert the observable effects (var math, entity mutation, temporal firing,
// edge semantics, environment, RNG determinism).
#include <cstdio>
#include <string>

#include <runtime/wac/compiler.h>
#include <runtime/wac/wac_system.h>
#include <runtime/world/ai.h>
#include <runtime/world/world.h>

using namespace opennova::wac;
using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

struct BehaviorWorld final : World {
    BehaviorWorld() { registry.configure_pool(0, 64); }
};

// Run a program for `executions` VM executions. The VM self-gates to every 62nd
// logic tick [orig: WacScript_AdvanceTick @0x4f81b1], so one execution = 62 ticks; WAC time
// units (past/elapse/Ticks) count executions, so the tests below keep reading in
// "script steps".
static void run(World &w, WacSystem &sys, int executions) {
    const int ticks = executions * WacSystem::kTicksPerExecution;
    for (int i = 0; i < ticks; ++i) w.run_logic_tick(/*is_authority=*/true);
}

// The 62-tick divider itself: nothing executes before the 62nd tick.
static void test_execution_cadence() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    sys.set_program(compile_source("if never() then set(v1,1) endif\n", env));
    w.add_system(&sys);
    w.load_systems();
    for (int i = 0; i < WacSystem::kTicksPerExecution - 1; ++i)
        w.run_logic_tick(/*is_authority=*/true);
    CHECK(w.vars.get_mission(1) == 0); // 61 ticks: not yet
    CHECK(sys.runs() == 0);
    w.run_logic_tick(/*is_authority=*/true);
    CHECK(w.vars.get_mission(1) == 1); // the 62nd tick executes the program
    CHECK(sys.runs() == 1);
}

// Mission startup executes the VM directly once, without consuming the normal
// 62-tick divider. Capturing/restoring that temporal state prevents initial
// edge predicates from firing a second time after Stop/Play.
static void test_initial_execution_and_runtime_state() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    sys.set_program(compile_source("if never() then inc(v1) endif\n", env));
    w.add_system(&sys);
    w.load_systems();

    CHECK(sys.execute_initial(w));
    CHECK(!sys.execute_initial(w));
    CHECK(w.vars.get_mission(1) == 1);
    CHECK(sys.runs() == 1);
    CHECK(sys.vm().time() == 1);
    const WacSystem::RuntimeState startup = sys.capture_runtime_state();

    for (int i = 0; i < WacSystem::kTicksPerExecution - 1; ++i)
        w.run_logic_tick(/*is_authority=*/true);
    CHECK(sys.runs() == 1);
    w.run_logic_tick(/*is_authority=*/true);
    CHECK(sys.runs() == 2);
    CHECK(w.vars.get_mission(1) == 1); // the initial edge remains active

    w.load_systems();
    CHECK(sys.runs() == 0);
    sys.restore_runtime_state(startup);
    CHECK(sys.runs() == 1);
    CHECK(sys.vm().time() == 1);
    for (int i = 0; i < WacSystem::kTicksPerExecution; ++i)
        w.run_logic_tick(/*is_authority=*/true);
    CHECK(sys.runs() == 2);
    CHECK(w.vars.get_mission(1) == 1);
}

static void test_var_math() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    Program p = compile_source(
        "if never() then set(v1,5) endif\n"
        "if eq(v1,5) then inc(v2) endif\n",
        env);
    CHECK(p.ok());
    sys.set_program(std::move(p));
    w.add_system(&sys);
    w.load_systems();

    run(w, sys, 1);
    CHECK(w.vars.get_mission(1) == 5);
    CHECK(w.vars.get_mission(2) == 1); // inc fires once on the rising edge

    run(w, sys, 5);
    CHECK(w.vars.get_mission(2) == 1); // edge semantics: does not re-fire while eq stays true
}

static void test_ssn_kill() {
    BehaviorWorld w;
    Entity a; a.net_id = 100; a.alive = true; w.registry.spawn(0, a);
    Entity b; b.net_id = 200; b.alive = true; w.registry.spawn(0, b);

    WacSystem sys;
    CompileEnv env;
    sys.set_program(compile_source("if SSNdead(100) then killSSN(200) endif\n", env));
    w.add_system(&sys);
    w.load_systems();

    run(w, sys, 1);
    CHECK(w.commands.ssn_alive(200)); // 100 still alive -> nothing happens

    w.commands.kill_ssn(100);
    run(w, sys, 1);
    CHECK(w.commands.ssn_dead(200)); // 100 dead -> killSSN(200) fired
}

static void test_temporal_past() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    sys.set_program(compile_source("if past(3) then set(v5,1) endif\n", env));
    w.add_system(&sys);
    w.load_systems();

    run(w, sys, 3); // logic_tick reaches 0,1,2 during execution -> not yet >= 3
    CHECK(w.vars.get_mission(5) == 0);
    run(w, sys, 2); // now logic_tick hits 3
    CHECK(w.vars.get_mission(5) == 1);
}

static void test_else_branch() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    // v1 starts 0 -> else branch sets v2=2; then set v1=1 -> then branch sets v2=1.
    sys.set_program(compile_source(
        "if true(v1) then set(v2,1) else set(v2,2) endif\n", env));
    w.add_system(&sys);
    w.load_systems();

    run(w, sys, 1);
    CHECK(w.vars.get_mission(2) == 2); // v1==0 -> else

    w.vars.set_mission(1, 1);
    run(w, sys, 1);
    CHECK(w.vars.get_mission(2) == 1); // v1!=0 -> then
}

static void test_environment() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    sys.set_program(compile_source(
        "if never() then fogtype(1) fogdist(225) endif\n", env));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 1);
    CHECK(w.env.fog_type == 1);
    CHECK(w.env.fog_dist == 225);
    CHECK(w.env.generation >= 2);
}

static void test_paren_less_and_effects() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    // paren-less args + an unimplemented command recorded as an effect.
    sys.set_program(compile_source(
        "if never then dropflare() endif\n", env));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 1);
    CHECK(w.effects.count("dropflare") == 1);
}

// `flash` is a weather handler now: it arms the short lightning sequencer
// on the World's weather home instead of surfacing as an effect record
// [orig: Env_TriggerLightningFlashA @ 0x4ed500].
static void test_flash_arms_the_weather_home() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    sys.set_program(compile_source(
        "if never then flash() endif\n", env));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 1);
    CHECK(w.effects.count("flash") == 0);
    CHECK(w.weather.core.lightning.timer_a == 16);
}

// WAC scripted voice: wave/pwave route to a "dialog_wav" effect carrying the
// filename so the host can play it [orig: wave/pwave @ 0x4ED610]. Without the
// explicit handler they fall through to the default case as an unrouted "wave".
static void test_wac_wave_emits_dialog_wav() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    sys.set_program(compile_source("if never then wave(brief1) endif\n", env));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 1);
    CHECK(w.effects.count("dialog_wav") == 1);
    CHECK(w.effects.count("wave") == 0); // not the unrouted default-case kind
    bool carried_filename = false;
    for (const Effect &e : w.effects.entries())
        if (e.kind == "dialog_wav" && e.str == "brief1") carried_filename = true;
    CHECK(carried_filename);
}

// Mission text and the on-screen debug console are separate retail channels:
// text/text# (and their peer-broadcast ptext twin) feed the player message
// presentation, while consol/consol# and pconsol feed Chat_AddDebugMessage and
// must remain distinguishable for embedders that deliberately do not present them.
static void test_wac_text_and_console_use_distinct_effect_channels() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    Program p = compile_source(
        "if never then "
        "text(local_text) ptext(peer_text) text#(numbered_text,7) "
        "consol(local_debug) pconsol(peer_debug) consol#(numbered_debug,9) "
        "endif\n",
        env);
    CHECK(p.ok());
    sys.set_program(std::move(p));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 1);

    CHECK(w.effects.count("text") == 3);
    CHECK(w.effects.count("debug_text") == 3);
    bool saw_local_text = false;
    bool saw_peer_text = false;
    bool saw_numbered_text = false;
    bool saw_local_debug = false;
    bool saw_peer_debug = false;
    bool saw_numbered_debug = false;
    for (const Effect &e : w.effects.entries()) {
        saw_local_text |= e.kind == "text" && e.str == "local_text" && e.a == 0;
        saw_peer_text |= e.kind == "text" && e.str == "peer_text" && e.a == 0;
        saw_numbered_text |= e.kind == "text" && e.str == "numbered_text" && e.a == 7;
        saw_local_debug |= e.kind == "debug_text" && e.str == "local_debug" && e.a == 0;
        saw_peer_debug |= e.kind == "debug_text" && e.str == "peer_debug" && e.a == 0;
        saw_numbered_debug |= e.kind == "debug_text" && e.str == "numbered_debug" && e.a == 9;
    }
    CHECK(saw_local_text);
    CHECK(saw_peer_text);
    CHECK(saw_numbered_text);
    CHECK(saw_local_debug);
    CHECK(saw_peer_debug);
    CHECK(saw_numbered_debug);
}

static void test_authority_gate() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    sys.set_program(compile_source("if never() then set(v9,1) endif\n", env));
    w.add_system(&sys);
    w.load_systems();
    // 62 non-authority ticks: the divider never advances, scripting skipped.
    for (int i = 0; i < WacSystem::kTicksPerExecution; ++i)
        w.run_logic_tick(/*is_authority=*/false);
    CHECK(w.vars.get_mission(9) == 0);
    run(w, sys, 1); // one authoritative execution
    CHECK(w.vars.get_mission(9) == 1);
}

// ---- round outcome (world-wac-ai-re §20) ----

// lose(0) resolves the KILLEDGREEN banner key and ends the round with winner 2
// (red wins = the player side loses); the world latch never double-fires.
// [orig: WacAction_Lose @0x4ed3f0; Server_ProcessRoundEnd @0x5164f0]
static void test_lose_ends_round_with_banner_key() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    Program p = compile_source("if true(greenkills) then lose(0) endif\n", env);
    CHECK(p.ok());
    sys.set_program(std::move(p));
    w.add_system(&sys);
    w.load_systems();

    run(w, sys, 1);
    CHECK(!w.match.outcome().ended); // no green kills yet
    CHECK(w.effects.count("lose") == 0);

    w.kill_stats.greenkills_by_player = 1;
    run(w, sys, 1);
    CHECK(w.match.outcome().ended);
    CHECK(w.match.outcome().winner_team == 2);
    CHECK(w.effects.count("lose") == 1);
    CHECK(w.effects.count("round_end") == 1);
    for (const Effect &e : w.effects.entries()) {
        if (e.kind == "lose") { CHECK(e.a == 0); CHECK(e.str == "STRMISC_KILLEDGREEN"); }
        if (e.kind == "round_end") CHECK(e.a == 2);
    }

    run(w, sys, 2); // the latch: no second round_end even while the condition holds
    CHECK(w.effects.count("round_end") == 1);
}

// Lose(n) for n outside {0,1} is a witnessed NO-OP [orig: WacAction_Lose returns 0
// without touching the round @0x4ed45b..].
static void test_lose_other_team_noop() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    sys.set_program(compile_source("if past(1) then lose(2) endif\n", env));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 3);
    CHECK(!w.match.outcome().ended);
    CHECK(w.effects.count("lose") == 0);
    CHECK(w.effects.count("round_end") == 0);
}

// win(team) ends the round straight through, and the outcome builtins
// (GameOver/WinVar/LoseVar/humans) read the witnessed derivations.
// [orig: WacAction_Win @0x4ed4a0; the cache derivation @0x4f57bb/c9/cf]
static void test_win_and_outcome_builtins() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    Program p = compile_source(
        "if past(2) then win(1) endif\n"
        "if true(GameOver) then set(v1,1) endif\n"
        "if true(WinVar) then set(v2,1) endif\n"
        "if true(LoseVar) then set(v3,1) endif\n"
        "if true(humans) then set(v4,1) endif\n",
        env);
    CHECK(p.ok());
    sys.set_program(std::move(p));
    w.add_system(&sys);
    w.load_systems();
    w.cached.humans = 1;

    run(w, sys, 1);
    CHECK(!w.match.outcome().ended);
    CHECK(w.vars.get_mission(1) == 0); // GameOver stays 0 pre-round-end
    CHECK(w.vars.get_mission(4) == 1); // humans visible from the first execution

    run(w, sys, 3); // past(2) fires -> win(1); the builtins read it the same pass
    CHECK(w.match.outcome().ended);
    CHECK(w.match.outcome().winner_team == 1);
    CHECK(w.vars.get_mission(1) == 1); // GameOver
    CHECK(w.vars.get_mission(2) == 1); // WinVar
    CHECK(w.vars.get_mission(3) == 0); // LoseVar stays 0 on a win
}

// 04TR.WAC's outcome block, verbatim (JOX corpus): greenkills -> Lose(0).
static void test_04tr_outcome_block_greenkills() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    Program p = compile_source(
        "If true(bluekills) then\n"
        "\t\tLose (1)\n"
        "Else if true(greenkills) then\n"
        "\t\tLose (0)\n"
        "endif\n",
        env);
    CHECK(p.ok());
    sys.set_program(std::move(p));
    w.add_system(&sys);
    w.load_systems();
    w.kill_stats.greenkills_by_player = 1;
    run(w, sys, 1);
    CHECK(w.match.outcome().ended);
    CHECK(w.match.outcome().winner_team == 2);
    bool green_banner = false;
    for (const Effect &e : w.effects.entries())
        green_banner |= e.kind == "lose" && e.a == 0 && e.str == "STRMISC_KILLEDGREEN";
    CHECK(green_banner);
}

// The same block with BOTH counters set: the bluekills branch wins the else-if chain.
static void test_04tr_outcome_block_blue_priority() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    sys.set_program(compile_source(
        "If true(bluekills) then\n"
        "\t\tLose (1)\n"
        "Else if true(greenkills) then\n"
        "\t\tLose (0)\n"
        "endif\n",
        env));
    w.add_system(&sys);
    w.load_systems();
    w.kill_stats.bluekills_by_player = 1;
    w.kill_stats.greenkills_by_player = 1;
    run(w, sys, 1);
    CHECK(w.match.outcome().ended);
    bool blue_banner = false;
    for (const Effect &e : w.effects.entries())
        blue_banner |= e.kind == "lose" && e.a == 1 && e.str == "STRMISC_KILLEDBLUE";
    CHECK(blue_banner);
    CHECK(w.effects.count("lose") == 1); // the green branch never also fires
}

static void test_wac_spatial_wounded_and_mount_predicates() {
    BehaviorWorld w;

    Entity source{};
    source.net_id = 100;
    source.item_id = 1001;
    source.alive = true;
    source.health = 40;
    source.health_max = 100;
    source.position = {0.0f, 0.0f, 0.0f};
    source.yaw = 90; // mission 90 = engine heading 0 = +X
    w.registry.spawn(0, source);

    Entity target = source;
    target.net_id = 200;
    target.health = 100;
    target.position = {10.0f, 0.0f, 0.0f};
    w.registry.spawn(0, target);

    Entity mount = source;
    mount.net_id = 300;
    mount.position = {20.0f, 0.0f, 0.0f};
    const EntityHandle mount_h = w.registry.spawn(0, mount);

    Entity local = source;
    local.net_id = 400;
    local.mounted = true;
    local.mount_target = mount_h;
    local.mount_type = SeatType::Driver;
    const EntityHandle local_h = w.registry.spawn(0, local);
    w.cached.local_player = local_h;

    WacSystem sys;
    CompileEnv env;
    Program program = compile_source(
            "if SSNwounded(100) then set(v1,1) endif\n"
            "if SSNnearSSN(100,200,10) then set(v2,1) endif\n"
            "if SSNlosSSN(100,200,10) then set(v3,1) endif\n"
            "if SSNseesSSN(100,200,10) then set(v4,1) endif\n"
            "if meattached(300) then set(v5,1) endif\n"
            "if medrive(300) then set(v6,1) endif\n"
            "if meongun(300) then set(v7,1) endif\n",
            env);
    CHECK(program.ok());
    sys.set_program(std::move(program));
    w.add_system(&sys);
    w.load_systems();

    run(w, sys, 1);
    CHECK(w.vars.get_mission(1) == 1);
    CHECK(w.vars.get_mission(2) == 1); // inclusive distance boundary
    CHECK(w.vars.get_mission(3) == 1);
    CHECK(w.vars.get_mission(4) == 1);
    CHECK(w.vars.get_mission(5) == 1);
    CHECK(w.vars.get_mission(6) == 1);
    CHECK(w.vars.get_mission(7) == 0);

    w.registry.get(local_h)->mount_type = SeatType::Gunner;
    run(w, sys, 1);
    CHECK(w.vars.get_mission(7) == 1);
}

static void test_wac_accuracy_guard_speed_and_group_remove() {
    BehaviorWorld w;
    AiSystem ai;
    w.ai = &ai;

    Entity single{};
    single.net_id = 42;
    single.item_id = 1001;
    single.group_id = 4;
    single.alive = true;
    const EntityHandle single_h = w.registry.spawn(0, single);

    Entity group_member = single;
    group_member.net_id = 43;
    group_member.group_id = 3;
    const EntityHandle group_h = w.registry.spawn(0, group_member);

    Entity removable = single;
    removable.net_id = 44;
    removable.group_id = 9;
    w.registry.spawn(0, removable);

    ai.attach(single_h);
    ai.attach(group_h);
    AiEntity &single_ai = *ai.for_handle(single_h);
    AiEntity &group_ai = *ai.for_handle(group_h);
    single_ai.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
    single_ai.brain.f[AiBrain::kPendState] = kAiGroundFollowWp;
    group_ai.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
    group_ai.brain.f[AiBrain::kPendState] = kAiGroundFollowWp;

    WacSystem sys;
    CompileEnv env;
    Program program = compile_source(
            "if never() then "
            "setaccuracy(42,70,80) "
            "Gsetaccuracy(3,60,50) "
            "ssnguard(42,1) "
            "ssncspd(42,36) "
            "ssnpspd(42,18) "
            "Gremove(9) "
            "endif\n",
            env);
    CHECK(program.ok());
    sys.set_program(std::move(program));
    w.add_system(&sys);
    w.load_systems();

    run(w, sys, 1);
    CHECK(single_ai.slot.f[AiSlot::kAimErrorSecondary] == 30);
    CHECK(single_ai.slot.f[AiSlot::kAimErrorPrimary] == 20);
    CHECK(group_ai.slot.f[AiSlot::kAimErrorSecondary] == 40);
    CHECK(group_ai.slot.f[AiSlot::kAimErrorPrimary] == 50);
    CHECK((w.registry.get(single_h)->flags & kEntityFlagMounted) != 0);
    CHECK((w.registry.get(single_h)->engine_flags & kEntityFlagMounted) != 0);
    CHECK(!w.commands.ssn_exists(44));
    CHECK(single_ai.brain.f[AiBrain::kSpeedA] == 0);
    CHECK(single_ai.brain.f[AiBrain::kSpeedB] == 0);
    CHECK(ai.events.count() == 2);

    ai.events.process_timed(ai, w);
    CHECK(single_ai.brain.f[AiBrain::kSpeedA] == 10485);
    CHECK(single_ai.brain.f[AiBrain::kSpeedB] == 5242);
}

int main() {
    test_execution_cadence();
	test_initial_execution_and_runtime_state();
    test_var_math();
    test_ssn_kill();
    test_temporal_past();
    test_else_branch();
    test_environment();
    test_paren_less_and_effects();
    test_flash_arms_the_weather_home();
    test_wac_wave_emits_dialog_wav();
    test_wac_text_and_console_use_distinct_effect_channels();
    test_authority_gate();
    test_lose_ends_round_with_banner_key();
    test_lose_other_team_noop();
    test_win_and_outcome_builtins();
    test_04tr_outcome_block_greenkills();
    test_wac_spatial_wounded_and_mount_predicates();
    test_wac_accuracy_guard_speed_and_group_remove();
    test_04tr_outcome_block_blue_priority();
    std::printf(failures ? "BEHAVIOR TESTS FAILED (%d)\n" : "behavior tests passed\n", failures);
    return failures ? 1 : 0;
}
