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

    if (failures == 0) std::printf("collision_ir_test: OK\n");
    return failures == 0 ? 0 : 1;
}
