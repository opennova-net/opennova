// WAC behavior tests: compile + run small scripts on a fake world across ticks
// and assert the observable effects (var math, entity mutation, temporal firing,
// edge semantics, environment, RNG determinism).
#include <cstdio>
#include <string>

#include "wac/compiler.h"
#include "wac/wac_system.h"
#include "world/world.h"

using namespace opennova::wac;
using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static World make_world() {
    World w;
    w.registry.configure_pool(0, 64);
    return w;
}

// Run a program for `executions` VM executions. The VM self-gates to every 62nd
// logic tick [orig: sub_4F81A0 @0x4f81b1], so one execution = 62 ticks; WAC time
// units (past/elapse/Ticks) count executions, so the tests below keep reading in
// "script steps".
static void run(World &w, WacSystem &sys, int executions) {
    const int ticks = executions * WacSystem::kTicksPerExecution;
    for (int i = 0; i < ticks; ++i) w.run_logic_tick(/*is_authority=*/true);
}

// The 62-tick divider itself: nothing executes before the 62nd tick.
static void test_execution_cadence() {
    World w = make_world();
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

static void test_var_math() {
    World w = make_world();
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
    World w = make_world();
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
    World w = make_world();
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
    World w = make_world();
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
    World w = make_world();
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
    World w = make_world();
    WacSystem sys;
    CompileEnv env;
    // paren-less args + an unimplemented command recorded as an effect.
    sys.set_program(compile_source(
        "if never then flash() endif\n", env));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 1);
    CHECK(w.effects.count("flash") == 1);
}

// WAC scripted voice: wave/pwave route to a "dialog_wav" effect carrying the
// filename so the host can play it [orig: wave/pwave @ 0x4ED610]. Without the
// explicit handler they fall through to the default case as an unrouted "wave".
static void test_wac_wave_emits_dialog_wav() {
    World w = make_world();
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
    World w = make_world();
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
    World w = make_world();
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
    World w = make_world();
    WacSystem sys;
    CompileEnv env;
    Program p = compile_source("if true(greenkills) then lose(0) endif\n", env);
    CHECK(p.ok());
    sys.set_program(std::move(p));
    w.add_system(&sys);
    w.load_systems();

    run(w, sys, 1);
    CHECK(!w.round_end.ended); // no green kills yet
    CHECK(w.effects.count("lose") == 0);

    w.kill_stats.greenkills_by_player = 1;
    run(w, sys, 1);
    CHECK(w.round_end.ended);
    CHECK(w.round_end.winner_team == 2);
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
    World w = make_world();
    WacSystem sys;
    CompileEnv env;
    sys.set_program(compile_source("if past(1) then lose(2) endif\n", env));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 3);
    CHECK(!w.round_end.ended);
    CHECK(w.effects.count("lose") == 0);
    CHECK(w.effects.count("round_end") == 0);
}

// win(team) ends the round straight through, and the outcome builtins
// (GameOver/WinVar/LoseVar/humans) read the witnessed derivations.
// [orig: WacAction_Win @0x4ed4a0; the cache derivation @0x4f57bb/c9/cf]
static void test_win_and_outcome_builtins() {
    World w = make_world();
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
    CHECK(!w.round_end.ended);
    CHECK(w.vars.get_mission(1) == 0); // GameOver stays 0 pre-round-end
    CHECK(w.vars.get_mission(4) == 1); // humans visible from the first execution

    run(w, sys, 3); // past(2) fires -> win(1); the builtins read it the same pass
    CHECK(w.round_end.ended);
    CHECK(w.round_end.winner_team == 1);
    CHECK(w.vars.get_mission(1) == 1); // GameOver
    CHECK(w.vars.get_mission(2) == 1); // WinVar
    CHECK(w.vars.get_mission(3) == 0); // LoseVar stays 0 on a win
}

// 04TR.WAC's outcome block, verbatim (JOX corpus): greenkills -> Lose(0).
static void test_04tr_outcome_block_greenkills() {
    World w = make_world();
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
    CHECK(w.round_end.ended);
    CHECK(w.round_end.winner_team == 2);
    bool green_banner = false;
    for (const Effect &e : w.effects.entries())
        green_banner |= e.kind == "lose" && e.a == 0 && e.str == "STRMISC_KILLEDGREEN";
    CHECK(green_banner);
}

// The same block with BOTH counters set: the bluekills branch wins the else-if chain.
static void test_04tr_outcome_block_blue_priority() {
    World w = make_world();
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
    CHECK(w.round_end.ended);
    bool blue_banner = false;
    for (const Effect &e : w.effects.entries())
        blue_banner |= e.kind == "lose" && e.a == 1 && e.str == "STRMISC_KILLEDBLUE";
    CHECK(blue_banner);
    CHECK(w.effects.count("lose") == 1); // the green branch never also fires
}

int main() {
    test_execution_cadence();
    test_var_math();
    test_ssn_kill();
    test_temporal_past();
    test_else_branch();
    test_environment();
    test_paren_less_and_effects();
    test_wac_wave_emits_dialog_wav();
    test_wac_text_and_console_use_distinct_effect_channels();
    test_authority_gate();
    test_lose_ends_round_with_banner_key();
    test_lose_other_team_noop();
    test_win_and_outcome_builtins();
    test_04tr_outcome_block_greenkills();
    test_04tr_outcome_block_blue_priority();
    std::printf(failures ? "BEHAVIOR TESTS FAILED (%d)\n" : "behavior tests passed\n", failures);
    return failures ? 1 : 0;
}
