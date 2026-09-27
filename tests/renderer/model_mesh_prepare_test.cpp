#include <runtime/renderer/model_mesh_prepare.h>
#include "common/test_expect.h"

#include <array>
#include <vector>

using namespace opennova::threedi;
using namespace opennova::renderer;

int main() {
    ThreediVertex vertices[5]{};
    for (int i = 0; i < 5; ++i) {
        auto &v = vertices[i];
        v.position[0] = static_cast<float>(i + 1);
        v.position[1] = 10.0f;
        v.normal[2] = 1.0f;
        v.tangent[0] = 1.0f;
        v.bitangent[1] = 1.0f;
        v.uv0[0] = 0.25f;
        v.uv1[1] = 0.75f;
        v.bone_indices[0] = 1;
        v.bone_indices[1] = 0;
        v.bone_indices[2] = 2; // Outside the two-entry local bone table.
        v.bone_indices[3] = 1;
        v.bone_weights[0] = 0.5f;
        v.bone_weights[1] = 0.25f;
        v.bone_weights[2] = 0.125f;
    }
    vertices[1].bone_weights[0] = vertices[1].bone_weights[1] = vertices[1].bone_weights[2] = 0;
    // Retail's four-decimal weights sum past 1 in float (ArmGlovD).
    vertices[3].bone_weights[0] = 0.4487f;
    vertices[3].bone_weights[1] = 0.3871f;
    vertices[3].bone_weights[2] = 0.1643f;
    vertices[4].normal[2] = 0; // Zero dot still follows the mirrored handedness rule.
    uint16_t indices[]{0, 1, 2, 3, 2, 3, 4};
    ThreediTriangleStrip strips[3]{};
    strips[0].material_index = 42;
    strips[0].num_indices = 4;
    strips[0].num_vertices = 4;
    strips[0].is_strip = 1;
    strips[0].bone_table_length = 2;
    strips[0].bone_table[0] = 7;
    strips[0].bone_table[1] = 9;
    strips[1].material_index = 1; // In-range material-array fallback.
    strips[1].index_offset = 4;
    strips[1].num_indices = 3;
    strips[1].start_vertex = 2; // Absolute indices, not strip-local indices.
    strips[1].num_vertices = 3;
    strips[2] = strips[0];
    strips[2].material_index = 999;
    strips[2].bone_table_length = 0;
    ThreediRenderObject parts[2]{};
    parts[0].num_strips = 1;
    parts[0].num_alpha_strips = 1;
    parts[0].parent_index = -1;
    parts[0].abs[0] = 3.0f;
    parts[1].num_strips = 1;
    ThreediMaterial materials[2]{};
    materials[0].index = 17;
    materials[1].index = 42;
    ThreediLod lod{};
    lod.vertices.items = vertices;
    lod.vertices.count = 5;
    lod.indices.indices = indices;
    lod.indices.count = 7;
    lod.strips = strips;
    lod.strip_count = 3;
    lod.render_objects = parts;
    lod.render_object_count = 2;
    Threedi3di3 model{};
    model.lods = &lod;
    model.lod_count = 1;
    model.materials = materials;
    model.material_count = 2;

    const auto ordinary = prepare_model_mesh(model, 0);
    TEST_EXPECT(ordinary.size() == 3);
    const auto &first = ordinary[0];
    TEST_EXPECT(first.vertices.size() == 6);
    const float expected_x[]{-1, -2, -3, -2, -4, -3};
    for (size_t i = 0; i < first.vertices.size(); ++i)
        TEST_EXPECT(first.vertices[i][0] == expected_x[i]);
    TEST_EXPECT(first.indices == std::vector<int32_t>({0, 1, 2, 3, 4, 5}));
    TEST_EXPECT((first.normals[0] == std::array<float, 3>{0, 0, 1}));
    TEST_EXPECT((first.tangents[0] == std::array<float, 4>{-1, 0, 0, -1}));
    TEST_EXPECT(first.uvs[0][0] == 0.25f && first.uvs2[0][1] == 0.75f);
    // Retail's blend: the stored weights as they are and 1 - (w0 + w1 + w2)
    // on byte 3, never renormalized; a byte past the table rides bone 0.
    TEST_EXPECT((first.bones[0] == std::array<int32_t, 4>{9, 7, 0, 9}));
    TEST_EXPECT((first.weights[0] == std::array<float, 4>{0.5f, 0.25f, 0.125f, 0.125f}));
    // No stored weight: byte 3's bone takes the whole vertex.
    TEST_EXPECT((first.weights[1] == std::array<float, 4>{0, 0, 0, 1}));
    // A sum past 1 leaves byte 3 a negative weight, kept as the shader keeps it.
    const float rest = 1.0f - ((0.4487f + 0.3871f) + 0.1643f);
    TEST_EXPECT(rest < 0.0f && (first.weights[4] == std::array<float, 4>{0.4487f, 0.3871f, 0.1643f, rest}));
    TEST_EXPECT(first.material_array_index == 1 && first.material_index == 42);
    TEST_EXPECT(first.parent_index == -1 && first.abs[0] == -3);
    TEST_EXPECT(!first.is_alpha && ordinary[1].is_alpha && !ordinary[2].is_alpha);
    TEST_EXPECT(ordinary[1].vertex_offset == 2 && ordinary[1].vertices[0][0] == -3);
    TEST_EXPECT(ordinary[1].material_array_index == 1 && ordinary[2].material_array_index == -1);
    TEST_EXPECT(ordinary[2].part_index == 1 && ordinary[2].bones.empty());

    const auto native = prepare_model_mesh(model, 0, {true, 1, true});
    TEST_EXPECT(native[0].vertices[0][0] == 1 && native[0].abs[0] == -3);
    TEST_EXPECT(native[0].indices == std::vector<int32_t>({0, 2, 1, 3, 5, 4}));
    TEST_EXPECT((native[0].tangents[0] == std::array<float, 4>{1, 0, 0, 1}));
    TEST_EXPECT(native[1].tangents[2][3] == -1); // Zero dot: negate the original +1.
    TEST_EXPECT(native[0].bones == first.bones && native[0].weights == first.weights);
    TEST_EXPECT((native[2].bones[0] == std::array<int32_t, 4>{0, 0, 0, 0}));
    TEST_EXPECT((native[2].weights[0] == std::array<float, 4>{1, 0, 0, 0}));
    TEST_EXPECT(prepare_model_mesh(model, 0, {true, 0, false})[2].bones[0][0] == 1);

    // A rejected strip consumes its place in the ROBJ walk.
    strips[1].index_offset = 999;
    const auto skipped = prepare_model_mesh(model, 0);
    TEST_EXPECT(skipped.size() == 2 && skipped[1].primitive_index == 2 && skipped[1].part_index == 1);
    strips[1].index_offset = 4;
    // Tangent admission covers the entire vertex window, even unused vertices.
    strips[0].num_vertices = 5;
    vertices[4].tangent[0] = vertices[4].bitangent[1] = 0;
    TEST_EXPECT(prepare_model_mesh(model, 0)[0].tangents.empty());
    vertices[4].flags = THREEDI_VERTEX_FLAG_TANGENTS;
    TEST_EXPECT(prepare_model_mesh(model, 0)[0].tangents.size() == 6);
    TEST_EXPECT(prepare_model_mesh(model, -1).empty() && prepare_model_mesh(model, 1).empty());
    TEST_EXPECT(prepare_model_mesh(Threedi3di3{}, 0).empty());
    return 0;
}
