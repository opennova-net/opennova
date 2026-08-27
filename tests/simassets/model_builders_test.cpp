/* engine/runtime/simassets: the sim-side .3di derivations, headless.

   Pins the collision/occlusion model builds and the model predicates against
   the committed fixture corpus, and the SimModelCache parse-once/negative/name
   rule over a ResourceIndex mounted at the fixtures root. Bird1's face-only
   collision (242 CFAC, 0 BVOL) is the documented witness the GUT suite pins
   through the full sim; here the builder is pinned directly. */

#include <cstdio>
#include <string>

#include "common/test_expect.h"
#include "common/test_paths.h"

#include <base/resource_index/resource_index.h>
#include <runtime/simassets/model_builders.h>
#include <runtime/simassets/sim_model_cache.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/world/collision.h>
#include <runtime/world/occlusion.h>

namespace {

std::string fixture_dir() {
    const char *root = test_paths_repo_root(__FILE__);
    return std::string(root) + "/fixtures/threedi/3di3";
}

int read_model(const char *name, Threedi3di3 &out) {
    const std::string path = fixture_dir() + "/" + name;
    return threedi_3di3_read(path.c_str(), &out);
}

} // namespace

int main() {
    using namespace opennova::simassets;

    // --- Bird1: the face-only witness (242 CFAC, 0 BVOL) -------------------
    {
        Threedi3di3 model{};
        TEST_EXPECT(read_model("Bird1.3di", model) == 0);
        opennova::world::CollisionModel cm;
        TEST_EXPECT(collision_model_from_3di(model.collision, cm,
                model_has_collision(model)));
        TEST_EXPECT(cm.faces.size() == 242);
        TEST_EXPECT(cm.volumes.empty());
        TEST_EXPECT(cm.sections.size() == 9);
        TEST_EXPECT(model_is_skinned(model, 0));
        TEST_EXPECT(model_has_collision(model));
        TEST_EXPECT(model.header.has_header);
        TEST_EXPECT(model.header.max_radius_fp16 == 51963);
        TEST_EXPECT(model_bound_radius_q16_from_3di(model) == 51963);
        TEST_EXPECT(model_bound_radius_from_3di(model) ==
                static_cast<float>(model.header.max_radius_fp16) / 65536.0f);
        std::printf("[simassets] Bird1: faces=%zu sections=%zu skinned=%d "
                    "has_collision=%d radius=%f\n",
                cm.faces.size(), cm.sections.size(),
                model_is_skinned(model, 0) ? 1 : 0,
                model_has_collision(model) ? 1 : 0,
                model_bound_radius_from_3di(model));
        threedi_3di3_free(&model);
    }

    // --- House: volume-carrying building + occlusion presence report -------
    {
        Threedi3di3 model{};
        TEST_EXPECT(read_model("House.3di", model) == 0);
        opennova::world::CollisionModel cm;
        const bool built = collision_model_from_3di(model.collision, cm,
                model_has_collision(model));
        opennova::world::OcclusionModel om;
        const bool occ = occlusion_model_from_3di(model, om);
        std::printf("[simassets] House: built=%d faces=%zu volumes=%zu "
                    "sections=%zu occl=%d records=%zu radius=%f\n",
                built ? 1 : 0, cm.faces.size(), cm.volumes.size(),
                cm.sections.size(), occ ? 1 : 0, om.records.size(),
                model_bound_radius_from_3di(model));
        TEST_EXPECT(built);
        TEST_EXPECT(cm.faces.size() == 826);
        TEST_EXPECT(cm.volumes.size() == 70);
        TEST_EXPECT(cm.sections.size() == 1);
        threedi_3di3_free(&model);
    }

    // --- CharModel: the skinned-person predicates ---------------------------
    {
        Threedi3di3 model{};
        TEST_EXPECT(read_model("CharModel.3di", model) == 0);
        std::printf("[simassets] CharModel: skinned=%d has_collision=%d\n",
                model_is_skinned(model, 0) ? 1 : 0,
                model_has_collision(model) ? 1 : 0);
        TEST_EXPECT(model_is_skinned(model, 0));
        threedi_3di3_free(&model);
    }

    // --- SimModelCache: name rule, parse-once, negatives, reset ------------
    {
        opennova::ResourceIndex index;
        TEST_EXPECT(index.scan(fixture_dir()));
        SimModelCache cache;
        cache.set_index(&index);
        const Threedi3di3 *bird = cache.model_for("Bird1");
        TEST_EXPECT(bird != nullptr);
        TEST_EXPECT(bird->collision != nullptr);
        // Any authored spelling resolves to the same parse (the <basename>.3di
        // rule, case-insensitive).
        TEST_EXPECT(cache.model_for("BIRD1.3di") == bird);
        TEST_EXPECT(cache.model_for("models\\Bird1.ext") == bird);
        TEST_EXPECT(cache.parsed_count() == 1);
        TEST_EXPECT(cache.model_for("definitely_missing") == nullptr);
        TEST_EXPECT(cache.model_for("definitely_missing") == nullptr);
        TEST_EXPECT(cache.negative_count() == 1);
        const Threedi3di3 *house = cache.model_for("House");
        TEST_EXPECT(house != nullptr && house != bird);
        TEST_EXPECT(cache.parsed_count() == 2);
        cache.reset();
        TEST_EXPECT(cache.parsed_count() == 0);
        TEST_EXPECT(cache.model_for("Bird1") != nullptr);
        // No index -> negative, never a crash.
        SimModelCache empty;
        TEST_EXPECT(empty.model_for("Bird1") == nullptr);
    }

    std::printf("simassets model builders: OK\n");
    return 0;
}
