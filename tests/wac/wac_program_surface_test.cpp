// The WAC program surface the retired WacProgram / WacState bindings used to
// expose to GUT (wac_program_test.gd, ADR 0043 d10): the registry-less compile
// surface, the layered resource-root load, and the end-to-end execution of an
// installed program through the faithful 62-tick divider, the pause gate, the
// eager mission-start execution and the restart rewind — every case over the
// engine's own compiler, layered loader and WacSystem on a scripted world.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <formats/wac/program.h>
#include <runtime/mission/runtime_boot.h>
#include <runtime/wac/compiler.h>
#include <runtime/wac/wac_layered_load.h>
#include <runtime/wac/wac_system.h>
#include <runtime/world/world.h>

using namespace opennova;
using namespace opennova::wac;
using world::World;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

struct ScriptedWorld final : World {
    ScriptedWorld() { registry.configure_pool(0, 16); cached.humans = 1; }
};

void tick_n(World &w, int n) {
    for (int i = 0; i < n; ++i) w.run_logic_tick(/*is_authority=*/true);
}

mission::BootFileSource source_over(const std::map<std::string, std::string> *files) {
    mission::BootFileSource s;
    s.has_file = [files](const std::string &name) { return files->find(name) != files->end(); };
    s.read_file = [files](const std::string &name, std::vector<uint8_t> &out) {
        const auto it = files->find(name);
        if (it == files->end()) return false;
        out.assign(it->second.begin(), it->second.end());
        return true;
    };
    return s;
}

} // namespace

// compile_source: one good rule compiles clean — no diagnostics, one
// top-level event, a non-empty code stream.
static void test_compile_good_source() {
    CompileEnv env;
    const Program p = compile_source("if never() then set(v1,1) endif\n", env);
    CHECK(p.ok());
    CHECK(p.error_count() == 0);
    CHECK(p.event_count == 1);
    CHECK(!p.code.empty());
}

// The compiler is faithfully LENIENT (the original's parser recovers rather
// than rejecting): an unknown command still compiles, with a warning
// diagnostic, and the program installs. ok() reflects hard errors only.
static void test_lenient_compile_surfaces_warnings() {
    CompileEnv env;
    const Program p = compile_source("if never() then bogus_command_xyz(1) endif\n", env);
    CHECK(p.ok());
    CHECK(p.error_count() == 0);
    CHECK(!p.diagnostics.empty());
    if (!p.diagnostics.empty()) {
        CHECK(p.diagnostics[0].line > 0);
        CHECK(!p.diagnostics[0].message.empty());
        CHECK(!p.diagnostics[0].error); // lenient: a warning, not an error
    }
}

// Several sources compile into ONE program, events numbered across all of
// them — the original's game.wac -> server.wac -> <mission>.wac layering.
static void test_compile_sources_numbers_events_across_files() {
    CompileEnv env;
    const std::vector<std::string> sources = {
        "if never() then set(v1,1) endif\n",
        "if never() then set(v2,1) endif\nif never() then set(v3,1) endif\n",
    };
    const Program p = compile_program(sources, env);
    CHECK(p.ok());
    CHECK(p.event_count == 3);
}

// A root with only <mission>.wac: game.wac/server.wac skip silently
// [orig: WacScript_InitAndLoad], the mission file compiles and installs. No
// .wac anywhere is the BMS-only case — callers leave the VM unloaded.
static void test_layered_load_layers_and_skips_absent() {
    std::map<std::string, std::string> files;
    files["m01.wac"] = "if never() then set(v1,1) endif\n";
    {
        WacSystem sys;
        std::string error;
        const WacLayeredLoadStatus status = wac_layered_load(sys, source_over(&files), "m01",
                /*registry=*/nullptr, /*strict_diagnostics=*/false, error);
        CHECK(status == WacLayeredLoadStatus::kLoaded);
        CHECK(sys.program().event_count == 1);
        CHECK(sys.vm().loaded());
    }
    {
        std::map<std::string, std::string> empty;
        WacSystem sys;
        std::string error;
        const WacLayeredLoadStatus status = wac_layered_load(sys, source_over(&empty), "m01",
                /*registry=*/nullptr, /*strict_diagnostics=*/false, error);
        CHECK(status == WacLayeredLoadStatus::kAbsent);
        CHECK(!sys.vm().loaded());
    }
}

// An installed program on a scripted world: unloaded until a program is set,
// then executed only every 62nd tick [orig: dword_C6EAD4 / cmp 0x3E], never
// while the script-disable gate holds [orig: dword_C6EB28], and the
// completed-runs counter rewinds with the system's on_load (the restart).
static void test_installed_program_runs_at_the_62_tick_divider() {
    ScriptedWorld w;
    WacSystem sys;
    w.add_system(&sys);
    w.load_systems();
    CHECK(!sys.vm().loaded()); // no program installed -> VM unloaded (BMS-only case)

    // v1 starts 0, so eq(v1,0) fires on the first VM execution and sets v2=7.
    // The registry-aware compile (symbolic names resolve through the world).
    CompileEnv env;
    env.registry = &w.registry;
    Program p = compile_source("if eq(v1,0) then set(v2,7) endif\n", env);
    CHECK(p.ok());
    sys.set_program(std::move(p));
    CHECK(sys.vm().loaded());
    CHECK(sys.program().event_count == 1);

    tick_n(w, WacSystem::kTicksPerExecution - 1);
    CHECK(w.script.vars.get_mission(2) == 0); // 61 ticks: the divider has not fired yet
    tick_n(w, 1);
    CHECK(w.script.vars.get_mission(2) == 7);  // tick 62: the program ran and set v2
    CHECK(sys.runs() == 1);                     // one completed execution counted

    // Pause gate: a paused script never advances.
    w.script.vars.set_mission(2, 0);
    sys.paused = true;
    tick_n(w, 2 * WacSystem::kTicksPerExecution);
    CHECK(w.script.vars.get_mission(2) == 0); // paused: no executions
    sys.paused = false;

    // The restart: the system's on_load resets the accumulator + runs.
    w.load_systems();
    CHECK(sys.runs() == 0);
}

// Mission-start WAC is eager, idempotent per program, consumes no logic
// tick, and the sealed post-eager state restores on restart with the next
// execution still landing on the divider.
static void test_mission_start_wac_is_eager_idempotent_and_restartable() {
    ScriptedWorld w;
    WacSystem sys;
    CompileEnv env;
    env.registry = &w.registry;
    sys.set_program(compile_source("if never() then inc(v2) endif\n", env));
    w.add_system(&sys);
    w.load_systems();

    const uint32_t logic_tick_before = w.logic_tick;
    CHECK(sys.execute_initial(w));  // the authority executes startup WAC immediately
    CHECK(!sys.execute_initial(w)); // startup execution is idempotent per program
    CHECK(w.script.vars.get_mission(2) == 1);
    CHECK(sys.runs() == 1);
    CHECK(w.logic_tick == logic_tick_before); // eager WAC consumes no world logic tick
    const WacSystem::RuntimeState sealed = sys.capture_runtime_state();

    tick_n(w, WacSystem::kTicksPerExecution - 1);
    CHECK(sys.runs() == 1);
    tick_n(w, 1);
    CHECK(sys.runs() == 2);                     // the next WAC run remains tick 62
    CHECK(w.script.vars.get_mission(2) == 1);  // the startup edge does not refire

    sys.restore_runtime_state(sealed);
    CHECK(sys.runs() == 1);                     // restart restores the sealed post-eager VM
    CHECK(w.script.vars.get_mission(2) == 1);
    tick_n(w, WacSystem::kTicksPerExecution);
    CHECK(sys.runs() == 2);
    CHECK(w.script.vars.get_mission(2) == 1);
}

// A program installed before the first load applies on load and re-applies
// on a reload (the system re-loads its retained program in on_load), and the
// re-applied program executes.
static void test_installed_program_survives_a_reload() {
    ScriptedWorld w;
    WacSystem sys;
    CompileEnv env;
    sys.set_program(compile_source("if eq(v1,0) then set(v2,5) endif\n", env));
    w.add_system(&sys);
    w.load_systems();
    CHECK(sys.vm().loaded()); // the load applied the held program
    w.load_systems();         // reload
    CHECK(sys.vm().loaded()); // the held program re-applies on reload
    tick_n(w, WacSystem::kTicksPerExecution);
    CHECK(w.script.vars.get_mission(2) == 5); // the re-applied program executes
}

int main() {
    test_compile_good_source();
    test_lenient_compile_surfaces_warnings();
    test_compile_sources_numbers_events_across_files();
    test_layered_load_layers_and_skips_absent();
    test_installed_program_runs_at_the_62_tick_divider();
    test_mission_start_wac_is_eager_idempotent_and_restartable();
    test_installed_program_survives_a_reload();
    if (failures == 0) std::printf("OK: wac_program_surface\n");
    return failures == 0 ? 0 : 1;
}
