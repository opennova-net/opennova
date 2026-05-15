// Phase B Task 1: struct skeleton smoke test.
// Verifies the header parses, struct layouts have non-zero sizeof,
// and the POD types are zero-initializable.
#include "object/flat_mesh.h"
#include "common/test_expect.h"
#include <cstring>

int main(void) {
    // sizeof checks — protect against accidentally empty struct.
    TEST_EXPECT(sizeof(FlatMeshVertex) == 12);            // 3 floats
    TEST_EXPECT(sizeof(FlatMeshFace) == 20);              // v[3] (3*4) + material_id (4) + smoothing_group_mask (4)
    TEST_EXPECT(sizeof(FlatMeshFaceCornerData) == 28);    // 4 floats UV + 3 floats normal
    TEST_EXPECT(sizeof(FlatMeshBoneInfluence) == 8);      // int32 + float

    // Zero-init of FlatMesh leaves all pointers NULL and counts 0.
    FlatMesh fm;
    std::memset(&fm, 0, sizeof(fm));
    TEST_EXPECT(fm.vertex_count == 0);
    TEST_EXPECT(fm.face_count == 0);
    TEST_EXPECT(fm.vertices == nullptr);
    TEST_EXPECT(fm.faces == nullptr);
    TEST_EXPECT(fm.corners == nullptr);
    TEST_EXPECT(fm.material_id_set == nullptr);
    TEST_EXPECT(fm.vertex_influence_offsets == nullptr);
    TEST_EXPECT(fm.vertex_influences == nullptr);

    FlatMeshArray arr;
    std::memset(&arr, 0, sizeof(arr));
    TEST_EXPECT(arr.count == 0);
    TEST_EXPECT(arr.meshes == nullptr);

    // Array alloc/free roundtrip.
    FlatMeshArray arr2;
    TEST_EXPECT(object_flat_mesh_array_alloc(3, &arr2) == 0);
    TEST_EXPECT(arr2.count == 3);
    TEST_EXPECT(arr2.meshes != nullptr);
    // All inner meshes zero-initialized.
    for (int i = 0; i < 3; ++i) {
        TEST_EXPECT(arr2.meshes[i].vertex_count == 0);
        TEST_EXPECT(arr2.meshes[i].vertices == nullptr);
    }
    object_flat_mesh_array_free(&arr2);
    TEST_EXPECT(arr2.count == 0);
    TEST_EXPECT(arr2.meshes == nullptr);

    // Per-mesh alloc/free roundtrip (used when a mesh's vertex/face arrays
    // need to be populated by the converter and freed by the array-free).
    FlatMesh fm2;
    std::memset(&fm2, 0, sizeof(fm2));
    TEST_EXPECT(object_flat_mesh_resize(&fm2, 10, 5, 2, 0) == 0);
    TEST_EXPECT(fm2.vertex_count == 10);
    TEST_EXPECT(fm2.face_count == 5);
    TEST_EXPECT(fm2.material_id_set_count == 2);
    TEST_EXPECT(fm2.vertices != nullptr);
    TEST_EXPECT(fm2.faces != nullptr);
    TEST_EXPECT(fm2.corners != nullptr);
    TEST_EXPECT(fm2.material_id_set != nullptr);
    TEST_EXPECT(fm2.vertex_influence_offsets == nullptr);  // not requested
    TEST_EXPECT(fm2.vertex_influences == nullptr);
    object_flat_mesh_free(&fm2);
    TEST_EXPECT(fm2.vertex_count == 0);
    TEST_EXPECT(fm2.vertices == nullptr);
    TEST_EXPECT(fm2.faces == nullptr);
    TEST_EXPECT(fm2.corners == nullptr);

    // With skin influences.
    FlatMesh fm3;
    std::memset(&fm3, 0, sizeof(fm3));
    TEST_EXPECT(object_flat_mesh_resize(&fm3, 4, 2, 1, /*total_influences=*/6) == 0);
    TEST_EXPECT(fm3.vertex_count == 4);
    TEST_EXPECT(fm3.vertex_influence_offsets != nullptr);
    TEST_EXPECT(fm3.vertex_influences != nullptr);
    object_flat_mesh_free(&fm3);
    TEST_EXPECT(fm3.vertex_influence_offsets == nullptr);
    TEST_EXPECT(fm3.vertex_influences == nullptr);

    return 0;
}
