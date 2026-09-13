/* engine/runtime/simassets: the sim-side .3di derivations, headless.

   Pins the collision/occlusion model builds and the model predicates against
   the synthetic model set (fixtures/threedi/synth, minted by
   tests/fixtures/minimal_3di_gen.cpp), and the SimModelCache
   parse-once/negative/name rule over a ResourceIndex mounted at that
   directory. The bird's face-only collision (18 CFAC over nine bone
   sections, 0 BVOL) is the witness the GUT suite pins through the full sim;
   here the builder is pinned directly. */

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

using namespace opennova::threedi;

namespace {

std::string fixture_dir() {
    const char *root = test_paths_repo_root(__FILE__);
    return std::string(root) + "/fixtures/threedi/synth";
}

int read_model(const char *name, Threedi3di3 &out) {
    const std::string path = fixture_dir() + "/" + name;
    return threedi_3di3_read(path.c_str(), &out);
}

} // namespace

int main() {
    using namespace opennova::simassets;

    // Entity-bound initialization feeds collision, live draws and static rows.
    // The authored scale folds before the signed unscaled-first-husk max;
    // a CDTA block stamps padding even for a zero model sphere.
    {
        EntityBoundRadiusInputs input;
        input.model_radius_q16 = 8 << 16;
        input.uniform_scale_q16 = 1 << 15;
        input.has_first_husk = true;
        input.first_husk_radius_q16 = 6 << 16;
        TEST_EXPECT(entity_bound_radius_q16(input) == 0);
        input.has_collision_block = true;
        TEST_EXPECT(entity_bound_radius_q16(input) == (6 << 16) + 0x1000);
        input.uniform_scale_q16 = 2 << 16;
        TEST_EXPECT(entity_bound_radius_q16(input) == (16 << 16) + 0x1000);
        input.has_first_husk = false;
        input.uniform_scale_q16 = 0;
        TEST_EXPECT(entity_bound_radius_q16(input) == (8 << 16) + 0x1000);
        input.model_radius_q16 = 0x10001;
        input.uniform_scale_q16 = 0x18000;
        TEST_EXPECT(entity_bound_radius_q16(input) == 0x19002);
        input.model_radius_q16 = 0;
        TEST_EXPECT(entity_bound_radius_q16(input) == 0x1000);
    }

    // --- bird: the face-only witness (18 CFAC over 9 sections, 0 BVOL) ------
    {
        Threedi3di3 model{};
        TEST_EXPECT(read_model("bird.3di", model) == 0);
        opennova::world::CollisionModel cm;
        TEST_EXPECT(collision_model_from_3di(model.collision, cm,
                model_has_collision(model)));
        TEST_EXPECT(cm.faces.size() == 18);
        TEST_EXPECT(cm.volumes.empty());
        TEST_EXPECT(cm.sections.size() == 9);
        TEST_EXPECT(model_is_skinned(model, 0));
        TEST_EXPECT(model_has_collision(model));
        TEST_EXPECT(model.header.has_header);
        TEST_EXPECT(model.header.max_radius_fp16 > 0);
        // Production files carry GHDR's exact Q16 radius, and that carrier is
        // what the builder hands back.
        TEST_EXPECT(model_bound_radius_q16_from_3di(model) == model.header.max_radius_fp16);
        TEST_EXPECT(model_bound_radius_from_3di(model) ==
                static_cast<float>(model.header.max_radius_fp16) / 65536.0f);
        std::printf("[simassets] bird: faces=%zu sections=%zu skinned=%d "
                    "has_collision=%d radius=%f\n",
                cm.faces.size(), cm.sections.size(),
                model_is_skinned(model, 0) ? 1 : 0,
                model_has_collision(model) ? 1 : 0,
                model_bound_radius_from_3di(model));
        threedi_3di3_free(&model);
    }

    // --- house: volume-carrying building + occlusion presence report ------
    {
        Threedi3di3 model{};
        TEST_EXPECT(read_model("house.3di", model) == 0);
        opennova::world::CollisionModel cm;
        const bool built = collision_model_from_3di(model.collision, cm,
                model_has_collision(model));
        opennova::world::OcclusionModel om;
        const bool occ = occlusion_model_from_3di(model, om);
        std::printf("[simassets] house: built=%d faces=%zu volumes=%zu "
                    "sections=%zu occl=%d records=%zu radius=%f\n",
                built ? 1 : 0, cm.faces.size(), cm.volumes.size(),
                cm.sections.size(), occ ? 1 : 0, om.records.size(),
                model_bound_radius_from_3di(model));
        TEST_EXPECT(built);
        TEST_EXPECT(cm.faces.size() == 12);
        TEST_EXPECT(cm.volumes.size() == 4);
        TEST_EXPECT(cm.sections.size() == 1);
        // The house authors no OOBJ block: the occlusion build reports absence.
        TEST_EXPECT(!occ);
        TEST_EXPECT(om.records.empty());
        threedi_3di3_free(&model);
    }

    // --- armory: the OOBJ-bearing building --------------------------------
    {
        Threedi3di3 model{};
        TEST_EXPECT(read_model("armory.3di", model) == 0);
        opennova::world::OcclusionModel om;
        TEST_EXPECT(occlusion_model_from_3di(model, om));
        TEST_EXPECT(om.records.size() == 8);
        threedi_3di3_free(&model);
    }

    // --- person: the skinned-person predicates -----------------------------
    {
        Threedi3di3 model{};
        TEST_EXPECT(read_model("person.3di", model) == 0);
        std::printf("[simassets] person: skinned=%d has_collision=%d\n",
                model_is_skinned(model, 0) ? 1 : 0,
                model_has_collision(model) ? 1 : 0);
        TEST_EXPECT(model_is_skinned(model, 0));
        TEST_EXPECT(model_has_collision(model));
        threedi_3di3_free(&model);
    }

    // --- SimModelCache: name rule, parse-once, negatives, reset ------------
    {
        opennova::ResourceIndex index;
        TEST_EXPECT(index.scan(fixture_dir()));
        SimModelCache cache;
        cache.set_index(&index);
        const Threedi3di3 *bird = cache.model_for("bird");
        TEST_EXPECT(bird != nullptr);
        TEST_EXPECT(bird->collision != nullptr);
        // Any authored spelling resolves to the same parse (the <basename>.3di
        // rule, case-insensitive).
        TEST_EXPECT(cache.model_for("BIRD.3di") == bird);
        TEST_EXPECT(cache.model_for("models\\Bird.ext") == bird);
        TEST_EXPECT(cache.parsed_count() == 1);
        TEST_EXPECT(cache.model_for("definitely_missing") == nullptr);
        TEST_EXPECT(cache.model_for("definitely_missing") == nullptr);
        TEST_EXPECT(cache.negative_count() == 1);
        const Threedi3di3 *house = cache.model_for("house");
        TEST_EXPECT(house != nullptr && house != bird);
        TEST_EXPECT(cache.parsed_count() == 2);
        cache.reset();
        TEST_EXPECT(cache.parsed_count() == 0);
        TEST_EXPECT(cache.model_for("bird") != nullptr);
        // No index -> negative, never a crash.
        SimModelCache empty;
        TEST_EXPECT(empty.model_for("bird") == nullptr);
    }

    std::printf("simassets model builders: OK\n");
    return 0;
}
