#include <cmath>
#include <cstdio>
#include <cstring>

#include "threedi/threedi_3di3.h"
#include "threedi/threedi_gp.h"
#include "threedi/threedi_ir.h"

static int failures = 0;

static void check(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    }
}

static bool approx(float a, float b) {
    return std::fabs(a - b) < 1e-5f;
}

static void test_3di_userpoint_side_axis_is_mirrored() {
    ThreediUserPoint userpoint;
    std::memset(&userpoint, 0, sizeof(userpoint));
    std::strcpy(userpoint.name, "ctrlx10");
    userpoint.x = 1 << 16;
    userpoint.y = 2 << 16;
    userpoint.z = 3 << 16;
    userpoint.rot_x = 4 << 16;
    userpoint.rot_y = 5 << 16;
    userpoint.rot_z = 6 << 16;
    userpoint.subobject_index = 7;
    userpoint.userpoint_type = 8;

    Threedi3di3 model;
    std::memset(&model, 0, sizeof(model));
    model.user_points = &userpoint;
    model.user_point_count = 1;

    ThreediModelIR ir;
    const int rc = threedi_ir_from_3di3(&model, &ir);
    check(rc == 0, "3DI userpoint converts to IR");
    if (rc != 0) {
        return;
    }
    check(ir.userpoint_count == 1, "3DI userpoint count preserved");

    const ThreediIRUserPoint &converted = ir.userpoints[0];
    check(std::strcmp(converted.name, "ctrlx10") == 0, "3DI userpoint name preserved");
    check(approx(converted.position[0], -2.0f), "3DI side axis mirrors y into IR x");
    check(approx(converted.position[1], 3.0f), "3DI vertical axis maps z into IR y");
    check(approx(converted.position[2], 1.0f), "3DI forward axis maps x into IR z");
    check(approx(converted.direction[0], -5.0f), "3DI direction side axis mirrors rot_y into IR x");
    check(approx(converted.direction[1], 6.0f), "3DI direction vertical axis maps rot_z into IR y");
    check(approx(converted.direction[2], 4.0f), "3DI direction forward axis maps rot_x into IR z");
    check(converted.part_index == 7, "3DI userpoint part index preserved");
    check(converted.type_code == 8, "3DI userpoint type preserved");

    threedi_ir_free(&ir);
}

static void test_gp_userpoint_side_axis_is_mirrored() {
    ThreediGpUserPoint userpoint;
    std::memset(&userpoint, 0, sizeof(userpoint));
    std::strcpy(userpoint.name, "sitex00");
    userpoint.x = 9 << 16;
    userpoint.y = 10 << 16;
    userpoint.z = 11 << 16;
    userpoint.rot_x = 12 << 16;
    userpoint.rot_y = 13 << 16;
    userpoint.rot_z = 14 << 16;
    userpoint.parent_subobject = 15;
    userpoint.type_code = 16;

    ThreediGpFile gp;
    std::memset(&gp, 0, sizeof(gp));
    gp.userpoints = &userpoint;
    gp.userpoint_count = 1;

    ThreediModelIR ir;
    const int rc = threedi_ir_from_gp(&gp, &ir);
    check(rc == 0, "GP userpoint converts to IR");
    if (rc != 0) {
        return;
    }
    check(ir.userpoint_count == 1, "GP userpoint count preserved");

    const ThreediIRUserPoint &converted = ir.userpoints[0];
    check(std::strcmp(converted.name, "sitex00") == 0, "GP userpoint name preserved");
    check(approx(converted.position[0], -10.0f), "GP side axis mirrors y into IR x");
    check(approx(converted.position[1], 11.0f), "GP vertical axis maps z into IR y");
    check(approx(converted.position[2], 9.0f), "GP forward axis maps x into IR z");
    check(approx(converted.direction[0], -13.0f), "GP direction side axis mirrors rot_y into IR x");
    check(approx(converted.direction[1], 14.0f), "GP direction vertical axis maps rot_z into IR y");
    check(approx(converted.direction[2], 12.0f), "GP direction forward axis maps rot_x into IR z");
    check(converted.part_index == 15, "GP userpoint part index preserved");
    check(converted.type_code == 16, "GP userpoint type preserved");

    threedi_ir_free(&ir);
}

int main() {
    test_3di_userpoint_side_axis_is_mirrored();
    test_gp_userpoint_side_axis_is_mirrored();

    if (failures == 0) {
        std::printf("userpoint_ir_test: OK\n");
        return 0;
    }
    std::printf("userpoint_ir_test: %d FAILED\n", failures);
    return 1;
}
