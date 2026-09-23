#include <cstdio>
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

struct Motion : IRootMotionSource {
    int missing_state = -1;
    bool has_clip(int, int state) const override {
        return state > 0 && state < 252 && state != missing_state;
    }
    int32_t clip_length_ticks(int, int, int) const override { return -1; }
    bool advance(int, int state, int32_t &phase, RootMotionFrame &out) override {
        if (!has_clip(0, state)) return false;
        ++phase;
        out = {};
        if (state == anim_state::kWalkForward) out.dx = 4096;
        return true;
    }
};

struct Fixture {
    World world;
    Motion motion;
    EntityHandle actor;
    Fixture() {
        for (int pool = 0; pool < 4; ++pool) world.registry.configure_pool(pool, 8);
        Entity e;
        e.net_id = 1; e.item_id = 11; e.has_item_def = true;
        e.item_type_index = 7; // the def row's ordinal (+0x1C)
        e.kind = EntityKind::Organic; e.item_type = 3; e.health = 100;
        actor = world.registry.spawn(0, e);
        world.cached.local_player = actor;
        world.ai.attach(actor);
        body().net_id = 1;
        body().health = 100;
        body().inf.active = true;
        body().inf.adm_id = 1;
        world.ai.root_motion = &motion;
        world.ai.is_authority = true;
    }
    AiEntity &body() { return *world.ai.for_handle(actor); }
    Entity &entity() { return *world.registry.get(actor); }
    void script(const char *source) {
        CompileEnv env;
        const auto program = compile_source(source, env);
        CHECK(program.ok());
        WacVm vm; vm.load(program); vm.execute(world);
    }
    int query(const char *source) {
        world.script.vars.set_mission(1, 0);
        script(source);
        return world.script.vars.get_mission(1);
    }
};

static void test_animation_reaches_motor_and_preserves_pending() {
    Fixture f;
    auto &inf = f.body().inf;
    inf.anim_pending = 149;
    inf.clip_phase = 7;
    f.entity().net_anim_pending = 149;
    f.script("SSNanim(1,ANIM_EMOTE_1) store(v1)\n");
    CHECK(f.world.script.vars.get_mission(1) == 1);
    CHECK(inf.anim_state == 115 && inf.anim_pending == 149);
    CHECK(f.entity().net_anim_state == 115 && f.entity().net_anim_pending == 149);
    CHECK(f.entity().body_anim_slot == body_anim_slot_from_state(115));
    f.world.ai.tick_infantry(f.body(), f.world, 1); // outside staggered think
    CHECK(inf.anim_state == 115 && inf.clip_phase > 0);
    CHECK(inf.anim_pending == 149);

    // The raw type index, not health or the resolved ItemDef, gates SSNanim.
    f.entity().health = 0;
    f.entity().has_item_def = false;
    f.script("SSNanim(1,emote_2) store(v2)\n");
    CHECK(f.world.script.vars.get_mission(2) == 1 && inf.anim_state == 116);
    f.entity().item_type_index = 0;
    f.script("SSNanim(1,emote_3) store(v3)\n");
    CHECK(f.world.script.vars.get_mission(3) == 0 && inf.anim_state == 116);
    f.script("anim(emote_4) store(v4)\n"); // local command only checks allocation
    CHECK(f.world.script.vars.get_mission(4) == 0 && inf.anim_state == 118);
    CHECK(inf.anim_pending == 149);
    // Every Anim operand is a state NAME, numbers included: "anim_118" names
    // nothing, so the slot holds 0 with the first error "Unknown ANIM".
    // [orig: WacScript_ResolveParameter @0x4F2EF2..0x4F2F91]
    CompileEnv env;
    for (const char *source : {"anim(ANIM_missing_state)\n", "anim(118)\n"}) {
        const Program missing = compile_source(source, env);
        CHECK(missing.ok() && missing.diagnostics.size() == 1 &&
                missing.diagnostics[0].message == "Unknown ANIM");
    }
    f.world.cached.local_player = {};
    f.script("anim(emote_5) store(v5)\n");
    CHECK(f.world.script.vars.get_mission(5) == 1 && inf.anim_state == 118);
}

static void test_force_animation_think_cadence_and_retry() {
    Fixture f;
    WacSystem system;
    f.world.add_system(&system);
    f.world.load_systems();
    f.body().inf.anim_pending = 149;
    f.script("forceanim(emote_1) store(v2)\n");
    CHECK(f.world.script.vars.get_mission(2) == 0);
    const auto baseline = f.world.snapshot();
    f.world.ai.tick_infantry(f.body(), f.world, 11); // key=47
    CHECK(f.body().inf.anim_state != 115);
    f.world.ai.tick_infantry(f.body(), f.world, 12); // key=48
    CHECK(f.body().inf.anim_state == 115);
    CHECK(f.body().inf.anim_pending == 149); // equality skips the arbiter
    CHECK(f.body().inf.move_mode == 0 && f.body().inf.target_dist == 0);
    f.script("forceanim(reset)\n");
    f.world.restore(baseline);
    CHECK(f.world.script.forced_animation == 115);
    f.world.load_systems();
    CHECK(f.world.script.forced_animation == 0);

    f.body().inf.is_local_player = true;
    f.body().inf.reset_body_animation();
    f.script("forceanim(emote_1)\n");
    f.world.ai.tick_infantry(f.body(), f.world, 12);
    CHECK(f.body().inf.anim_state != 115); // org2 has no forced-state consumer

    f.body().inf.is_local_player = false;
    f.world.ai.is_authority = false;
    f.world.ai.tick_infantry(f.body(), f.world, 28);
    CHECK(f.body().inf.anim_state != 115); // no local NPC think on a replica

    f.world.ai.is_authority = true;
    f.motion.missing_state = 115;
    f.body().inf.reset_body_animation();
    f.world.ai.tick_infantry(f.body(), f.world, 44);
    CHECK(f.body().inf.anim_state == 115); // missing ADM slot still keeps the state ID
}

static void test_fall_changes_only_live_z_and_wraps() {
    Fixture f;
    auto &body = f.body();
    body.pos[0] = 65536; body.pos[1] = -65536; body.pos[2] = 2147418112;
    body.inf.vel[2] = -99;
    body.inf.jump_cooldown = 7;
    f.entity().position = {1.0f, -1.0f, 32767.0f};
    const int32_t expected = static_cast<int32_t>(uint32_t(body.pos[2]) + 0x1F40000u);
    f.script("fall() store(v1)\n");
    CHECK(f.world.script.vars.get_mission(1) == 1);
    CHECK(body.pos[0] == 65536 && body.pos[1] == -65536 && body.pos[2] == expected);
    CHECK(f.entity().position.z == float(from_fixed(expected)));
    CHECK(body.inf.vel[2] == -99 && body.inf.jump_cooldown == 7);
    f.world.cached.local_player = {};
    f.script("fall() store(v2)\n");
    CHECK(f.world.script.vars.get_mission(2) == 0);
}

static void test_spawn_team_uses_the_spawn_registry() {
    Fixture f;
    constexpr const char *blue = "if IsPSPallteam(1) then set(v1,1) endif\n";
    CHECK(f.query(blue) == 0);
    // Organic and marker pools do not enter SpawnZoneList.
    f.entity().is_spawn_point = true;
    f.entity().team = 2;
    Entity point;
    point.item_id = 12; point.is_spawn_point = true; point.team = 1;
    const auto a = f.world.registry.spawn(1, point);
    point.health = 0; point.flags = 2;
    const auto b = f.world.registry.spawn(2, point);
    point.team = 2;
    f.world.registry.spawn(3, point);
    CHECK(f.world.zones.chain.zones.empty());
    CHECK(f.query(blue) == 1);
    f.world.registry.get(b)->team = 2; // dead spawn points still participate
    CHECK(f.query(blue) == 0);
    f.world.registry.get(a)->team = 255;
    f.world.registry.get(b)->team = 255;
    CHECK(f.query("if IsPSPallteam(-1) then set(v1,1) endif\n") == 1);
    CHECK(f.query("if IsPSPallteam(255) then set(v1,1) endif\n") == 0);
    CHECK(f.query("if IsPSPallteam(511) then set(v1,1) endif\n") == 0);
}

static void test_dropflare_reaches_round_sim_and_checks_itemdef() {
    Fixture f;
    f.world.tables.ammo.entries.resize(3);
    for (int i = 1; i <= 2; ++i) {
        auto &ammo = f.world.tables.ammo.entries[i];
        ammo.valid = true; ammo.name = i == 1 ? "FLARE" : "GROUND_FLARE";
        ammo.velocity = 10; ammo.max_age_ticks = 62; ammo.flags = kAmmoFlagNoGravity;
    }
    f.body().profile.type = 1;
    VehicleTraits traits;
    traits.flare_points = {{{65536, 0, 0}, {65536, 0, 0}},
                           {{-65536, 0, 0}, {65536, 0, 65536}}};
    f.world.vehicles.traits.set(f.entity().item_id, traits);
    f.entity().position = {100.0f, 200.0f, 5.0f};
    f.entity().yaw = 90;
    f.script("dropflare(1) store(v1)\n");
    CHECK(f.world.script.vars.get_mission(1) == 1);
    CHECK(f.world.out.rounds.count == 2);
    CHECK(f.world.out.rounds.records[0].adm_index == 1);
    CHECK(f.world.out.rounds.records[0].origin_x == 101 * 65536);

    f.entity().has_item_def = false;
    f.script("dropflare(1) store(v2)\n");
    CHECK(f.world.script.vars.get_mission(2) == 0 && f.world.out.rounds.count == 2);
    f.entity().has_item_def = true;
    f.entity().item_id = 0;
    f.entity().item_type_index = 0; // +0x1C is not this command's gate
    f.entity().health = 0;
    f.body().profile.type = 2;
    f.script("dropflare(1) store(v3)\n");
    CHECK(f.world.script.vars.get_mission(3) == 1 && f.world.out.rounds.count == 3);
    CHECK(f.world.out.rounds.records[2].adm_index == 2);
}


static void test_turn_reaches_the_motor_and_org2_word() {
    Fixture f;
    f.body().inf.body_heading = 0;
    f.body().heading = 0;
    f.entity().yaw = 90;
    f.script("ssnturn(1,0) store(v1)\n");
    CHECK(f.world.script.vars.get_mission(1) == 1);
    CHECK(f.body().inf.target_heading == 0x40000000);
    CHECK(f.body().heading == 0 && f.entity().yaw == 90);
    f.world.ai.tick_infantry(f.body(), f.world, 1);
    CHECK(f.body().heading == 60614190); // body carry followed by independent idle look chase
    CHECK(f.body().inf.body_heading == 69273360);

    f.entity().has_item_def = false;
    f.entity().health = 0; // neither gates the command
    f.script("ssnturn(1,91) store(v2)\n");
    CHECK(f.world.script.vars.get_mission(2) == 1);
    CHECK(f.body().inf.target_heading == -11927552);
    f.script("ssnturn(1,65536)\n");
    CHECK(f.body().inf.target_heading == 0x40000000); // wrapped first shift
    f.entity().item_type_index = 0;
    f.script("ssnturn(1,90) store(v3)\n");
    CHECK(f.world.script.vars.get_mission(3) == 0);
    CHECK(f.body().inf.target_heading == 0x40000000);
    f.entity().item_type_index = 7;

    for (int player_kind = 0; player_kind < 3; ++player_kind) {
        f.body().inf.is_local_player = player_kind == 0;
        f.body().net_is_remote_peer = player_kind == 1;
        f.entity().engine_flags = player_kind == 2 ? kEntityFlagPlayer : 0;
        f.body().inf.jump_cooldown = 7;
        f.script("ssnturn(1,91)\n");
        CHECK(f.body().inf.jump_cooldown == -11927552);
        CHECK(f.body().inf.target_heading == 0x40000000);
    }
    Entity prop;
    prop.net_id = 2; prop.item_id = 12; prop.yaw = 17; prop.item_type_index = 7;
    const auto h = f.world.registry.spawn(2, prop);
    f.script("ssnturn(2,0) store(v4)\nssnturn(99,0) store(v5)\n");
    CHECK(f.world.script.vars.get_mission(4) == 1);
    CHECK(f.world.script.vars.get_mission(5) == 0);
    CHECK(f.world.registry.get(h)->yaw == 17);
}

static void test_tele_copies_live_and_saved_position_without_resetting_actor() {
    Fixture f;
    Entity target;
    target.net_id = 2;
    target.item_id = 0; target.has_item_def = false; target.health = 0;
    target.position = {100.0f, 200.0f, 30.0f};
    target.yaw = 90; target.pitch = 45; target.roll = -30;
    const auto h = f.world.registry.spawn(1, target);
    f.world.ai.attach(h);
    const int32_t precise[3] = {6553601, 13107203, 1966105};
    auto *target_body = f.world.ai.for_handle(h);
    for (int axis = 0; axis < 3; ++axis) target_body->pos[axis] = precise[axis];
    f.entity().health = 0; f.entity().alive = false;
    f.entity().flags = kEntityFlagDead;
    f.entity().yaw = 31; f.entity().pitch = 7; f.entity().roll = -4;
    f.entity().saved_live_yaw = 123;
    f.entity().saved_live_pitch = 456;
    f.entity().saved_live_roll = 789;
    f.entity().saved_live_valid = true;
    f.entity().mounted = true; f.entity().mount_target = h;
    f.entity().mount_type = SeatType::Passenger;
    f.body().health = 0;
    f.body().heading = 111; f.body().pitch = 222; f.body().roll = 333;
    f.body().inf.vel[0] = 17; f.body().inf.vel[1] = -21; f.body().inf.vel[2] = 91;
    f.body().inf.jump_cooldown = 7;
    f.world.ai.capture_spawn_baseline(); // MissionKernel seals both owners
    const auto baseline = f.world.snapshot();

    f.script("tele(2) store(v1)\n");
    CHECK(f.world.script.vars.get_mission(1) == 1);
    CHECK(f.entity().health == 20000 && f.body().health == 20000);
    CHECK(!f.entity().alive && f.entity().flags == kEntityFlagDead);
    CHECK(f.entity().ground_target == h && f.entity().mount_toggle_fallback == h);
    CHECK(f.entity().mounted && f.entity().mount_target == h);
    CHECK(f.entity().mount_type == SeatType::Passenger);
    CHECK(f.entity().yaw == 31 && f.entity().pitch == 7 && f.entity().roll == -4);
    CHECK(f.body().heading == 111 && f.body().pitch == 222 && f.body().roll == 333);
    CHECK(f.entity().saved_live_yaw == 123 && f.entity().saved_live_pitch == 456 &&
          f.entity().saved_live_roll == 789 && f.entity().saved_live_valid);
    CHECK(f.body().inf.vel[0] == 17 && f.body().inf.vel[1] == -21 &&
          f.body().inf.vel[2] == 91 && f.body().inf.jump_cooldown == 7);
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(f.body().pos[axis] == precise[axis]);
        CHECK(f.body().net_saved_live_pose[axis] == precise[axis]);
        CHECK(f.entity().saved_live_pos[axis] == precise[axis]);
    }
    CHECK(f.entity().position.x == float(from_fixed(precise[0])));
    CHECK(f.entity().position.y == float(from_fixed(precise[1])));
    CHECK(f.entity().position.z == float(from_fixed(precise[2])));
    f.world.restore(baseline);
    CHECK(f.entity().health == 0 && f.body().health == 0);
    CHECK(!f.entity().ground_target.valid() && !f.entity().mount_toggle_fallback.valid());
    f.script("tele(99) store(v2)\n");
    CHECK(f.world.script.vars.get_mission(2) == 0 && f.entity().health == 0);
    f.world.cached.local_player = {};
    f.script("tele(2) store(v3)\n");
    CHECK(f.world.script.vars.get_mission(3) == 0 && f.entity().health == 0);
}

int main() {
    test_animation_reaches_motor_and_preserves_pending();
    test_force_animation_think_cadence_and_retry();
    test_fall_changes_only_live_z_and_wraps();
    test_spawn_team_uses_the_spawn_registry();
    test_turn_reaches_the_motor_and_org2_word();
    test_tele_copies_live_and_saved_position_without_resetting_actor();
    test_dropflare_reaches_round_sim_and_checks_itemdef();
    std::printf(failures ? "WAC actor tests FAILED (%d)\n" : "WAC actor tests passed\n", failures);
    return failures ? 1 : 0;
}
