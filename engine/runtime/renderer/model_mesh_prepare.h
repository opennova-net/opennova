#pragma once

#include <formats/threedi/threedi_3di3.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::renderer {

struct MeshPreparationOptions {
    bool skeletal = false;
    int bone_count = 0;
    bool native_frame = false;
};

// CPU columns ready for device array packing. The ordinary render frame
// reflects X; native_frame reverses that reflection and triangle winding.
// Part origins retain their placement frame in either mesh convention.
struct PreparedMeshSurface {
    size_t primitive_index = 0;
    int material_index = 0;
    int material_array_index = -1;
    int part_index = 0;
    int parent_index = -1;
    std::array<float, 3> abs{};
    bool is_alpha = false;
    uint32_t vertex_offset = 0;
    std::vector<std::array<float, 3>> vertices;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::array<float, 2>> uvs2;
    std::vector<std::array<float, 4>> tangents;
    std::vector<std::array<int32_t, 4>> bones;
    std::vector<std::array<float, 4>> weights;
    // Per skinned vertex: the parts of the strip's bone-table entries before
    // the one its first index byte names, nearest first (-1 past the table's
    // start). The lit skinned effects light a vertex through its first
    // palette entry's inverse; when that matrix has none, retail's fill keeps
    // the inverse of the nearest earlier entry that had one.
    std::vector<std::array<int32_t, 4>> light_fallback_bones;
    std::vector<int32_t> indices;
};

std::vector<PreparedMeshSurface> prepare_model_mesh(
        const threedi::Threedi3di3 &model, int lod_index,
        MeshPreparationOptions options = {});

} // namespace opennova::renderer
