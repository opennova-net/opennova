#include "object/flat_mesh.h"
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

template <typename T>
T* alloc_zeroed(int count) {
    if (count <= 0) return nullptr;
    T* p = static_cast<T*>(std::calloc(static_cast<size_t>(count), sizeof(T)));
    return p;
}

}  // namespace

int object_flat_mesh_array_alloc(int count, FlatMeshArray* out) {
    if (!out) return -1;
    if (count < 0) return -1;
    out->count = count;
    if (count == 0) {
        out->meshes = nullptr;
        return 0;
    }
    out->meshes = alloc_zeroed<FlatMesh>(count);
    if (!out->meshes) {
        out->count = 0;
        return -1;
    }
    return 0;
}

void object_flat_mesh_array_free(FlatMeshArray* arr) {
    if (!arr) return;
    if (arr->meshes) {
        for (int i = 0; i < arr->count; ++i) {
            object_flat_mesh_free(&arr->meshes[i]);
        }
        std::free(arr->meshes);
    }
    arr->meshes = nullptr;
    arr->count = 0;
}

int object_flat_mesh_resize(FlatMesh* fm, int vertex_count, int face_count,
                            int material_id_set_count, int total_influences) {
    if (!fm) return -1;
    if (vertex_count < 0 || face_count < 0 || material_id_set_count < 0 || total_influences < 0) {
        return -1;
    }
    // Free any prior allocations first (preserves name/part_index/origin).
    object_flat_mesh_free(fm);

    fm->vertex_count = vertex_count;
    fm->face_count = face_count;
    fm->material_id_set_count = material_id_set_count;

    fm->vertices = alloc_zeroed<FlatMeshVertex>(vertex_count);
    fm->faces = alloc_zeroed<FlatMeshFace>(face_count);
    fm->corners = alloc_zeroed<FlatMeshFaceCornerData>(face_count * 3);
    fm->material_id_set = alloc_zeroed<int32_t>(material_id_set_count);

    if (total_influences > 0) {
        fm->vertex_influence_offsets = alloc_zeroed<int32_t>(vertex_count + 1);
        fm->vertex_influences = alloc_zeroed<FlatMeshBoneInfluence>(total_influences);
        if (!fm->vertex_influence_offsets || !fm->vertex_influences) {
            object_flat_mesh_free(fm);
            return -1;
        }
    }

    // Sanity: if any requested allocation failed, free everything.
    bool ok = true;
    if (vertex_count > 0 && !fm->vertices) ok = false;
    if (face_count > 0 && (!fm->faces || !fm->corners)) ok = false;
    if (material_id_set_count > 0 && !fm->material_id_set) ok = false;
    if (!ok) {
        object_flat_mesh_free(fm);
        return -1;
    }
    return 0;
}

void object_flat_mesh_free(FlatMesh* fm) {
    if (!fm) return;
    std::free(fm->vertices);
    std::free(fm->faces);
    std::free(fm->corners);
    std::free(fm->material_id_set);
    std::free(fm->vertex_influence_offsets);
    std::free(fm->vertex_influences);
    fm->vertices = nullptr;
    fm->faces = nullptr;
    fm->corners = nullptr;
    fm->material_id_set = nullptr;
    fm->vertex_influence_offsets = nullptr;
    fm->vertex_influences = nullptr;
    fm->vertex_count = 0;
    fm->face_count = 0;
    fm->material_id_set_count = 0;
    // name, part_index, origin preserved.
}
