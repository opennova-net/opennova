#include <formats/def/def.h>
#include <runtime/mission/item_traits.h>
#include <runtime/mission/event_runtime.h>
#include <runtime/wac/compiler.h>
#include <runtime/wac/wac_system.h>
#include <runtime/world/world.h>
#include <runtime/world/pose_provider.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include "common/retail_paths.h"
#include <base/resource_index/resource_index.h>
#include <runtime/mission/collision_resolve.h>
#include <runtime/assets/asset_store.h>
#include <runtime/world/entity_pose.h>
#include <runtime/world/collision.h>
#include <runtime/world/occlusion.h>

using namespace opennova;
using namespace opennova::world;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static EntityHandle door(World &w, int pool = 2, int group = 7, int count = 2,
        int step = 529) {
    Entity e;
    e.kind = EntityKind::Building;
    e.item_type = 5;
    e.item_id = 1998;
    e.has_item_def = true;
    e.group_id = static_cast<uint8_t>(group);
    e.door_count = static_cast<int8_t>(count);
    e.door_first_bone = 1;
    e.door_event = e.door_motion = true;
    const EntityHandle handle = w.registry.spawn(pool, e);
    CHECK(handle.valid());
    w.doors.initialize(*w.registry.get(handle), step, 0);
    return handle;
}

static void test_parse_and_bind() {
    const char source[] =
        "begin \"door\"\n id 101998\n type building\n ai_function door\n move_function door\n"
        " num_doors 2\n first_door 3\n open_rate 2\n max_angle 90\n"
        " door_type 1 0 1\n door_dir 0 1 1\n"
        " door_open_sound_id DOOR_OPN_MTL\n door_close_sound_id DOOR_CLS_MTL\nend\n"
        "begin \"aliases\"\n id 101999\n type building\n num_doors 8\n first_door 4\n"
        " deathtime 0\n door_dir 1 1\n clipsize 9\nend\n"
        "begin \"clamps\"\n id 102000\n num_doors 80\n first_door -1\n open_rate 0\nend\n";
    def::DefItemsFile items{};
    CHECK(def::def_parse_items_memory(reinterpret_cast<const uint8_t *>(source),
            sizeof(source) - 1, &items) == 0);
    CHECK(items.count == 3);
    if (items.count != 3) return;
    const auto &d = items.entries[0];
    CHECK(d.deathtime_ticks == 0x0202);
    CHECK((d.attrib & def::DEF_ITEM_ATTRIB_DOOR) != 0);
    CHECK(d.door_open_rate_q16 == 528);
    CHECK(d.door_max_angle_bam == 1073741823);
    CHECK(d.door_type == 10);
    CHECK(d.clipsize == 12);
    CHECK(std::strcmp(d.door_open_sound, "DOOR_OPN_MTL") == 0);
    CHECK(items.entries[1].deathtime_ticks == 558); // explicit zero defaults to 8 seconds + grace
    CHECK(items.entries[1].clipsize == 9);
    CHECK(items.entries[2].deathtime_ticks == 30);
    CHECK(items.entries[2].door_open_rate_q16 == 0);

    World w;
    w.registry.configure_pool(2, 4);
    Entity e;
    e.item_id = 1998;
    e.kind = EntityKind::Building;
    const auto h = w.registry.spawn(2, e);
    mission::resolve_item_traits(w, items, {});
    const Entity *bound = w.registry.get(h);
    CHECK(bound->door_event && bound->door_motion);
    CHECK(bound->door_count == 2 && bound->door_first_bone == 2);
    CHECK(w.doors.allocated() == 2);
    const DoorSystem::Slot *slot = w.doors.slot(*bound, 0);
    CHECK(slot != nullptr);
    if (slot != nullptr) CHECK(slot->step == 528);
    mission::resolve_item_traits(w, items, {});
    CHECK(w.doors.allocated() == 2);
    def::def_free_items(&items);
}

struct Pivot final : IPoseProvider {
    int last = -1;
    bool resolve_section_pivot(World &, EntityHandle, int section, int32_t out[3]) override {
        last = section;
        out[0] = 12 * 65536; out[1] = 3 * 65536; out[2] = 2 * 65536;
        return true;
    }
};

static void test_motion_contact_sound_restart() {
    World w;
    w.registry.configure_pool(2, 4);
    const auto h = door(w);
    Entity &e = *w.registry.get(h);
    std::strcpy(e.door_open_sound, "OPEN");
    std::strcpy(e.door_close_sound, "CLOSE");
    Pivot pivot;
    w.pose_provider = &pivot;
    auto baseline = std::make_unique<World::Snapshot>(w.snapshot());
    w.doors.command(w, e, 6, 2); // touch only the second section
    CHECK(w.doors.slot(e, 0)->state == 0 && w.doors.slot(e, 1)->state == 1);
    CHECK(w.out.slot_sounds.size() == 1);
    CHECK(pivot.last == 2 && w.out.slot_sounds[0].pos[0] == 12 * 65536);
    CHECK(e.class_think_ticks == 1920);
    w.doors.command(w, e, 6, 2); // opening touches do not restart/replay
    CHECK(w.out.slot_sounds.size() == 1);
    w.doors.command(w, e, 7);
    for (int i = 0; i < 123; ++i) w.doors.tick(w);
    CHECK(w.doors.slot(e, 0)->phase == 65067);
    CHECK(!w.doors.group_open(w, 7));
    CHECK(w.doors.passable_sections(e) == 6);
    w.doors.tick(w);
    CHECK(w.doors.slot(e, 0)->phase == 65536);
    CHECK(w.doors.group_open(w, 7));
    w.doors.command(w, e, 8);
    w.doors.tick(w);
    CHECK(w.doors.slot(e, 0)->phase == 65007);
    CHECK(w.doors.passable_sections(e) == 0);
    const size_t sounds = w.out.slot_sounds.size();
    w.doors.command(w, e, 6, 3); // touch while closing does not reverse
    CHECK(w.out.slot_sounds.size() == sounds);
    w.doors.command(w, e, 7);
    CHECK(w.doors.slot(e, 0)->state == 1);
    w.doors.tick(w);
    CHECK(w.doors.slot(e, 0)->phase == 65536);
    int32_t phases[30]{};
    CHECK(w.doors.write_phases(e, phases, 30) == 2 && phases[0] == 65536);
    w.restore(*baseline);
    Entity &reset = *w.registry.get(h);
    CHECK(w.doors.slot(reset, 0)->state == 0 && w.doors.slot(reset, 0)->phase == 0);
    CHECK(w.out.slot_sounds.empty());
    // Removed slots are never reused, and a new lifetime must not drive old state.
    w.registry.despawn(h);
    const auto replacement = door(w);
    CHECK(replacement == h);
    CHECK(w.registry.get(replacement)->door_slot == 2);
    CHECK(w.doors.allocated() == 4);
}

static void test_wac_and_bms_pool_scope() {
    World w;
    for (int p = 0; p < 4; ++p) w.registry.configure_pool(p, 4);
    const auto p0 = door(w, 0);
    const auto p1 = door(w, 1);
    const auto first = door(w, 2);
    const auto second = door(w, 2);
    // DoorOpen uses the first matching callback, even if a later one is open.
    w.doors.command(w, *w.registry.get(second), 7);
    for (int i = 0; i < 124; ++i) w.doors.tick(w);
    CHECK(!w.doors.group_open(w, 7));
    wac::CompileEnv env;
    wac::WacSystem script;
    script.set_program(wac::compile_source(
            "if never then opendoors(7) endif\n"
            "if dooropen(7) then set(v1,1) endif\n", env));
    w.cached.humans = 1;
    w.add_system(&script);
    w.load_systems();
    for (int i = 0; i < 62; ++i) w.run_logic_tick(true);
    CHECK(w.doors.slot(*w.registry.get(first), 0)->state == 1);
    CHECK(w.doors.slot(*w.registry.get(p1), 0)->state == 0);
    CHECK(w.doors.slot(*w.registry.get(p0), 0)->state == 0);
    for (int i = 0; i < 186; ++i) w.run_logic_tick(true);
    CHECK(w.script.vars.get_mission(1) == 1);
    mission::BmsEventSystem bms;
    opennova::bms::Action a;
    a.param1 = 7;
    a.action_type = opennova::bms::ActionType::GroupOpenDoorAction;
    bms.dispatch_action_for_test(w, a);
    CHECK(w.doors.slot(*w.registry.get(p1), 0)->state == 1);
    CHECK(w.doors.slot(*w.registry.get(p0), 0)->state == 0);
    a.action_type = opennova::bms::ActionType::GroupCloseDoorAction;
    bms.dispatch_action_for_test(w, a);
    CHECK(w.doors.slot(*w.registry.get(p1), 0)->state == 3);
    CHECK(w.doors.slot(*w.registry.get(first), 0)->state == 3);
}

static void test_pool_limit_and_wrapping_rate() {
    World w;
    w.registry.configure_pool(2, 335);
    EntityHandle last;
    for (int i = 0; i < 334; ++i) last = door(w, 2, 7, 30);
    CHECK(w.doors.allocated() == 10000);
    Entity &partial = *w.registry.get(last);
    CHECK(w.doors.slot(partial, 9) != nullptr);
    CHECK(w.doors.slot(partial, 10) == nullptr);
    const auto full = door(w, 2, 7, 2);
    CHECK(w.registry.get(full)->door_slot == -1);
    w.doors.command_group(w, 7, true, false);
    w.doors.tick(w);
    CHECK(!w.doors.group_open(w, 7));

    World wrap;
    wrap.registry.configure_pool(2, 1);
    Entity &e = *wrap.registry.get(door(wrap, 2, 7, 1, -1));
    wrap.doors.command(wrap, e, 7);
    wrap.doors.tick(wrap);
    CHECK(wrap.doors.slot(e, 0)->phase == -1);
    CHECK(wrap.doors.slot(e, 0)->state == 1);
}

static void test_retail_door_pose() {
    const std::string assets = retail::assets();
    if (assets.empty()) {
        retail::skip_leg("door retail pose (OPENNOVA_JO_ASSETS unset)");
        return;
    }
    ResourceIndex index;
    CHECK(index.scan(assets, std::string(), VfsMountMode::LooseOnly));
    assets::AssetStore models{&index};

    world::EntityPoseProvider pose;
    pose.set_assets(&models);
    CollisionWorld collision;
    OcclusionWorld occlusion;
    mission::CollisionResolveState resolve;
    World w;
    w.registry.configure_pool(2, 1);
    const char source[] = "begin \"Iblock01\"\n id 101998\n type building\n"
            " graphic Iblock01\n ai_function door\n move_function door\n"
            " first_door 2\n num_doors 1\n open_rate 2\nend\n";
    def::DefItemsFile items{};
    CHECK(def::def_parse_items_memory(reinterpret_cast<const uint8_t *>(source),
            sizeof(source) - 1, &items) == 0);
    Entity e;
    e.kind = EntityKind::Building;
    e.item_id = 1998;
    e.yaw = 90;
    const auto h = w.registry.spawn(2, e);
    mission::resolve_item_traits(w, items, {});
    const mission::CollisionResolveDeps deps{collision, occlusion, pose, models};
    CHECK(mission::resolve_collision_instances(w, items, resolve, deps) == 1);
    w.pose_provider = &pose;
    collision.set_pose_provider(&pose);
    w.collision = &collision;
    w.ai.collision = &collision;
    Entity &live = *w.registry.get(h);
    CHECK(w.doors.slot(live, 0) != nullptr);
    if (w.doors.slot(live, 0) == nullptr) { def::def_free_items(&items); return; }
    CollisionMatrix closed, open, fixed_closed, fixed_open;
    CHECK(collision.entity_section_matrix(w, h, 1, closed));
    CHECK(collision.entity_section_matrix(w, h, 0, fixed_closed));
    w.doors.command_group(w, 0, true, false);
    for (int i = 0; i < 124; ++i) w.doors.tick(w);
    CHECK(w.doors.slot(live, 0)->phase == 65472);
    CHECK(!w.doors.group_open(w, 0));
    w.doors.tick(w);
    CHECK(w.doors.group_open(w, 0));
    CHECK(collision.entity_section_matrix(w, h, 1, open));
    CHECK(collision.entity_section_matrix(w, h, 0, fixed_open));
    CHECK(std::memcmp(closed.m, open.m, sizeof(closed.m)) != 0);
    CHECK(std::memcmp(fixed_closed.m, fixed_open.m, sizeof(fixed_closed.m)) == 0);
    int32_t pivot[3]{};
    CHECK(pose.resolve_section_pivot(w, h, 1, pivot));
    std::printf("retail Iblock01: closed/open door transforms differ; building remains fixed\n");
    def::def_free_items(&items);
}

int main() {
    test_retail_door_pose();
    test_parse_and_bind();
    test_motion_contact_sound_restart();
    test_wac_and_bms_pool_scope();
    test_pool_limit_and_wrapping_rate();
    std::printf("doors: %d failure(s)\n", failures);
    return failures != 0;
}
