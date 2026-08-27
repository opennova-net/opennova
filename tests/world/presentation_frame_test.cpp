// The mission <-> presentation frame map (world/presentation_frame.h): the
// (x, y, z) -> (x, z, -y) remap and its inverse, the view forward from mission
// euler angles, the aim ray endpoint, and the rangefinder clamp.
#include <cmath>
#include <cstdio>

#include <runtime/world/presentation_frame.h>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

void test_remap_and_inverse() {
    const float m[3] = {1.0f, 2.0f, 3.0f};
    float p[3];
    presentation_from_mission(m, p);
    CHECK(p[0] == 1.0f && p[1] == 3.0f && p[2] == -2.0f);
    float back[3];
    mission_from_presentation(p, back);
    CHECK(back[0] == 1.0f && back[1] == 2.0f && back[2] == 3.0f);
}

void test_forward_from_angles() {
    float f[3];
    presentation_forward_from_angles(0.0f, 0.0f, f); // mission north -> -z
    CHECK(near(f[0], 0.0f) && near(f[1], 0.0f) && near(f[2], -1.0f));
    presentation_forward_from_angles(90.0f, 0.0f, f); // mission east -> +x
    CHECK(near(f[0], 1.0f) && near(f[1], 0.0f) && near(f[2], 0.0f));
    presentation_forward_from_angles(0.0f, 90.0f, f); // straight up -> +y
    CHECK(near(f[0], 0.0f) && near(f[1], 1.0f) && near(f[2], 0.0f));
    presentation_forward_from_angles(45.0f, -30.0f, f);
    const float len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    CHECK(near(len, 1.0f));
    CHECK(f[1] < 0.0f);
}

void test_aim_ray_and_rangefinder() {
    const float eye[3] = {10.0f, 2.0f, -5.0f};
    float end[3];
    aim_ray_endpoint(eye, 0.0f, 0.0f, 1000.0f, end);
    CHECK(near(end[0], 10.0f) && near(end[1], 2.0f) && near(end[2], -1005.0f));
    const float pos[3] = {10.0f, 0.0f, -5.0f};
    CHECK(rangefinder_units(pos, end) == 1000); // 1000.002 truncates, clamps to 1000
    const float close[3] = {10.5f, 0.0f, -5.0f};
    CHECK(rangefinder_units(pos, close) == 1); // 0.5 truncates to 0, floors at 1
    const float mid[3] = {10.0f, 0.0f, -5.0f - 123.9f};
    CHECK(rangefinder_units(pos, mid) == 123); // truncation, not rounding
    const float far[3] = {10.0f, 0.0f, -5.0f - 5000.0f};
    CHECK(rangefinder_units(pos, far) == 1000);
}

void test_roll_sign() {
    CHECK(near(presentation_roll_rad(180.0f), 3.14159265f, 1e-4f));
    CHECK(presentation_roll_rad(-10.0f) < 0.0f);
}

} // namespace

int main() {
    test_remap_and_inverse();
    test_forward_from_angles();
    test_aim_ray_and_rangefinder();
    test_roll_sign();
    if (failures == 0) std::printf("presentation_frame_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
