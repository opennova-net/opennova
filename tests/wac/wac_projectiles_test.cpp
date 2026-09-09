#include <cstdio>
#include <memory>
#include <runtime/wac/compiler.h>
#include <runtime/wac/vm.h>
#include <runtime/wac/wac_system.h>
#include <runtime/world/world.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/simassets/sim_pose_provider.h>
#include <formats/threedi/threedi_3di3.h>

using namespace opennova::world;
using namespace opennova::wac;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

struct Fixture {
    std::unique_ptr<World> storage = std::make_unique<World>();
    World &world = *storage;
    Fixture() {
        for (int pool = 0; pool < 4; ++pool) world.registry.configure_pool(pool, 16);
        world.tables.ammo.entries.resize(3);
        auto &null = world.tables.ammo.entries[0];
        null.valid = true; null.name = "AT_NULL";
        for (int i = 1; i <= 2; ++i) {
            auto &ammo = world.tables.ammo.entries[i];
            ammo.valid = true;
            ammo.name = i == 1 ? "SCRIPT" : "ammo_fallback";
            ammo.velocity = 620; ammo.max_age_ticks = 62;
            ammo.flags = kAmmoFlagNoGravity;
            ammo.weight_in_grains = 875; ammo.max_damage = 25;
            ammo.ai_launch_set = "TEST_SHOT";
        }
    }
    Program compile_script(const std::string &text) {
        CompileEnv env; env.registry = &world.registry; env.ammo = &world.tables.ammo;
        return compile_source(text, env);
    }
    void script(const std::string &text) {
        const Program p = compile_script(text);
        CHECK(p.ok());
        WacVm vm; vm.load(p); vm.execute(world);
    }
    EntityHandle marker(int number, Vec3 position, int pool = 3, bool itemdef = true) {
        Entity e;
        e.item_id = kParticleEffectMarkerTypeId; e.has_item_def = itemdef;
        e.wp_number = number; e.position = position; e.yaw = 90;
        e.health = 0; e.alive = false; // no health gate in these scans
        return world.registry.spawn(pool, e);
    }
};

static void test_ammo_names_bind_indices_and_reject_unknown_names() {
    Fixture f;
    f.marker(77, {0, 0, 20});
    // Retail uppercases a bare token [orig: Script_Compile @0x4f3418], so the
    // AMMO_ prefix and the name walk are case-insensitive
    // [orig: AmmoDef_LookupByName @0x409870, stricmp].
    f.script("v1 = AMMO_SCRIPT\nv2 = AMMO_FALLBACK\n"
             "ammo2tgt(v1,77) store(v3)\nammo2tgt(aMmO_sCrIpT,77) store(v4)\n");
    CHECK(f.world.script.vars.get_mission(1) == 1);
    CHECK(f.world.script.vars.get_mission(2) == 2);
    CHECK(f.world.script.vars.get_mission(3) == 0 && f.world.script.vars.get_mission(4) == 0);
    CHECK(f.world.out.rounds.count == 2);
    CHECK(f.world.out.rounds.records[0].adm_index == 1);
    CHECK(f.world.out.rounds.records[1].adm_index == 1);
    CHECK(!f.compile_script("ammo2tgt(MISSING,77)\n").ok());
    CHECK(!f.compile_script("ammo2tgt(1,77)\n").ok()); // literal Ammo parameters are names
    CHECK(!f.compile_script("ammo2tgt(AMMO_AT_NULL,77)\n").ok());
    // A quoted token keeps its opening quote in retail's buffer [orig:
    // Script_Compile @0x4f3338]: the AMMO_ prefix never matches it and the
    // Ammo leg walks the names with the quoted text, an Unknown AMMO
    // [orig: WacScript_ResolveParameter @0x4f2cc5..0x4f2e8a].
    CHECK(!f.compile_script("ammo2tgt(\"AMMO_SCRIPT\",77)\n").ok());
    CHECK(!compile_source("v1 = AMMO_SCRIPT\n", {}).ok()); // unavailable table is observable
}

static void test_target_scan_and_projectile_damage() {
    Fixture f;
    f.marker(77, {99, 0, 20}, 2); // pool 2 is excluded
    f.marker(77, {88, 0, 20}, 3, false);
    const auto chosen = f.marker(77, {0, 0, 0.9f});
    f.marker(77, {77, 0, 20}); // first match wins
    Entity target;
    target.kind = EntityKind::Organic; target.has_item_def = true; target.item_type = 3;
    target.position = {5, 0, 0}; target.health = 100;
    const auto victim = f.world.registry.spawn(0, target);
    f.script("ammo2tgt(SCRIPT,77) store(v1)\n");
    CHECK(f.world.script.vars.get_mission(1) == 0);
    CHECK(f.world.out.rounds.count == 1 && f.world.round_sim.active_count == 1);
    const auto &round = f.world.out.rounds.records[0];
    CHECK(round.origin_x == 0 && round.origin_z == to_fixed(0.9f));
    CHECK(round.dir_yaw == 0 && round.dir_pitch == 0);
    CHECK(round.shooter_handle == 0xFFFF);
    f.world.round_sim.tick(f.world, nullptr);
    CHECK(f.world.registry.get(victim)->health == 75);
    CHECK(!f.world.round_sim.hits.empty());
    CHECK(f.world.registry.get(chosen)->health == 0);

    f.script("v2=0\nammo2tgt(v2,77) store(v3)\nammo2tgt(SCRIPT,999) store(v4)\n");
    CHECK(f.world.script.vars.get_mission(3) == 1 && f.world.script.vars.get_mission(4) == 1);
    CHECK(f.world.out.rounds.count == 1);
    // The command tests a dword, but the firing entry selects the low byte.
    f.script("v5=257\nammo2tgt(v5,77)\n");
    CHECK(f.world.out.rounds.count == 2 && f.world.out.rounds.records[1].adm_index == 1);
}

static void test_area_draws_use_ai_stream_and_actual_surface_height() {
    Fixture f;
    std::vector<uint16_t> heights(512 * 512, 10 * 256);
    std::vector<int> sectors(256, 1);
    opennova::terrain::TerrainHeightField terrain;
    terrain.heightmap = heights.data(); terrain.dim = 512;
    terrain.layout.sector_grid = sectors.data();
    f.world.ai.terrain = &terrain;
    f.world.ai.prng_a = 0;
    const Aabb bounds{{100, -100, 1000}, {101, -98, 1100}};
    f.world.registry.register_area("artillery", bounds, false, 91, bounds);
    const Program p = f.compile_script("ammoarea(SCRIPT,91) store(v1)\n");
    CHECK(p.ok());
    WacVm vm; vm.load(p); vm.execute(f.world);
    CHECK(f.world.script.vars.get_mission(1) == 0);
    CHECK(f.world.out.rounds.count == 1 && f.world.round_sim.active_count == 1);
    const auto &round = f.world.out.rounds.records[0];
    CHECK(round.origin_x == 100 * 65536 + 1);
    CHECK(round.origin_y == -100 * 65536 + 2 * 32785);
    CHECK(round.origin_z == 210 * 65536 + 50 * 33041);
    CHECK(round.dir_yaw == 0 && uint32_t(round.dir_pitch) == 0xC0000040u);
    CHECK(f.world.ai.prng_a == 1074823441u);
    CHECK(vm.capture_runtime_state().rng_seed == 0x12333333u);
    const auto saved_ai_seed = f.world.ai.prng_a;
    f.script("v2=0\nammoarea(v2,91) store(v3)\nammoarea(SCRIPT,92) store(v4)\n");
    CHECK(f.world.script.vars.get_mission(3) == 1 && f.world.script.vars.get_mission(4) == 1);
    CHECK(f.world.ai.prng_a == saved_ai_seed && f.world.out.rounds.count == 1);
    for (int i = 1; i <= 128; ++i)
        f.world.registry.register_area("", bounds, true, 1000 + i, bounds);
    f.script("ammoarea(SCRIPT,1128) store(v5)\n");
    CHECK(f.world.script.vars.get_mission(5) == 1); // the retail area table has 128 slots
    CHECK(f.world.ai.prng_a == saved_ai_seed);
}

static void test_rain_rng_exact_pose_and_restore() {
    Fixture f;
    Entity player; player.position = {100, 200, 5};
    const auto local = f.world.registry.spawn(0, player);
    f.world.cached.local_player = local;
    f.world.ai.attach(local);
    auto &body = *f.world.ai.for_handle(local);
    body.pos[0] = 100 * 65536 + 7;
    body.pos[1] = 200 * 65536 + 9;
    body.pos[2] = 5 * 65536 + 11;
    const Program p = f.compile_script("ammorain(SCRIPT) store(v1)\n");
    CHECK(p.ok());
    WacVm vm; vm.load(p);
    const auto baseline = vm.capture_runtime_state();
    vm.execute(f.world);
    CHECK(f.world.script.vars.get_mission(1) == 0);
    CHECK(f.world.out.rounds.count == 1);
    const auto first = f.world.out.rounds.records[0];
    CHECK(first.origin_x == 100 * 65536 + 7 + 1310700);
    CHECK(first.origin_y == 200 * 65536 + 9 + 1307400);
    CHECK(first.origin_z == 5 * 65536 + 11 + 1965056);
    CHECK(uint32_t(first.dir_pitch) == 0xC0000040u);
    CHECK(vm.capture_runtime_state().rng_seed == 0x66666624u);
    CHECK(f.world.ai.prng_a == 0);
    vm.restore_runtime_state(p, baseline);
    vm.execute(f.world);
    CHECK(f.world.out.rounds.records[1].origin_x == first.origin_x);
    CHECK(f.world.out.rounds.records[1].origin_y == first.origin_y);
    CHECK(f.world.out.rounds.records[1].origin_z == first.origin_z);
    const Program no_ammo = f.compile_script("v2=0\nammorain(v2) store(v3)\n");
    vm.load(no_ammo); vm.execute(f.world);
    CHECK(f.world.script.vars.get_mission(3) == 1);
    CHECK(vm.capture_runtime_state().rng_seed == 0x12333333u);
}

static void test_entry_authority_and_ceasefire_presentation() {
    Fixture f;
    f.marker(77, {0, 0, 20});
    Entity player; player.position = {100, 200, 5};
    f.world.cached.local_player = f.world.registry.spawn(0, player);
    f.world.rules.mp_session = true; f.world.rules.logic_authority = false;
    f.script("ammo2tgt(SCRIPT,77)\nammorain(SCRIPT)\n");
    CHECK(f.world.round_sim.fired.size() == 1); // source entry presents, NPC entry gates first
    CHECK(f.world.out.rounds.count == 0 && f.world.round_sim.active_count == 0);
    CHECK(f.world.out.source_fires.empty()); // no invented shooter for unowned client fire
    f.world.rules.logic_authority = true; f.world.rules.cease_fire = true;
    f.script("ammo2tgt(SCRIPT,77)\nammorain(SCRIPT)\n");
    CHECK(f.world.round_sim.fired.size() == 3);
    CHECK(f.world.out.rounds.count == 0 && f.world.round_sim.active_count == 0);
    f.world.rules.cease_fire = false;
    f.script("ammo2tgt(SCRIPT,77)\nammorain(SCRIPT)\n");
    CHECK(f.world.round_sim.fired.size() == 5);
    CHECK(f.world.out.rounds.count == 2 && f.world.round_sim.active_count == 2);
}


static void test_ssn_fire_uses_the_models_alternating_userpoints() {
    Fixture f;
    CollisionWorld collision;
    opennova::simassets::SimPoseProvider poses;
    opennova::threedi::ThreediUserPoint points[2] = {};
    points[0].x = 65536; points[0].y = 2 * 65536; points[0].z = 3 * 65536;
    points[1].x = -65536; points[1].z = 2 * 65536;
    // These authored directions are ignored when the caller passes null outDirection.
    points[0].rot_y = 65536; points[1].rot_z = -65536;
    opennova::threedi::Threedi3di3 model = {};
    model.user_points = points;
    model.user_point_count = 2;
    f.world.collision = &collision;
    f.world.pose_provider = &poses;
    Entity seed;
    seed.net_id = 1; seed.item_id = 11; seed.has_item_def = true;
    seed.kind = EntityKind::Item; seed.health = 100;
    seed.position = {10, 20, 30}; seed.yaw = 90;
    const auto shooter = f.world.registry.spawn(1, seed);
    const int model_id = collision.add_model(CollisionModel{});
    collision.assign_entity(shooter, model_id);
    poses.register_userpoint_model(model_id, &model);
    Entity target;
    target.net_id = 2; target.position = {-100, -200, -300};
    const auto victim = f.world.registry.spawn(0, target);
    const auto baseline = f.world.snapshot();

    f.script("ammo2ssn(SCRIPT,1,2) store(v1)\n");
    CHECK(f.world.script.vars.get_mission(1) == 1);
    CHECK(f.world.out.rounds.count == 1);
    CHECK(f.world.out.rounds.records[0].origin_x == 11 * 65536);
    CHECK(f.world.out.rounds.records[0].origin_y == 22 * 65536);
    CHECK(f.world.out.rounds.records[0].origin_z == 33 * 65536);
    CHECK(f.world.out.rounds.records[0].dir_yaw == 0);
    CHECK(f.world.out.rounds.records[0].dir_pitch == 0); // not aimed at victim
    CHECK(f.world.out.rounds.records[0].shooter_handle == shooter.packed);
    CHECK(f.world.round_sim.fired.size() == 1);
    CHECK(f.world.round_sim.fired[0].shooter_handle == shooter.packed);
    const auto entity = [&]() -> Entity & { return *f.world.registry.get(shooter); };
    CHECK(!entity().primary_occupant.valid());
    CHECK(entity().last_attacker == victim && entity().script_fire_userpoint_counter == 1);

    entity().primary_occupant = victim;
    f.script("ammo2ssn(SCRIPT,1,99) store(v2)\n");
    CHECK(f.world.script.vars.get_mission(2) == 1); // an unresolved target is accepted
    CHECK(f.world.out.rounds.count == 2);
    CHECK(f.world.out.rounds.records[1].origin_x == 9 * 65536);
    CHECK(f.world.out.rounds.records[1].origin_y == 20 * 65536);
    CHECK(f.world.out.rounds.records[1].origin_z == 32 * 65536);
    CHECK(f.world.out.rounds.records[1].shooter_handle == victim.packed);
    CHECK(f.world.round_sim.fired[1].shooter_handle == shooter.packed);
    CHECK(entity().primary_occupant == victim && !entity().last_attacker.valid());

    entity().script_fire_userpoint_counter = 255;
    f.world.rules.cease_fire = true;
    f.script("ammo2ssn(SCRIPT,1,2) store(v3)\n");
    CHECK(f.world.script.vars.get_mission(3) == 1 && entity().script_fire_userpoint_counter == 0);
    CHECK(f.world.out.rounds.count == 2 && f.world.round_sim.fired.size() == 3);
    CHECK(entity().primary_occupant == victim);
    const uint32_t last_stat = f.world.out.rounds.last_stat();
    f.world.restore(baseline);
    CHECK(!f.world.rules.cease_fire);
    CHECK(f.world.out.rounds.count == 0 && f.world.out.rounds.cursor == 0);
    CHECK(f.world.out.rounds.last_stat() == last_stat);
    CHECK(entity().script_fire_userpoint_counter == 0 && !entity().last_attacker.valid());
    CHECK(!entity().primary_occupant.valid());
    f.script("ammo2ssn(SCRIPT,1,2)\n");
    CHECK(f.world.out.rounds.count == 1 && f.world.out.rounds.records[0].origin_x == 11 * 65536);
    CHECK(f.world.out.rounds.records[0].stat == last_stat + 1);

    // ItemDef/health-word/model/zero-ammo gates precede the alternating byte.
    entity().has_item_def = false;
    f.script("ammo2ssn(SCRIPT,1,2) store(v4)\n");
    CHECK(f.world.script.vars.get_mission(4) == 0 && entity().script_fire_userpoint_counter == 1);
    entity().has_item_def = true; entity().health = 65536;
    f.script("ammo2ssn(SCRIPT,1,2) store(v4)\n");
    CHECK(f.world.script.vars.get_mission(4) == 0 && entity().script_fire_userpoint_counter == 1);
    entity().item_id = 0; entity().health = -1; entity().alive = false;
    f.script("set(v9,0)\nammo2ssn(v9,1,2) store(v4)\n");
    CHECK(f.world.script.vars.get_mission(4) == 0 && entity().script_fire_userpoint_counter == 1);
    f.script("ammo2ssn(SCRIPT,1,2) store(v4)\n");
    CHECK(f.world.script.vars.get_mission(4) == 1 && entity().script_fire_userpoint_counter == 2);
    f.world.rules.mp_session = true;
    f.world.rules.logic_authority = false;
    f.world.cached.local_player = shooter;
    f.script("ammo2ssn(SCRIPT,1,2)\n");
    CHECK(f.world.out.source_fires.size() == 1);
    CHECK(!entity().primary_occupant.valid());
    entity().primary_occupant = victim;
    const size_t presentations = f.world.round_sim.fired.size();
    f.script("ammo2ssn(SCRIPT,1,2)\n");
    CHECK(f.world.out.source_fires.size() == 1); // only the owning client sends
    CHECK(f.world.round_sim.fired.size() == presentations + 1);
    CHECK(entity().primary_occupant == victim);
    const uint8_t counter = entity().script_fire_userpoint_counter;
    f.world.pose_provider = nullptr;
    f.script("ammo2ssn(SCRIPT,1,2) store(v4)\n");
    CHECK(f.world.script.vars.get_mission(4) == 0 && entity().script_fire_userpoint_counter == counter);
}

int main() {
    test_ammo_names_bind_indices_and_reject_unknown_names();
    test_target_scan_and_projectile_damage();
    test_area_draws_use_ai_stream_and_actual_surface_height();
    test_rain_rng_exact_pose_and_restore();
    test_entry_authority_and_ceasefire_presentation();
    test_ssn_fire_uses_the_models_alternating_userpoints();
    std::printf(failures ? "WAC projectile tests FAILED (%d)\n" : "WAC projectile tests passed\n", failures);
    return failures ? 1 : 0;
}
