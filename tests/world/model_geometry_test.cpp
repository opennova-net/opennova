/* engine/runtime/world: the sim-side .3di derivations, headless.

   Pins the collision/occlusion model builds and the model predicates against
   the synthetic model set (fixtures/threedi/synth, minted by
   tests/fixtures/minimal_3di_gen.cpp), and the assets::AssetStore
   parse-once/negative/name rule over a ResourceIndex mounted at that
   directory. The bird's face-only collision (18 CFAC over nine bone
   sections, 0 BVOL) is the witness the GUT suite pins through the full sim;
   here the builder is pinned directly. */

#include <cstdio>
#include <string>

#include "common/test_expect.h"
#include "common/test_paths.h"

#include <base/resource_index/resource_index.h>
#include <runtime/world/model_geometry.h>
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
    using namespace opennova::world;

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

    // The projection sphere reads exact CMDL bounds, independently of GHDR
    // and usable collision geometry. These words exceed float's exact Q16
    // integer range; poisoned float fields must not replace the source words.
    {
        Threedi3di3 model{};
        // No collision block: retail never stamps entity+0x1FC/+0x208, and
        // the collector projects those zero spawn words as radius zero.
        // [orig: Entity_InitFromModel @0x40de97; Entity_ComputeBoundingSphere @0x5c69be]
        const auto unstamped = collision_projection_sphere_from_3di(model);
        TEST_EXPECT(unstamped.valid && unstamped.radius_q16 == 0);
        TEST_EXPECT(unstamped.center_q16[0] == 0 && unstamped.center_q16[1] == 0 &&
                unstamped.center_q16[2] == 0);
        TEST_EXPECT(collision_projection_sphere_from_3di(model, 0x18000, 0, true).radius_q16 == 0);
        ThreediCollisionModel collision{};
        model.collision = &collision;
        collision.model_data.has_bbox_fp16 = 1;
        const int32_t exact[] = {0x02000001, -3, 10, 0x02000008, 6, 15};
        for (int i = 0; i < 6; ++i) {
            collision.model_data.bbox_fp16[i] = exact[i];
            collision.model_data.bbox[i] = -100.0f;
        }
        const auto sphere = collision_projection_sphere_from_3di(model);
        TEST_EXPECT(sphere.valid && sphere.radius_q16 == 7);
        TEST_EXPECT(sphere.center_q16[0] == 0x02000004);
        TEST_EXPECT(sphere.center_q16[1] == 1 && sphere.center_q16[2] == 12);
        const auto scaled = collision_projection_sphere_from_3di(model, 0, 0x18000);
        TEST_EXPECT(scaled.radius_q16 == 11);
        TEST_EXPECT(scaled.center_q16[0] == 0x03000006);
        TEST_EXPECT(scaled.center_q16[1] == 2 && scaled.center_q16[2] == 18);
    }

    // The eweap-powerup leg (type 6 with attrib 0x20): the entity init zeroes
    // the center first, so the halves are the maxima themselves and the
    // collision center and the projection sphere share one predicate.
    // [orig: Entity_InitFromModel @0x40df06..0x40df16, halves @0x40df66..0x40df76,
    //  radius @0x40dfac, scale @0x40dfd0..0x40e03c]
    {
        TEST_EXPECT(item_def_zero_bbox_center(6, 0x20u));
        TEST_EXPECT(item_def_zero_bbox_center(6, 0x21u));
        TEST_EXPECT(!item_def_zero_bbox_center(6, 0x10u));
        TEST_EXPECT(!item_def_zero_bbox_center(2, 0x20u));
        TEST_EXPECT(!item_def_zero_bbox_center(1, 0x20u));
        Threedi3di3 model{};
        ThreediCollisionModel collision{};
        model.collision = &collision;
        collision.model_data.has_bbox_fp16 = 1;
        const int32_t exact[] = {-3, -2, 10, 4, 7, 15};
        for (int i = 0; i < 6; ++i) collision.model_data.bbox_fp16[i] = exact[i];
        const auto midpoint = collision_projection_sphere_from_3di(model);
        TEST_EXPECT(midpoint.radius_q16 == 7);
        TEST_EXPECT(midpoint.center_q16[0] == 0 && midpoint.center_q16[1] == 2 &&
                midpoint.center_q16[2] == 12);
        const auto zeroed = collision_projection_sphere_from_3di(model, 0, 0, true);
        TEST_EXPECT(zeroed.valid && zeroed.radius_q16 == 17); // sqrt(16 + 49 + 225)
        TEST_EXPECT(zeroed.center_q16[0] == 0 && zeroed.center_q16[1] == 0 &&
                zeroed.center_q16[2] == 0);
        const auto zeroed_scaled = collision_projection_sphere_from_3di(model, 0x18000, 0, true);
        TEST_EXPECT(zeroed_scaled.radius_q16 == 26); // (17 * 0x18000 + 0x8000) >> 16
        TEST_EXPECT(zeroed_scaled.center_q16[0] == 0 && zeroed_scaled.center_q16[1] == 0 &&
                zeroed_scaled.center_q16[2] == 0);
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
        TEST_EXPECT(model.collision->model_data.has_bbox_fp16 == 1);
        for (int i = 0; i < 6; ++i) {
            TEST_EXPECT(static_cast<float>(model.collision->model_data.bbox_fp16[i]) / 65536.0f ==
                    model.collision->model_data.bbox[i]);
        }
        TEST_EXPECT(collision_projection_sphere_from_3di(model).valid);
        TEST_EXPECT(model.header.has_header);
        TEST_EXPECT(model.header.max_radius_fp16 > 0);
        // Production files carry GHDR's exact Q16 radius, and that carrier is
        // what the builder hands back.
        TEST_EXPECT(model_bound_radius_q16_from_3di(model) == model.header.max_radius_fp16);
        TEST_EXPECT(model_bound_radius_from_3di(model) ==
                static_cast<float>(model.header.max_radius_fp16) / 65536.0f);
        std::printf("[native world] bird: faces=%zu sections=%zu skinned=%d "
                    "has_collision=%d radius=%f\n",
                cm.faces.size(), cm.sections.size(),
                model_is_skinned(model, 0) ? 1 : 0,
                model_has_collision(model) ? 1 : 0,
                model_bound_radius_from_3di(model));
        threedi_3di3_free(&model);
    }

    // --- the per-section runtime-safety gate ------------------------------
    // Retail's loader copies every pool as authored and links the COBJ runs
    // with no check, so a CFAC corner past its section's run reads outside it
    // (Pinegr_L, broken in retail) while the model's volumes stay live
    // [orig: Threedi_BuildCollisionModelFromChunks @ 0x5B3BF0, the corners
    //  copied as words @ 0x5B3EC7..0x5B3EEA, the runs @ 0x5B4326..0x5B43C4].
    // The port gates each pool section on its own: a face mesh the walkers
    // cannot read safely loads as no faces with the volumes kept, a volume
    // pool they cannot read loads as no volumes with the faces kept, and only
    // a block with neither is refused.
    {
        ThreediCollisionVertex vertices[3] = {};
        ThreediCollisionFace face = {};
        face.vert_index[0] = 0;
        face.vert_index[1] = 1;
        face.vert_index[2] = -1; // a signed-negative corner: read outside the run in retail
        face.normal_index = -1;
        ThreediBoundingPlane planes[2] = {};
        ThreediBoundingVolume volume = {};
        volume.plane_count = 2;
        ThreediCollisionObject object = {};
        object.num_vertices = 3;
        object.num_faces = 1;
        object.num_bounding_volumes = 1;
        ThreediCollisionModel col = {};
        col.vertices = vertices;
        col.vertex_count = 3;
        col.faces = &face;
        col.face_count = 1;
        col.planes = planes;
        col.plane_count = 2;
        col.volumes = &volume;
        col.volume_count = 1;
        col.objects = &object;
        col.object_count = 1;
        TEST_EXPECT(threedi_3di3_collision_is_runtime_safe(&col) == 0);
        TEST_EXPECT(threedi_3di3_collision_faces_runtime_safe(&col) == 0);
        TEST_EXPECT(threedi_3di3_collision_volumes_runtime_safe(&col) == 1);
        opennova::world::CollisionModel cm;
        TEST_EXPECT(collision_model_from_3di(&col, cm));
        TEST_EXPECT(cm.faces.empty() && cm.vertices.empty());
        TEST_EXPECT(cm.volumes.size() == 1 && cm.planes.size() == 2);
        TEST_EXPECT(cm.sections.size() == 1);
        TEST_EXPECT(cm.sections[0].face_count == 0 && cm.sections[0].vertex_count == 0);
        TEST_EXPECT(cm.sections[0].volume_count == 1);

        // The other way round: a good face mesh over an overrunning BPLN window.
        face.vert_index[2] = 2;
        volume.plane_count = 3;
        TEST_EXPECT(threedi_3di3_collision_faces_runtime_safe(&col) == 1);
        TEST_EXPECT(threedi_3di3_collision_volumes_runtime_safe(&col) == 0);
        opennova::world::CollisionModel faces_only;
        TEST_EXPECT(collision_model_from_3di(&col, faces_only));
        TEST_EXPECT(faces_only.faces.size() == 1 && faces_only.vertices.size() == 3);
        TEST_EXPECT(faces_only.volumes.empty() && faces_only.planes.empty());
        TEST_EXPECT(faces_only.sections.size() == 1 && faces_only.sections[0].volume_count == 0);
        TEST_EXPECT(faces_only.sections[0].face_count == 1);

        // Neither section walkable: refused.
        face.vert_index[2] = -1;
        opennova::world::CollisionModel none;
        TEST_EXPECT(!collision_model_from_3di(&col, none));
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
        std::printf("[native world] house: built=%d faces=%zu volumes=%zu "
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
        std::printf("[native world] person: skinned=%d has_collision=%d\n",
                model_is_skinned(model, 0) ? 1 : 0,
                model_has_collision(model) ? 1 : 0);
        TEST_EXPECT(model_is_skinned(model, 0));
        TEST_EXPECT(model_has_collision(model));
        threedi_3di3_free(&model);
    }

    std::printf("native world model builders: OK\n");
    return 0;
}
