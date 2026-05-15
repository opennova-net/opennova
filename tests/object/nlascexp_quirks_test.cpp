#include "object/nlascexp_quirks.h"
#include <cstdio>
#include <cstring>

#define EXPECT_EQ(actual, expected) \
    do { auto a = (actual); auto e = (expected); \
        if (a != e) { std::fprintf(stderr, "FAIL %s:%d: %s = %lld, expected %lld\n", \
            __FILE__, __LINE__, #actual, (long long)a, (long long)e); return 1; } \
    } while (0)

int main() {
    // Handedness — identity basis -> positive (returns 0).
    {
        float tm[4][3] = {{1,0,0}, {0,1,0}, {0,0,1}, {0,0,0}};
        EXPECT_EQ(object_node_tm_has_negative_handedness(tm), 0);
    }
    // Mirror-X — det < 0, returns 1.
    {
        float tm[4][3] = {{-1,0,0}, {0,1,0}, {0,0,1}, {0,0,0}};
        EXPECT_EQ(object_node_tm_has_negative_handedness(tm), 1);
    }
    // Y-up vs Z-up rotation (positive handedness).
    {
        float tm[4][3] = {{0,0,1}, {1,0,0}, {0,1,0}, {0,0,0}};
        EXPECT_EQ(object_node_tm_has_negative_handedness(tm), 0);
    }

    // fixup_name — trims trailing spaces.
    {
        char out[64];
        size_t n = object_fixup_name("01 Mesh0  ", out, sizeof(out));
        EXPECT_EQ((int)n, 8);
        EXPECT_EQ(std::strcmp(out, "01 Mesh0"), 0);
    }
    // fixup_name — preserves leading and interior chars.
    {
        char out[64];
        object_fixup_name("_01 center", out, sizeof(out));
        EXPECT_EQ(std::strcmp(out, "_01 center"), 0);
    }

    // _decode — finds first NUL and stops.
    {
        char out[64];
        size_t n = object_decode_ir_string("akcrate\0\0\0", 10, out, sizeof(out));
        EXPECT_EQ((int)n, 7);
        EXPECT_EQ(std::strcmp(out, "akcrate"), 0);
    }

    std::printf("PASS nlascexp_quirks_test\n");
    return 0;
}
