#include "object/smoothing_groups.h"
#include <cstdio>
#include <cstring>

#define EXPECT_EQ(actual, expected) \
    do { auto a = (actual); auto e = (expected); \
        if (a != e) { \
            std::fprintf(stderr, "FAIL %s:%d: %s = %d, expected %d\n", \
                __FILE__, __LINE__, #actual, (int)a, (int)e); \
            return 1; \
        } \
    } while (0)

int main() {
    // Two coplanar faces sharing an edge -> same smoothing group.
    // All normals identical -> flat component -> SG=0 (no smoothing needed).
    // See pyopennova/mesh_utils.py::compute_smoothing_groups, flat_comps path.
    int faces[] = { 0,1,2,  2,1,3 };  // face0=(0,1,2), face1=(2,1,3)
    float normals[] = {
        0,0,1,  0,0,1,  0,0,1,   // face0 corners all (0,0,1)
        0,0,1,  0,0,1,  0,0,1,   // face1 corners all (0,0,1)
    };
    uint32_t out[2] = {0, 0};
    object_compute_smoothing_groups(2, faces, normals, out);
    // Both faces are coplanar -> one flat component -> both get 0.
    EXPECT_EQ(out[0], out[1]);
    EXPECT_EQ(out[0], 0u);  // Flat component: SG=0 (mirrors Python flat_comps logic)

    // Two perpendicular faces sharing an edge -> different groups (not smooth-adjacent).
    // Each gets its own non-flat component -> distinct non-zero SG bits.
    int faces2[] = { 0,1,2,  2,1,3 };
    float normals2[] = {
        0,0,1,  0,0,1,  0,0,1,    // face0 z-up
        1,0,0,  1,0,0,  1,0,0,    // face1 x-up
    };
    uint32_t out2[2] = {0, 0};
    object_compute_smoothing_groups(2, faces2, normals2, out2);
    // Perpendicular faces are not smooth-adjacent; each is its own flat component -> both 0.
    // They have different normals so they are NOT in the same smooth component, but each
    // individual component is itself flat (uniform normals within each face) -> SG=0.
    // The disjoint-bits requirement applies to genuinely non-flat components.
    // Both will be 0 here since each single-face component is flat.
    if (out2[0] != 0u || out2[1] != 0u) {
        std::fprintf(stderr, "FAIL: single-face perpendicular components should be flat (SG=0): %08x %08x\n",
                     out2[0], out2[1]);
        return 1;
    }

    // Non-flat component test: two faces sharing an edge but with DIFFERENT normals per vertex
    // (i.e. smooth-adjacent because normals happen to match at shared verts, but the component
    // has varying normals across its corners -> non-flat -> gets a real SG bit).
    // face0: corner normals (0,0,1),(0,0,1),(1,0,0)   -- varies within face0
    // face1: corner normals (1,0,0),(0,0,1),(0,0,1)   -- varies within face1
    // Shared edge verts 1 and 2 have matching normals at the shared corners -> smooth adjacent.
    // Component contains varying normals -> non-flat -> gets SG bit 1.
    int faces3[] = { 0,1,2,  2,1,3 };
    float normals3[] = {
        0,0,1,  0,0,1,  1,0,0,   // face0: corners (0,0,1),(0,0,1),(1,0,0)
        1,0,0,  0,0,1,  0,0,1,   // face1: corners (1,0,0),(0,0,1),(0,0,1)
        // shared edge (1,2): face0 corner1=(0,0,1), face1 corner1=(0,0,1) -> match
        //                    face0 corner2=(1,0,0), face1 corner0=(1,0,0) -> match
    };
    uint32_t out3[2] = {0, 0};
    object_compute_smoothing_groups(2, faces3, normals3, out3);
    // One smooth-adjacent component, but it has varying normals -> non-flat -> SG bit 1 set.
    EXPECT_EQ(out3[0], out3[1]);  // Same component
    EXPECT_EQ(out3[0], 1u);       // First non-flat component gets color=0 -> bit 1<<0 = 1

    std::printf("PASS smoothing_groups_test\n");
    return 0;
}
