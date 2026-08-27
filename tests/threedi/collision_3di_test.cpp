// Collision (CDTA) fidelity on the parsed 3DI3 model: packed struct layout
// pins (the 44-B runtime CFAC record depends on them), the runtime-safety
// validator, per-COBJ run prefix sums, and exact recovery of authored
// fixed-point values through the float parse.
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>

#include "common/test_paths.h"
#include <formats/threedi/threedi_3di3.h>

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

    const ThreediCollisionModel *collision = model.collision;
    check(collision != nullptr, "JetSki carries a collision block");
    if (collision != nullptr) {
        check(collision->object_count == 2, "JetSki keeps both COBJ records");
        check(collision->translation_count == 1, "JetSki keeps its sole CXLT record");
        if (collision->object_count == 2 && collision->translation_count == 1) {
            const int32_t *offset = collision->objects[1].offset;
            const int32_t *translation = collision->translations[0].translation;
            check(offset[0] == 23193 && offset[1] == 39 && offset[2] == 34085,
                  "COBJ offset remains raw fp16 metadata");
            check(translation[0] == offset[0] && translation[1] == offset[1] &&
                          translation[2] == offset[2],
                  "CXLT is preserved independently and equals JetSki COBJ 1 metadata");

            ThreediCollisionObjectRun runs[2] = {};
            check(threedi_collision_object_runs(collision, runs) == 1,
                  "JetSki COBJ runs resolve");
            const int32_t vertex_start = runs[1].vertex_start;
            const int32_t vertex_count = collision->objects[1].num_vertices;
            check(vertex_start == collision->objects[0].num_vertices,
                  "COBJ 1's CVRT run starts after COBJ 0's");
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

    threedi_3di3_free(&model);
}

static void test_charmodel_cobj_preserves_exact_bone_sphere() {
    // Retail's organic/skeletal broad phase reads these authored COBJ values
    // directly as signed 16.16 integers. CharModel COBJ 14 is the head and is
    // a useful fidelity witness because it owns no CFAC/CVRT run of its own.
    char path[4096];
    std::snprintf(path, sizeof(path), "%s/fixtures/threedi/3di3/CharModel.3di",
                  test_paths_repo_root(__FILE__));

    Threedi3di3 model = {};
    const int read_rc = threedi_3di3_read(path, &model);
    check(read_rc == 0, "CharModel COBJ sphere fixture parses");
    if (read_rc != 0) return;

    const ThreediCollisionModel *collision = model.collision;
    check(collision != nullptr, "CharModel carries a collision block");
    if (collision != nullptr) {
        check(collision->object_count == 19, "CharModel keeps all 19 COBJ records");
        if (collision->object_count > 14) {
            const ThreediCollisionObject &head = collision->objects[14];
            check(head.parent_subobject_index == 13,
                  "CharModel head COBJ keeps its parent bone");
            check(head.num_vertices == 0 && head.num_faces == 0,
                  "CharModel head COBJ is a sphere-only skeletal section");
            check(head.med[0] == 3578 && head.med[1] == 65 &&
                          head.med[2] == 54371,
                  "CharModel head COBJ center remains exact signed 16.16 data");
            check(head.radius == 10345,
                  "CharModel head COBJ radius remains exact signed 16.16 data");
        }
    }

    threedi_3di3_free(&model);
}

static void test_collision_probe_boxes_follow_the_witnessed_folds() {
    // The vehicle platform-solve probe boxes [orig:
    // Threedi_BuildCollisionModelFromChunks @ 0x5b3bf0, tail
    // @ 0x5b4455..0x5b45db]: box Z = the CMDL header bbox Z pair; box X/Y
    // folds type-1 BVOLs whose min-Z is below CMDL minZ + zspan/2; the
    // footprint folds those below minZ + zspan/8 and clamps each side to at
    // least q + 0x2000, q = (box Y span) >> 2. Shaped after the DTruck1
    // witness: the CMDL floor sits at wheel contact (+0.01) while wheel
    // volumes dip below the origin.
    ThreediBoundingVolume volumes[5] = {};
    // [0] wheels: bottom-eighth, folds into BOTH boxes.
    volumes[0].collidable_type = 1;
    volumes[0].min_x_fp16 = -(8 << 16);
    volumes[0].max_x_fp16 = 3 << 16;
    volumes[0].min_y_fp16 = -((3 << 16) / 2);
    volumes[0].max_y_fp16 = (3 << 16) / 2;
    volumes[0].min_z_fp16 = -21889; // -0.334 — below the CMDL floor
    volumes[0].max_z_fp16 = 2 << 16;
    // [1] cab: lower half only (min-Z above the eighth threshold).
    volumes[1].collidable_type = 1;
    volumes[1].min_x_fp16 = -(9 << 16);
    volumes[1].max_x_fp16 = 4 << 16;
    volumes[1].min_y_fp16 = -(2 << 16);
    volumes[1].max_y_fp16 = 2 << 16;
    volumes[1].min_z_fp16 = 1 << 16;
    volumes[1].max_z_fp16 = 3 << 16;
    // [2] canopy: upper half, folds into NEITHER box.
    volumes[2].collidable_type = 1;
    volumes[2].min_x_fp16 = -(12 << 16);
    volumes[2].max_x_fp16 = 6 << 16;
    volumes[2].min_y_fp16 = -(3 << 16);
    volumes[2].max_y_fp16 = 3 << 16;
    volumes[2].min_z_fp16 = 3 << 16;
    volumes[2].max_z_fp16 = 5 << 16;
    // [3] a non-type-1 volume at the very bottom: ignored by both folds.
    volumes[3].collidable_type = 7;
    volumes[3].min_x_fp16 = -(20 << 16);
    volumes[3].max_x_fp16 = 20 << 16;
    volumes[3].min_y_fp16 = -(20 << 16);
    volumes[3].max_y_fp16 = 20 << 16;
    volumes[3].min_z_fp16 = -(2 << 16);
    volumes[3].max_z_fp16 = 0;
    // [4] unowned trailing type-1 BVOL at the bottom: dead data, ignored.
    volumes[4].collidable_type = 1;
    volumes[4].min_x_fp16 = -(30 << 16);
    volumes[4].max_x_fp16 = 30 << 16;
    volumes[4].min_y_fp16 = -(30 << 16);
    volumes[4].max_y_fp16 = 30 << 16;
    volumes[4].min_z_fp16 = -(1 << 16);
    volumes[4].max_z_fp16 = 0;
    ThreediCollisionObject objects[2] = {};
    objects[0].num_bounding_volumes = 3;
    objects[1].num_bounding_volumes = 1;
    ThreediCollisionModel collision = {};
    collision.volumes = volumes;
    collision.volume_count = 5;
    collision.objects = objects;
    collision.object_count = 2;
    // CMDL floor at +0.01, deck at +4.65 (the DTruck1 pair): thresholds are
    // 655 + (304198 >> 1) = 152754 (~2.33) and 655 + (304198 >> 3) = 38679
    // (~0.59).
    collision.model_data.bbox[2] = 655.0f / 65536.0f;
    collision.model_data.bbox[5] = 304853.0f / 65536.0f;

    ThreediCollisionProbeBoxes boxes = {};
    check(threedi_3di3_collision_probe_boxes(&collision, &boxes) == 1,
          "probe boxes derive from a wheeled-hull collision block");
    check(boxes.box_z_lo == 655 && boxes.box_z_hi == 304853,
          "box Z pair is the CMDL header bbox Z pair, not the deepest vertex");
    check(boxes.box_x_lo == -(9 << 16) && boxes.box_x_hi == 4 << 16,
          "box X folds only the lower-half type-1 volumes");
    check(boxes.box_y_lo == -(2 << 16) && boxes.box_y_hi == 2 << 16,
          "box Y folds only the lower-half type-1 volumes");
    check(boxes.foot_x_lo == -(8 << 16) && boxes.foot_x_hi == 3 << 16,
          "footprint X folds only the bottom-eighth type-1 volumes");
    check(boxes.foot_y_lo == -((3 << 16) / 2) && boxes.foot_y_hi == (3 << 16) / 2,
          "footprint Y folds only the bottom-eighth type-1 volumes");

    // The footprint minimum-extent clamps: shrink the wheel volume so each
    // side lands inside q + 0x2000 of the origin. q = (4 << 16) >> 2.
    volumes[0].min_x_fp16 = -0x1000;
    volumes[0].max_x_fp16 = 0x1000;
    volumes[0].min_y_fp16 = -0x1000;
    volumes[0].max_y_fp16 = 0x1000;
    check(threedi_3di3_collision_probe_boxes(&collision, &boxes) == 1,
          "probe boxes derive with a narrow wheel volume");
    const int32_t q = ((2 << 16) - (-(2 << 16))) >> 2;
    check(boxes.foot_x_lo == -0x2000 - q && boxes.foot_x_hi == q + 0x2000,
          "footprint X clamps to at least q + 0x2000 from the origin");
    check(boxes.foot_y_lo == -0x2000 - q && boxes.foot_y_hi == q + 0x2000,
          "footprint Y clamps to at least q + 0x2000 from the origin");
    check(boxes.box_x_lo == -(9 << 16) && boxes.box_x_hi == 4 << 16,
          "the half box is stored unclamped");

    // Degenerate rejections: no CMDL Z span, and no type-1 volume in the
    // lower half.
    collision.model_data.bbox[5] = collision.model_data.bbox[2];
    check(threedi_3di3_collision_probe_boxes(&collision, &boxes) == 0,
          "a zero CMDL Z span keeps the caller's degenerate fallback");
    collision.model_data.bbox[5] = 304853.0f / 65536.0f;
    volumes[0].collidable_type = 7;
    volumes[1].collidable_type = 7;
    check(threedi_3di3_collision_probe_boxes(&collision, &boxes) == 0,
          "no lower-half type-1 volume keeps the caller's degenerate fallback");
}

int main() {
    // Packed struct layout pins: the 44-B runtime CFAC record shape depends
    // on these staying exact.
    check(sizeof(ThreediCollisionNormal) == 14, "collision normal is 14 packed bytes");
    check(sizeof(ThreediCollisionFace) == 44,
          "collision face matches the 44-B runtime CFAC record");
    check(offsetof(ThreediCollisionFace, normal_index) == 6,
          "collision face normal index occupies the authored CFAC slot");
    check(offsetof(ThreediCollisionFace, plane_dist_fp16) == 8,
          "collision face plane distance offset is stable");
    check(offsetof(ThreediCollisionFace, material_flags) == 36,
          "collision face material_flags offset is stable");
    check(offsetof(ThreediCollisionFace, poly_type) == 40,
          "collision face poly_type offset is stable");
    check(sizeof(ThreediCollisionObject) == 88, "collision object ABI is 88 bytes");
    check(offsetof(ThreediCollisionObject, offset) == 36,
          "collision object exact offset is stable");
    check(offsetof(ThreediCollisionObject, med) == 72,
          "collision object exact center offset is stable");
    check(offsetof(ThreediCollisionObject, radius) == 84,
          "collision object exact radius offset is stable");

    // Q14 CNRM and Q8 CVRT values survive the float parse exactly: dividing by
    // a power of two and re-multiplying is lossless in float, which is what
    // lets the runtime collision build recover the authored words verbatim.
    {
        const int16_t q14_samples[] = {1, -1, 4096, -4096, 8192, 12288, 16383, -16384};
        for (const int16_t q : q14_samples) {
            const float parsed = static_cast<float>(q) / 16384.0f;
            check(static_cast<int16_t>(std::lround(parsed * 16384.0f)) == q,
                  "authored Q14 word recovers exactly through the float parse");
        }
        const int16_t q8_samples[] = {1, -1, 255, -255, 12345, -32768, 32767};
        for (const int16_t q : q8_samples) {
            const float parsed = static_cast<float>(q) / 256.0f;
            check(static_cast<int16_t>(std::lround(parsed * 256.0f)) == q,
                  "authored Q8 word recovers exactly through the float parse");
        }
    }

    // Runtime-safety validation on the raw collision block.
    ThreediBoundingPlane planes[2] = {};
    ThreediBoundingVolume volume = {};
    volume.plane_count = 2;
    ThreediCollisionModel collision = {};
    collision.planes = planes;
    collision.plane_count = 2;
    collision.volumes = &volume;
    collision.volume_count = 1;

    check(threedi_3di3_collision_is_runtime_safe(&collision) == 1,
          "bounded non-empty plane window is safe");

    volume.plane_count = 0;
    check(threedi_3di3_collision_is_runtime_safe(&collision) == 0,
          "plane-less volume is rejected");
    volume.plane_count = 3;
    check(threedi_3di3_collision_is_runtime_safe(&collision) == 0,
          "overrunning plane window is rejected");
    volume.plane_count = 2;
    collision.planes = nullptr;
    check(threedi_3di3_collision_is_runtime_safe(&collision) == 0,
          "missing plane array is rejected");
    collision.planes = planes;
    collision.volumes = nullptr;
    check(threedi_3di3_collision_is_runtime_safe(&collision) == 0,
          "missing volume array is rejected");
    check(threedi_3di3_collision_is_runtime_safe(nullptr) == 0,
          "null collision block is rejected");

    test_cxlt_is_preserved_metadata_not_a_vertex_offset();
    test_charmodel_cobj_preserves_exact_bone_sphere();
    test_collision_probe_boxes_follow_the_witnessed_folds();

    // Retail models (Zodiacs, mounted weapons, large buildings) author
    // TRAILING BVOLs owned by no COBJ. Every retail walker consumes volumes
    // only through the per-COBJ runs, so the unowned tail is dead data and
    // must not strip the whole model's collision.
    ThreediBoundingPlane grouped_planes[6] = {};
    ThreediBoundingVolume grouped_volumes[3] = {};
    ThreediCollisionObject grouped_objects[2] = {};
    grouped_objects[0].num_bounding_volumes = 1;
    grouped_objects[1].num_bounding_volumes = 1;
    grouped_volumes[0].plane_count = 2;
    grouped_volumes[1].plane_count = 2;
    grouped_volumes[2].plane_count = 2;
    ThreediCollisionModel grouped = {};
    grouped.planes = grouped_planes;
    grouped.plane_count = 6;
    grouped.volumes = grouped_volumes;
    grouped.volume_count = 3;
    grouped.objects = grouped_objects;
    grouped.object_count = 2;
    check(threedi_3di3_collision_is_runtime_safe(&grouped) == 1,
          "an unowned trailing BVOL tail is safe (retail corpus shape)");
    {
        ThreediCollisionObjectRun runs[2] = {};
        check(threedi_collision_object_runs(&grouped, runs) == 1,
              "grouped BVOL runs resolve");
        check(runs[0].volume_start == 0 && runs[1].volume_start == 1,
              "BVOL run starts are the prefix sums over the COBJ counts");
    }
    grouped_objects[1].num_bounding_volumes = 3; // claims past the pool
    check(threedi_3di3_collision_is_runtime_safe(&grouped) == 0,
          "COBJ volume runs overrunning the BVOL pool are rejected");
    {
        ThreediCollisionObjectRun runs[2] = {};
        check(threedi_collision_object_runs(&grouped, runs) == 0,
              "overrunning runs are rejected by the run resolver too");
    }
    grouped_objects[1].num_bounding_volumes = 1;
    grouped_volumes[2].plane_count = 9; // tail plane windows stay validated
    check(threedi_3di3_collision_is_runtime_safe(&grouped) == 0,
          "a tail volume with an overrunning plane window is rejected");
    grouped_volumes[2].plane_count = 2;

    // A face whose CNRM index is -1 is skipped by the runtime CFAC walker
    // [orig: the CNRM resolve gate @ 0x4e4cb0]; it must not reject the model.
    ThreediCollisionVertex loose_vertices[3] = {};
    ThreediCollisionFace loose_face = {};
    loose_face.vert_index[0] = 0;
    loose_face.vert_index[1] = 1;
    loose_face.vert_index[2] = 2;
    loose_face.normal_index = -1;
    ThreediCollisionObject loose_object = {};
    loose_object.num_vertices = 3;
    loose_object.num_faces = 1;
    ThreediCollisionModel loose = {};
    loose.vertices = loose_vertices;
    loose.vertex_count = 3;
    loose.faces = &loose_face;
    loose.face_count = 1;
    loose.objects = &loose_object;
    loose.object_count = 1;
    check(threedi_3di3_collision_is_runtime_safe(&loose) == 1,
          "a face with CNRM index -1 is safe (the runtime walker skips it)");
    loose_face.normal_index = 0; // >= the object's zero-normal run
    check(threedi_3di3_collision_is_runtime_safe(&loose) == 0,
          "an out-of-run CNRM index is still rejected");
    loose_face.normal_index = -1;
    loose_face.vert_index[2] = 3; // outside the object's CVRT run
    check(threedi_3di3_collision_is_runtime_safe(&loose) == 0,
          "an out-of-run CVRT index is rejected");
    loose_face.vert_index[2] = 2;
    loose.objects = nullptr;
    loose.object_count = 0;
    check(threedi_3di3_collision_is_runtime_safe(&loose) == 0,
          "an object-less block with face data is rejected");

    if (failures == 0) std::printf("collision_3di_test: OK\n");
    return failures == 0 ? 0 : 1;
}
