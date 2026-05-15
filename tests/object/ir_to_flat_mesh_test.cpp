// Phase B Task 4-12: IR → FlatMesh conversion tests against a synthetic IR.
#include "object/flat_mesh.h"
#include "object/ir_to_flat_mesh.h"
#include "threedi/threedi_3di3.h"
#include "common/test_expect.h"
#include "synthetic_ir.h"
#include <cstring>

int main(void) {
    Threedi3di3 ir;
    TEST_EXPECT(synthetic_ir_build_quad(&ir) == 0);

    // Task 4: API exists, converts a synthetic 1-LOD model to 1 FlatMesh
    // with the correct vertex_count and face_count.
    FlatMeshArray arr;
    std::memset(&arr, 0, sizeof(arr));
    TEST_EXPECT(object_ir_to_flat_meshes(&ir, /*lod_index=*/0,
                                         /*include_empty_parts=*/0,
                                         /*track_bone_data=*/0,
                                         &arr) == 0);
    TEST_EXPECT(arr.count == 1);
    TEST_EXPECT(arr.meshes != nullptr);
    TEST_EXPECT(arr.meshes[0].vertex_count == 4);
    TEST_EXPECT(arr.meshes[0].face_count == 2);
    object_flat_mesh_array_free(&arr);

    // Re-convert with the now-fixed body.
    object_flat_mesh_array_free(&arr);
    TEST_EXPECT(object_ir_to_flat_meshes(&ir, 0, 0, 0, &arr) == 0);

    // Vertex subrange per part: the synthetic quad's part owns verts 0..3.
    // Raw indices are (0,1,2, 0,2,3). After (0,2,1) winding swap:
    //   triangle 0: (0, 2, 1)
    //   triangle 1: (0, 3, 2)
    TEST_EXPECT(arr.meshes[0].vertex_count == 4);
    TEST_EXPECT(arr.meshes[0].faces[0].v[0] == 0);
    TEST_EXPECT(arr.meshes[0].faces[0].v[1] == 2);
    TEST_EXPECT(arr.meshes[0].faces[0].v[2] == 1);
    TEST_EXPECT(arr.meshes[0].faces[1].v[0] == 0);
    TEST_EXPECT(arr.meshes[0].faces[1].v[1] == 3);
    TEST_EXPECT(arr.meshes[0].faces[1].v[2] == 2);
    object_flat_mesh_array_free(&arr);

    object_flat_mesh_array_free(&arr);
    TEST_EXPECT(object_ir_to_flat_meshes(&ir, 0, 0, 0, &arr) == 0);

    // UVs for face 0: after (0,2,1) swap, face 0 visits vertices (0, 2, 1).
    // vertex.uv0 = position.xy, so after V-flip (v_out = 1 - v_in):
    //   corner 0 -> vert 0 -> uv0=(0, 1-0)=(0, 1)
    //   corner 1 -> vert 2 -> uv0=(1, 1-1)=(1, 0)
    //   corner 2 -> vert 1 -> uv0=(1, 1-0)=(1, 1)
    const FlatMeshFaceCornerData* c = arr.meshes[0].corners;
    TEST_EXPECT(c[0].u0 == 0.0f && c[0].v0 == 1.0f);
    TEST_EXPECT(c[1].u0 == 1.0f && c[1].v0 == 0.0f);
    TEST_EXPECT(c[2].u0 == 1.0f && c[2].v0 == 1.0f);
    // uv1 is zero for the synthetic quad (vertex.uv1 not populated), so after V-flip: v1 = 1-0 = 1.
    TEST_EXPECT(c[0].u1 == 0.0f && c[0].v1 == 1.0f);
    object_flat_mesh_array_free(&arr);

    object_flat_mesh_array_free(&arr);
    TEST_EXPECT(object_ir_to_flat_meshes(&ir, 0, 0, 0, &arr) == 0);

    // All corner normals: synthetic quad raw normal = (0, 0, 1).
    // After render_space(0, 0, 1) = (-0, -1, 0) = (0, -1, 0).
    for (int f = 0; f < 2; ++f) {
        for (int c = 0; c < 3; ++c) {
            const FlatMeshFaceCornerData* cd = &arr.meshes[0].corners[f * 3 + c];
            TEST_EXPECT(cd->nx == 0.0f);
            TEST_EXPECT(cd->ny == -1.0f);
            TEST_EXPECT(cd->nz == 0.0f);
        }
    }
    object_flat_mesh_array_free(&arr);

    object_flat_mesh_array_free(&arr);
    TEST_EXPECT(object_ir_to_flat_meshes(&ir, 0, 0, 0, &arr) == 0);

    TEST_EXPECT(arr.meshes[0].material_id_set_count == 1);
    TEST_EXPECT(arr.meshes[0].material_id_set[0] == 0);
    TEST_EXPECT(arr.meshes[0].faces[0].material_id == 0);
    TEST_EXPECT(arr.meshes[0].faces[1].material_id == 0);
    object_flat_mesh_array_free(&arr);

    object_flat_mesh_array_free(&arr);
    TEST_EXPECT(object_ir_to_flat_meshes(&ir, 0, 0, 0, &arr) == 0);
    // Synthetic quad: both faces coplanar with identical normals -> flat component -> SG=0.
    // Task 7 cascade: was hardcoded 1; now compute_smoothing_groups returns 0 for flat comps.
    TEST_EXPECT(arr.meshes[0].faces[0].smoothing_group_mask == 0u);
    TEST_EXPECT(arr.meshes[0].faces[1].smoothing_group_mask == 0u);
    object_flat_mesh_array_free(&arr);

    // Task 5: bone_table resolution + 3-weight cap + part_idx fallback.
    // Skinned synthetic has bone_table[0..2]={11,22,33} on the strip.
    // Per-vertex weights drive variable CSR entry counts.
    Threedi3di3 ir_skinned;
    TEST_EXPECT(synthetic_ir_build_quad_skinned(&ir_skinned) == 0);

    FlatMeshArray arr_skinned;
    std::memset(&arr_skinned, 0, sizeof(arr_skinned));
    TEST_EXPECT(object_ir_to_flat_meshes(&ir_skinned, 0,
                                         /*include_empty_parts=*/0,
                                         /*track_bone_data=*/1,
                                         &arr_skinned) == 0);
    const FlatMesh* fm = &arr_skinned.meshes[0];
    TEST_EXPECT(fm->vertex_count == 4);
    TEST_EXPECT(fm->vertex_influence_offsets != nullptr);
    TEST_EXPECT(fm->vertex_influences != nullptr);

    // CSR offsets: vert0->3 entries, vert1->2, vert2->1(fallback), vert3->1
    // offsets: [0, 3, 5, 6, 7]
    TEST_EXPECT(fm->vertex_influence_offsets[0] == 0);
    TEST_EXPECT(fm->vertex_influence_offsets[1] == 3);
    TEST_EXPECT(fm->vertex_influence_offsets[2] == 5);
    TEST_EXPECT(fm->vertex_influence_offsets[3] == 6);
    TEST_EXPECT(fm->vertex_influence_offsets[4] == 7);

    // Vert 0: weights (0.5, 0.3, 0.2), bone_indices (0,1,2)
    // -> bone_table resolves to (11,0.5),(22,0.3),(33,0.2)
    int v0 = fm->vertex_influence_offsets[0];
    TEST_EXPECT(fm->vertex_influences[v0 + 0].bone_index == 11);
    TEST_EXPECT(fm->vertex_influences[v0 + 0].weight == 0.5f);
    TEST_EXPECT(fm->vertex_influences[v0 + 1].bone_index == 22);
    TEST_EXPECT(fm->vertex_influences[v0 + 1].weight == 0.3f);
    TEST_EXPECT(fm->vertex_influences[v0 + 2].bone_index == 33);
    TEST_EXPECT(fm->vertex_influences[v0 + 2].weight == 0.2f);

    // Vert 1: weights (0.6, 0.4, 0.0) -> 2 entries (11,0.6),(22,0.4)
    int v1 = fm->vertex_influence_offsets[1];
    TEST_EXPECT(fm->vertex_influences[v1 + 0].bone_index == 11);
    TEST_EXPECT(fm->vertex_influences[v1 + 0].weight == 0.6f);
    TEST_EXPECT(fm->vertex_influences[v1 + 1].bone_index == 22);
    TEST_EXPECT(fm->vertex_influences[v1 + 1].weight == 0.4f);

    // Vert 2: weights (0,0,0) -> no positives -> fallback (part_idx=0, 1.0)
    int v2 = fm->vertex_influence_offsets[2];
    TEST_EXPECT(fm->vertex_influences[v2 + 0].bone_index == 0);  // part_index
    TEST_EXPECT(fm->vertex_influences[v2 + 0].weight == 1.0f);

    // Vert 3: weights (1.0, 0, 0) -> 1 entry (11, 1.0)
    int v3 = fm->vertex_influence_offsets[3];
    TEST_EXPECT(fm->vertex_influences[v3 + 0].bone_index == 11);
    TEST_EXPECT(fm->vertex_influences[v3 + 0].weight == 1.0f);

    object_flat_mesh_array_free(&arr_skinned);

    // track_bone_data=0 should leave the arrays NULL even for skinned IR.
    TEST_EXPECT(object_ir_to_flat_meshes(&ir_skinned, 0, 0, 0, &arr_skinned) == 0);
    TEST_EXPECT(arr_skinned.meshes[0].vertex_influence_offsets == nullptr);
    TEST_EXPECT(arr_skinned.meshes[0].vertex_influences == nullptr);
    object_flat_mesh_array_free(&arr_skinned);

    synthetic_ir_free(&ir_skinned);

    // Task 6: source-index dedup via object_ir_to_flat_meshes_v2.
    // 8-vert IR, 2 triangles, indices [5,3,7,3,5,0].
    // After (0,2,1) winding swap: tri0=(5,7,3), tri1=(3,0,5).
    // With preserve_source_indexing=1: first-seen order gives
    //   source 5->local 0, source 7->local 1, source 3->local 2, source 0->local 3.
    // Faces: (0,1,2) and (2,3,0).
    {
        Threedi3di3 dedup_ir = make_dedup_test_ir();
        FlatMeshArray dedup_arr;
        std::memset(&dedup_arr, 0, sizeof(dedup_arr));
        TEST_EXPECT(object_ir_to_flat_meshes_v2(&dedup_ir, /*lod_index=*/0,
                                                /*include_empty_parts=*/0,
                                                /*track_bone_data=*/0,
                                                /*preserve_source_indexing=*/1,
                                                &dedup_arr) == 0);
        TEST_EXPECT(dedup_arr.count == 1);
        const FlatMesh* fm_dedup = &dedup_arr.meshes[0];
        // Only 4 unique source indices visited in corner order.
        TEST_EXPECT(fm_dedup->vertex_count == 4);
        TEST_EXPECT(fm_dedup->face_count == 2);
        // face 0 = (local 0, local 1, local 2) = (src 5, src 7, src 3)
        TEST_EXPECT(fm_dedup->faces[0].v[0] == 0);
        TEST_EXPECT(fm_dedup->faces[0].v[1] == 1);
        TEST_EXPECT(fm_dedup->faces[0].v[2] == 2);
        // face 1 = (local 2, local 3, local 0) = (src 3, src 0, src 5)
        TEST_EXPECT(fm_dedup->faces[1].v[0] == 2);
        TEST_EXPECT(fm_dedup->faces[1].v[1] == 3);
        TEST_EXPECT(fm_dedup->faces[1].v[2] == 0);
        object_flat_mesh_array_free(&dedup_arr);
        synthetic_ir_free(&dedup_ir);
    }

    synthetic_ir_free(&ir);
    return 0;
}
