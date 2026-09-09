// The script command, local binding dispatcher, weapon FSM and load/retry
// lifecycle share one fire-control state. No Godot/input-device fixture needed.
#include <cstdio>

#include <runtime/wac/compiler.h>
#include <runtime/wac/wac_system.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/world.h>

using namespace opennova::world;
using namespace opennova::wac;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

struct Fixture {
    World world;
    LocalPlayerWeapon weapon;
    PlayerViewState view;
    EntityHandle player;

    explicit Fixture(int category = 3) {
        world.registry.configure_pool(0, 8);
        Entity entity;
        entity.kind = EntityKind::Organic;
        entity.alive = true;
        entity.health = 100;
        entity.equipped_adm_index = 1;
        player = world.registry.spawn(0, entity);
        world.cached.local_player = player;
        world.cached.humans = 1;
        world.tables.weapons.entries.resize(2);
        auto &def = world.tables.weapons.entries[1];
        def.valid = true;
        def.category = static_cast<uint8_t>(category);
        def.weapon_class_slot = 8; // not the WAC category
        weapon.active = true;
        weapon_fsm_bake(nullptr, 0, nullptr, nullptr, nullptr, weapon.def);
        weapon.def.clip_capacity = 30;
        weapon.def.actions[weapon_action::kFire].delay_end = 6;
        weapon.slot.clip = 30;
    }

    void script(const char *source) {
        CompileEnv env;
        const Program program = compile_source(source, env);
        CHECK(program.ok());
        WacVm vm;
        vm.load(program);
        vm.execute(world);
    }

    void pump(bool held, bool pressed = false) {
        ++world.logic_tick;
        weapon.fire_held = held;
        weapon.fire_pressed = pressed;
        LocalWeaponPumpIO io;
        io.view = &view;
        local_weapon_pump_tick(world, weapon, io);
    }
};

static void test_blocked_request_is_observable_and_unblock_fires() {
    Fixture f;
    f.script("blockfire(3,1)\n");
    f.pump(true, true);
    CHECK(f.weapon.slot.clip == 30);
    CHECK(f.weapon.fired_serial == 0);
    CHECK(f.world.script.weapon_input.fire_requested(3));
    CHECK(!f.world.script.weapon_input.fire_requested(8));
    f.script(
        "if weaponfired(3) then inc(v1) endif\n"
        "if weaponfired(3) then inc(v1) endif\n");
    CHECK(f.world.script.vars.get_mission(1) == 2); // query does not consume
    CHECK(!f.world.script.weapon_input.fire_requested(3)); // pass tail does
    f.pump(true); // a held semi-auto trigger is not a new binding request
    CHECK(!f.world.script.weapon_input.fire_requested(3));
    f.pump(true, true); // the block persists across script passes
    CHECK(f.weapon.slot.clip == 30);
    f.script("blockfire(3,0)\n");
    f.pump(true, true);
    CHECK(f.weapon.slot.clip == 29);
    CHECK(f.weapon.fired_serial == 1);
    CHECK(f.world.script.weapon_input.fire_requested(3));
}

static void test_auto_refire_passes_through_the_same_gate() {
    Fixture f;
    f.weapon.def.auto_fire = true;
    f.pump(true, true);
    CHECK(f.weapon.slot.clip == 29);
    for (int tick = 0; tick < 20 && !f.weapon.slot.refire_queued; ++tick)
        f.pump(true);
    CHECK(f.weapon.slot.refire_queued); // queued by the actual recoil handler
    f.script("blockfire(3,1)\n");
    CHECK(!f.world.script.weapon_input.fire_requested(3));
    f.pump(true);
    CHECK(f.world.script.weapon_input.fire_requested(3));
    CHECK(f.weapon.slot.clip == 29);
    for (int tick = 0; tick < 20; ++tick) f.pump(true);
    CHECK(f.weapon.fired_serial == 1);
    CHECK(f.weapon.slot.clip == 29);
}

static void test_block_does_not_revoke_an_accepted_delayed_shot() {
    Fixture f;
    f.weapon.def.actions[weapon_action::kFire].delay_start = 3;
    f.pump(true, true);
    CHECK(f.weapon.slot.current == weapon_action::kFire);
    CHECK(f.weapon.fired_serial == 0);
    f.script("blockfire(3,1)\n");
    for (int tick = 0; tick < 6; ++tick) f.pump(false);
    CHECK(f.weapon.fired_serial == 1);
    CHECK(f.weapon.slot.clip == 29);
    CHECK(!f.world.script.weapon_input.fire_requested(3));
}

static void test_power_throw_bypasses_the_ordinary_binding_latches() {
    Fixture f(5);
    f.weapon.def.flags = weapon_flag::kPowerThrow;
    f.script("blockfire(5,1)\n");
    f.pump(true, true);
    CHECK(f.weapon.power_throw_start_tick != 0);
    CHECK(f.weapon.slot.clip == 30);
    CHECK(!f.world.script.weapon_input.fire_requested(5));
    f.pump(false);
    CHECK(f.weapon.fired_serial == 1);
    CHECK(f.weapon.slot.clip == 29);
    CHECK(!f.world.script.weapon_input.fire_requested(5));
}

static void test_categories_and_seat_rejection() {
    Fixture f(10);
    f.script(
        "if blockfire(10,1) then inc(v1) endif\n"
        "if blockfire(-1,1) then inc(v1) endif\n"
        "if weaponfired(10) then inc(v1) endif\n"
        "if weaponfired(-1) then inc(v1) endif\n");
    CHECK(f.world.script.vars.get_mission(1) == 0);
    f.pump(true, true);
    CHECK(f.weapon.fired_serial == 1);
    CHECK(!f.world.script.weapon_input.fire_requested(10));

    Fixture driver;
    Entity *player = driver.world.registry.get(driver.player);
    player->mounted = true;
    player->mount_type = SeatType::Driver;
    driver.pump(true, true);
    CHECK(driver.weapon.slot.clip == 30);
    CHECK(driver.world.script.weapon_input.fire_requested(3));
}

static void test_script_divider_load_and_retry() {
    Fixture f;
    WacSystem system;
    CompileEnv env;
    system.set_program(compile_source(
        "if weaponfired(3) then inc(v1) endif\n", env));
    f.world.add_system(&system);
    f.world.load_systems();
    f.pump(true, true);
    for (int i = 0; i < WacSystem::kTicksPerExecution - 1; ++i)
        f.world.run_logic_tick(true);
    CHECK(f.world.script.weapon_input.fire_requested(3));
    CHECK(f.world.script.vars.get_mission(1) == 0);
    f.world.run_logic_tick(true);
    CHECK(f.world.script.vars.get_mission(1) == 1);
    CHECK(!f.world.script.weapon_input.fire_requested(3));

    f.script("blockfire(3,2)\n"); // every nonzero value blocks
    const World::Snapshot baseline = f.world.snapshot();
    f.script("blockfire(3,0)\n");
    f.world.script.weapon_input.record_fire_request(2);
    f.world.restore(baseline);
    CHECK(!f.world.script.weapon_input.fire_requested(2));
    CHECK(!f.world.script.weapon_input.record_fire_request(3)); // baseline block restored
    f.world.load_systems();
    CHECK(!f.world.script.weapon_input.fire_requested(3));
    CHECK(f.world.script.weapon_input.record_fire_request(3)); // fresh load unblocks
}

int main() {
    test_blocked_request_is_observable_and_unblock_fires();
    test_auto_refire_passes_through_the_same_gate();
    test_block_does_not_revoke_an_accepted_delayed_shot();
    test_power_throw_bypasses_the_ordinary_binding_latches();
    test_categories_and_seat_rejection();
    test_script_divider_load_and_retry();
    std::printf(failures ? "WAC weapon input tests FAILED (%d)\n" :
            "WAC weapon input tests passed\n", failures);
    return failures ? 1 : 0;
}
