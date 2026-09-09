// WAC behavior tests: compile + run small scripts on a fake world across ticks
// and assert the observable effects (var math, entity mutation, temporal firing,
// edge semantics, environment, RNG determinism).
#include <cstdio>
#include <string>

#include <runtime/wac/compiler.h>
#include <runtime/mission/event_runtime.h>
#include <formats/wac/bytecode.h>
#include <formats/wac/command.h>
#include <runtime/wac/wac_system.h>
#include <runtime/world/ai.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/world/world.h>

using namespace opennova::wac;
using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

struct BehaviorWorld final : World {
    BehaviorWorld() { registry.configure_pool(0, 64); cached.humans = 1; }
};

// Run a program for `executions` VM executions. The VM self-gates to every 62nd
// logic tick [orig: WacScript_AdvanceTick @0x4f81b1], so one execution = 62 ticks; WAC time
// units (past/elapse/Ticks) count executions, so the tests below keep reading in
// "script steps".
static void run(World &w, WacSystem &sys, int executions) {
    w.cached.humans = 1; // this fixture models a human playing, including after restore
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
    CHECK(w.script.vars.get_mission(1) == 0); // 61 ticks: not yet
    CHECK(sys.runs() == 0);
    w.run_logic_tick(/*is_authority=*/true);
    CHECK(w.script.vars.get_mission(1) == 1); // the 62nd tick executes the program
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
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK(sys.runs() == 1);
    CHECK(sys.vm().time() == 1);
    const WacSystem::RuntimeState startup = sys.capture_runtime_state();

    for (int i = 0; i < WacSystem::kTicksPerExecution - 1; ++i)
        w.run_logic_tick(/*is_authority=*/true);
    CHECK(sys.runs() == 1);
    w.run_logic_tick(/*is_authority=*/true);
    CHECK(sys.runs() == 2);
    CHECK(w.script.vars.get_mission(1) == 1); // the initial edge remains active

    w.load_systems();
    CHECK(sys.runs() == 0);
    sys.restore_runtime_state(startup);
    CHECK(sys.runs() == 1);
    CHECK(sys.vm().time() == 1);
    for (int i = 0; i < WacSystem::kTicksPerExecution; ++i)
        w.run_logic_tick(/*is_authority=*/true);
    CHECK(sys.runs() == 2);
    CHECK(w.script.vars.get_mission(1) == 1);
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
    CHECK(w.script.vars.get_mission(1) == 5);
    CHECK(w.script.vars.get_mission(2) == 1); // first true THEN evaluation

    run(w, sys, 5);
    CHECK(w.script.vars.get_mission(2) == 6); // THEN repeats while true
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
    CHECK(w.script.vars.get_mission(5) == 0);
    run(w, sys, 2); // now logic_tick hits 3
    CHECK(w.script.vars.get_mission(5) == 1);
}

static void test_temporal_predecessors_and_intervals() {
    BehaviorWorld w;
    WacSystem sys;
    sys.set_program(compile_source(
        "if ontick(2) then if false(1) then inc(v9) endif endif\n"
        "if previous then inc(v1) endif\n"
        "if chain(2) then inc(v2) endif\n"
        "if before(3) then inc(v3) endif\n"
        "if elapse(3) then inc(v4) endif\n", {}));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 2); // ticks 0 and 1
    CHECK(w.script.vars.get_mission(1) == 0);
    CHECK(w.script.vars.get_mission(3) == 2);
    CHECK(w.script.vars.get_mission(4) == 1); // first elapse fires immediately
    run(w, sys, 1); // tick 2: skip the never-fired nested predecessor
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK(w.script.vars.get_mission(2) == 0);
    run(w, sys, 1); // tick 3
    CHECK(w.script.vars.get_mission(4) == 2);
    CHECK(w.script.vars.get_mission(3) == 3); // before is strict
    run(w, sys, 1); // tick 4, two ticks after previous
    CHECK(w.script.vars.get_mission(2) == 1);
    run(w, sys, 4);
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK(w.script.vars.get_mission(2) == 1);
    CHECK(w.script.vars.get_mission(4) == 3);
}

static void test_then_enter_leave_and_else() {
    BehaviorWorld w;
    WacSystem sys;
    Program p = compile_source(
        "if true(v0) then inc(v1) else inc(v2) endif\n"
        "if true(v0) enter inc(v3) else inc(v4) endif\n"
        "if true(v0) leave inc(v5) else inc(v6) endif\n", {});
    CHECK(p.ok());
    sys.set_program(std::move(p));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 2);
    CHECK(w.script.vars.get_mission(2) == 2);
    CHECK(w.script.vars.get_mission(5) == 0);
    w.script.vars.set_mission(0, 1);
    run(w, sys, 3);
    CHECK(w.script.vars.get_mission(1) == 3);
    CHECK(w.script.vars.get_mission(3) == 1);
    CHECK(w.script.vars.get_mission(5) == 0);
    w.script.vars.set_mission(0, 0);
    run(w, sys, 2);
    CHECK(w.script.vars.get_mission(5) == 1);
    CHECK(w.script.vars.get_mission(4) == 6); // ELSE runs whenever ENTER does not
    w.script.vars.set_mission(0, 1);
    run(w, sys, 1);
    CHECK(w.script.vars.get_mission(3) == 2);
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
    CHECK(w.script.vars.get_mission(2) == 2); // v1==0 -> else

    w.script.vars.set_mission(1, 1);
    run(w, sys, 1);
    CHECK(w.script.vars.get_mission(2) == 1); // v1!=0 -> then
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

static void test_paren_less_music_success() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    // Parenthesis-free calls preserve the dormant music stream's success result.
    sys.set_program(compile_source(
        "if never then music 1 store v1 endif\n", env));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 1);
    CHECK(w.out.effects.count("music") == 0);
    CHECK(w.script.vars.get_mission(1) == 1 && w.diagnostics.empty());
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
    CHECK(w.out.effects.count("flash") == 0);
    CHECK(w.weather.core.lightning.timer_a == 16);
}

// WAC scripted voice: wave/pwave route to a "dialog_wav" effect carrying the
// filename so the host can play it [orig: wave/pwave @ 0x4ED610]. Without the
// explicit handler they fall through to the default case as an unrouted "wave".
static void test_wac_wave_emits_dialog_wav() {
    BehaviorWorld w;
    Entity player;
    player.alive = true;
    w.cached.local_player = w.registry.spawn(0, player);
    WacSystem sys;
    CompileEnv env;
    sys.set_program(compile_source("if never then wave(brief1) endif\n", env));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 1);
    CHECK(w.out.effects.count("dialog_wav") == 1);
    CHECK(w.out.effects.count("wave") == 0); // not the unrouted default-case kind
    bool carried_filename = false;
    for (const Effect &e : w.out.effects.entries())
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

    CHECK(w.out.effects.count("text") == 3);
    CHECK(w.out.effects.count("debug_text") == 3);
    bool saw_local_text = false;
    bool saw_peer_text = false;
    bool saw_numbered_text = false;
    bool saw_local_debug = false;
    bool saw_peer_debug = false;
    bool saw_numbered_debug = false;
    for (const Effect &e : w.out.effects.entries()) {
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
    CHECK(w.script.vars.get_mission(9) == 0);
    run(w, sys, 1); // one authoritative execution
    CHECK(w.script.vars.get_mission(9) == 1);
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
    CHECK(w.out.effects.count("lose") == 0);

    w.kill_stats.greenkills_by_player = 1;
    run(w, sys, 1);
    CHECK(w.match.outcome().ended);
    CHECK(w.match.outcome().winner_team == 2);
    CHECK(w.out.effects.count("lose") == 1);
    CHECK(w.out.effects.count("round_end") == 1);
    for (const Effect &e : w.out.effects.entries()) {
        if (e.kind == "lose") { CHECK(e.a == 0); CHECK(e.str == "STRMISC_KILLEDGREEN"); }
        if (e.kind == "round_end") CHECK(e.a == 2);
    }

    run(w, sys, 2); // the latch: no second round_end even while the condition holds
    CHECK(w.out.effects.count("round_end") == 1);
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
    CHECK(w.out.effects.count("lose") == 0);
    CHECK(w.out.effects.count("round_end") == 0);
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
    CHECK(w.script.vars.get_mission(1) == 0); // GameOver stays 0 pre-round-end
    CHECK(w.script.vars.get_mission(4) == 1); // humans visible from the first execution

    run(w, sys, 3); // past(2) fires -> win(1); the next execution refreshes the cache
    CHECK(w.match.outcome().ended);
    CHECK(w.match.outcome().winner_team == 1);
    CHECK(w.script.vars.get_mission(1) == 1); // GameOver
    CHECK(w.script.vars.get_mission(2) == 1); // WinVar
    CHECK(w.script.vars.get_mission(3) == 0); // LoseVar stays 0 on a win
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
    for (const Effect &e : w.out.effects.entries())
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
    for (const Effect &e : w.out.effects.entries())
        blue_banner |= e.kind == "lose" && e.a == 1 && e.str == "STRMISC_KILLEDBLUE";
    CHECK(blue_banner);
    CHECK(w.out.effects.count("lose") == 1); // the green branch never also fires
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
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK(w.script.vars.get_mission(2) == 1); // inclusive distance boundary
    CHECK(w.script.vars.get_mission(3) == 1);
    CHECK(w.script.vars.get_mission(4) == 1);
    CHECK(w.script.vars.get_mission(5) == 1);
    CHECK(w.script.vars.get_mission(6) == 1);
    CHECK(w.script.vars.get_mission(7) == 0);

    w.registry.get(local_h)->mount_type = SeatType::Gunner;
    run(w, sys, 1);
    CHECK(w.script.vars.get_mission(7) == 1);
}

static void test_wac_accuracy_guard_speed_and_group_remove() {
    BehaviorWorld w;
    AiSystem &ai = w.ai;

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
    const EntityHandle remove_h = w.registry.spawn(0, removable);
    w.registry.set_script_group_members(w.registry.intern_group("remove_me"), {remove_h});

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
    env.registry = &w.registry;
    Program program = compile_source(
            "if never() then "
            "setaccuracy(42,70,80) "
            "Gsetaccuracy(3,60,50) "
            "ssnguard(42,1) "
            "ssncspd(42,36) "
            "ssnpspd(42,18) "
            "Gremove(G_remove_me) "
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

static void test_runtime_gaps_retain_source_and_restore_boot_evidence() {
    BehaviorWorld w;
    WacSystem sys;
    CompileEnv env;
    env.source_names = {"game.wac", "mission.wac"};
    Program program = compile_program({
        "if never then inc(v1) endif\n",
        "\nif never then inc(v2) endif\n"}, env);
    CHECK(program.ok());
    // Corrupt the two zero-argument condition calls after compilation. This
    // exercises missing dispatch without depending on an unfinished feature.
    for (const auto &site : program.instruction_sources) {
        if (instr_command_index(program.code[site.word]) == wac_command_index("never"))
            program.code[site.word] = encode_call(0xFFFF);
    }
    sys.set_program(std::move(program));
    w.add_system(&sys);
    w.load_systems();
    sys.execute_initial(w);
    CHECK(w.diagnostics.gaps().size() == 2);
    CHECK(w.diagnostics.total_calls() == 2);
    const auto baseline = w.snapshot();
    const auto vm_baseline = sys.capture_runtime_state();
    run(w, sys, 3);
    w.out.effects.clear();
    CHECK(w.diagnostics.total_calls() == 8);
    if (w.diagnostics.gaps().size() == 2) {
        const auto &first = w.diagnostics.gaps()[0];
        const auto &second = w.diagnostics.gaps()[1];
        CHECK(first.origin.source == "game.wac");
        CHECK(second.origin.source == "mission.wac");
        CHECK(second.origin.line == 2);
        CHECK(first.count == 4 && second.count == 4);
        CHECK(first.first_tick == 0);
        CHECK(first.last_tick > first.first_tick);
    }
    w.restore(baseline);
    sys.restore_runtime_state(vm_baseline);
    CHECK(w.diagnostics.total_calls() == 2);
    run(w, sys, 1);
    CHECK(w.diagnostics.total_calls() == 4);
    w.load_systems();
    CHECK(w.diagnostics.empty());
}

static void test_nested_conditions_and_accumulator_lifetime() {
    BehaviorWorld w;
    WacSystem sys;
    Program program = compile_source(
        "load(7) store(v1)\n"
        "if eq(result,7) then set(v2,1) endif\n"
        "if true(0) and (true(1) or true(1)) then inc(v3) endif\n"
        "if true(1) or (true(0) and true(0)) then inc(v4) endif\n"
        "if true(1) and not (true(0) or true(0)) then inc(v5) endif\n"
        "if true(0) or (true(1) and (true(0) or true(1))) then inc(v6) endif\n"
        "if true(1) then if true(1) then load(9) endif store(v7) endif\n"
        "set(v8,0) sub(v8,2) dec(v8) store(v9)\n"
        "set(v10,2147483647) inc(v10) store(v11)\n", {});
    CHECK(program.ok());
    CHECK(program.event_count == 7); // bare actions do not invent events
    sys.set_program(std::move(program));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 1);
    CHECK(w.script.vars.get_mission(1) == 7);
    CHECK(w.script.vars.get_mission(2) == 1);
    CHECK(w.script.vars.get_mission(3) == 0);
    CHECK(w.script.vars.get_mission(4) == 1);
    CHECK(w.script.vars.get_mission(5) == 0); // grouped NOT negates the saved byte
    CHECK(w.script.vars.get_mission(6) == 1);
    CHECK(w.script.vars.get_mission(7) == 9);
    CHECK(w.script.vars.get_mission(8) == -3);
    CHECK(w.script.vars.get_mission(9) == -3);
    CHECK(w.script.vars.get_mission(10) == INT32_MIN);
    CHECK(w.script.vars.get_mission(11) == INT32_MIN);
    CHECK(w.diagnostics.empty());
}

static void test_do_sections_cycle_and_restore_independently() {
    BehaviorWorld w;
    WacSystem sys;
    Program program = compile_source(
        "doseq\n"
        " inc(v1)\n"
        "next\n"
        " doseq inc(v2) next inc(v3) enddo\n"
        "next\n"
        " inc(v4)\n"
        "enddo\n"
        "dornd inc(v5) next inc(v6) enddo\n", {});
    CHECK(program.ok());
    CHECK(program.event_count == 0);
    CHECK(program.loop_count == 3);
    sys.set_program(std::move(program));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 2);
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK(w.script.vars.get_mission(2) == 1);
    CHECK(w.script.vars.get_mission(3) == 0);
    CHECK(w.script.vars.get_mission(4) == 0);
    // DORND intentionally cycles too: the retail compiler emits opcode 4.
    CHECK(w.script.vars.get_mission(5) == 1);
    CHECK(w.script.vars.get_mission(6) == 1);
    const auto baseline = w.snapshot();
    const auto vm_baseline = sys.capture_runtime_state();
    run(w, sys, 4);
    CHECK(w.script.vars.get_mission(1) == 2);
    CHECK(w.script.vars.get_mission(2) == 1);
    CHECK(w.script.vars.get_mission(3) == 1);
    CHECK(w.script.vars.get_mission(4) == 2);
    CHECK(w.script.vars.get_mission(5) == 3);
    CHECK(w.script.vars.get_mission(6) == 3);
    w.restore(baseline);
    sys.restore_runtime_state(vm_baseline);
    run(w, sys, 3);
    CHECK(w.script.vars.get_mission(1) == 2);
    CHECK(w.script.vars.get_mission(2) == 1);
    CHECK(w.script.vars.get_mission(3) == 1);
    CHECK(w.script.vars.get_mission(4) == 1);
    CHECK(w.script.vars.get_mission(5) == 3);
    CHECK(w.script.vars.get_mission(6) == 2);
    CHECK(w.diagnostics.empty());
}

static void test_named_event_reset_and_declared_variables() {
    BehaviorWorld w;
    WacSystem sys;
    Program p = compile_source(
        "var counter\narray spare\n"
        "if [root] never then inc(counter)\n"
        " if never [child] then inc(spare) endif\n"
        "endif\n"
        "if [adjacent] never then inc(v3) endif\n"
        "if ontick(1) then reset(root) endif\n"
        "set(v1,counter) set(v2,spare)\n"
        "if true(root) then inc(v4) endif\n", {});
    CHECK(p.ok());
    CHECK(p.diagnostics.empty());
    sys.set_program(std::move(p));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 2);
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK(w.script.vars.get_mission(2) == 1);
    CHECK(w.script.vars.get_mission(3) == 1);
    CHECK(w.script.vars.get_mission(4) == 1);
    run(w, sys, 1);
    CHECK(w.script.vars.get_mission(1) == 2);
    CHECK(w.script.vars.get_mission(2) == 2);
    CHECK(w.script.vars.get_mission(3) == 1); // reset stops at a sibling
    CHECK(w.script.vars.get_mission(4) == 2);
    CHECK(w.diagnostics.empty());
}

static void test_empty_server_holds_script_divider_after_boot() {
    BehaviorWorld w;
    WacSystem sys;
    sys.set_program(compile_source("inc(v1)\n", {}));
    w.add_system(&sys);
    w.load_systems();
    w.cached.humans = 0;
    for (int i = 0; i < WacSystem::kTicksPerExecution * 3; ++i) w.run_logic_tick();
    CHECK(sys.runs() == 1); // the first execution is admitted without humans
    CHECK(w.script.vars.get_mission(1) == 1);
    w.cached.humans = 1;
    for (int i = 0; i < 31; ++i) w.run_logic_tick();
    w.cached.humans = 0;
    for (int i = 0; i < 124; ++i) w.run_logic_tick();
    CHECK(sys.runs() == 1);
    w.cached.humans = 1;
    for (int i = 0; i < 31; ++i) w.run_logic_tick();
    CHECK(sys.runs() == 2); // the held divider resumes where it stopped
    CHECK(w.script.vars.get_mission(1) == 2);
}

static void test_arithmetic_assignment_and_retail_expression_order() {
    BehaviorWorld w;
    WacSystem sys;
    Program p = compile_source(
        "var count\ncount = 2 + 3 * 4\n"
        "v1 = count\n"
        "v2 = 20 - 2 * 3\n"
        "v3 = (20 - 2) * 3\n"
        "v4 = 300 + (2 * 3)\n"
        "v5 = 2 ^ 3 ^ 2\n"
        "v6 = -9 / 2\n"
        "v7 = 9 % 4\n"
        "if 1 or 0 and 0 then inc(v8) endif\n"
        "if 5 < 2 + 7 then inc(v9) endif\n"
        "if not (0 or 0) then inc(v10) endif\n"
        "if 1 and not (0 or 0) then inc(v11) endif\n"
        "v12 = (1 + (not (0 or 0)))\n"
        "v14 = 300 + (2)\n"
        "v15 = 2 ^ (3 ^ 2)\n", {});
    CHECK(p.ok());
    CHECK(p.diagnostics.empty());
    sys.set_program(std::move(p));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 1);
    CHECK(w.script.vars.get_mission(1) == 14);
    CHECK(w.script.vars.get_mission(2) == -14); // new result 6 minus saved byte 20
    CHECK(w.script.vars.get_mission(3) == 54);
    CHECK(w.script.vars.get_mission(4) == 50); // saved 300 narrows to 44
    CHECK(w.script.vars.get_mission(5) == 64); // equal precedence folds left to right
    CHECK(w.script.vars.get_mission(6) == -4);
    CHECK(w.script.vars.get_mission(7) == 1);
    CHECK(w.script.vars.get_mission(8) == 0); // AND and OR have equal precedence
    CHECK(w.script.vars.get_mission(9) == 0); // grouped compare tests 9 < 5
    CHECK(w.script.vars.get_mission(10) == 1);
    CHECK(w.script.vars.get_mission(11) == 0); // NOT negates the saved 1
    CHECK(w.script.vars.get_mission(12) == 2); // parentheses separate NOT from ADD's pop
    CHECK(w.script.vars.get_mission(14) == 46); // parentheses around one value still push
    CHECK(w.script.vars.get_mission(15) == 81); // power also folds the grouped result first
    CHECK(w.diagnostics.empty());

    sys.set_program(compile_source("v1 = 5 / 0\ninc(v13)\n", {}));
    run(w, sys, 1);
    CHECK(!w.diagnostics.empty());
    CHECK(w.script.vars.get_mission(13) == 0); // malformed arithmetic stops this pass
}

static void test_npc_wac_health_names_and_boarding_consumer() {
    BehaviorWorld w;
    w.registry.configure_pool(1, 8);
    Entity soldier;
    soldier.net_id = 42;
    soldier.item_id = 1001;
    soldier.item_type = 3;
    soldier.has_item_def = true;
    soldier.health = 20;
    soldier.health_max = 100;
    soldier.critical_hp = 25;
    const EntityHandle sh = w.registry.spawn(0, soldier);
    Entity carrier;
    carrier.net_id = 77;
    carrier.item_id = 1002;
    carrier.item_type = 1;
    carrier.has_item_def = true;
    carrier.item_attrib = kItemAttribPlayerControl;
    Seat seat;
    seat.type = SeatType::Passenger;
    seat.retail_slot = 0;
    seat.bone_index = 1;
    carrier.seats.push_back(seat);
    const EntityHandle ch = w.registry.spawn(1, carrier);
    w.ai.attach(sh);
    AiEntity &brain = *w.ai.for_handle(sh);
    brain.inf.active = true;
    brain.inf.wait_cooldown = 99;

    WacSystem sys;
    sys.set_program(compile_source(
        "if never then ssnname(42,\"abcdefghijklmnopqrstuvwxyz0123456789\") "
        "ssn2ssn(42,77) endif\n"
        "if SSNcritical(42) then inc(v1) endif\n"
        "if SSNride(77) then inc(v2) endif\n", {}));
    w.add_system(&sys);
    w.load_systems();
    run(w, sys, 1);
    CHECK(w.registry.get(sh)->display_name == "abcdefghijklmnopqrstuvwxyz01234");
    CHECK(brain.slot.f[37] == 125);
    CHECK(brain.slot.f[38] == 77);
    CHECK(brain.slot.f[36] == int32_t(ch.packed) + 1);
    CHECK(brain.inf.wait_cooldown == 0);
    CHECK(!w.registry.get(sh)->mounted); // command first, entry walk second
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK(w.script.vars.get_mission(2) == 0);

    w.ai.infantry_board_think(brain, w, brain.slot.f[37]);
    CHECK(w.registry.get(sh)->mounted);
    CHECK(w.registry.get(sh)->mount_target == ch);
    run(w, sys, 1);
    CHECK(w.script.vars.get_mission(2) == 1);
    w.registry.get(sh)->health = 0;
    w.registry.get(sh)->flags |= kEntityFlagDead;
    run(w, sys, 1);
    CHECK(w.script.vars.get_mission(1) == 2); // dead is not critical
    CHECK(w.script.vars.get_mission(2) == 1); // dead occupants do not count
    CHECK(!w.commands.set_ssn_name(42, ""));
    CHECK(w.registry.get(sh)->display_name.size() == 31);
    CHECK(w.diagnostics.empty());
}

// SSNride follows retail's +0x28 carrier link up to three hops; that one link
// covers a seated rider, an emplacement child and a deck-stander alike.
// [orig: WacCmd_SsnRide @0x4F7000]
static void test_ssn_rider_query_bounds_parent_depth_and_pool() {
    BehaviorWorld w;
    w.registry.configure_pool(1, 8);
    Entity target;
    target.net_id = 90;
    const EntityHandle th = w.registry.spawn(1, target);
    Entity parent;
    parent.net_id = 91;
    parent.emplacement_parent = th;
    const EntityHandle p1 = w.registry.spawn(1, parent);
    parent.net_id = 92;
    parent.emplacement_parent = p1;
    const EntityHandle p2 = w.registry.spawn(1, parent);
    Entity rider;
    rider.net_id = 93;
    rider.mounted = true;
    rider.mount_target = p2;
    const EntityHandle rh = w.registry.spawn(0, rider);
    CHECK(w.commands.ssn_has_rider(90)); // three links, even without an item id
    parent.net_id = 94;
    parent.emplacement_parent = p2;
    const EntityHandle p3 = w.registry.spawn(1, parent);
    w.registry.get(rh)->mount_target = p3;
    CHECK(!w.commands.ssn_has_rider(90)); // fourth link is outside the query
    w.registry.get(rh)->mounted = false;
    w.registry.get(rh)->mount_target = {};
    CHECK(!w.commands.ssn_has_rider(90)); // pool-1 riders are never scanned

    // Standing on the deck (the ground-probe store, not a seat) is riding.
    w.registry.get(rh)->ground_target = th;
    CHECK(w.commands.ssn_has_rider(90));
    // A mixed chain: deck-stander on an emplacement child of the hull.
    w.registry.get(rh)->ground_target = p1;
    CHECK(w.commands.ssn_has_rider(90));
    w.registry.get(rh)->ground_target = p3; // hull is the fourth hop again
    CHECK(!w.commands.ssn_has_rider(90));
    w.registry.get(rh)->ground_target = th;
    w.registry.get(rh)->flags |= kEntityFlagDead; // Flags 2 excludes the rider
    CHECK(!w.commands.ssn_has_rider(90));
}

static void test_player_group_loops_and_handle_aliases() {
    BehaviorWorld w;
    w.registry.configure_pool(1, 4);
    Entity seed; seed.item_id = 1001; seed.health = seed.health_max = 100;
    seed.flags = kEntityFlagPlayer; seed.team = 1; seed.net_id = 100;
    const EntityHandle first = w.registry.spawn(0, seed);
    seed.net_id = 500; seed.team = 2; seed.flags |= kEntityFlagDead;
    const EntityHandle second = w.registry.spawn(0, seed); // dead humans stay in lists
    seed.net_id = 1; seed.flags |= 1u;
    const EntityHandle excluded = w.registry.spawn(0, seed); // net ID collides with second's handle
    seed.net_id = 600; seed.flags = 1; seed.team = 1;
    w.registry.spawn(0, seed); // enters all AI, not blue AI
    seed.net_id = 601; seed.flags = 0;
    const EntityHandle blue_ai = w.registry.spawn(0, seed);
    seed.net_id = 602; seed.team = 2;
    const EntityHandle red_ai = w.registry.spawn(0, seed);
    seed.net_id = 700; seed.flags = kEntityFlagPlayer;
    const EntityHandle pool_one = w.registry.spawn(1, seed);
    seed.net_id = 701;
    w.registry.despawn(w.registry.spawn(0, seed)); // a destroyed slot inside the used count is skipped
    w.cached.local_player = first;
    const int custom = w.registry.intern_group("authored");
    w.registry.set_script_group_members(custom, {blue_ai, second, red_ai});
    CompileEnv env; env.registry = &w.registry;
    Program program = compile_source(
        "ploop\n"
        " v0 = auto\n"
        " v1 = v1*10+v0\n"
        " if pisteam(1) then inc(v2) endif\n"
        " ssnname(v0, \"visited\")\n"
        "end\n"
        "v3 = Player\n"
        "gloop G_ai inc(v4) end\n"
        "gloop(G_blueai) inc(v5) end\n"
        "gloop G_redai inc(v6) end\n"
        "gloop G_emptygroup inc(v7) end\n"
        "gloop G_authored v8 = v8*10+Item end\n"
        "v9 = auto\n"
        "v10 = SSN_500\n"
        "ssnname(v10, \"aliased\")\n",
        env);
    CHECK(program.ok() && program.diagnostics.empty());
    WacVm vm; vm.load(program); vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 10); // reverse pool order: handles 1,0
    CHECK(w.script.vars.get_mission(2) == 1);
    CHECK(w.script.vars.get_mission(3) == first.packed);
    CHECK(w.script.vars.get_mission(4) == 3);
    CHECK(w.script.vars.get_mission(5) == 1 && w.script.vars.get_mission(6) == 1);
    CHECK(w.script.vars.get_mission(7) == 0);
    CHECK(w.script.vars.get_mission(8) == 514); // authored order reversed: 5,1,4
    CHECK(w.script.vars.get_mission(9) == first.packed);
    CHECK(w.script.vars.get_mission(10) == second.packed);
    CHECK(w.registry.get(first)->display_name == "visited");
    CHECK(w.registry.get(second)->display_name == "aliased");
    CHECK(w.registry.get(excluded)->display_name.empty());
    CHECK(w.registry.get(pool_one)->display_name.empty());
    CHECK(w.diagnostics.empty());
    CHECK(!compile_source("ploop gloop G_ai inc(v1) end end", env).ok());

    // Both rows now share an SSN; a bound variable must still name second.
    w.registry.get(first)->net_id = 800;
    w.registry.get(second)->net_id = 800;
    w.registry.get(second)->display_name.clear();
    vm.execute(w);
    CHECK(w.registry.get(first)->display_name == "visited");
    CHECK(w.registry.get(second)->display_name == "aliased");

    // The compiled reference survives raw SSN changes and runtime restore.
    const auto baseline = vm.capture_runtime_state();
    w.registry.get(second)->net_id = 800;
    vm.execute(w);
    CHECK(w.script.vars.get_mission(10) == second.packed);
    vm.restore_runtime_state(program, baseline);
    vm.execute(w);
    CHECK(w.script.vars.get_mission(10) == second.packed);
}

static void test_named_group_actions_use_member_handles() {
    BehaviorWorld w;
    Entity e; e.item_id = 1001; e.health = 100; e.net_id = 10; e.group_id = 7;
    const EntityHandle first = w.registry.spawn(0, e);
    const EntityHandle second = w.registry.spawn(0, e);
    const EntityHandle keep = w.registry.spawn(0, e);
    const int kill = w.registry.intern_group("killset");
    const int remove = w.registry.intern_group("removeset");
    w.registry.set_script_group_members(kill, {first});
    w.registry.set_script_group_members(remove, {second});
    CompileEnv env; env.registry = &w.registry;
    const Program program = compile_source("Gkill(G_killset) Gremove(G_removeset)", env);
    CHECK(program.ok() && program.diagnostics.empty());
    WacVm vm; vm.load(program); vm.execute(w);
    CHECK(w.registry.get(first) != nullptr && w.registry.get(first)->health == 0);
    CHECK(w.registry.get(second) == nullptr);
    CHECK(w.registry.get(keep) != nullptr && w.registry.get(keep)->health == 100);
    CHECK(w.diagnostics.empty());
}

static void test_wac_area_and_location_queries() {
    BehaviorWorld w;
    w.registry.configure_pool(2, 4);
    Entity e; e.item_id = 1001; e.net_id = 42; e.health = e.health_max = 100;
    e.flags = kEntityFlagPlayer; e.position = {2, -2, 3};
    const EntityHandle player = w.registry.spawn(0, e);
    w.cached.local_player = player;
    Aabb area; area.min = {-2, -2, 0}; area.max = {2, 2, 1};
    w.registry.register_area("authored", area, false, 37);
    Aabb duplicate; duplicate.min = {-100, -100, -100}; duplicate.max = {100, 100, 100};
    w.registry.register_area("duplicate", duplicate, true, 37);
    w.registry.register_location(duplicate, 8);
    w.registry.register_location(area, 9);
    e.position = {}; e.item_id = 2001; e.net_id = 100;
    w.registry.spawn(2, e); // slot 0: packed blink hit must be nonzero
    const EntityHandle building = w.registry.spawn(2, e);
    Program program = compile_source(
        "v1=area(37) v2=area3D(37) v3=SSNarea(42,37) v4=SSNarea3D(42,37) "
        "v5=area(0) v6=outside v7=SSNloc(42,9) v8=location(9)", {});
    CHECK(program.ok() && program.diagnostics.empty());
    WacVm vm; vm.load(program); vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 1 && w.script.vars.get_mission(3) == 1);
    CHECK(w.script.vars.get_mission(2) == 0 && w.script.vars.get_mission(4) == 0);
    CHECK(w.script.vars.get_mission(5) == 0 && w.script.vars.get_mission(6) == 1);
    w.registry.get(player)->position.z = 1; // area edges are inclusive
    vm.execute(w);
    CHECK(w.script.vars.get_mission(2) == 1 && w.script.vars.get_mission(4) == 1);
    CHECK(w.script.vars.get_mission(7) == 0); // location edges are strict
    w.registry.get(player)->position = {0, 0, 0.5f};
    w.commands.update_local_location(player);
    vm.execute(w);
    CHECK(w.script.vars.get_mission(7) == 1 && w.script.vars.get_mission(8) == 1);

    w.registry.get(player)->blink_hits[0] = uint32_t(building.slot()) << 20;
    w.registry.get(building)->music_location = 0;
    w.commands.update_local_location(player);
    vm.execute(w);
    CHECK(w.script.vars.get_mission(6) == 0);
    CHECK(w.script.vars.get_mission(7) == 0 && w.script.vars.get_mission(8) == 1);
    CHECK(w.commands.ssn_at_location(player, 0)); // SSNloc honors an indoor zero
    w.registry.get(building)->music_location = -3;
    w.commands.update_local_location(player);
    CHECK(w.commands.ssn_at_location(player, -3));
    CHECK(w.script.wac_values.local_location == -3);
    const auto baseline = w.snapshot();
    w.registry.get(player)->blink_hits[0] = 0;
    w.registry.get(player)->blink_hits[1] = 1u << 20;
    w.registry.get(player)->health = 0;
    w.registry.get(player)->flags |= kEntityFlagDead;
    w.commands.update_local_location(player);
    vm.execute(w);
    CHECK(w.script.wac_values.local_location == -3); // dead body leaves cache alone
    CHECK(w.script.vars.get_mission(1) == 1 && w.script.vars.get_mission(2) == 1);
    CHECK(w.script.vars.get_mission(6) == 1); // only the first blink matters
    w.registry.get(player)->flags |= 1u;
    vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 0 && w.script.vars.get_mission(3) == 0);
    w.restore(baseline);
    CHECK(w.script.wac_values.local_location == -3);
    w.registry.get(player)->item_id = 0;
    vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 0 && w.script.vars.get_mission(4) == 0);
    CHECK(w.diagnostics.empty());
}

static void test_ssnuse_mounts_cached_child_and_clears_failed_choice() {
    BehaviorWorld w;
    w.registry.configure_pool(1, 8);
    Entity e; e.item_id = 1001; e.item_type = 3; e.net_id = 42;
    e.health = e.health_max = 100;
    const EntityHandle rider = w.registry.spawn(0, e);
    w.ai.attach(rider);
    AiEntity &ai = *w.ai.for_handle(rider);
    e.net_id = 77; e.item_id = 2001; e.item_type = 1;
    e.has_item_def = true; e.item_attrib = kItemAttribPlayerControl;
    e.position = {100, 100, 0};
    Seat driver; driver.type = SeatType::Driver; driver.bone_index = 1;
    e.seats.push_back(driver);
    const EntityHandle carrier = w.registry.spawn(1, e);
    e.net_id = 78; e.ground_target = carrier; e.seats.clear();
    Seat passenger; passenger.type = SeatType::Passenger; passenger.bone_index = 2;
    e.seats.push_back(passenger);
    const EntityHandle child = w.registry.spawn(1, e);
    ai.slot.f[36] = int32_t(carrier.packed) + 1;
    ai.slot.f[37] = 123; // cached root's driver must be skipped
    Program program = compile_source("v1=ssnuse(42)", {});
    WacVm vm; vm.load(program); vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK(w.registry.get(rider)->mount_target == child);
    CHECK(ai.slot.f[36] == int32_t(child.packed) + 1);
    vm.execute(w); // already mounted: do not replace the cached choice
    CHECK(w.script.vars.get_mission(1) == 0);
    CHECK(ai.slot.f[36] == int32_t(child.packed) + 1);
    w.vehicles.detach(rider);
    w.registry.get(child)->seats.clear();
    w.registry.get(rider)->flags |= kEntityFlagMounted;
    w.registry.get(rider)->engine_flags |= kEntityFlagMounted;
    w.registry.get(rider)->mount_type = SeatType::Passenger;
    vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 0 && ai.slot.f[36] == 0);
    CHECK(!w.registry.get(rider)->mount_target.valid());
    CHECK(w.registry.get(rider)->mount_type == SeatType::None);
    CHECK(((w.registry.get(rider)->flags | w.registry.get(rider)->engine_flags) & kEntityFlagMounted) == 0);
    CHECK(w.diagnostics.empty());
}

static void test_meride_reads_standing_carrier_and_remove_uses_command_group() {
    BehaviorWorld w;
    w.registry.configure_pool(1, 4);
    Entity e; e.item_id = 1001; e.net_id = 42; e.health = 100;
    const EntityHandle player = w.registry.spawn(0, e);
    w.cached.local_player = player;
    e.net_id = 77; e.group_id = 7;
    const EntityHandle carrier = w.registry.spawn(1, e);
    Program program = compile_source("v1=meride(77) v2=meattached(77)", {});
    WacVm vm; vm.load(program); vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 0 && w.script.vars.get_mission(2) == 0);
    w.registry.get(player)->ground_target = carrier;
    vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 1 && w.script.vars.get_mission(2) == 0);
    w.registry.get(player)->ground_target = {};
    w.registry.get(player)->mount_target = carrier;
    w.registry.get(player)->mounted = true;
    vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 0 && w.script.vars.get_mission(2) == 1);
    e.net_id = 78; e.item_id = 0; // removal has no item-definition gate
    const EntityHandle missing_def = w.registry.spawn(1, e);
    Program removal = compile_source("remove(0) remove(7)", {});
    vm.load(removal); vm.execute(w);
    CHECK(w.registry.get(player) != nullptr);
    CHECK(w.registry.get(carrier) == nullptr && w.registry.get(missing_def) == nullptr);
    CHECK(w.diagnostics.empty());
}


static void test_scripted_respawn_counts_are_not_immediate_spawns() {
    BehaviorWorld w;
    w.registry.configure_pool(1, 4);
    Entity seed;
    seed.net_id = 101;
    seed.health = 0;
    seed.alive = false;
    seed.group_id = 7;
    const auto one = w.registry.spawn(0, seed); // deliberately no item or AI
    seed.net_id = 102;
    const auto two = w.registry.spawn(0, seed);
    seed.net_id = 103;
    const auto vehicle = w.registry.spawn(1, seed);
    seed.net_id = 104;
    seed.group_id = 0;
    const auto zero_group = w.registry.spawn(0, seed);
    WacSystem script;
    script.set_program(compile_source(
            "v1 = SSNSpawn(101,65535)\n"
            "v2 = SSNSpawn(999,1)\n"
            "GroupSpawn(7,65538)\n"
            "SSNSpawn(101,65535)\n"
            "GroupSpawn(0,100)\n", {}));
    CHECK(script.execute_initial(w));
    CHECK(w.script.vars.get_mission(1) == 1 && w.script.vars.get_mission(2) == 0);
    CHECK(w.registry.get(one)->npc_respawns == -1);
    CHECK(w.registry.get(two)->npc_respawns == 2);
    CHECK(w.registry.get(vehicle)->npc_respawns == 0); // group writer is pool 0 only
    CHECK(w.registry.get(zero_group)->npc_respawns == 100);
    CHECK(w.registry.get(one)->health == 0 && !w.registry.get(one)->alive);
    CHECK(w.diagnostics.empty());
}

static void test_distance_literals_and_lead_queries() {
    BehaviorWorld w;
    Entity seed;
    seed.item_id = 1;
    seed.net_id = 101;
    seed.position = {0.0f, 0.0f, 2.5f};
    seed.yaw = 90;
    seed.health = 0; seed.alive = false; seed.flags = kEntityFlagDead;
    const auto a = w.registry.spawn(0, seed);
    seed.net_id = 102; seed.position.z = 5.0f;
    const auto b = w.registry.spawn(0, seed);
    seed.net_id = 103; seed.position.z = 0.0f;
    const auto goal = w.registry.spawn(0, seed);
    WacVm vm;
    Program program = compile_source(
            "v1 = SSNLeadSSN2SSN(101,102,103,2.5)\n"
            "v2 = SSNLeadSSN2SSN(101,102,103,2.499)\n"
            "v3 = SSNLeadSSN2SSN(102,101,103,-2.5)\n"
            "v5 = 163839\nv4 = SSNLeadSSN2SSN(101,102,103,v5)\n"
            "v6 = SSNnearSSN(101,103,2.5)\n"
            "v7 = SSNnearSSN(101,103,2.499)\n"
            "v8 = SSNlosSSN(101,103,2.5)\n"
            "v9 = SSNseesSSN(103,101,2.5)\n"
            "v10 = 2.5M\nv11 = 3.5F\n"
            "v12 = SSNLeadSSN2SSN(101,102,999,0)\n"
            "v13 = SSNnearSSN(101,103,v10)\n"
            "v14 = SSNnearSSN(101,103,3)\n"
            "v15 = SSNnearSSN(101,103,v14)\n"
            "v16 = 65536M\n", {});
    CHECK(program.ok() && program.diagnostics.empty());
    vm.load(program); vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 0); // equality does not lead
    CHECK(w.script.vars.get_mission(2) == 1);
    CHECK(w.script.vars.get_mission(3) == 0);
    CHECK(w.script.vars.get_mission(4) == 1); // raw variable is not multiplied again
    CHECK(w.script.vars.get_mission(6) == 1 && w.script.vars.get_mission(7) == 0);
    CHECK(w.script.vars.get_mission(8) == 1 && w.script.vars.get_mission(9) == 1);
    CHECK(w.script.vars.get_mission(10) == 163840);
    CHECK(w.script.vars.get_mission(11) == 75253); // 21501 * 3.5, truncation
    CHECK(w.script.vars.get_mission(12) == 0);
    CHECK(w.script.vars.get_mission(13) == 1);
    CHECK(w.script.vars.get_mission(14) == 1 && w.script.vars.get_mission(15) == 0);
    CHECK(w.script.vars.get_mission(16) == 0); // low dword of _ftol2_sse

    // Wrapped coordinate subtraction precedes the Euclidean length.
    w.registry.get(a)->position = {-32768.0f, 0.0f, 0.0f};
    w.registry.get(b)->position = {32767.0f, 0.0f, 0.0f};
    CHECK(w.commands.ssn_within_distance(a, b, 65536));
    CHECK(!w.commands.ssn_within_distance(a, b, 65535));
    w.registry.get(a)->position = {-32768.0f, -32768.0f, 0.0f};
    w.registry.get(b)->position = {32767.0f, 32767.0f, 0.0f};
    // Both lengths clamp to 0x7FFF0000 before the lead subtraction.
    CHECK(!w.commands.ssn_leads_target(a, b, goal, 0));
    CHECK(w.commands.ssn_leads_target(a, b, goal, -1));
    w.registry.get(goal)->item_id = 0;
    CHECK(!w.commands.ssn_leads_target(a, b, goal, -1));
    CHECK(w.diagnostics.empty());
}

static void test_script_ranges_drive_controller_and_perception() {
    BehaviorWorld w;
    w.registry.configure_pool(1, 4);
    Entity seed;
    seed.item_id = 1; seed.item_type = 3; seed.kind = EntityKind::Organic;
    seed.health = 100; seed.team = 1; seed.group_id = 7; seed.net_id = 101;
    const auto scanner = w.registry.spawn(0, seed);
    seed.team = 2; seed.group_id = 0; seed.net_id = 102; seed.position.x = 5.0f;
    const auto target = w.registry.spawn(0, seed);
    seed.item_id = 0; seed.health = 0; seed.group_id = 7; seed.net_id = 103;
    const auto itemless = w.registry.spawn(0, seed);
    seed.net_id = 104;
    const auto vehicle = w.registry.spawn(1, seed);
    seed.net_id = 105;
    const auto no_controller = w.registry.spawn(0, seed);
    for (auto handle : {scanner, itemless, vehicle}) w.ai.attach(handle);
    AiEntity &body = *w.ai.for_handle(scanner);
    body.health = 100; body.team = 1; body.inf.active = true;
    body.slot.bytes()[AiSlot::kAlertByte] = 2; // full sight range at phase 0
    WacVm vm;
    Program short_range = compile_source(
            "v1 = GroupMin(7,1.5)\nv2 = GroupMax(7,4)\n"
            "v3 = GroupAtt(7,8)\nv4 = GroupMax(999,20)\n"
            "v5 = SSNMax(105,2.5)\nv6 = SSNMax(999,2.5)\n", {});
    CHECK(short_range.ok()); vm.load(short_range); vm.execute(w);
    CHECK(body.slot.f[AiSlot::kEngageMin] == 98304);
    CHECK(body.slot.f[AiSlot::kSightRange] == 4 * 65536);
    CHECK(body.slot.f[AiSlot::kAttackRange] == 8 * 65536);
    CHECK(w.ai.for_handle(itemless)->slot.f[AiSlot::kSightRange] == 4 * 65536);
    CHECK(w.ai.for_handle(vehicle)->slot.f[AiSlot::kSightRange] == 0);
    CHECK(w.registry.get(no_controller) != nullptr);
    for (int v = 1; v <= 5; ++v) CHECK(w.script.vars.get_mission(v) == 1);
    CHECK(w.script.vars.get_mission(6) == 0);
    w.ai.infantry_combat_think(body, w, 0);
    CHECK(!body.inf.combat_target.valid());

    Program long_range = compile_source(
            "v7 = 6M\nSSNMax(101,v7)\nSSNMin(101,2.5)\nSSNAtt(101,3.5F)\n"
            "GroupAtt(0,9)\n", {});
    CHECK(long_range.ok()); vm.load(long_range); vm.execute(w);
    CHECK(body.slot.f[AiSlot::kSightRange] == 6 * 65536);
    CHECK(body.slot.f[AiSlot::kEngageMin] == 163840);
    CHECK(body.slot.f[AiSlot::kAttackRange] == 75253);
    w.ai.infantry_combat_think(body, w, 128);
    CHECK(body.inf.combat_target == target); // WAC now changes the live sight scan
    CHECK(w.diagnostics.empty());
}

static void test_fractional_script_fog_uses_one_fixed_point_conversion() {
    BehaviorWorld w;
    w.weather.fog_reference_q16 = 1000 * 65536;
    WacVm vm;
    Program program = compile_source("fogdist(2.5)\n", {});
    CHECK(program.ok()); vm.load(program); vm.execute(w);
    CHECK(w.weather.core.scalar_channels.fog_dist_target_fp == 163840);
    program = compile_source("v1 = 196608\nmovefog(v1,2)\n", {});
    CHECK(program.ok()); vm.load(program); vm.execute(w);
    CHECK(w.weather.core.scalar_channels.fog_dist_target_fp == 196608);
    // The host's whole-metre UI contract reaches the same Q16 handler.
    w.commands.set_fog_distance(4);
    CHECK(w.weather.core.scalar_channels.fog_dist_target_fp == 262144);
    CHECK(w.diagnostics.empty());
}

static void test_wac_positional_sound_and_teleport_quirk() {
    BehaviorWorld w;
    w.registry.configure_pool(1, 4);
    w.registry.configure_pool(2, 4);
    w.registry.configure_pool(3, 4);
    Entity seed;
    seed.net_id = 101; seed.position = {-2.0f, -3.0f, 4.0f};
    const auto itemless = w.registry.spawn(0, seed);
    seed.net_id = 102; seed.item_id = 1; seed.item_type = 3;
    seed.health = 0; seed.alive = false; seed.health_max = 80;
    seed.flags = seed.engine_flags = kEntityFlagDead;
    seed.group_id = 7;
    const auto person = w.registry.spawn(0, seed);
    w.ai.attach(person);
    w.ai.for_handle(person)->pos[0] = -2 * 65536;
    w.ai.for_handle(person)->pos[1] = -3 * 65536;
    w.ai.for_handle(person)->pos[2] = 4 * 65536;
    seed.net_id = 103; seed.item_type = 1;
    const auto vehicle = w.registry.spawn(1, seed);
    seed.net_id = 104;
    const auto building = w.registry.spawn(2, seed);
    Entity marker;
    marker.item_id = kParticleEffectMarkerTypeId;
    marker.item_type = 4;
    marker.wp_number = 9;
    marker.position = {20.0f, 10.0f, 5.0f};
    marker.yaw = 30; marker.roll = 4;
    marker.flags = marker.engine_flags = kEntityFlagBuilding;
    const auto first_marker = w.registry.spawn(3, marker);
    marker.position.x = 40.0f;
    const auto later_marker = w.registry.spawn(3, marker);
    opennova::lwf::File bank;
    for (const char *name : {"ABCDEFGHIJKLMNOPQRSTUVWX", "test"}) {
        opennova::lwf::Multi set; set.name = name; bank.multis.push_back(set);
    }
    opennova::audio::SoundSetIndex sounds; sounds.add_bank(0, bank);
    CompileEnv sound_env; sound_env.sounds = &sounds;
    WacVm vm;
    Program program = compile_source(
            "v1 = SS2SSN(SS_ABCDEFGHIJKLMNOPQRSTUVWX,102)\n"
            "v2 = SS2SSN(test,101)\nv3 = SS2SSN(test,999)\n"
            "v4 = teleSSN(101,9)\nv5 = teleSSN(999,9)\nv6 = teleSSN(101,99)\n", sound_env);
    CHECK(program.ok()); vm.load(program); vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 1 && w.out.slot_sounds.size() == 1);
    if (!w.out.slot_sounds.empty()) {
        const auto &sound = w.out.slot_sounds.front();
        CHECK(std::string(sound.set_name) == "abcdefghijklmnopqrstuvwx");
        CHECK(sound.source_handle == person.packed);
        CHECK(sound.pos[0] == -2 * 65536 && sound.pos[1] == -3 * 65536);
        CHECK(sound.pos[2] == 4 * 65536);
    }
    CHECK(w.script.vars.get_mission(2) == 0 && w.script.vars.get_mission(3) == 0);
    CHECK(w.script.vars.get_mission(4) == 1);
    CHECK(w.script.vars.get_mission(5) == 0 && w.script.vars.get_mission(6) == 0);
    CHECK(w.registry.get(itemless)->position.x == -2.0f); // retail loses the source
    CHECK((w.registry.get(first_marker)->engine_flags & kEntityFlagBuilding) == 0);
    CHECK((w.registry.get(later_marker)->engine_flags & kEntityFlagBuilding) != 0);
    program = compile_source("v7 = teleport(7,9)\n", {});
    CHECK(program.ok()); vm.load(program); vm.execute(w);
    CHECK(w.script.vars.get_mission(7) == 0); // handler's return is not the moved count
    for (auto handle : {person, vehicle, building}) {
        CHECK(w.registry.get(handle)->position.x == 20.0f);
        CHECK(w.registry.get(handle)->position.z == 5.0f);
    }
    CHECK(w.registry.get(person)->health == 80);
    CHECK(w.ai.for_handle(person)->pos[0] == 20 * 65536);
    CHECK(w.registry.get(itemless)->position.x == -2.0f);
    CHECK(w.diagnostics.empty());
}

static void test_player_values_cache_at_bytecode_entry() {
    BehaviorWorld w;
    Entity player;
    player.kind = EntityKind::Organic;
    player.item_id = 1;
    player.net_id = 10;
    player.health = 321;
    player.mana = 17;
    const auto h = w.registry.spawn(0, player);
    w.cached.local_player = h;
    w.cached.local_health = 999; // stale host value must be refreshed at entry
    CompileEnv env;
    Program program = compile_source(
        "if true(1) then set(v1,health) set(v2,mana) set(v3,CurTOD) "
        "SSNHP(10,600) set(v4,health) set(health,12345) set(mana,-5) "
        "set(v5,health) set(v6,mana) TOD(8) set(v7,CurTOD) set(v8,auto) "
        "set(CurTOD,123) set(v9,CurTOD) endif\n", env);
    CHECK(program.ok());
    WacVm vm;
    vm.load(program);
    vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 321);
    CHECK(w.script.vars.get_mission(2) == 17);
    CHECK(w.script.vars.get_mission(3) == 720);
    CHECK(w.script.vars.get_mission(4) == 321); // SSNHP did not refresh the cache
    CHECK(w.script.vars.get_mission(5) == 12345);
    CHECK(w.script.vars.get_mission(6) == -5);
    CHECK(w.script.vars.get_mission(7) == 720); // TOD change waits for the next entry
    CHECK(w.script.vars.get_mission(8) == h.packed);
    CHECK(w.script.vars.get_mission(9) == 123);
    CHECK(w.script.vars.get_mission(0) == 0);
    CHECK(w.registry.get(h)->health == 600);
    CHECK(w.registry.get(h)->mana == 17); // cached-word writes do not change the actor
    vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 600);
    CHECK(w.script.vars.get_mission(2) == 17);
    CHECK(w.script.vars.get_mission(3) == 480);
    w.registry.get(h)->health = 65535;
    w.registry.get(h)->mana = -32768;
    vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == -1);
    CHECK(w.script.vars.get_mission(2) == -32768);
    w.registry.despawn(h);
    vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 0);
    CHECK(w.script.vars.get_mission(2) == 0);
    CHECK(w.script.vars.get_mission(8) == 0xFFFF);
}

// Every row of the named-value table is an lvalue, and a token the resolver
// cannot bind is a logged compile diagnostic (the program still runs) whose
// operand is the shared scratch sink (zeroed at each bytecode entry), never V0;
// the same token then feeds the next slot, and the tokens left over are
// statement-level tokens (a value becomes load, anything else Unknown).
// [orig: WacScript_ResolveParameter @0x4f2a92..0x4f2a9f / @0x4f2a5e /
//  @0x4f2b7e / @0x4f2a62; Script_Compile @0x4f3ab2..0x4f3ae2 -> loc_4F3990
//  @0x4f3a71, the statement default @0x4f5108 / @0x4f5124 / @0x4f5293;
//  WacScript_CacheLocalPlayerState @0x4f57b5]
static void test_named_rows_are_lvalues_and_unresolved_arguments_sink() {
    BehaviorWorld w;
    w.kill_stats.bluekills_by_player = 2;
    Program program = compile_source(
        "set(ticks,5) set(v1,ticks)\n"
        "set(humans,3) set(v2,humans)\n"
        "set(bluekills,7) set(v3,bluekills)\n"
        "inc(greenkills) set(v4,greenkills)\n"
        "set(breathtime,4) set(v5,breathtime)\n"
        "set(autogain,0) set(v6,autogain)\n"
        "set(v7,breathtime) set(v8,autogain)\n"
        "set(result,9)\n"
        "set(night,1)\n", {});
    CHECK(program.ok());
    CHECK(program.diagnostics.empty());
    WacVm vm; vm.load(program); vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 5);
    CHECK(vm.time() == 6); // the run counter advances from the written word
    CHECK(w.script.vars.get_mission(2) == 3 && w.cached.humans == 3);
    CHECK(w.script.vars.get_mission(3) == 7 && w.kill_stats.bluekills_by_player == 7);
    CHECK(w.script.vars.get_mission(4) == 1 && w.kill_stats.greenkills_by_player == 1);
    CHECK(w.script.vars.get_mission(5) == 4 && w.script.wac_values.breathtime == 4);
    CHECK(w.script.vars.get_mission(6) == 0 && w.script.wac_values.autogain == 0);
    CHECK(w.script.vars.get_mission(7) == 4 && w.script.vars.get_mission(8) == 0);
    CHECK(w.script.vars.get_mission(0) == 0); // no named row aliases V0
    BehaviorWorld seeds;
    CHECK(seeds.script.wac_values.breathtime == 20 && seeds.script.wac_values.autogain == 1);

    // Unresolvable lvalues and values: the diagnostic is the retail action
    // signature (Script_SetCompileError's first-error buffer, which only the
    // script debug overlay shows; the program compiles and runs), the slot
    // takes the scratch sink and the SAME token feeds the next slot [orig:
    // Script_Compile @0x4f3ab2..0x4f3aed -> loc_4F3990 @0x4f3a71], so neither
    // V0 nor the guarded variable moves. Every diagnostic is non-fatal and the
    // first is the signature; the re-feed logs it once per slot the token
    // fails and the stray tokens add their own, so the count is not pinned.
    const char *sources[] = {
        "set(5,1)\n", "set(nosuchname,1)\n", "set(\"v1\",1)\n",
        "if [root] eq(1,2) then set(v9,1) endif\nset(root,1)\n",
        "set(v1,nosuchname)\n",
        "if SSNexists(1) then set(v1,notaname) endif\n"};
    for (const char *source : sources) {
        BehaviorWorld sink;
        Program bad = compile_source(source, {});
        CHECK(bad.ok());
        CHECK(!bad.diagnostics.empty());
        for (const Diagnostic &d : bad.diagnostics) CHECK(!d.error);
        WacVm bad_vm; bad_vm.load(bad); bad_vm.execute(sink);
        CHECK(sink.script.vars.get_mission(0) == 0);
        CHECK(sink.script.vars.get_mission(1) == 0);
        CHECK(sink.script.vars.get_mission(9) == 0);
    }
    Program bad = compile_source("set(nosuchname,1)\n", {});
    CHECK(!bad.diagnostics.empty() && bad.diagnostics[0].message == "  set (variable, value)");
    // A leftover the statement level cannot classify is "Unknown '<token>'"
    // [orig: Script_Compile @0x4f5293]; nothing is emitted for it.
    bad = compile_source("set(v1,nosuchname)\n", {});
    CHECK(!bad.diagnostics.empty() && bad.diagnostics[0].message == "  set (variable, value)");
    CHECK(bad.diagnostics.back().message == "Unknown 'nosuchname'");
    // The sink is one shared scratch word. set(nosuchname,6) re-feeds the
    // name into the value slot too (SET scratch,scratch; the 6 is a stray
    // load), so only its own zero ever reaches it and v2 reads 0 on every
    // execution.
    Program scratch = compile_source(
        "set(v1,nosuchname) set(nosuchname,6) set(v2,othername)\n", {});
    CHECK(scratch.ok() && !scratch.diagnostics.empty());
    BehaviorWorld shared;
    WacVm scratch_vm; scratch_vm.load(scratch);
    scratch_vm.execute(shared);
    CHECK(shared.script.vars.get_mission(1) == 0 && shared.script.vars.get_mission(2) == 0);
    shared.script.vars.set_mission(1, -1);
    shared.script.vars.set_mission(2, -1);
    scratch_vm.execute(shared);
    CHECK(shared.script.vars.get_mission(1) == 0 && shared.script.vars.get_mission(2) == 0);
    CHECK(shared.script.vars.get_mission(0) == 0);
}

// The re-feed's observable outcomes. World is a large object and MSVC sizes a
// frame for every local at entry, so these fixtures live in their own
// function rather than beside the named-row ones.
// [orig: Script_Compile @0x4f3ab2..0x4f3aed -> loc_4F3990 @0x4f3a71; the
//  stray load @0x4f5124 / @0x4f5321..0x4f533d]
static void test_refed_tokens_and_stray_loads() {
    // The name fills BOTH eq slots with the sink, so EQ is true, and the
    // stray literal becomes load(1): the accumulator stays 1 and the body runs...
    Program refed_program = compile_source("if eq(nosuchname,1) then set(v9,1) endif\n", {});
    CHECK(refed_program.ok() && !refed_program.diagnostics.empty());
    CHECK(refed_program.diagnostics[0].message == "  eq (number, number)");
    BehaviorWorld refed;
    WacVm refed_vm; refed_vm.load(refed_program); refed_vm.execute(refed);
    CHECK(refed.script.vars.get_mission(9) == 1);
    // ...while a stray 0 loads over the true comparison and the body stays cold.
    Program stray = compile_source("if eq(nosuchname,0) then set(v9,1) endif\n", {});
    CHECK(stray.ok() && !stray.diagnostics.empty() && stray.diagnostics[0].message == "  eq (number, number)");
    BehaviorWorld cold;
    WacVm stray_vm; stray_vm.load(stray); stray_vm.execute(cold);
    CHECK(cold.script.vars.get_mission(9) == 0);
    // A literal re-fed from the variable slot into the value slot DOES write
    // the sink word (SET scratch,5): it reads back within the execution, and
    // the next bytecode entry clears it before the first read.
    Program written = compile_source("set(v3,othername) set(5,6) set(v2,othername)\n", {});
    CHECK(written.ok());
    BehaviorWorld word;
    WacVm written_vm; written_vm.load(written);
    written_vm.execute(word);
    CHECK(word.script.vars.get_mission(3) == 0 && word.script.vars.get_mission(2) == 5);
    word.script.vars.set_mission(2, -1);
    written_vm.execute(word);
    CHECK(word.script.vars.get_mission(3) == 0 && word.script.vars.get_mission(2) == 5);
    CHECK(word.script.vars.get_mission(0) == 0);
}

// A quoted token binds only in a Text/Filename slot: retail's buffer keeps
// the opening quote, so a Number or Value slot's numeric test fails it into
// the NULL leg [orig: Script_Compile @0x4f3338; WacScript_ResolveParameter
// @0x4f2ce9 (17/18 only), @0x4f2d01 -> @0x4f2a62]. Both eq slots take the
// sink, the quoted token is a stray "Unknown", and the bare literal loads over
// the true comparison exactly as the unknown name does above. (A World is a
// large stack object: no more than three per test function.)
static void test_quoted_tokens_outside_text_slots_are_the_null_leg() {
    Program quoted = compile_source("if eq(\"1\",1) then set(v9,1) endif\n", {});
    CHECK(quoted.ok() && quoted.diagnostics.size() == 3);
    CHECK(quoted.diagnostics[0].message == "  eq (number, number)");
    CHECK(quoted.diagnostics[1].message == "  eq (number, number)");
    CHECK(quoted.diagnostics[2].message == "Unknown '1'");
    BehaviorWorld quoted_world;
    WacVm quoted_vm; quoted_vm.load(quoted); quoted_vm.execute(quoted_world);
    CHECK(quoted_world.script.vars.get_mission(9) == 1);
    Program quoted_cold = compile_source("if eq(\"1\",0) then set(v9,1) endif\n", {});
    BehaviorWorld quoted_cold_world;
    WacVm quoted_cold_vm; quoted_cold_vm.load(quoted_cold); quoted_cold_vm.execute(quoted_cold_world);
    CHECK(quoted_cold_world.script.vars.get_mission(9) == 0);
    // A Value slot is no different: set(v3,"5") writes the sink's 0.
    Program quoted_value = compile_source("set(v3,\"5\")\n", {});
    CHECK(quoted_value.ok() && quoted_value.diagnostics.size() == 2);
    CHECK(quoted_value.diagnostics[0].message == "  set (variable, value)");
    CHECK(quoted_value.diagnostics[1].message == "Unknown '5'");
    BehaviorWorld quoted_value_world;
    quoted_value_world.script.vars.set_mission(3, 7);
    WacVm quoted_value_vm; quoted_value_vm.load(quoted_value); quoted_value_vm.execute(quoted_value_world);
    CHECK(quoted_value_world.script.vars.get_mission(3) == 0);
}

// An IfName token naming no event misses retail's table 1 and falls through
// every leg to the NULL return [orig: WacScript_ResolveParameter @0x4f29c2 ->
// loc_4F29C4 .. @0x4f2a62]: reset's slot takes the sink, the token is a stray
// "Unknown", and at execution reset(0) clears event 0's history
// [orig: WacCmd_Reset @0x4ED300], so `never` fires every execution.
static void test_unknown_ifname_token_sinks_to_event_zero() {
    Program unknown_event = compile_source(
        "if never then inc(v1) endif\n"
        "reset(nosuchevent)\n", {});
    CHECK(unknown_event.ok() && unknown_event.diagnostics.size() == 2);
    CHECK(unknown_event.diagnostics[0].message == "  reset (ifname)");
    CHECK(unknown_event.diagnostics[1].message == "Unknown 'nosuchevent'");
    BehaviorWorld reset_world;
    WacVm reset_vm; reset_vm.load(unknown_event);
    for (int i = 0; i < 3; ++i) reset_vm.execute(reset_world);
    CHECK(reset_world.script.vars.get_mission(1) == 3);
}

// An Ssn slot never takes the scratch sink: retail's SSN leg atol's the token
// (0 for a name or a quoted token), looks the net id up and keeps the handle,
// logging "Unknown SSN" on a miss. The port binds at the first execution and
// asks the compile-time registry, when one is given, the same question.
// [orig: WacScript_ResolveParameter @0x4f2c94..0x4f2eed; Script_SetCompileError
//  @0x4f2edf; Script_Compile's token buffer keeps the quote @0x4f3338]
static void test_ssn_slot_binds_unknown_tokens_like_net_id_zero() {
    // No net-id-0 entity: the name binds to 0xFFFF and the compile-time
    // registry logs the miss, non-fatally, ahead of the value-slot signature.
    BehaviorWorld w;
    Entity e; e.net_id = 7; e.item_id = 1; e.alive = true;
    w.registry.spawn(0, e);
    CompileEnv env; env.registry = &w.registry;
    Program program = compile_source(
        "if SSNexists(nosuchssn) then set(v1,1) endif\n"
        "if SSNexists(7) then set(v2,1) endif\n"
        "if SSNexists(\"7\") then set(v4,1) endif\n"
        "set(v3,nosuchssn)\n", env);
    CHECK(program.ok());
    CHECK(program.diagnostics.size() >= 3);
    CHECK(program.diagnostics[0].message == "Unknown SSN" && !program.diagnostics[0].error);
    CHECK(program.diagnostics[1].message == "Unknown SSN");
    CHECK(program.diagnostics[2].message == "  set (variable, value)");
    WacVm vm; vm.load(program); vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 0);
    CHECK(w.script.vars.get_mission(2) == 1);
    CHECK(w.script.vars.get_mission(4) == 0);
    CHECK(w.script.vars.get_mission(3) == 0); // a value slot is still the sink
    // A net-id-0 entity is what the name binds to, and the registry is silent.
    BehaviorWorld zero;
    Entity z; z.net_id = 0; z.item_id = 1; z.alive = true;
    zero.registry.spawn(0, z);
    CompileEnv zero_env; zero_env.registry = &zero.registry;
    Program bound = compile_source(
        "if SSNexists(nosuchssn) then set(v1,1) endif\n"
        "if SSNexists(\"7\") then set(v4,1) endif\n", zero_env);
    CHECK(bound.ok() && bound.diagnostics.empty());
    WacVm bound_vm; bound_vm.load(bound); bound_vm.execute(zero);
    CHECK(zero.script.vars.get_mission(1) == 1);
    CHECK(zero.script.vars.get_mission(4) == 1);
    // No compile-time registry: nothing to ask, so no diagnostic; the binding
    // still waits for the world and misses there.
    Program deferred = compile_source("if SSNexists(nosuchssn) then set(v1,1) endif\n", {});
    CHECK(deferred.ok() && deferred.diagnostics.empty());
    BehaviorWorld empty;
    WacVm deferred_vm; deferred_vm.load(deferred); deferred_vm.execute(empty);
    CHECK(empty.script.vars.get_mission(1) == 0);
}

// Retail tests the SSN leg (expectedType 11 / SSN_) before the AMMO leg
// (23 / AMMO_) [orig: WacScript_ResolveParameter @0x4f2c94 before @0x4f2cc5]:
// an AMMO_ token in an Ssn slot is atol'd to net id 0 and looked up, and an
// SSN_ token in an Ammo slot is the SSN leg's, never AMMO's. Both miss an
// empty registry with the leg's own "Unknown SSN" and bind silently once the
// net ids exist.
static void test_ssn_leg_precedes_ammo_leg() {
    BehaviorWorld w;
    CompileEnv env; env.registry = &w.registry;
    Program program = compile_source(
        "if SSNexists(AMMO_nosuch) then set(v1,1) endif\n"
        "ammorain(SSN_7)\n", env);
    CHECK(program.ok());
    CHECK(program.diagnostics.size() == 2);
    CHECK(program.diagnostics[0].message == "Unknown SSN" && !program.diagnostics[0].error);
    CHECK(program.diagnostics[1].message == "Unknown SSN" && !program.diagnostics[1].error);
    BehaviorWorld bound;
    Entity zero; zero.net_id = 0; zero.item_id = 1; zero.alive = true;
    Entity seven; seven.net_id = 7; seven.item_id = 1; seven.alive = true;
    bound.registry.spawn(0, zero);
    bound.registry.spawn(0, seven);
    CompileEnv bound_env; bound_env.registry = &bound.registry;
    Program bound_program = compile_source(
        "if SSNexists(AMMO_nosuch) then set(v1,1) endif\n"
        "ammorain(SSN_7)\n", bound_env);
    CHECK(bound_program.ok() && bound_program.diagnostics.empty());
}

static void test_outcome_cache_changes_on_next_execution() {
    BehaviorWorld w;
    CompileEnv env;
    Program program = compile_source(
        "if never then win(1) endif\n"
        "if true(1) then set(v1,GameOver) set(v2,WinVar) "
        "set(GameOver,7) set(v3,GameOver) endif\n", env);
    CHECK(program.ok());
    WacVm vm;
    vm.load(program);
    vm.execute(w);
    CHECK(w.match.outcome().winner_team == 1);
    CHECK(w.script.vars.get_mission(1) == 0);
    CHECK(w.script.vars.get_mission(2) == 0);
    CHECK(w.script.vars.get_mission(3) == 7);
    vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK(w.script.vars.get_mission(2) == 1);
    CHECK(w.script.vars.get_mission(3) == 7);
}

static void test_bms_event_query_reads_active_during_delay() {
    BehaviorWorld w;
    opennova::mission::BmsEventSystem events;
    opennova::bms::Event event{};
    event.delay = 2;
    events.load({event}, {}, {});
    w.add_system(&events);
    w.load_systems();
    CompileEnv env;
    Program program = compile_source(
        "if event(0) then set(v1,1) else set(v1,0) endif\n"
        "if event(1) then set(v2,1) else set(v2,0) endif\n"
        "if event(-1) then set(v3,1) else set(v3,0) endif\n", env);
    CHECK(program.ok());
    WacVm vm;
    vm.load(program);
    vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 0);
    for (int tick = 0; tick < 16; ++tick) w.run_logic_tick(true);
    CHECK(events.is_active(0));
    CHECK(!events.event_fired(0)); // BMS trigger waits for the delay; WAC does not
    vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 1);
    CHECK(w.script.vars.get_mission(2) == 0);
    CHECK(w.script.vars.get_mission(3) == 0);
    events.on_load(w);
    vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 0);
    events.load({}, {}, {}); // query follows the owner across table replacement
    vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 0);
    w.script.bms_events = nullptr;
    vm.execute(w);
    CHECK(w.script.vars.get_mission(1) == 0);
}

int main() {
    test_player_values_cache_at_bytecode_entry();
    test_outcome_cache_changes_on_next_execution();
    test_named_rows_are_lvalues_and_unresolved_arguments_sink();
    test_refed_tokens_and_stray_loads();
    test_quoted_tokens_outside_text_slots_are_the_null_leg();
    test_unknown_ifname_token_sinks_to_event_zero();
    test_ssn_slot_binds_unknown_tokens_like_net_id_zero();
    test_ssn_leg_precedes_ammo_leg();
    test_bms_event_query_reads_active_during_delay();
    test_distance_literals_and_lead_queries();
    test_script_ranges_drive_controller_and_perception();
    test_fractional_script_fog_uses_one_fixed_point_conversion();
    test_wac_positional_sound_and_teleport_quirk();
    test_scripted_respawn_counts_are_not_immediate_spawns();
    test_wac_area_and_location_queries();
    test_ssnuse_mounts_cached_child_and_clears_failed_choice();
    test_meride_reads_standing_carrier_and_remove_uses_command_group();
    test_player_group_loops_and_handle_aliases();
    test_named_group_actions_use_member_handles();
    test_npc_wac_health_names_and_boarding_consumer();
    test_ssn_rider_query_bounds_parent_depth_and_pool();
    test_arithmetic_assignment_and_retail_expression_order();
    test_named_event_reset_and_declared_variables();
    test_empty_server_holds_script_divider_after_boot();
    test_nested_conditions_and_accumulator_lifetime();
    test_do_sections_cycle_and_restore_independently();
    test_runtime_gaps_retain_source_and_restore_boot_evidence();
    test_execution_cadence();
	test_initial_execution_and_runtime_state();
    test_var_math();
    test_ssn_kill();
    test_temporal_past();
    test_temporal_predecessors_and_intervals();
    test_then_enter_leave_and_else();
    test_else_branch();
    test_environment();
    test_paren_less_music_success();
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
