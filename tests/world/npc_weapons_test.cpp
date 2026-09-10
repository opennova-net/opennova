// Organic ammo binding and event consumers through the real round path.
// [orig: Entity_InitOrganicAI @0x4BFCC0; Entity_UpdateInfantryAI @0x4BF322]
#include "common/test_paths.h"
#include <formats/threedi/threedi_build.h>
#include <base/io/bam.h>
#include <base/resource_index/resource_index.h>
#include <formats/def/def.h>
#include <runtime/simassets/item_traits.h>
#include <runtime/simassets/sim_model_cache.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/world.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>

using namespace opennova;
using namespace opennova::world;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)

struct Points final : IPoseProvider {
    std::vector<int> seen;
    EntityHandle parent_seen;
    int parent_point = 0;
    bool parent_resolves = true;
    bool resolve_organic_attachment(World &, EntityHandle, uint8_t index, int32_t out[3]) override {
        seen.push_back(index);
        if (index == 0 || index > 3) return false;
        out[0] = index * 65536 + 7;
        out[1] = index * -65536 + 11;
        out[2] = index * 131072 + 13;
        return true;
    }
    bool resolve_userpoint_transform(World &, EntityHandle parent, int index, int32_t out[6]) override {
        parent_seen = parent;
        parent_point = index;
        if (!parent_resolves || index == 0) return false;
        const int32_t pose[6] = {101, 202, 303, 404, 505, 606};
        for (int i = 0; i < 6; ++i) out[i] = pose[i];
        return true;
    }
};

struct Fixture {
    std::unique_ptr<World> heap = std::make_unique<World>();
    World &world = *heap;
    Points points;
    EntityHandle shooter, target;
    Fixture() {
        world.registry.configure_pool(0, 4);
        world.registry.configure_pool(1, 4);
        Entity seed;
        seed.item_id = 510; seed.item_type = 3; seed.has_item_def = true;
        seed.net_id = 7; seed.team = 1; seed.health = 100;
        shooter = world.registry.spawn(0, seed);
        seed.net_id = 8; seed.team = 2;
        target = world.registry.spawn(0, seed);
        world.ai.attach(shooter);
        auto &b = body();
        b.inf.active = true;
        b.pos[0] = 100; b.pos[1] = 200; b.pos[2] = 300;
        b.heading = 0x12340000; b.pitch = -0x01230000; b.roll = 0x00780000;
        b.inf.aim_heading = -123; b.inf.aim_pitch = 456;
        b.inf.recoil_pitch = 789; b.inf.aim_established = true;
        b.inf.combat_target = target; b.inf.magazine = 5;
        b.profile.organic.ammo = {1, 2, 3, 4};
        b.profile.organic.launch = {1, 2, 3};
        world.ai.is_authority = true;
        world.pose_provider = &points;
        world.tables.ammo.entries.resize(5);
        for (int i = 1; i <= 4; ++i) {
            auto &ammo = world.tables.ammo.entries[i];
            ammo.name = "ROUND" + std::to_string(i);
            ammo.valid = true; ammo.velocity = 62; ammo.max_age_ticks = 100;
        }
    }
    AiEntity &body() { return *world.ai.for_handle(shooter); }
    Entity &entity() { return *world.registry.get(shooter); }
    void fire(uint32_t tick) { world.ai.infantry_fire_pass(body(), world, tick); }
};

static void test_event_order_pose_and_magazine() {
    Fixture f;
    f.body().inf.last_events = 0x1C;
    f.fire(1);
    CHECK(f.world.out.rounds.count == 4 && f.world.round_sim.active_count == 4);
    CHECK((f.points.seen == std::vector<int>{1, 3, 2}));
    const int ids[4] = {1, 4, 2, 3};
    const int points[4] = {1, 3, 2, 2};
    for (int i = 0; i < 4; ++i) {
        const auto &event = f.world.out.rounds.records[i];
        CHECK(event.adm_index == ids[i] && event.mode_flags == 1);
        CHECK(event.shooter_handle == f.shooter.packed);
        CHECK(event.origin_x == points[i] * 65536 + 7);
        CHECK(event.origin_y == points[i] * -65536 + 11);
        CHECK(event.origin_z == points[i] * 131072 + 13);
        // Entity angles, not the aim solution, recoil, or attachment's Euler.
        CHECK(event.dir_yaw == f.body().heading && event.dir_pitch == f.body().pitch);
        CHECK(f.world.round_sim.rounds[i].ammo_index == ids[i]);
    }
    CHECK(f.body().inf.magazine == 4);
    CHECK(f.body().inf.last_advanced_ammo == 3);
    CHECK(f.body().inf.aim_ref0 == f.target);
    CHECK(!f.body().inf.fire_secondary_latch);
    CHECK(f.entity().equipped_adm_index == 0);
    CHECK((f.entity().flags & kEntityFlagPriorityTarget) != 0);
    CHECK(f.world.round_sim.fired.size() == 4);

    // Event bits wait for an odd tick; an already set walking latch does not.
    f.fire(2);
    CHECK(f.world.out.rounds.count == 4);
    f.body().inf.fire_secondary_latch = true;
    f.fire(2);
    CHECK(f.world.out.rounds.count == 6 && f.body().inf.magazine == 3);
    f.body().profile.organic.ammo[2] = 2; // duplicate advanced ammo is suppressed
    f.body().inf.fire_secondary_latch = true;
    f.fire(4);
    CHECK(f.world.out.rounds.count == 7 && f.body().inf.magazine == 2);
    CHECK(f.body().inf.last_advanced_ammo == 3); // duplicate does not rewrite it

    f.body().profile.organic.ammo[1] = 0;
    f.body().profile.organic.ammo[2] = 3;
    f.body().inf.fire_secondary_latch = true;
    f.fire(6);
    CHECK(f.world.out.rounds.count == 8 && f.body().inf.magazine == 2);
}

static void test_rejected_fire_still_consumes_caller_state() {
    Fixture f;
    f.body().profile.organic.ammo = {0, 2, 2, 0};
    f.body().inf.last_events = 0x8;
    f.body().inf.magazine = 0;
    f.world.rules.cease_fire = true;
    f.fire(1);
    CHECK(f.world.out.rounds.count == 0 && f.world.round_sim.active_count == 0);
    CHECK(f.world.round_sim.fired.size() == 1); // launch presentation survives cease-fire
    CHECK(f.body().inf.magazine == -1);

    f.world.rules.cease_fire = false;
    f.world.ai.is_in_session = true;
    f.world.ai.is_authority = false;
    f.world.rules.mp_session = true;
    f.world.rules.logic_authority = false;
    f.body().inf.magazine = -32768;
    f.body().inf.fire_secondary_latch = true;
    f.fire(2);
    CHECK(f.body().inf.magazine == 32767);
    CHECK(f.world.out.rounds.count == 0 && f.world.round_sim.fired.size() == 1);
    CHECK(f.entity().equipped_adm_index == 0);
    CHECK((f.entity().flags & kEntityFlagPriorityTarget) != 0);

    f.world.ai.is_authority = true;
    f.world.rules.logic_authority = true;
    f.body().profile.organic.ammo = {0, 255, 255, 0}; // missing table entry
    f.body().inf.fire_secondary_latch = true;
    f.fire(4);
    CHECK(f.body().inf.magazine == 32766 && f.world.out.rounds.count == 0);

    f.body().profile.organic.ammo.fill(0);
    f.body().inf.aim_ref0 = {};
    f.body().inf.last_events = 0x14;
    f.entity().equipped_adm_index = 99;
    f.fire(5);
    CHECK(f.body().inf.aim_ref0 == f.target && f.entity().equipped_adm_index == 0);
    CHECK(f.body().inf.magazine == 32766);
}

static void test_attachment_fallback_and_special_parent() {
    Fixture f;
    f.body().profile.organic.launch[0] = 0;
    int32_t pose[6];
    f.world.ai.organic_fire_pose(f.world, f.body(), 0, pose);
    CHECK(pose[0] == 100 && pose[1] == 200 && pose[2] == 300);
    CHECK(pose[3] == f.body().heading && pose[4] == f.body().pitch);

    Entity parent;
    parent.item_id = 9; parent.has_item_def = true; parent.item_attrib = 0x20;
    parent.position = {10, 20, 30};
    parent.weapon_userpoint_bytes[0][0] = 7;
    const auto carrier = f.world.registry.spawn(1, parent);
    f.entity().mount_target = carrier;
    f.entity().mount_type = SeatType::Gunner;
    f.world.ai.organic_fire_pose(f.world, f.body(), 0, pose);
    CHECK(f.points.parent_seen == carrier && f.points.parent_point == 7);
    CHECK(pose[0] == 101 && pose[3] == 404 && pose[4] == 505 && pose[5] == 606);
    f.points.parent_resolves = false;
    f.world.ai.organic_fire_pose(f.world, f.body(), 0, pose);
    CHECK(pose[0] == 10 * 65536 && pose[1] == 20 * 65536 && pose[2] == 30 * 65536);
    f.world.ai.attach(carrier);
    auto *parent_body = f.world.ai.for_handle(carrier);
    parent_body->pos[0] = 71; parent_body->pos[1] = 72; parent_body->pos[2] = 73;
    parent_body->heading = 74; parent_body->pitch = 75; parent_body->roll = 76;
    f.world.ai.organic_fire_pose(f.world, f.body(), 0, pose);
    CHECK(pose[0] == 71 && pose[3] == 74 && pose[4] == 75 && pose[5] == 76);
    f.world.registry.get(carrier)->item_attrib = 0;
    f.world.ai.organic_fire_pose(f.world, f.body(), 0, pose);
    CHECK(pose[0] == 100 && pose[3] == f.body().heading);
}

static void test_definition_names_byte_width_and_missing_resources() {
    const auto root = std::filesystem::path(test_paths_temp_dir()) /
            ("opennova_npc_weapons_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    CHECK(std::filesystem::create_directory(root));
    const auto path = root / "WEAPONS.3di";
    {
        threedi::ThreediBuildModel model;
        model.name = "WEAPONS";
        const int lod = model.add_lod();
        model.add_part(lod, 0, {});
        model.add_user_point("Muzzle", {}, {}, 0, 83);
        model.add_user_point("MUZZLE", {}, {}, 0, 83);
        model.add_user_point("Rocket", {}, {}, 0, 83);
        while (model.user_points.size() < 254) model.add_user_point("", {}, {}, 0, 83);
        model.add_user_point("Marker", {}, {}, 0, 83);
        model.add_user_point("Wrapped", {}, {}, 0, 83);
        model.add_user_point("Next", {}, {}, 0, 83);
        threedi::ThreediAssembled assembled;
        threedi::threedi_build_assemble(model, assembled);
        CHECK(threedi::threedi_3di3_write(path.string().c_str(), &assembled.model) == 0);
        ResourceIndex index;
        CHECK(index.scan(root.string(), "", VfsMountMode::LooseOnly));
        simassets::SimModelCache models;
        models.set_index(&index);
        CHECK(models.model_for("WEAPONS") != nullptr);
        def::DefItemDef definition{};
        definition.id = 100510;
        definition.clipsize = 65535;
        std::strcpy(definition.graphic, "WEAPONS");
        std::strcpy(definition.ammo_closeattack, "a");
        std::strcpy(definition.ammo_easyrocket, "B");
        std::strcpy(definition.ammo_advancedrocket, "C");
        std::strcpy(definition.ammo_marker3, "D");
        std::strcpy(definition.launchups_closeattack, "muzzle");
        std::strcpy(definition.launchups_rocket, "ROCKET");
        std::strcpy(definition.launchups_marker3, "Marker");
        def::DefItemsFile items{};
        items.entries = &definition; items.count = 1;
        Fixture f;
        for (int i = 1; i <= 4; ++i) f.world.tables.ammo.entries[i].name = std::string(1, char('A' + i - 1));
        CHECK(simassets::resolve_ai_weapons(f.world, items, f.shooter, &models) == 1);
        CHECK((f.body().profile.organic.ammo == std::array<uint8_t, 4>{1, 2, 3, 4}));
        CHECK((f.body().profile.organic.launch == std::array<uint8_t, 3>{1, 3, 255}));
        CHECK(f.body().inf.magazine == -1);

        f.world.tables.ammo.entries.resize(258);
        f.world.tables.ammo.entries[256].name = "WRAP";
        f.world.tables.ammo.entries[257].name = "NEXT";
        f.world.tables.ammo.entries[256].valid = true;
        f.world.tables.ammo.entries[257].valid = true;
        std::strcpy(definition.ammo_closeattack, "WRAP");
        std::strcpy(definition.ammo_easyrocket, "NEXT");
        std::strcpy(definition.ammo_advancedrocket, "MISSING");
        definition.ammo_marker3[0] = 0; // preserves the prior byte
        std::strcpy(definition.launchups_closeattack, "Wrapped");
        std::strcpy(definition.launchups_rocket, "Next");
        std::strcpy(definition.launchups_marker3, "Missing");
        simassets::resolve_ai_weapons(f.world, items, f.shooter, &models);
        CHECK((f.body().profile.organic.ammo == std::array<uint8_t, 4>{0, 1, 0, 4}));
        CHECK((f.body().profile.organic.launch == std::array<uint8_t, 3>{0, 1, 0}));
        definition.ammo_marker3[0] = 'D';
        f.world.tables.ammo.entries.clear();
        simassets::resolve_ai_weapons(f.world, items, f.shooter);
        CHECK((f.body().profile.organic.ammo == std::array<uint8_t, 4>{}));
        CHECK((f.body().profile.organic.launch == std::array<uint8_t, 3>{}));
        CHECK(f.body().inf.magazine == -1); // initialization still runs without ammo/models
    }
    // Remove only this fixture's known file and now-empty directory.
    CHECK(std::filesystem::remove(path));
    CHECK(std::filesystem::remove(root));
}

int main() {
    test_event_order_pose_and_magazine();
    test_rejected_fire_still_consumes_caller_state();
    test_attachment_fallback_and_special_parent();
    test_definition_names_byte_width_and_missing_resources();
    if (!failures) std::puts("npc_weapons: OK");
    return failures ? 1 : 0;
}
