#include <cstdio>
#include <formats/mus/mus.h>
#include <memory>
#include <runtime/wac/compiler.h>
#include <runtime/wac/vm.h>
#include <runtime/wac/wac_system.h>
#include <runtime/world/world.h>

using namespace opennova::world;
using namespace opennova::wac;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

struct Fixture {
    std::unique_ptr<World> storage = std::make_unique<World>();
    World &world = *storage;
    Fixture() { world.registry.configure_pool(0, 8); }
    void run(const std::string &source) {
        const auto program = compile_source(source, {}); CHECK(program.ok());
        WacVm vm; vm.load(program); vm.execute(world);
    }
    int32_t value(int index) const { return world.script.vars.get_mission(index); }
};

static void test_squad_selection_clear_and_retry() {
    Fixture f;
    Entity entity; entity.net_id = 7; entity.item_id = 1; entity.health = 100;
    const auto handle = f.world.registry.spawn(0, entity);
    auto &queue = f.world.script.squad_events;
    CHECK(queue.publish(handle.packed, 24, 7, 3) == 7);
    queue.publish(0x1002, 25, 8, 1);
    queue.publish(0x2003, 35, 7, 4);
    queue.publish(0x3004, 45, 9, 5);
    queue.publish(0x4005, 55, 10, 6); // replaces the smallest TTL, not the oldest slot
    f.run("squadevent(8) store(v1)\nsquadevent(7) store(v2)\n"
          "v3=SquadSSN\nv4=SquadWho\nSSNHP(SquadSSN,42)\n"
          "squadevent(99) store(v5)\nv6=SquadWho\n");
    CHECK(f.value(1) == 0 && f.value(2) == 1 && f.value(5) == 0);
    CHECK(f.value(3) == handle.packed && f.value(4) == 24 && f.value(6) == 24);
    CHECK(f.world.registry.get(handle)->health == 42); // named SSN carries a packed handle
    const auto baseline = f.world.snapshot();
    f.run("squadclear() store(v7)\nsquadevent(7) store(v8)\nv9=SquadWho\n");
    CHECK(f.value(7) == 0 && f.value(8) == 1 && f.value(9) == 35);
    f.world.restore(baseline);
    f.run("v10=SquadWho\nsquadclear()\nsquadevent(7) store(v11)\nv12=SquadWho\n");
    CHECK(f.value(10) == 24 && f.value(11) == 1 && f.value(12) == 35);
    CHECK(f.world.diagnostics.empty());
}

static void test_squad_ttl_is_per_execution_and_exports_are_mutable() {
    Fixture f;
    WacSystem system;
    system.set_program(compile_source(
            "squadevent(3) store(v1)\nv2=SquadWho\n", {}));
    f.world.add_system(&system);
    f.world.load_systems();
    f.world.cached.humans = 1;
    f.world.script.squad_events.publish(0x1234, 71, 3, 2);
    CHECK(system.execute_initial(f.world));
    CHECK(f.value(1) == 1 && f.value(2) == 71);
    for (int i = 0; i < 61; ++i) f.world.run_logic_tick(true);
    CHECK(f.value(1) == 1 && system.runs() == 1);
    f.world.run_logic_tick(true);
    CHECK(f.value(1) == 1 && system.runs() == 2); // observed before TTL reaches zero
    for (int i = 0; i < 62; ++i) f.world.run_logic_tick(true);
    CHECK(f.value(1) == 0 && f.value(2) == 71); // miss preserves selected exports
    f.run("set(SquadSSN,4660)\nset(SquadWho,-7)\n"
          "squadevent(99)\nv3=SquadSSN\nv4=SquadWho\n"
          "squadclear() store(v5)\nv6=SquadSSN\nv7=SquadWho\n");
    CHECK(f.value(3) == 4660 && f.value(4) == -7);
    CHECK(f.value(5) == 0 && f.value(6) == 0 && f.value(7) == 0);
    f.world.script.squad_events.publish(5, 6, 3, -1);
    f.run("squadevent(3) store(v8)\n");
    CHECK(f.value(8) == 1); // negative timers are active and continue decrementing
    f.world.load_systems();
    CHECK(!f.world.script.squad_events.query(3));
    CHECK(f.world.script.squad_events.selected_who == 0);
}

static void test_random_named_result_and_wide_signed_product() {
    Fixture f;
    // Fixed expected words from IMUL/add-0x8000/SHRD-16/+1 with seed 0x12333333.
    // Zero still consumes a draw; large limits must retain the high product word.
    f.run("random(0) store(v1)\nv2=RND\n"
          "random(2147483647) store(v3)\nv4=RND\n"
          "random(-2147483648) store(v5)\nv6=RND\n"
          "random(1) store(v7)\nv8=RND\n"
          "random(65536) store(v9)\nv10=RND\n");
    CHECK(f.value(1) == 1 && f.value(2) == 1);
    CHECK(f.value(3) == 0 && f.value(4) == 32145409);
    CHECK(f.value(5) == 0 && f.value(6) == -860946431);
    CHECK(f.value(7) == 1 && f.value(8) == 1);
    CHECK(f.value(9) == 0 && f.value(10) == 62355);
    const auto baseline = f.world.snapshot();
    f.run("set(RND,-17)\nv11=RND\n");
    CHECK(f.value(11) == -17);
    f.world.restore(baseline);
    f.run("v12=RND\n");
    CHECK(f.value(12) == 62355);
    CHECK(f.world.diagnostics.empty());
}

static void test_music_closed_stream_is_a_witnessed_success() {
    Fixture f;
    f.run("music(0) store(v1)\nmusic(-1) store(v2)\nmusic(2147483647) store(v3)\n");
    CHECK(f.value(1) == 1 && f.value(2) == 1 && f.value(3) == 1);
    CHECK(f.world.diagnostics.empty() && f.world.out.effects.entries().empty());
}

static void test_auto_dword_assignment_and_partial_cache_refresh() {
    Fixture f;
    Entity player; player.item_id = 1; player.health = 100; player.team = 1;
    const auto local = f.world.registry.spawn(0, player);
    player.team = 2;
    const auto selected = f.world.registry.spawn(0, player);
    CHECK(selected.packed == 1);
    f.world.cached.local_player = local;
    const Program program = compile_source(
            "if never then set(auto,196609) set(v1,auto) set(v2,Player) set(v3,Item) "
            "pisteam(2) store(v4) set(auto,-2147483647) set(v5,auto) "
            "set(auto,196609) endif\nv6=auto\n", {});
    CHECK(program.ok());
    WacVm vm; vm.load(program); vm.execute(f.world);
    CHECK(f.value(1) == 196609 && f.value(2) == 196609 && f.value(3) == 196609);
    CHECK(f.value(4) == 1); // entity consumers use the low-word selected handle
    CHECK(f.value(5) == -2147483647);
    CHECK(f.value(6) == 196609);
    const auto baseline = vm.capture_runtime_state();
    CHECK(baseline.auto_item == 0x00030001u);
    vm.execute(f.world);
    CHECK(f.value(6) == int32_t(0x00030000u | local.packed));
    f.world.cached.local_player = {};
    vm.execute(f.world);
    CHECK(f.value(6) == 0x0003FFFF); // missing-player cache writes only LOWORD
    vm.load(program);
    vm.restore_runtime_state(program, baseline);
    CHECK(vm.capture_runtime_state().auto_item == 0x00030001u);
    vm.execute(f.world);
    CHECK(f.value(6) == 0x0003FFFF);
}

static void test_auto_group_bindings_preserve_high_word_and_restore_it() {
    Fixture f;
    Entity player; player.item_id = 1; player.health = 100; player.team = 1;
    const auto local = f.world.registry.spawn(0, player);
    player.team = 2;
    const auto selected = f.world.registry.spawn(0, player);
    f.world.cached.local_player = local;
    const int group = f.world.registry.intern_group("picked");
    f.world.registry.set_script_group_members(group, {selected});
    CompileEnv env; env.registry = &f.world.registry;
    const Program program = compile_source(
            "if never then set(auto,196609) endif\n"
            "gloop G_picked v1=auto set(auto,327681) v2=auto end\nv3=auto\n", env);
    CHECK(program.ok());
    WacVm vm; vm.load(program); vm.execute(f.world);
    CHECK(f.value(1) == int32_t(0x00030000u | selected.packed));
    CHECK(f.value(2) == 327681);
    CHECK(f.value(3) == int32_t(0x00050000u | local.packed));
    const auto baseline = vm.capture_runtime_state();
    CHECK(baseline.auto_item == (0x00050000u | local.packed));
    f.world.cached.local_player = {};
    vm.execute(f.world);
    CHECK(f.value(1) == int32_t(0x00050000u | selected.packed));
    CHECK(f.value(3) == 0x0005FFFF); // exhausted loop restores absent local player
    vm.load(program);
    vm.restore_runtime_state(program, baseline);
    f.world.cached.local_player = local;
    vm.execute(f.world);
    CHECK(f.value(1) == int32_t(0x00050000u | selected.packed));
    CHECK(f.value(3) == int32_t(0x00050000u | local.packed));
}


static void test_music_operands_bind_the_actual_context_at_compilation() {
    using namespace opennova::mus;
    Fixture f;
    MusScript script{};
    int line = 0, column = 0; const char *error = nullptr;
    const int compiled = mus_compile("script t\nsection Begin\n{\nVar07 = 73\ndone\n}\n",
                                     &script, &line, &column, &error);
    CHECK(compiled == 0);
    if (compiled != 0) return;
    std::unique_ptr<MusVM, decltype(&mus_vm_destroy)> audio(mus_vm_create(), mus_vm_destroy);
    CHECK(audio != nullptr && !mus_vm_globals(audio.get()));
    // No context means every M# resolves to the same per-entry scratch slot.
    Program inactive = compile_source("set(m1,17) set(v1,m2)\n", {});
    CHECK(inactive.ok() && !inactive.music_globals);
    CHECK(mus_vm_load_script(audio.get(), &script) == 0);
    CompileEnv env; env.music_globals = mus_vm_globals(audio.get());
    CHECK(env.music_globals != nullptr);
    int notifications = 0;
    MusVMHooks hooks{};
    hooks.user = &notifications;
    hooks.on_var_changed = [](void *user, uint8_t, int32_t) { ++*static_cast<int *>(user); };
    mus_vm_set_hooks(audio.get(), &hooks);
    mus_vm_set_var(audio.get(), 7, 99);
    CHECK(notifications == 1);
    Program active = compile_source("set(v1,m7) set(m7,55) set(v2,m7)\n", env);
    CHECK(active.ok());
    WacVm vm; vm.load(active); vm.execute(f.world);
    CHECK(f.value(1) == 99 && f.value(2) == 55);
    CHECK(mus_vm_get_var(audio.get(), 7) == 55 && notifications == 1);
    // Actual MUS bytecode writes the same bank that the next WAC execution reads.
    mus_vm_start(audio.get()); mus_vm_tick(audio.get(), 16);
    CHECK(mus_vm_get_var(audio.get(), 7) == 73 && notifications == 2);
    vm.execute(f.world);
    CHECK(f.value(1) == 73 && mus_vm_get_var(audio.get(), 7) == 55);
    CHECK(notifications == 2); // raw WAC lvalues never notify the audio VM
    vm.load(inactive); vm.execute(f.world);
    CHECK(f.value(1) == 17 && mus_vm_get_var(audio.get(), 1) == 0);
    Program scratch_read = compile_source("set(v3,m1)\n", {});
    vm.load(scratch_read); vm.execute(f.world);
    CHECK(f.value(3) == 0); // scratch resets at execution entry

    // Restarting the context does not silently rebind an already-compiled operand
    // (retail stores the raw pointer at compile time). What that stale operand
    // points at is REIMPL-DEFINED from here on: retail frees the block on stop
    // and reallocates on open, leaving a dangling pointer [orig:
    // AudioVM_FreeSoundBuffer @0x672E1D; ScriptInstance_Init @0x672F8B]; the
    // reimpl keeps the retired block alive as a memory-safety decision
    // (mus-sbf-re.md, WAC access to music globals). Unreachable in the retail
    // mission flow.
    const auto retired = active.music_globals;
    mus_vm_unload_script(audio.get());
    CHECK(!mus_vm_globals(audio.get()));
    CHECK(mus_vm_load_script(audio.get(), &script) == 0);
    mus_vm_set_var(audio.get(), 7, 22);
    vm.load(active); vm.execute(f.world);
    CHECK(f.value(1) == 55 && mus_vm_get_var(audio.get(), 7) == 22);
    CHECK(mus_globals_read(*retired, 7) == 55);
    env.music_globals = mus_vm_globals(audio.get());
    Program replacement = compile_source("set(v4,m7) set(m7,88)\n", env);
    vm.load(replacement); vm.execute(f.world);
    CHECK(f.value(4) == 22 && mus_vm_get_var(audio.get(), 7) == 88);
    audio.reset();
    vm.execute(f.world); // reimpl-defined: the retired storage outlives its VM
    CHECK(f.value(4) == 88);
    mus_script_free(&script);
}

int main() {
    test_music_operands_bind_the_actual_context_at_compilation();
    test_auto_dword_assignment_and_partial_cache_refresh();
    test_auto_group_bindings_preserve_high_word_and_restore_it();
    test_squad_selection_clear_and_retry();
    test_squad_ttl_is_per_execution_and_exports_are_mutable();
    test_random_named_result_and_wide_signed_product();
    test_music_closed_stream_is_a_witnessed_success();
    std::printf("wac_state: %d failures\n", failures);
    return failures ? 1 : 0;
}
