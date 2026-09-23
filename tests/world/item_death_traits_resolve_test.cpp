// The collision sweep's death-trait mining (runtime/mission/collision_resolve
// over world::ItemDeathTraits), the cases the retired DestructionDebugCard
// binding served to GUT (simulation_test.gd's KZ / glass witnesses, ADR 0043
// d10): retail walks every exact, case-insensitive "KZ" point on the active
// FIRST husk and queues a radius-5 blast there; the unitType-11 bridge
// callback mines exact "DEAD" points from that same husk; a huskFinal-only
// definition supplies the live-model gate but no KZ source; authored husk
// names never stand in for the live retail pointer; and the intact model's
// GLASS userpoint reaches the traits only through retail's exact static
// model/surface table; the same sweep seeds a vehicle brain's intact and husk
// floors (brain[11]/[12]). Driven by a manual World, the model cache over a
// loose temp root holding byte-patched copies of the committed synthetic fixtures.
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/test_paths.h"

#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <formats/def/def.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/mission/collision_resolve.h>
#include <runtime/mission/item_traits.h>
#include <runtime/assets/asset_store.h>
#include <runtime/world/entity_pose.h>
#include <runtime/world/collision.h>
#include <runtime/world/ai.h>
#include <runtime/world/destruction.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/world.h>

using namespace opennova;
using namespace opennova::world;
using namespace opennova::def;
using namespace opennova::threedi;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

std::string repo_path(const char *rel) {
    return std::string(test_paths_repo_root(__FILE__)) + "/" + rel;
}

// Rename one 16-byte USRP name field in raw .3di bytes (whole-name match) —
// the byte-patch technique the retired GUT cases used, so a committed model
// can stand in for any retail userpoint name without another binary fixture.
std::vector<uint8_t> with_renamed_user_point(std::vector<uint8_t> bytes, const char *from,
                                             const char *to) {
    const size_t from_len = std::strlen(from);
    size_t offset = std::string::npos;
    for (size_t i = 0; i + from_len < bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, from, from_len) == 0 && bytes[i + from_len] == 0) {
            offset = i;
            break;
        }
    }
    CHECK(offset != std::string::npos);
    if (offset == std::string::npos) return bytes;
    for (size_t j = 0; j < 16; ++j) bytes[offset + j] = 0;
    const size_t to_len = std::strlen(to);
    for (size_t j = 0; j < to_len && j < 16; ++j) bytes[offset + j] = static_cast<uint8_t>(to[j]);
    return bytes;
}

// A fresh loose asset root under the temp dir.
struct TempRoot {
    std::string dir;
    explicit TempRoot(const char *tag) {
        static int serial = 0;
        dir = std::string(test_paths_temp_dir()) + "/opennova_death_traits_" + tag + "_" +
              std::to_string(++serial);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
    }
    ~TempRoot() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void put(const char *name, const std::vector<uint8_t> &bytes) const {
        CHECK(test_io::write_file(dir + "/" + name, bytes));
    }
};

// The items table: the committed fixture file, or an inline text.
struct Items {
    DefItemsFile file{};
    bool ok = false;
    explicit Items(const std::vector<uint8_t> &text) {
        ok = def_parse_items_memory(text.data(), text.size(), &file) == 0;
        CHECK(ok);
    }
    ~Items() { def_free_items(&file); }
};

std::vector<uint8_t> text_bytes(const char *text) {
    return std::vector<uint8_t>(text, text + std::strlen(text));
}

// One placed building of `item_id` (raw type: items.def id - 100000), the
// engine systems a resolve writes into, and the sweep itself.
struct Rig {
    ResourceIndex index;
    assets::AssetStore models{&index};
    world::EntityPoseProvider pose;
    CollisionWorld collision;
    OcclusionWorld occlusion;
    mission::CollisionResolveState state;
    World w;
    EntityHandle building;

    Rig(const TempRoot &root, const Items &items, int32_t item_id) {
        CHECK(index.scan(root.dir, std::string(), VfsMountMode::LooseOnly));

        pose.set_assets(&models);
        w.registry.configure_pool(0, 8);
        w.registry.configure_pool(1, 8);
        w.registry.configure_pool(2, 8);
        Entity e;
        e.kind = EntityKind::Building;
        e.item_id = item_id;
        e.bms_id = 1;
        e.health = 100;
        e.alive = true;
        building = w.registry.spawn(2, e);
        CHECK(building.valid());
        // The trait rows the sweep fills come from the items.def fold first.
        mission::resolve_item_traits(w, items.file, [](int) -> uint8_t { return 0; });
        const mission::CollisionResolveDeps deps{collision, occlusion, pose, models};
        CHECK(mission::resolve_collision_instances(w, items.file, state, deps) == 1);
    }
    const ItemDeathTraits *traits(int32_t item_id) const {
        return w.tables.item_death_traits.get(item_id);
    }
};

// Every exact, case-insensitive `name` userpoint of a raw model, in the
// mission-local frame the destruction core consumes (decoded model space is
// (-source y, source z, source x): the sweep maps (x, y, z) <- (z, -x, y)).
void expected_points(const std::vector<uint8_t> &bytes, const char *name,
                     std::vector<Vec3> &positions, std::vector<Vec3> &directions) {
    Threedi3di3 model{};
    CHECK(threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) == 0);
    for (size_t i = 0; model.user_points != nullptr && i < model.user_point_count; ++i) {
        const ThreediUserPoint &point = model.user_points[i];
        if (!strutil::iequals(point.name, name)) continue;
        float pos[3];
        float dir[3];
        threedi_user_point_position(&point, pos);
        threedi_user_point_direction(&point, dir);
        positions.push_back(Vec3{pos[2], -pos[0], pos[1]});
        directions.push_back(Vec3{dir[2], -dir[0], dir[1]});
    }
    threedi_3di3_free(&model);
}

bool same(const Vec3 &a, const Vec3 &b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

} // namespace

// The first husk's KZ bank: both "KZ" and "kz" reach the traits with their
// mission-local axes, the live-model gate lifts, and the DEAD bank is mined
// from the same husk independently of KZ.
static void test_first_husk_kz_and_dead_points_feed_death_traits() {
    const std::vector<uint8_t> house = test_io::read_file(repo_path("fixtures/threedi/synth/house.3di"));
    const std::vector<uint8_t> armory = test_io::read_file(repo_path("fixtures/threedi/synth/armory.3di"));
    const Items items(test_io::read_file(repo_path("fixtures/def/items.def")));
    CHECK(!house.empty() && !armory.empty() && items.ok);
    if (house.empty() || armory.empty() || !items.ok) return;

    // items.def 105002 "Barrel": graphic Barrel1, husk Barrel1X, huskfinal
    // Barrel1XF. The husk is the armory with its two userpoints relabelled
    // KZ / kz; the root deliberately omits Barrel1XF (KZ belongs to the first
    // husk, the final husk is only the preferred death-piece model).
    std::vector<uint8_t> kz_bytes = with_renamed_user_point(armory, "Armory", "KZ");
    kz_bytes = with_renamed_user_point(kz_bytes, "Ground", "kz");
    std::vector<Vec3> expected;
    std::vector<Vec3> ignored;
    expected_points(kz_bytes, "KZ", expected, ignored);
    CHECK(expected.size() == 2); // the fixture carries a real multi-point KZ bank
    {
        const TempRoot root("kz");
        root.put("Barrel1.3di", house);
        root.put("Barrel1X.3di", kz_bytes);
        const Rig rig(root, items, 5002);
        const ItemDeathTraits *t = rig.traits(5002);
        CHECK(t != nullptr);
        if (t == nullptr) return;
        CHECK(t->has_husk);
        CHECK(t->husk_model_loaded); // a successfully opened first husk supplies the retail live-model gate
        CHECK(t->kz_points.size() == expected.size()); // all first-husk KZ points reach the traits
        for (size_t i = 0; i < t->kz_points.size() && i < expected.size(); ++i)
            CHECK(same(t->kz_points[i], expected[i])); // retail mission-local axes preserved
        CHECK(t->bridge_dead_points.empty());
    }

    // The sibling unitType-11 callback mines exact case-insensitive DEAD
    // points from the same first husk; the sweep retains that bank
    // independently of KZ.
    const std::vector<uint8_t> dead_bytes = with_renamed_user_point(armory, "Armory", "dEaD");
    std::vector<Vec3> expected_dead;
    expected_points(dead_bytes, "DEAD", expected_dead, ignored);
    CHECK(expected_dead.size() == 1);
    {
        const TempRoot root("dead");
        root.put("Barrel1.3di", house);
        root.put("Barrel1X.3di", dead_bytes);
        const Rig rig(root, items, 5002);
        const ItemDeathTraits *t = rig.traits(5002);
        CHECK(t != nullptr);
        if (t == nullptr) return;
        CHECK(t->bridge_dead_points.size() == 1);
        if (t->bridge_dead_points.size() == 1 && expected_dead.size() == 1)
            CHECK(same(t->bridge_dead_points[0], expected_dead[0]));
        CHECK(t->kz_points.empty());
    }
}

// Retail reads entity+52 huskModel for the KZ walk. A final-only definition
// may use huskFinal for pieces (and the legacy collision fallback), but it
// must not mine that model for KZ anchors; the empty bank selects the origin
// fallback blast.
static void test_final_only_husk_supplies_the_gate_but_no_kz_source() {
    const std::vector<uint8_t> house = test_io::read_file(repo_path("fixtures/threedi/synth/house.3di"));
    const std::vector<uint8_t> armory = test_io::read_file(repo_path("fixtures/threedi/synth/armory.3di"));
    const Items items(text_bytes(
            "begin \"Final-only KZ witness\"\n"
            "  id 105099\n"
            "  type object\n"
            "  graphic Barrel1\n"
            "  sid final_only_kz\n"
            "  huskfinal Barrel1XF\n"
            "  hp 75\n"
            "  kz 4.0\n"
            "  unit_type 6\n"
            "end\n"));
    CHECK(!house.empty() && !armory.empty() && items.ok);
    if (house.empty() || armory.empty() || !items.ok) return;
    const TempRoot root("final");
    root.put("Barrel1.3di", house);
    root.put("Barrel1XF.3di", with_renamed_user_point(armory, "Armory", "KZ"));
    const Rig rig(root, items, 5099);
    const ItemDeathTraits *t = rig.traits(5099);
    CHECK(t != nullptr);
    if (t == nullptr) return;
    CHECK(t->husk_model_loaded); // a successfully opened final-only husk also supplies the retail gate
    CHECK(t->kz_points.empty()); // huskFinal alone does not replace retail's first-stage KZ source
}

// Authored names do not stand in for the live retail pointer: a root that
// resolves the main graphic but neither husk leaves the callback gate clear.
static void test_missing_husk_assets_leave_the_live_model_gate_clear() {
    const std::vector<uint8_t> house = test_io::read_file(repo_path("fixtures/threedi/synth/house.3di"));
    const Items items(test_io::read_file(repo_path("fixtures/def/items.def")));
    CHECK(!house.empty() && items.ok);
    if (house.empty() || !items.ok) return;
    const TempRoot root("missing");
    root.put("Barrel1.3di", house);
    const Rig rig(root, items, 5002);
    const ItemDeathTraits *t = rig.traits(5002);
    CHECK(t != nullptr);
    if (t == nullptr) return;
    CHECK(t->has_husk);            // items.def still records the authored husk name
    CHECK(!t->husk_model_loaded);  // missing/corrupt husk assets leave the retail live-model gate clear
    CHECK(t->kz_points.empty());
}

// Terrain_SpawnEffectsAtUserPoint first selects one hard-coded retail
// model/surface pair, then resolves that exact point case-insensitively: the
// eurhr2 row maps GLASS, a near-name model is not in the static table.
static void test_retail_glass_model_maps_exact_userpoint_into_death_traits() {
    const std::vector<uint8_t> armory = test_io::read_file(repo_path("fixtures/threedi/synth/armory.3di"));
    CHECK(!armory.empty());
    if (armory.empty()) return;
    const std::vector<uint8_t> glass_bytes = with_renamed_user_point(armory, "Armory", "gLaSs");
    std::vector<Vec3> expected_pos;
    std::vector<Vec3> expected_dir;
    expected_points(glass_bytes, "GLASS", expected_pos, expected_dir);
    CHECK(expected_pos.size() == 1 && expected_dir.size() == 1);
    {
        const Items items(text_bytes(
                "begin \"Retail glass witness\"\n"
                "  id 105099\n"
                "  type object\n"
                "  graphic eurhr2\n"
                "  hp 1000\n"
                "end\n"));
        CHECK(items.ok);
        if (!items.ok) return;
        const TempRoot root("glass");
        root.put("eurhr2.3di", glass_bytes);
        const Rig rig(root, items, 5099);
        const ItemDeathTraits *t = rig.traits(5099);
        CHECK(t != nullptr);
        if (t == nullptr) return;
        CHECK(t->glass_points.size() == 1);
        if (t->glass_points.size() == 1 && expected_pos.size() == 1) {
            CHECK(same(t->glass_points[0].local_pos, expected_pos[0])); // mission-local point axes
            CHECK(same(t->glass_points[0].local_dir, expected_dir[0])); // the authored shatter orientation
        }
    }
    {
        // A near-name is not in retail's static table, even with the same userpoint.
        const Items items(text_bytes(
                "begin \"Near-name glass witness\"\n"
                "  id 105098\n"
                "  type object\n"
                "  graphic eurhr2x\n"
                "  hp 1000\n"
                "end\n"));
        CHECK(items.ok);
        if (!items.ok) return;
        const TempRoot root("glassx");
        root.put("eurhr2x.3di", glass_bytes);
        const Rig rig(root, items, 5098);
        const ItemDeathTraits *t = rig.traits(5098);
        CHECK(t != nullptr);
        if (t == nullptr) return;
        CHECK(t->glass_points.empty()); // retail's model table is an exact case-insensitive match
    }
}

// The class init seeds the vehicle brain's two floors from the models it
// binds: brain[11] from the intact model's CMDL floor and brain[12] from the
// husk, both under one class gate: the helicopter family always, the vehicle
// family unless its profile is a boat (subtype 1). An entity without a brain
// takes neither. [orig: Entity_InitHelicopterAIFromDef @0x4684E2..0x468527;
//  Entity_InitVehicleAIFromDef `cmp [edi+14h],ebx` @0x46881C, stores
//  @0x468836/@0x468858]
static void test_class_init_seeds_the_brain_floors() {
    const std::vector<uint8_t> house = test_io::read_file(repo_path("fixtures/threedi/synth/house.3di"));
    const std::vector<uint8_t> armory = test_io::read_file(repo_path("fixtures/threedi/synth/armory.3di"));
    const Items items(text_bytes(
            "begin \"Floor witness heli\"\n"
            "  id 105098\n"
            "  type vehicle\n"
            "  graphic Floor1\n"
            "  husk Floor1X\n"
            "  ai_function chel\n"
            "  move_function chel\n"
            "  hp 100\n"
            "end\n"
            "begin \"Floor witness boat\"\n"
            "  id 105097\n"
            "  type vehicle\n"
            "  graphic Floor1\n"
            "  husk Floor1X\n"
            "  ai_function cbot\n"
            "  move_function cbot\n"
            "  hp 100\n"
            "end\n"));
    CHECK(!house.empty() && !armory.empty() && items.ok);
    if (house.empty() || armory.empty() || !items.ok) return;
    const TempRoot root("floors");
    root.put("Floor1.3di", house);
    root.put("Floor1X.3di", armory);

    ResourceIndex index;
    CHECK(index.scan(root.dir, std::string(), VfsMountMode::LooseOnly));
    assets::AssetStore models{&index};
    world::EntityPoseProvider pose;
    pose.set_assets(&models);
    CollisionWorld collision;
    OcclusionWorld occlusion;
    mission::CollisionResolveState state;
    auto owned = std::make_unique<World>();
    World &w = *owned;
    w.registry.configure_pool(1, 4);
    Entity e;
    e.kind = EntityKind::Item;
    e.health = 100;
    e.alive = true;
    e.item_id = 5098;
    e.bms_id = 1;
    const EntityHandle heli = w.registry.spawn(1, e);
    e.item_id = 5097;
    e.bms_id = 2;
    const EntityHandle boat = w.registry.spawn(1, e);
    e.item_id = 5098;
    e.bms_id = 3;
    const EntityHandle bare = w.registry.spawn(1, e); // no brain
    w.ai.attach(heli);
    w.ai.attach(boat);
    w.ai.for_handle(boat)->profile.subtype = 1;
    mission::resolve_item_traits(w, items.file, [](int) -> uint8_t { return 0; });
    const mission::CollisionResolveDeps deps{collision, occlusion, pose, models};
    CHECK(mission::resolve_collision_instances(w, items.file, state, deps) == 3);

    const ItemDeathTraits *t = w.tables.item_death_traits.get(5098);
    CHECK(t != nullptr && t->husk_model_loaded && t->husk_rest_min_z != 0.0f);
    if (t == nullptr) return;
    const AiBrain &hb = w.ai.for_handle(heli)->brain;
    CHECK(hb.f[AiBrain::kHuskFloor] == to_fixed(std::abs(t->husk_rest_min_z)));
    CHECK(hb.f[AiBrain::kModelFloor] == w.registry.get(heli)->veh.air_probe_z_off);
    const AiBrain &bb = w.ai.for_handle(boat)->brain;
    CHECK(bb.f[AiBrain::kModelFloor] == 0 && bb.f[AiBrain::kHuskFloor] == 0);
    CHECK(w.ai.for_handle(bare) == nullptr);
}

int main() {
    test_first_husk_kz_and_dead_points_feed_death_traits();
    test_final_only_husk_supplies_the_gate_but_no_kz_source();
    test_missing_husk_assets_leave_the_live_model_gate_clear();
    test_retail_glass_model_maps_exact_userpoint_into_death_traits();
    test_class_init_seeds_the_brain_floors();
    if (failures == 0) std::printf("OK: item_death_traits_resolve\n");
    return failures == 0 ? 0 : 1;
}
