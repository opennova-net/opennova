#include <cstdio>
#include <cstring>
#include <memory>
#include <runtime/wac/compiler.h>
#include <runtime/wac/vm.h>
#include <runtime/world/world.h>
#include <runtime/particle/effect_catalog_names.h>
#include <runtime/particle/script_effects.h>
#include <runtime/mission/promote.h>

using namespace opennova;
using namespace opennova::world;
using namespace opennova::wac;
namespace p = opennova::particle;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

static p::EffectSceneConfig catalog(bool stock = false) {
    p::EffectSceneConfig config;
    p::EffectCatalogDocument document;
    document.source = "script.ptl";
    p::ParticleDef particle;
    particle.id = "loop";
    particle.flags = p::particle_flag::ForeverEmit;
    particle.emit_burst = 1; particle.emit_rate = 20; particle.age = 0.05f;
    document.file.particles.push_back(particle);
    particle.id = "brief"; particle.flags = 0; particle.emit_dur = 0.016f;
    document.file.particles.push_back(particle);
    document.file.effects = {{"FLASH", {"loop"}}, {"BRIEF", {"brief"}}, {"EMPTY", {}}};
    if (stock) document.file.effects.push_back({"stockeffect", {"loop"}});
    config.documents.push_back(std::move(document));
    return config;
}

struct Fixture {
    std::unique_ptr<World> storage = std::make_unique<World>();
    World &world = *storage;
    p::EffectScene scene;
    // The compiler's name catalog (MissionKernel::script_effect_catalog), built
    // from the same documents the runtime scene opens.
    p::EffectCatalogNames names;
    Fixture(bool stock = false) {
        for (int pool = 0; pool < 4; ++pool) world.registry.configure_pool(pool, 16);
        const p::EffectSceneConfig config = catalog(stock);
        for (const auto &document : config.documents) names.add_document(document.file);
        scene.open(config);
    }
    Program compile_script(const std::string &source) {
        CompileEnv env; env.registry = &world.registry; env.effects = &names;
        return compile_source(source, env);
    }
    void script(const std::string &source) {
        const Program program = compile_script(source);
        CHECK(program.ok());
        WacVm vm; vm.load(program); vm.execute(world);
    }
    EntityHandle entity(int pool, int ssn, int number = 7, bool itemdef = true) {
        Entity entity;
        entity.net_id = uint16_t(ssn); entity.item_id = kParticleEffectMarkerTypeId;
        entity.has_item_def = itemdef; entity.wp_number = number;
        entity.position = {10, 20, 30}; entity.yaw = 90;
        entity.health = 0; entity.alive = false; // no health or alive gate
        entity.script_effect_name = "FLASH";
        return world.registry.spawn(pool, entity);
    }
    p::EffectSpawnReceipt present(size_t index) {
        // The shell re-interns every compile-time name into the one runtime
        // scene before spawning (GameWorld::route_script_effects), so a stock
        // clone a script bound at compile time answers later named lookups.
        for (const auto &name : names.interned_names()) scene.intern(name);
        const auto &event = world.out.script_effects[index];
        const uint64_t token = uint64_t(event.owner.packed) + 1;
        return p::spawn_script_effect(scene, event, {token}, {token});
    }
};

static void test_handles_gates_pose_and_restore() {
    Fixture f;
    const auto owner = f.entity(1, 1);
    f.world.logic_tick = 19;
    // A bare word reaches the catalog upper-cased. [orig: Script_Compile @0x4F3418..0x4F341D]
    f.script("v1=FX_FLASH\nfx2ssn(v1,1) store(v2)\nfx2ssn(fLaSh,1) store(v3)\n");
    CHECK(f.world.script.vars.get_mission(1) == 1);
    CHECK(f.world.script.vars.get_mission(2) == 1 && f.world.script.vars.get_mission(3) == 1);
    CHECK(f.world.out.script_effects.size() == 2);
    const auto &event = f.world.out.script_effects[0];
    CHECK(event.name == "FLASH" && event.owner == owner && event.release_previous);
    CHECK(event.position[0] == 10 * 65536 && event.position[1] == 20 * 65536);
    CHECK(event.position[2] == 30 * 65536);
    CHECK(event.direction[0] == 65536 && event.direction[1] == 0 && event.direction[2] == 0);
    CHECK(event.source_tick == 19 && event.source_order == 0);
    const auto baseline = f.world.snapshot();
    const auto first = f.present(0);
    CHECK(first.spawned());
    const auto group = f.scene.inspect(false).groups[0];
    CHECK(group.binding == p::EffectBinding::World); // owner identity does not follow the actor
    CHECK(group.pose.position.x == 10 && group.pose.position.y == 30 && group.pose.position.z == -20);
    CHECK(group.pose.forward.x == 1 && group.pose.forward.y == 0 && group.pose.forward.z == 0);
    f.world.out.script_effects.clear();
    f.world.registry.get(owner)->position.x = 88;
    f.script("fx2ssn(FLASH,1)\n");
    CHECK(f.world.out.script_effects[0].position[0] == 88 * 65536);
    f.world.restore(baseline);
    CHECK(f.world.out.script_effects.size() == 2);
    CHECK(f.world.out.script_effects[0].position[0] == 10 * 65536);
    f.scene.reset_runtime_state();
    CHECK(f.present(0).spawned()); CHECK(f.present(1).spawned());
    const auto restarted = f.scene.inspect(false);
    CHECK(restarted.groups.size() == 2);
    CHECK(restarted.groups[0].detached && !restarted.groups[1].detached);

    f.world.out.script_effects.clear();
    f.world.registry.get(owner)->has_item_def = false;
    f.script("fx2ssn(FLASH,1) store(v4)\nfx2ssn(FLASH,99) store(v5)\n"
             "v6=0\nfx2ssn(v6,1) store(v7)\n");
    CHECK(f.world.out.script_effects.empty());
    CHECK(f.world.script.vars.get_mission(4) == 0 && f.world.script.vars.get_mission(5) == 0);
    CHECK(f.world.script.vars.get_mission(7) == 0);
    CHECK(!f.compile_script("fx2ssn(MISSING,1)\n").ok());
    CHECK(!f.compile_script("fx2ssn(1,1)\n").ok()); // a literal FX argument is a name
    // A quoted token keeps its quote, so it names no effect.
    // [orig: Script_Compile @0x4F3338; WacScript_ResolveParameter @0x4F305F]
    CHECK(!f.compile_script("fx2ssn(\"FLASH\",1)\n").ok());
    CHECK(!compile_source("v1=FX_FLASH\n", {}).ok());

    Fixture fallback(true);
    fallback.entity(1, 1);
    fallback.script("fx2ssn(MISSING,1)\n");
    CHECK(fallback.present(0).spawned());
    CHECK(fallback.scene.inspect(false).groups[0].effect_name == "MISSING");
    CHECK(fallback.scene.find("MISSING")); // the existing stock clone is named
    CHECK(!fallback.scene.find("not interned"));
    const auto named = fallback.entity(3, 7);
    fallback.world.registry.get(named)->script_effect_name = "COMPILE_ONLY";
    fallback.script("v8=FX_COMPILE_ONLY\ntargetfx(7)\n");
    CHECK(fallback.present(1).spawned()); // clone created at compile time, never spawned by WAC
}

static void test_targets_and_bms_names() {
    Fixture f;
    f.entity(2, 1); // excluded pool
    f.entity(3, 2, 7, false); // missing ItemDef
    const auto first = f.entity(3, 3);
    const auto second = f.entity(3, 4);
    f.world.registry.get(first)->yaw = 0;
    f.world.registry.get(second)->script_effect_name = "MISSING";
    f.script("fx2tgt(FLASH,7) store(v1)\ntargetfx(7) store(v2)\n"
             "fx2tgt(FLASH,99) store(v3)\nv4=0\nfx2tgt(v4,7) store(v5)\n");
    CHECK(f.world.script.vars.get_mission(1) == 0 && f.world.script.vars.get_mission(2) == 0);
    CHECK(f.world.script.vars.get_mission(3) == 1 && f.world.script.vars.get_mission(5) == 1);
    CHECK(f.world.out.script_effects.size() == 3);
    const auto &target = f.world.out.script_effects[0];
    CHECK(target.owner == first && !target.release_previous && !target.lookup_by_name);
    CHECK(target.direction[0] == 0 && target.direction[1] == 65536 && target.direction[2] == 0);
    CHECK(f.world.out.script_effects[1].owner == first && f.world.out.script_effects[1].lookup_by_name);
    CHECK(f.world.out.script_effects[2].owner == second && f.world.out.script_effects[2].name == "MISSING");
    CHECK(f.world.out.script_effects[1].direction[0] == 0 && f.world.out.script_effects[1].direction[1] == 0);
    CHECK(f.present(0).spawned() && f.present(1).spawned());
    CHECK(!f.present(2).spawned()); // marker names do not fall back
    const auto groups = f.scene.inspect(false).groups;
    CHECK(groups.size() == 2 && !groups[0].detached && !groups[1].detached);

    bms::File mission{};
    bms::Entity marker{};
    marker.id = 77; marker.type = bms::ItemType::Marker;
    marker.type_id = kParticleEffectMarkerTypeId; marker.wp_number = 7;
    std::memset(marker.gen_string, 'x', 31); // the complete fixed field can be non-NUL
    mission.markers.push_back(marker);
    mission::promote_mission(mission, f.world);
    const auto promoted = f.world.registry.find_by_net_id(77);
    CHECK(promoted.valid());
    CHECK(f.world.registry.get(promoted)->script_effect_name == std::string(31, 'x'));
}

static void test_rain_random_and_exact_motor_pose() {
    Fixture f;
    const auto local = f.entity(0, 1);
    f.world.cached.local_player = local;
    f.world.ai.attach(local);
    auto &body = *f.world.ai.for_handle(local);
    body.pos[0] = 10 * 65536 + 7; body.pos[1] = 20 * 65536 + 9; body.pos[2] = 30 * 65536 + 11;
    body.heading = 0; body.body_pitch = int32_t(0x40000000u);
    f.script("fx2ssn(FLASH,1)\n");
    CHECK(f.world.out.script_effects[0].position[0] == 10 * 65536 + 7);
    CHECK(f.world.out.script_effects[0].direction[0] == 0 && f.world.out.script_effects[0].direction[2] == 65536);
    const Program program = f.compile_script("fxrain(FLASH) store(v1)\n");
    WacVm vm; vm.load(program);
    const auto baseline = vm.capture_runtime_state();
    vm.execute(f.world);
    const auto rain = f.world.out.script_effects.back();
    CHECK(f.world.script.vars.get_mission(1) == 0);
    CHECK(rain.owner == local && !rain.store_slot && !rain.release_previous);
    CHECK(rain.position[0] == 10 * 65536 + 7 + 157284); // signed high half of 0x66666624 * 6
    CHECK(rain.position[1] == 20 * 65536 + 9 + 156888);
    CHECK(rain.position[2] == 38 * 65536 + 11);
    CHECK(rain.direction[0] == 0 && rain.direction[1] == 0 && rain.direction[2] == -32768);
    CHECK(vm.capture_runtime_state().rng_seed == 0x66666624u && f.world.ai.prng_a == 0);
    vm.restore_runtime_state(program, baseline); vm.execute(f.world);
    CHECK(f.world.out.script_effects.back().position == rain.position);
    const Program zero = f.compile_script("v2=0\nfxrain(v2) store(v3)\n");
    vm.load(zero); vm.execute(f.world);
    CHECK(f.world.script.vars.get_mission(3) == 1 && vm.capture_runtime_state().rng_seed == 0x12333333u);
    f.world.commands.rain_effect(1, "FLASH", 0x8000FFFFu);
    CHECK(f.world.out.script_effects.back().position[0] == 10 * 65536 + 7 - 196608);
    CHECK(f.world.out.script_effects.back().position[1] == 20 * 65536 + 9 - 6);
}

static void test_release_store_and_death_callback_ownership() {
    Fixture f(true);
    const auto marker = f.entity(3, 1);
    const p::EffectSlotToken slot{uint64_t(marker.packed) + 1};
    f.script("fx2tgt(BRIEF,7)\nfx2tgt(FLASH,7)\n");
    CHECK(f.present(0).spawned()); CHECK(f.present(1).spawned());
    for (int tick = 0; tick < 20; ++tick) f.scene.advance_simulation({0.016f});
    auto groups = f.scene.inspect(false).groups;
    CHECK(groups.size() == 1 && groups[0].effect_name == "FLASH");
    f.scene.detach_slot(slot); // reaping the older BRIEF must not clear the newer slot
    CHECK(f.scene.inspect(false).groups[0].detached);

    f.scene.reset_runtime_state(); f.world.out.script_effects.clear();
    f.script("fx2tgt(FLASH,7)\n");
    CHECK(f.present(0).spawned());
    f.world.registry.get(marker)->script_effect_name = "unknown marker";
    f.script("targetfx(7)\n");
    CHECK(!f.present(1).spawned()); // strict name lookup despite stockeffect
    f.scene.detach_slot(slot);
    CHECK(!f.scene.inspect(false).groups[0].detached); // failed store cleared the slot, not the old group
    f.script("fx2ssn(EMPTY,1)\n");
    CHECK(!f.present(2).spawned());
    CHECK(!f.scene.inspect(false).groups[0].detached); // no longer in the slot

    f.scene.reset_runtime_state(); f.world.out.script_effects.clear();
    f.script("fx2ssn(FLASH,1)\nfx2ssn(EMPTY,1)\n");
    CHECK(f.present(0).spawned()); CHECK(!f.present(1).spawned());
    CHECK(f.scene.inspect(false).groups[0].detached); // release precedes failure

    auto capped = catalog(); capped.max_live_groups = 1;
    f.scene.open(capped);
    CHECK(f.present(0).spawned());
    const auto rejected = f.present(0);
    CHECK(rejected.status == p::EffectSpawnStatus::GroupCapacityReached);
    CHECK(f.scene.inspect(false).groups[0].detached);
}

int main() {
    test_handles_gates_pose_and_restore();
    test_targets_and_bms_names();
    test_rain_random_and_exact_motor_pose();
    test_release_store_and_death_callback_ownership();
    std::printf(failures ? "WAC effect tests FAILED (%d)\n" : "WAC effect tests passed\n", failures);
    return failures ? 1 : 0;
}
