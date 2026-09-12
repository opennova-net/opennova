// The tower section-piece render pivot bank (ItemDeathTraits::
// husk_section_origins_q16) is mined from the PRIMARY husk's COBJ list: a
// spawned section is Entity_SpawnSectionEntity's memset template, which
// stores only +0x34 huskModel (never +0x38 huskFinalModel), so the death
// renderer's +0x38 ?: +0x34 pick always lands on the primary husk
// [orig: Entity_SpawnSectionEntity @0x440322 / @0x440343;
//  Entity_BuildDeathSectionTransforms @0x492B46 / @0x492B4D]. The final husk
// stands in only where retail would dereference a null +0x34. Driven by a
// manual World and the model cache over a loose temp root holding the
// committed synthetic fixtures (house: one COBJ; armory: four).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/test_paths.h"

#include <base/resource_index/resource_index.h>
#include <formats/def/def.h>
#include <runtime/simassets/collision_resolve.h>
#include <runtime/simassets/item_traits.h>
#include <runtime/simassets/sim_model_cache.h>
#include <runtime/simassets/sim_pose_provider.h>
#include <runtime/world/collision.h>
#include <runtime/world/destruction.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/world.h>

using namespace opennova;
using namespace opennova::world;
using namespace opennova::def;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

std::string repo_path(const char *rel) {
    return std::string(test_paths_repo_root(__FILE__)) + "/" + rel;
}

// A fresh loose asset root under the temp dir.
struct TempRoot {
    std::string dir;
    explicit TempRoot(const char *tag) {
        static int serial = 0;
        dir = std::string(test_paths_temp_dir()) + "/opennova_husk_origins_" + tag + "_" +
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

struct Items {
    DefItemsFile file{};
    bool ok = false;
    explicit Items(const char *text) {
        ok = def_parse_items_memory(reinterpret_cast<const uint8_t *>(text), std::strlen(text),
                                    &file) == 0;
        CHECK(ok);
    }
    ~Items() { def_free_items(&file); }
};

// One placed building of `item_id` (raw type: items.def id - 100000), the
// engine systems a resolve writes into, and the sweep itself.
struct Rig {
    ResourceIndex index;
    simassets::SimModelCache models;
    simassets::SimPoseProvider pose;
    CollisionWorld collision;
    OcclusionWorld occlusion;
    simassets::CollisionResolveState state;
    World w;
    EntityHandle building;

    Rig(const TempRoot &root, const Items &items, int32_t item_id) {
        CHECK(index.scan(root.dir, std::string(), VfsMountMode::LooseOnly));
        models.set_index(&index);
        pose.set_resource_index(&index);
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
        simassets::resolve_item_traits(w, items.file, [](int) -> uint8_t { return 0; });
        const simassets::CollisionResolveDeps deps{collision, occlusion, pose, models};
        CHECK(simassets::resolve_collision_instances(w, items.file, state, deps) == 1);
    }
    const ItemDeathTraits *traits(int32_t item_id) const {
        return w.tables.item_death_traits.get(item_id);
    }
};

} // namespace

// Both husk stages present: the bank is the primary husk's COBJ list (the
// one-object house), not the final husk's (the four-object armory).
static void test_primary_husk_supplies_the_section_pivots() {
    const std::vector<uint8_t> house = test_io::read_file(repo_path("fixtures/threedi/synth/house.3di"));
    const std::vector<uint8_t> armory = test_io::read_file(repo_path("fixtures/threedi/synth/armory.3di"));
    const Items items(
            "begin \"Tower witness\"\n"
            "  id 105050\n"
            "  type object\n"
            "  graphic Barrel1\n"
            "  sid tower_witness\n"
            "  husk Barrel1X\n"
            "  huskfinal Barrel1XF\n"
            "  hp 100\n"
            "end\n");
    CHECK(!house.empty() && !armory.empty() && items.ok);
    if (house.empty() || armory.empty() || !items.ok) return;
    const TempRoot root("both");
    root.put("Barrel1.3di", house);
    root.put("Barrel1X.3di", house);
    root.put("Barrel1XF.3di", armory);
    const Rig rig(root, items, 5050);
    const ItemDeathTraits *t = rig.traits(5050);
    CHECK(t != nullptr);
    if (t == nullptr) return;
    CHECK(t->primary_husk_loaded);
    CHECK(t->husk_section_origins_q16.size() == 1); // the primary husk's single COBJ, not the final's four
}

// A final-only definition: retail's +0x34 is null there, so the final husk
// is the only COBJ source the sweep can offer.
static void test_final_only_husk_stands_in_for_the_null_primary() {
    const std::vector<uint8_t> house = test_io::read_file(repo_path("fixtures/threedi/synth/house.3di"));
    const std::vector<uint8_t> armory = test_io::read_file(repo_path("fixtures/threedi/synth/armory.3di"));
    const Items items(
            "begin \"Final-only tower witness\"\n"
            "  id 105051\n"
            "  type object\n"
            "  graphic Barrel1\n"
            "  sid tower_final_only\n"
            "  huskfinal Barrel1XF\n"
            "  hp 100\n"
            "end\n");
    CHECK(!house.empty() && !armory.empty() && items.ok);
    if (house.empty() || armory.empty() || !items.ok) return;
    const TempRoot root("final");
    root.put("Barrel1.3di", house);
    root.put("Barrel1XF.3di", armory);
    const Rig rig(root, items, 5051);
    const ItemDeathTraits *t = rig.traits(5051);
    CHECK(t != nullptr);
    if (t == nullptr) return;
    CHECK(!t->primary_husk_loaded);
    CHECK(t->husk_section_origins_q16.size() == 4);
}

int main() {
    test_primary_husk_supplies_the_section_pivots();
    test_final_only_husk_stands_in_for_the_null_primary();
    if (failures == 0) std::printf("OK: husk_section_origins\n");
    return failures == 0 ? 0 : 1;
}
