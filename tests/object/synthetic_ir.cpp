#include "synthetic_ir.h"
#include <cstdlib>
#include <cstring>

namespace {
template <typename T>
T* alloc_zeroed(int count) {
    if (count <= 0) return nullptr;
    return static_cast<T*>(std::calloc(static_cast<size_t>(count), sizeof(T)));
}
}

int synthetic_ir_build_quad(Threedi3di3* out) {
    if (!out) return -1;
    std::memset(out, 0, sizeof(*out));
    out->version = 3;

    out->lod_count = 1;
    out->lods = alloc_zeroed<ThreediLod>(1);
    if (!out->lods) return -1;

    ThreediLod* lod = &out->lods[0];
    std::strcpy(lod->model_type, "MESH");
    lod->lod_threshold = 0;

    // 4 vertices (quad corners), static-simple layout (stride=40, flags=0x01).
    lod->vertices.count = 4;
    lod->vertices.stride = 40;
    lod->vertices.flags = 0x01;
    lod->vertices.items = alloc_zeroed<ThreediVertex>(4);
    if (!lod->vertices.items) return -1;

    const float positions[4][3] = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
    };
    for (int i = 0; i < 4; ++i) {
        ThreediVertex* v = &lod->vertices.items[i];
        v->position[0] = positions[i][0];
        v->position[1] = positions[i][1];
        v->position[2] = positions[i][2];
        v->normal[0] = 0.0f;
        v->normal[1] = 0.0f;
        v->normal[2] = 1.0f;
        v->uv0[0] = positions[i][0];
        v->uv0[1] = positions[i][1];
        v->flags = 0x01;
        v->has_tangents = 0;
        v->is_skinned = 0;
    }

    // 6 indices: 2 triangles (0,1,2) and (0,2,3).
    lod->indices.count = 6;
    lod->indices.indices = alloc_zeroed<uint16_t>(6);
    if (!lod->indices.indices) return -1;
    static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    std::memcpy(lod->indices.indices, idx, sizeof(idx));

    // 1 triangle strip covering both triangles as a list (is_strip=0).
    lod->strip_count = 1;
    lod->strip_record_size = 48;
    lod->strips = alloc_zeroed<ThreediTriangleStrip>(1);
    if (!lod->strips) return -1;
    ThreediTriangleStrip* s = &lod->strips[0];
    s->material_index = 0;
    s->index_offset = 0;
    s->num_indices = 6;
    s->num_triangles = 2;
    s->is_strip = 0;
    s->start_vertex = 0;
    s->num_vertices = 4;
    s->min[0] = 0.0f; s->min[1] = 0.0f; s->min[2] = 0.0f;
    s->max[0] = 1.0f; s->max[1] = 1.0f; s->max[2] = 0.0f;
    s->bone_table_length = 0;

    // 1 render object owning that strip.
    lod->render_object_count = 1;
    lod->render_objects = alloc_zeroed<ThreediRenderObject>(1);
    if (!lod->render_objects) return -1;
    ThreediRenderObject* ro = &lod->render_objects[0];
    ro->num_strips = 1;
    ro->num_alpha_strips = 0;
    ro->parent_index = -1;
    ro->rel[0] = 0.0f; ro->rel[1] = 0.0f; ro->rel[2] = 0.0f;
    ro->abs[0] = 0.0f; ro->abs[1] = 0.0f; ro->abs[2] = 0.0f;
    ro->bounding_center[0] = 0.5f; ro->bounding_center[1] = 0.5f; ro->bounding_center[2] = 0.0f;
    ro->bounding_radius = 0.7071f;

    return 0;
}

int synthetic_ir_build_quad_skinned(Threedi3di3* out) {
    int rc = synthetic_ir_build_quad(out);
    if (rc != 0) return rc;
    // Mutate: set is_skinned + populate bone_weights/bone_indices.
    ThreediLod* lod = &out->lods[0];
    lod->vertices.stride = 56;
    lod->vertices.flags = 0x41;

    // Per-vertex weights chosen to exercise the 3-weight cap, zero-weight
    // skip, and part_idx fallback (Python mesh_build.py:163-181).
    //
    // bone_table on the strip maps local indices (0,1,2) -> skel indices (11,22,33).
    //
    // Vert 0: weights (0.5, 0.3, 0.2)  -> 3 entries: (11,0.5),(22,0.3),(33,0.2)
    // Vert 1: weights (0.6, 0.4, 0.0)  -> 2 entries: (11,0.6),(22,0.4)
    // Vert 2: weights (0.0, 0.0, 0.0)  -> 0 positives -> fallback (part_idx=0, 1.0)
    // Vert 3: weights (1.0, 0.0, 0.0)  -> 1 entry: (11, 1.0)
    static const float wts[4][3] = {
        {0.5f, 0.3f, 0.2f},
        {0.6f, 0.4f, 0.0f},
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
    };
    for (int i = 0; i < 4; ++i) {
        ThreediVertex* v = &lod->vertices.items[i];
        v->is_skinned = 1;
        v->flags = 0x41;
        v->bone_weights[0] = wts[i][0];
        v->bone_weights[1] = wts[i][1];
        v->bone_weights[2] = wts[i][2];
        v->bone_indices[0] = 0;
        v->bone_indices[1] = 1;
        v->bone_indices[2] = 2;
        v->bone_indices[3] = 0;
    }

    // Set bone_table on the strip: local 0->11, 1->22, 2->33.
    ThreediTriangleStrip* s = &lod->strips[0];
    s->bone_table_length = 3;
    s->bone_table[0] = 11;
    s->bone_table[1] = 22;
    s->bone_table[2] = 33;

    return 0;
}

Threedi3di3 make_dedup_test_ir(void) {
    Threedi3di3 model;
    std::memset(&model, 0, sizeof(model));
    model.version = 3;

    model.lod_count = 1;
    model.lods = alloc_zeroed<ThreediLod>(1);
    ThreediLod* lod = &model.lods[0];

    // 8 vertices with arbitrary distinct positions; source index == array index.
    lod->vertices.count = 8;
    lod->vertices.stride = 40;
    lod->vertices.flags = 0x01;
    lod->vertices.items = alloc_zeroed<ThreediVertex>(8);
    for (int i = 0; i < 8; ++i) {
        lod->vertices.items[i].position[0] = static_cast<float>(i);
        lod->vertices.items[i].position[1] = 0.0f;
        lod->vertices.items[i].position[2] = 0.0f;
        lod->vertices.items[i].normal[0] = 0.0f;
        lod->vertices.items[i].normal[1] = 0.0f;
        lod->vertices.items[i].normal[2] = 1.0f;
        lod->vertices.items[i].flags = 0x01;
        lod->vertices.items[i].is_skinned = 0;
    }

    // 6 indices: 2 triangles (source indices before winding swap):
    //   tri 0: (5, 3, 7)  -> after (0,2,1) swap: (5, 7, 3)
    //   tri 1: (3, 5, 0)  -> after (0,2,1) swap: (3, 0, 5)
    lod->indices.count = 6;
    lod->indices.indices = alloc_zeroed<uint16_t>(6);
    static const uint16_t raw_idx[6] = {5, 3, 7, 3, 5, 0};
    std::memcpy(lod->indices.indices, raw_idx, sizeof(raw_idx));

    // 1 strip (is_strip=0), covers indices [0..5].
    lod->strip_count = 1;
    lod->strip_record_size = 48;
    lod->strips = alloc_zeroed<ThreediTriangleStrip>(1);
    ThreediTriangleStrip* s = &lod->strips[0];
    s->material_index = 0;
    s->index_offset = 0;
    s->num_indices = 6;
    s->num_triangles = 2;
    s->is_strip = 0;
    s->start_vertex = 0;
    s->num_vertices = 8;
    s->bone_table_length = 0;

    // 1 render object owning 1 strip.
    lod->render_object_count = 1;
    lod->render_objects = alloc_zeroed<ThreediRenderObject>(1);
    ThreediRenderObject* ro = &lod->render_objects[0];
    ro->num_strips = 1;
    ro->num_alpha_strips = 0;
    ro->parent_index = -1;
    ro->abs[0] = 0.0f; ro->abs[1] = 0.0f; ro->abs[2] = 0.0f;

    return model;
}

void synthetic_ir_free(Threedi3di3* ir) {
    if (!ir) return;
    if (ir->lods) {
        ThreediLod* lod = &ir->lods[0];
        std::free(lod->vertices.items);
        std::free(lod->indices.indices);
        std::free(lod->strips);
        std::free(lod->render_objects);
        std::free(ir->lods);
    }
    std::memset(ir, 0, sizeof(*ir));
}
