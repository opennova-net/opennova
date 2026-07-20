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
            const int32_t *offset = collision->objects[1].offset;
            const int32_t *translation = collision->translations[0].translation;
            check(offset[0] == 23193 && offset[1] == 39 && offset[2] == 34085,
                  "COBJ offset remains raw fp16 metadata");
            check(translation[0] == offset[0] && translation[1] == offset[1] &&
                          translation[2] == offset[2],
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

    ThreediModelIR ir = {};
    const int ir_rc = threedi_ir_from_3di3(&model, &ir);
    check(ir_rc == 0, "CharModel converts to collision IR");
    if (ir_rc != 0) {
        threedi_3di3_free(&model);
        return;
    }

    const ThreediIRCollision *collision = ir.collision;
    check(collision != nullptr, "CharModel carries collision IR");
    if (collision != nullptr) {
        check(collision->object_count == 19, "CharModel keeps all 19 COBJ records");
        if (collision->object_count > 14) {
            const ThreediIRCollisionObject &head = collision->objects[14];
            check(head.parent_subobject_index == 13,
                  "CharModel head COBJ keeps its parent bone");
            check(head.num_vertices == 0 && head.num_faces == 0,
                  "CharModel head COBJ is a sphere-only skeletal section");
            check(head.center_fp16[0] == 3578 && head.center_fp16[1] == 65 &&
                          head.center_fp16[2] == 54371,
                  "CharModel head COBJ center remains exact signed 16.16 data");
            check(head.radius_fp16 == 10345,
                  "CharModel head COBJ radius remains exact signed 16.16 data");
        }
    }

    threedi_ir_free(&ir);
    threedi_3di3_free(&model);
}

int main() {
    check(sizeof(ThreediIRCollisionNormal) == 8, "collision normal ABI is 8 bytes");
    check(sizeof(ThreediIRCollisionFace) == 52,
          "collision face ABI includes indexed and self-contained normal fields");
    check(offsetof(ThreediIRCollisionFace, normal_index) == 6,
          "collision face normal index occupies the authored CFAC slot");
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
    check(sizeof(ThreediIRCollisionObject) == 72, "collision object ABI is 72 bytes");
    check(offsetof(ThreediIRCollisionObject, offset) == 20,
          "collision object exact offset is stable");
    check(offsetof(ThreediIRCollisionObject, mid) == 56 &&
                  offsetof(ThreediIRCollisionObject, center_fp16) == 56,
          "collision object center aliases share exact storage");
    check(offsetof(ThreediIRCollisionObject, radius) == 68 &&
                  offsetof(ThreediIRCollisionObject, radius_fp16) == 68,
          "collision object radius aliases share exact storage");
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
    test_charmodel_cobj_preserves_exact_bone_sphere();

    ThreediIRCollisionPlane grouped_planes[4] = {};
    ThreediIRCollisionVolume grouped_volumes[2] = {};
    ThreediIRCollisionObject grouped_objects[2] = {};
    grouped_objects[0].num_bounding_volumes = 1;
    grouped_objects[1].num_bounding_volumes = 1;
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

    // Retail models (Zodiacs, mounted weapons, large buildings) author
    // TRAILING BVOLs owned by no COBJ. Every retail walker consumes volumes
    // only through the per-COBJ runs, so the unowned tail is dead data and
    // must not strip the whole model's collision.
    ThreediIRCollisionVolume tail_volumes[3] = {};
    tail_volumes[0] = grouped_volumes[0];
    tail_volumes[1] = grouped_volumes[1];
    tail_volumes[2].plane_start = 0;
    tail_volumes[2].plane_count = 2;
    tail_volumes[2].object_index = -1;
    grouped.volumes = tail_volumes;
    grouped.volume_count = 3;
    check(threedi_ir_collision_is_runtime_safe(&grouped) == 1,
          "an unowned trailing BVOL tail is safe (retail corpus shape)");
    tail_volumes[2].object_index = 0; // an owned volume outside every run
    check(threedi_ir_collision_is_runtime_safe(&grouped) == 0,
          "an owned volume outside every COBJ run is rejected");
    tail_volumes[2].object_index = -1;
    tail_volumes[2].plane_count = 9; // tail plane windows stay validated
    check(threedi_ir_collision_is_runtime_safe(&grouped) == 0,
          "a tail volume with an overrunning plane window is rejected");
    grouped.volumes = grouped_volumes;
    grouped.volume_count = 2;

    // A face whose CNRM index is -1 is skipped by the runtime CFAC walker
    // [orig: the CNRM resolve gate @ 0x4e4cb0]; it must not reject the model.
    ThreediIRCollisionVertex loose_vertices[3] = {};
    ThreediIRCollisionFace loose_face = {};
    loose_face.vert_index[0] = 0;
    loose_face.vert_index[1] = 1;
    loose_face.vert_index[2] = 2;
    loose_face.normal_index = -1;
    ThreediIRCollisionObject loose_object = {};
    loose_object.num_vertices = 3;
    loose_object.num_faces = 1;
    ThreediIRCollision loose = {};
    loose.vertices = loose_vertices;
    loose.vertex_count = 3;
    loose.faces = &loose_face;
    loose.face_count = 1;
    loose.objects = &loose_object;
    loose.object_count = 1;
    check(threedi_ir_collision_is_runtime_safe(&loose) == 1,
          "a face with CNRM index -1 is safe (the runtime walker skips it)");
    loose_face.normal_index = 0; // >= the object's zero-normal run
    check(threedi_ir_collision_is_runtime_safe(&loose) == 0,
          "an out-of-run CNRM index is still rejected");

    // Synthetic modern CDTA conversion retains every query-relevant CNRM,
    // CFAC, and COBJ field without a float/fixed-point information loss.
    ThreediCollisionVertex src_vertices[3] = {};
    src_vertices[0].position[0] = 1.0f;
    src_vertices[1].position[1] = 2.0f;
    src_vertices[2].position[2] = 3.0f;
    ThreediCollisionNormal src_normal = {};
    src_normal.normal[0] = 0.5f;
    src_normal.normal[1] = -0.25f;
    src_normal.normal[2] = 0.75f;
    src_normal.dominate_axis = 4;
    ThreediCollisionFace src_face = {};
    src_face.vert_index[0] = 0;
    src_face.vert_index[1] = 1;
    src_face.vert_index[2] = 2;
    src_face.normal_index = 0;
    src_face.plane_dist_fp16 = -123456;
    src_face.min_x_fp16 = -11;
    src_face.min_y_fp16 = -22;
    src_face.min_z_fp16 = -33;
    src_face.max_x_fp16 = 44;
    src_face.max_y_fp16 = 55;
    src_face.max_z_fp16 = 66;
    src_face.material_flags = 0x12345678u;
    src_face.poly_type = 17;
    ThreediCollisionObject src_object = {};
    src_object.num_vertices = 3;
    src_object.num_faces = 1;
    src_object.num_normals = 1;
    src_object.num_bounding_volumes = 0;
    src_object.parent_subobject_index = 7;
    for (int k = 0; k < 3; ++k) {
        src_object.offset[k] = 0x12345679 + k;
        src_object.min[k] = -0x12345679 - k;
        src_object.max[k] = 0x23456789 + k;
        src_object.med[k] = 0x10203041 + k;
    }
    src_object.radius = 0x3456789;
    ThreediCollisionModel src_collision = {};
    src_collision.vertices = src_vertices;
    src_collision.vertex_count = 3;
    src_collision.normals = &src_normal;
    src_collision.normal_count = 1;
    src_collision.faces = &src_face;
    src_collision.face_count = 1;
    src_collision.objects = &src_object;
    src_collision.object_count = 1;
    Threedi3di3 src_model = {};
    src_model.collision = &src_collision;
    ThreediModelIR converted;
    threedi_ir_init(&converted);
    check(threedi_ir_from_3di3(&src_model, &converted) == 0,
          "synthetic modern CDTA converts");
    check(converted.collision != nullptr, "converted collision exists");
    if (converted.collision != nullptr) {
        const ThreediIRCollision *c = converted.collision;
        check(c->normal_count == 1 && c->face_count == 1 && c->object_count == 1,
              "normal/face/object counts retained");
        check(c->normals[0].normal_q14[0] == 8192 &&
              c->normals[0].normal_q14[1] == -4096 &&
              c->normals[0].normal_q14[2] == 12288 &&
              c->normals[0].dominant_axis == 4,
              "exact Q14 normal and dominant axis retained");
        const ThreediIRCollisionFace &f = c->faces[0];
        check(f.vert_index[0] == 0 && f.vert_index[1] == 1 && f.vert_index[2] == 2 &&
              f.normal_index == 0 && f.plane_dist_fp16 == -123456,
              "face indices and plane retained");
        check(f.normal[0] == 8192 && f.normal[1] == -4096 && f.normal[2] == 12288 &&
                      f.dominate_axis == 4,
              "face carries the resolved Q14 normal for direct round raycasts");
        check(f.min_fp16[0] == -11 && f.min_fp16[1] == -22 && f.min_fp16[2] == -33 &&
              f.max_fp16[0] == 44 && f.max_fp16[1] == 55 && f.max_fp16[2] == 66 &&
              f.material_flags == 0x12345678u && f.poly_type == 17,
              "face bounds and material retained");
        const ThreediIRCollisionObject &o = c->objects[0];
        check(o.num_vertices == 3 && o.num_faces == 1 && o.num_planes == 1 &&
              o.num_bounding_volumes == 0 && o.parent_subobject_index == 7,
              "all COBJ counts and parent retained");
        check(o.offset[2] == 0x1234567B && o.min[1] == -0x1234567A &&
              o.max[2] == 0x2345678B && o.mid[0] == 0x10203041 &&
              o.radius == 0x3456789,
              "COBJ offset/bounds/mid/radius retain low bits above 2^24");
        check(threedi_ir_collision_is_runtime_safe(c) == 1,
              "face-only exact collision IR is runtime safe");
    }
    threedi_ir_free(&converted);

    if (failures == 0) std::printf("collision_ir_test: OK\n");
    return failures == 0 ? 0 : 1;
}
