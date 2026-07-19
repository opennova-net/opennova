#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>

#include "common/test_paths.h"
#include "threedi/threedi_3di3.h"
#include "threedi/threedi_ir.h"

static int failures = 0;

static void check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static bool near(float actual, float expected, float epsilon = 1e-6f) {
    return std::fabs(actual - expected) <= epsilon;
}

static void test_cxlt_is_preserved_metadata_not_a_vertex_offset() {
    // Retail BuildCollision preserves CXLT in collision metadata, but the
    // projectile face walker gets section matrices from the model callback and
    // never adds CXLT/COBJ offsets to CVRT. JetSki is a decisive witness: its
    // sole CXLT equals COBJ 1's offset while that object's CVRT run is already
    // in render-model coordinates, so adding either value would double-shift it.
    char path[4096];
    std::snprintf(path, sizeof(path), "%s/fixtures/threedi/3di3/JetSki.3di",
                  test_paths_repo_root(__FILE__));

    Threedi3di3 model = {};
    const int read_rc = threedi_3di3_read(path, &model);
    check(read_rc == 0, "JetSki CXLT fixture parses");
    if (read_rc != 0) return;

    ThreediModelIR ir = {};
    const int ir_rc = threedi_ir_from_3di3(&model, &ir);
    check(ir_rc == 0, "JetSki converts to collision IR");
    if (ir_rc != 0) {
        threedi_3di3_free(&model);
        return;
    }

    const ThreediIRCollision *collision = ir.collision;
    check(collision != nullptr, "JetSki carries collision IR");
    if (collision != nullptr) {
        check(collision->object_count == 2, "JetSki keeps both COBJ records");
        check(collision->translation_count == 1, "JetSki keeps its sole CXLT record");
        if (collision->object_count == 2 && collision->translation_count == 1) {
            const float *offset = collision->objects[1].offset;
            const float *translation = collision->translations[0].translation;
            check(near(offset[0], 23193.0f) && near(offset[1], 39.0f) &&
                          near(offset[2], 34085.0f),
                  "COBJ offset remains raw fp16 metadata");
            check(near(translation[0], offset[0]) && near(translation[1], offset[1]) &&
                          near(translation[2], offset[2]),
                  "CXLT is preserved independently and equals JetSki COBJ 1 metadata");

            const int32_t vertex_start = collision->objects[0].num_vertices;
            const int32_t vertex_count = collision->objects[1].num_vertices;
            const bool bounded = vertex_start >= 0 && vertex_count > 0 &&
                                 static_cast<size_t>(vertex_start + vertex_count) <=
                                     collision->vertex_count;
            check(bounded, "JetSki COBJ 1 owns a bounded CVRT run");
            if (bounded) {
                float min_v[3] = {
                    std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::infinity()};
                float max_v[3] = {
                    -std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity()};
                for (int32_t i = 0; i < vertex_count; ++i) {
                    const float *v = collision->vertices[vertex_start + i].position;
                    for (int axis = 0; axis < 3; ++axis) {
                        if (v[axis] < min_v[axis]) min_v[axis] = v[axis];
                        if (v[axis] > max_v[axis]) max_v[axis] = v[axis];
                    }
                }
                check(near(min_v[0], 0.12109375f) && near(min_v[1], -0.34375f) &&
                              near(min_v[2], 0.53515625f),
                      "COBJ 1 CVRT minimum remains unshifted model-space data");
                check(near(max_v[0], 0.40234375f) && near(max_v[1], 0.359375f) &&
                              near(max_v[2], 0.6953125f),
                      "COBJ 1 CVRT maximum remains unshifted model-space data");
            }
        }
    }

    threedi_ir_free(&ir);
    threedi_3di3_free(&model);
}

int main() {
    check(sizeof(ThreediIRCollisionFace) == 52,
          "collision face ABI stride includes the round-raycast fields");
    check(offsetof(ThreediIRCollisionFace, material_flags) == 8,
          "collision face material_flags offset is stable");
    check(offsetof(ThreediIRCollisionFace, poly_type) == 12,
          "collision face poly_type offset is stable");
    check(offsetof(ThreediIRCollisionFace, normal) == 14,
          "collision face normal offset is stable");
    check(offsetof(ThreediIRCollisionFace, dominate_axis) == 20,
          "collision face dominate_axis offset is stable");
    check(offsetof(ThreediIRCollisionFace, plane_dist_fp16) == 24,
          "collision face plane distance offset is stable");
    check(offsetof(ThreediIRCollisionFace, min_fp16) == 28,
          "collision face min AABB offset is stable");
    check(offsetof(ThreediIRCollisionFace, max_fp16) == 40,
          "collision face max AABB offset is stable");

    ThreediIRCollisionPlane planes[2] = {};
    ThreediIRCollisionVolume volume = {};
    volume.plane_count = 2;
    volume.object_index = -1;
    ThreediIRCollision collision = {};
    collision.planes = planes;
    collision.plane_count = 2;
    collision.volumes = &volume;
    collision.volume_count = 1;

    check(threedi_ir_collision_is_runtime_safe(&collision) == 1,
          "bounded non-empty plane window is safe");

    volume.plane_start = -1;
    check(threedi_ir_collision_is_runtime_safe(&collision) == 0,
          "negative plane start is rejected");
    volume.plane_start = 0;
    volume.plane_count = 0;
    check(threedi_ir_collision_is_runtime_safe(&collision) == 0,
          "plane-less volume is rejected");
    volume.plane_count = 3;
    check(threedi_ir_collision_is_runtime_safe(&collision) == 0,
          "overrunning plane window is rejected");
    volume.plane_count = 2;
    volume.object_index = 0;
    check(threedi_ir_collision_is_runtime_safe(&collision) == 0,
          "missing collision object is rejected");
    volume.object_index = -1;
    collision.planes = nullptr;
    check(threedi_ir_collision_is_runtime_safe(&collision) == 0,
          "missing plane array is rejected");
    collision.planes = planes;
    collision.volumes = nullptr;
    check(threedi_ir_collision_is_runtime_safe(&collision) == 0,
          "missing volume array is rejected");
    check(threedi_ir_collision_is_runtime_safe(nullptr) == 0,
          "null collision block is rejected");

    test_cxlt_is_preserved_metadata_not_a_vertex_offset();

    ThreediIRCollisionPlane grouped_planes[4] = {};
    ThreediIRCollisionVolume grouped_volumes[2] = {};
    ThreediIRCollisionObject grouped_objects[2] = {};
    grouped_volumes[0].plane_start = 0;
    grouped_volumes[0].plane_count = 2;
    grouped_volumes[0].object_index = 1;
    grouped_volumes[1].plane_start = 2;
    grouped_volumes[1].plane_count = 2;
    grouped_volumes[1].object_index = 0;
    ThreediIRCollision grouped = {};
    grouped.planes = grouped_planes;
    grouped.plane_count = 4;
    grouped.volumes = grouped_volumes;
    grouped.volume_count = 2;
    grouped.objects = grouped_objects;
    grouped.object_count = 2;
    check(threedi_ir_collision_is_runtime_safe(&grouped) == 0,
          "non-monotonic object groups are rejected");
    grouped_volumes[0].object_index = 0;
    grouped_volumes[1].object_index = 1;
    check(threedi_ir_collision_is_runtime_safe(&grouped) == 1,
          "ordered object groups are safe");

    if (failures == 0) std::printf("collision_ir_test: OK\n");
    return failures == 0 ? 0 : 1;
}
