// Userpoint decode semantics: the authored 16.16 triples decode into model
// space with the side axis mirrored (x->z, y->-x, z->y), so a consumer's
// render-space -X transform preserves the authored driver/passenger side.
#include <cmath>
#include <cstdio>
#include <cstring>

#include <formats/threedi/threedi_3di3.h>

using namespace opennova::threedi;

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

static void test_userpoint_side_axis_is_mirrored() {
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

    float position[3] = {};
    float direction[3] = {};
    threedi_user_point_position(&userpoint, position);
    threedi_user_point_direction(&userpoint, direction);

    check(approx(position[0], -2.0f), "side axis mirrors y into model x");
    check(approx(position[1], 3.0f), "vertical axis maps z into model y");
    check(approx(position[2], 1.0f), "forward axis maps x into model z");
    check(approx(direction[0], -5.0f), "direction side axis mirrors rot_y into model x");
    check(approx(direction[1], 6.0f), "direction vertical axis maps rot_z into model y");
    check(approx(direction[2], 4.0f), "direction forward axis maps rot_x into model z");
}

static void test_userpoint_fractional_fixed_point() {
    ThreediUserPoint userpoint;
    std::memset(&userpoint, 0, sizeof(userpoint));
    userpoint.x = 0x18000;  // 1.5
    userpoint.y = -0x8000;  // -0.5
    userpoint.z = 0x28000;  // 2.5

    float position[3] = {};
    threedi_user_point_position(&userpoint, position);
    check(approx(position[0], 0.5f), "fractional side axis decodes (mirrored)");
    check(approx(position[1], 2.5f), "fractional vertical axis decodes");
    check(approx(position[2], 1.5f), "fractional forward axis decodes");
}

// A `sitex` seat is a case-insensitive prefix from byte zero; the seat scan
// binds 8 of them.
static void test_userpoint_sitex_seats() {
    check(THREEDI_SITEX_SEAT_LIMIT == 8, "the seat scan binds 8 sitex seats");
    for (const char *name : {"sitex", "sitex00d", "SiteX12a", "SITEX"})
        check(threedi_user_point_is_sitex(name), name);
    for (const char *name : {"", "sit", "sitx00", " sitex01", "xsitex", "ctrlx00", "drvrx01", "UseGun"})
        check(!threedi_user_point_is_sitex(name), name);
}

int main() {
    test_userpoint_side_axis_is_mirrored();
    test_userpoint_fractional_fixed_point();
    test_userpoint_sitex_seats();

    if (failures == 0) {
        std::printf("userpoint_test: OK\n");
        return 0;
    }
    std::printf("userpoint_test: %d FAILED\n", failures);
    return 1;
}
