#include <cstddef>
#include <cstdio>

#include "threedi/threedi_ir.h"

static int failures = 0;

static void check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
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
