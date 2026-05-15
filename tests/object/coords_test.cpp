#include "object/coords.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

#define EXPECT_FLOAT_NEAR(actual, expected, tol) \
    do { float a = (actual); float e = (expected); \
        if (std::abs(a - e) > (tol)) { \
            std::fprintf(stderr, "FAIL %s:%d: %s = %.6f, expected %.6f\n", \
                __FILE__, __LINE__, #actual, a, e); \
            return 1; \
        } \
    } while (0)

int main() {
    // render_space((x, y, z)) = (-x, -z, y)
    float out[3];
    object_render_space(1.0f, 2.0f, 3.0f, out);
    EXPECT_FLOAT_NEAR(out[0], -1.0f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[1], -3.0f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[2],  2.0f, 1e-6f);

    object_render_space(-0.8269f, -1.0f, 0.325f, out);
    EXPECT_FLOAT_NEAR(out[0],  0.8269f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[1], -0.325f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[2], -1.0f, 1e-6f);

    std::printf("PASS coords_test\n");
    return 0;
}
