// The collectors' render model, produced where the model resolves (the same
// CMDL-sphere producer the placer and ObjectModel use): every entity whose
// graphic loads is stamped with its collision-block sphere (radius 0 without
// the block), one whose graphic does not load is not, and a decoded wire
// row's resolved shape carries the same sphere. [orig: the entity+0x30 gates
// Terrain_CollectVisibleEntities_0 @ 0x5c6fd8..0x5c6fe1 /
// Terrain_CollectVisibleEntitiesForTerrain @ 0x5c8cf6..0x5c8cff;
// Entity_ComputeBoundingSphere @ 0x5c69a0, its null-block early out
// @ 0x5c69be; Entity_InitFromModel @ 0x40df06..0x40dfac] Driven by a manual
// World and the model cache over a loose temp root holding copies of the
// committed synthetic fixtures (crate: a CMDL block with a CB volume; gun: a
// CMDL block without usable collision geometry).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/test_paths.h"

#include <base/resource_index/resource_index.h>
#include <formats/def/def.h>
#include <formats/mission/mission.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/assets/asset_store.h>
#include <runtime/mission/collision_resolve.h>
#include <runtime/renderer/object_lod.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity_pose.h>
#include <runtime/world/model_geometry.h>
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

struct TempRoot {
    std::string dir;
    TempRoot() {
        dir = std::string(test_paths_temp_dir()) + "/" + test_paths_unique("opennova_render_model_resolve");
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

renderer::ObjectProjectionSphere expected_sphere(const std::vector<uint8_t> &bytes) {
    Threedi3di3 model{};
    CHECK(threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) == 0);
    const renderer::ObjectProjectionSphere sphere = collision_projection_sphere_from_3di(model);
    threedi_3di3_free(&model);
    return sphere;
}

bool same(const renderer::ObjectProjectionSphere &a, const renderer::ObjectProjectionSphere &b) {
    return a.valid == b.valid && a.radius_q16 == b.radius_q16 &&
           a.center_q16[0] == b.center_q16[0] && a.center_q16[1] == b.center_q16[1] &&
           a.center_q16[2] == b.center_q16[2];
}

} // namespace

int main() {
    const std::vector<uint8_t> crate = test_io::read_file(repo_path("fixtures/threedi/synth/crate.3di"));
    const std::vector<uint8_t> gun = test_io::read_file(repo_path("fixtures/threedi/synth/gun.3di"));
    CHECK(!crate.empty() && !gun.empty());
    const Items items(
            "begin \"Render Crate\"\n  id 106501\n  type object\n  graphic crate\nend\n"
            "begin \"Render Gun\"\n  id 106502\n  type object\n  graphic gun\nend\n"
            "begin \"Render Missing\"\n  id 106503\n  type object\n  graphic nosuch\nend\n");
    if (crate.empty() || gun.empty() || !items.ok) return 1;
    const TempRoot root;
    root.put("crate.3di", crate);
    root.put("gun.3di", gun);
    const renderer::ObjectProjectionSphere crate_sphere = expected_sphere(crate);
    CHECK(crate_sphere.valid && crate_sphere.radius_q16 > 0);

    ResourceIndex index;
    CHECK(index.scan(root.dir, std::string(), VfsMountMode::LooseOnly));
    assets::AssetStore models{&index};
    EntityPoseProvider pose;
    pose.set_assets(&models);
    CollisionWorld collision;
    OcclusionWorld occlusion;
    mission::CollisionResolveState state;
    World w;
    w.registry.configure_pool(2, 8);
    const auto spawn = [&](int32_t item_id) {
        Entity e;
        e.kind = EntityKind::Item;
        e.item_id = item_id;
        e.alive = true;
        return w.registry.spawn(2, e);
    };
    const EntityHandle with_block = spawn(6501);
    const EntityHandle without_block = spawn(6502);
    const EntityHandle missing = spawn(6503);
    const mission::CollisionResolveDeps deps{collision, occlusion, pose, models};
    mission::resolve_collision_instances(w, items.file, state, deps);

    // A model with the block: its CMDL sphere, beside its collision instance.
    const renderer::ObjectProjectionSphere *stamped = occlusion.render_model_sphere(with_block);
    CHECK(stamped != nullptr && same(*stamped, crate_sphere));
    CHECK(collision.model_for(w, with_block) != nullptr);
    // A model whose collision block carries no usable geometry (the gun: a
    // CMDL header, no volumes or faces): no collision instance, so the
    // collector's model leg reads this stamp, the block's own CMDL sphere
    // (model_geometry_test pins the no-block form, radius 0).
    const renderer::ObjectProjectionSphere gun_sphere = expected_sphere(gun);
    stamped = occlusion.render_model_sphere(without_block);
    CHECK(stamped != nullptr && same(*stamped, gun_sphere));
    CHECK(collision.model_for(w, without_block) == nullptr);
    // A graphic that does not load: no render model, never collected.
    CHECK(!occlusion.has_render_model(missing));

    // A decoded wire row's resolved shape carries the same render model.
    const ResolvedCollisionShape wire_crate =
            mission::collision_shape_for_runtime_type(6501, items.file, state, deps);
    CHECK(wire_crate.has_render_model);
    CHECK(wire_crate.render_sphere_radius_q16 == crate_sphere.radius_q16);
    CHECK(wire_crate.render_sphere_center_q16.x == crate_sphere.center_q16[0] &&
          wire_crate.render_sphere_center_q16.y == crate_sphere.center_q16[1] &&
          wire_crate.render_sphere_center_q16.z == crate_sphere.center_q16[2]);
    const ResolvedCollisionShape wire_gun =
            mission::collision_shape_for_runtime_type(6502, items.file, state, deps);
    CHECK(wire_gun.has_render_model &&
          wire_gun.render_sphere_radius_q16 == gun_sphere.radius_q16);
    CHECK(wire_gun.render_sphere_center_q16.x == gun_sphere.center_q16[0] &&
          wire_gun.render_sphere_center_q16.y == gun_sphere.center_q16[1] &&
          wire_gun.render_sphere_center_q16.z == gun_sphere.center_q16[2]);
    CHECK(collision.model(wire_gun.model_id) == nullptr); // the sphere is the only bound
    const ResolvedCollisionShape wire_missing =
            mission::collision_shape_for_runtime_type(6503, items.file, state, deps);
    CHECK(!wire_missing.has_render_model);

    // Re-resolving a handle whose new occupant's graphic does not load drops
    // the old occupant's render model.
    w.registry.despawn(with_block);
    const EntityHandle reused = spawn(6503);
    mission::resolve_collision_instances(w, items.file, state, deps);
    if (reused == with_block) CHECK(!occlusion.has_render_model(reused));

    if (failures == 0) std::printf("OK: render_model_resolve\n");
    return failures == 0 ? 0 : 1;
}
