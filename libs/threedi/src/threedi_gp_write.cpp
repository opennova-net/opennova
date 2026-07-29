// GP format (GPM/GPS/GPP) writer - pure C implementation

// Split out of threedi_gp.cpp (quality campaign W3-3). Motion only — every body is
// unchanged. (libs/threedi is built from the format records in docs/threedi/, not
// from decompiled functions, so there is no original-code citation to carry.)
//
// The write half: the growable byte writer and one write_* per section. A parity
// writer — it builds output from scratch, never by passing input bytes through
// (ADR 0003).

#include "threedi/threedi_gp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Writer
// ============================================================================

typedef struct GpWriter {
    uint8_t *data;
    size_t len;
    size_t cap;
} GpWriter;

static void gp_writer_init(GpWriter *w) {
    w->data = NULL;
    w->len = 0;
    w->cap = 0;
}

static void gp_writer_free(GpWriter *w) {
    free(w->data);
    w->data = NULL;
    w->len = 0;
    w->cap = 0;
}

static int gp_writer_grow(GpWriter *w, size_t need) {
    if (w->len + need <= w->cap) return 0;
    size_t new_cap = w->cap ? w->cap * 2 : 4096;
    while (new_cap < w->len + need) new_cap *= 2;
    uint8_t *new_data = (uint8_t *)realloc(w->data, new_cap);
    if (!new_data) return -1;
    w->data = new_data;
    w->cap = new_cap;
    return 0;
}

static int gp_write_bytes(GpWriter *w, const void *src, size_t n) {
    if (gp_writer_grow(w, n) != 0) return -1;
    memcpy(w->data + w->len, src, n);
    w->len += n;
    return 0;
}

static int gp_write_u8(GpWriter *w, uint8_t v) {
    return gp_write_bytes(w, &v, 1);
}

static int gp_write_u16(GpWriter *w, uint16_t v) {
    uint8_t buf[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return gp_write_bytes(w, buf, 2);
}

static int gp_write_i16(GpWriter *w, int16_t v) {
    return gp_write_u16(w, (uint16_t)v);
}

static int gp_write_u32(GpWriter *w, uint32_t v) {
    uint8_t buf[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return gp_write_bytes(w, buf, 4);
}

static int gp_write_i32(GpWriter *w, int32_t v) {
    return gp_write_u32(w, (uint32_t)v);
}

static int gp_write_f32(GpWriter *w, float v) {
    uint32_t raw;
    memcpy(&raw, &v, sizeof(float));
    return gp_write_u32(w, raw);
}

static int gp_write_padded(GpWriter *w, const char *s, size_t len) {
    if (gp_writer_grow(w, len) != 0) return -1;
    size_t slen = strlen(s);
    if (slen > len) slen = len;
    memcpy(w->data + w->len, s, slen);
    memset(w->data + w->len + slen, 0, len - slen);
    w->len += len;
    return 0;
}

// Write userpoints (48 bytes each)
static int write_userpoints(GpWriter *w, const ThreediGpFile *gp) {
    for (size_t i = 0; i < gp->userpoint_count; ++i) {
        const ThreediGpUserPoint *up = &gp->userpoints[i];
        if (gp_write_i32(w, up->x) != 0) return -1;
        if (gp_write_i32(w, up->y) != 0) return -1;
        if (gp_write_i32(w, up->z) != 0) return -1;
        if (gp_write_i32(w, up->rot_x) != 0) return -1;
        if (gp_write_i32(w, up->rot_y) != 0) return -1;
        if (gp_write_i32(w, up->rot_z) != 0) return -1;
        if (gp_write_i32(w, up->parent_subobject) != 0) return -1;
        if (gp_write_i32(w, up->type_code) != 0) return -1;
        if (gp_write_padded(w, up->name, 16) != 0) return -1;
    }
    return 0;
}

// Write material lookup table (count u32 + 60 bytes each)
static int write_material_lookup(GpWriter *w, const ThreediGpFile *gp) {
    if (gp_write_u32(w, (uint32_t)gp->material_lookup_count) != 0) return -1;
    for (size_t i = 0; i < gp->material_lookup_count; ++i) {
        const ThreediGpMaterialLookup *ml = &gp->material_lookups[i];
        if (gp_write_padded(w, ml->texture_name, 16) != 0) return -1;
        if (gp_write_u32(w, ml->unk_10) != 0) return -1;
        if (gp_write_u32(w, ml->unk_14) != 0) return -1;
        if (gp_write_u32(w, ml->unk_18) != 0) return -1;
        if (gp_write_u32(w, ml->unk_1C) != 0) return -1;
        if (gp_write_u32(w, ml->unk_20) != 0) return -1;
        if (gp_write_u8(w, ml->seq_index) != 0) return -1;
        if (gp_write_u8(w, ml->pad_25) != 0) return -1;
        if (gp_write_u8(w, ml->flags_26) != 0) return -1;
        if (gp_write_u8(w, ml->slot_type) != 0) return -1;
        if (gp_write_u16(w, ml->tex_width) != 0) return -1;
        if (gp_write_u16(w, ml->tex_height) != 0) return -1;
        if (gp_write_u32(w, ml->runtime_2C) != 0) return -1;
        if (gp_write_u32(w, ml->runtime_30) != 0) return -1;
        if (gp_write_u32(w, ml->unk_34) != 0) return -1;
        if (gp_write_u32(w, ml->unk_38) != 0) return -1;
    }
    return 0;
}

// Write collision data
static int write_collision(GpWriter *w, const ThreediGpFile *gp) {
    const ThreediGpCollision *col = gp->collision;
    if (!col) {
        // Write minimal collision header with zero data
        for (int i = 0; i < THREEDI_GP_COLLISION_HEADER_SIZE / 4; ++i) {
            if (gp_write_u32(w, 0) != 0) return -1;
        }
        return 0;
    }

    // Write raw header (preserves all fields including unknowns)
    if (gp_write_bytes(w, col->raw_header, THREEDI_GP_COLLISION_HEADER_SIZE) != 0) return -1;

    // Write collision data blob
    // Order: vertices, normals, faces, objects, translations, planes, volumes

    // Vertices (8 bytes each)
    for (int32_t i = 0; i < col->vertex_count; ++i) {
        if (gp_write_i16(w, col->vertices[i].x) != 0) return -1;
        if (gp_write_i16(w, col->vertices[i].y) != 0) return -1;
        if (gp_write_i16(w, col->vertices[i].z) != 0) return -1;
        if (gp_write_i16(w, col->vertices[i].material_index) != 0) return -1;
    }

    // Normals (8 bytes each)
    for (int32_t i = 0; i < col->normal_count; ++i) {
        if (gp_write_i16(w, col->normals[i].nx) != 0) return -1;
        if (gp_write_i16(w, col->normals[i].ny) != 0) return -1;
        if (gp_write_i16(w, col->normals[i].nz) != 0) return -1;
        if (gp_write_i16(w, col->normals[i].dominant_axis) != 0) return -1;
    }

    // Faces (44 bytes each)
    for (int32_t i = 0; i < col->face_count; ++i) {
        const ThreediGpCollisionFace *f = &col->faces[i];
        if (gp_write_u16(w, f->vertex_indices[0]) != 0) return -1;
        if (gp_write_u16(w, f->vertex_indices[1]) != 0) return -1;
        if (gp_write_u16(w, f->vertex_indices[2]) != 0) return -1;
        if (gp_write_i16(w, f->normal_index) != 0) return -1;
        if (gp_write_i32(w, f->plane_d) != 0) return -1;
        if (gp_write_i32(w, f->bbox_min_x) != 0) return -1;
        if (gp_write_i32(w, f->bbox_max_x) != 0) return -1;
        if (gp_write_i32(w, f->bbox_min_y) != 0) return -1;
        if (gp_write_i32(w, f->bbox_max_y) != 0) return -1;
        if (gp_write_i32(w, f->bbox_min_z) != 0) return -1;
        if (gp_write_i32(w, f->bbox_max_z) != 0) return -1;
        if (gp_write_i32(w, f->surface_flags) != 0) return -1;
        if (gp_write_u8(w, f->surface_type) != 0) return -1;
        if (gp_write_u8(w, f->pad) != 0) return -1;
        if (gp_write_i16(w, f->reserved) != 0) return -1;
    }

    // Objects (128 bytes each)
    for (int32_t i = 0; i < col->object_count; ++i) {
        const ThreediGpCollisionObject *obj = &col->objects[i];
        if (gp_write_i32(w, obj->flags) != 0) return -1;
        if (gp_write_i32(w, obj->vertex_count) != 0) return -1;
        if (gp_write_u32(w, obj->vertex_ptr) != 0) return -1;
        if (gp_write_i32(w, obj->face_count) != 0) return -1;
        if (gp_write_u32(w, obj->face_ptr) != 0) return -1;
        if (gp_write_i32(w, obj->normal_count) != 0) return -1;
        if (gp_write_u32(w, obj->normal_ptr) != 0) return -1;
        if (gp_write_i32(w, obj->volume_count) != 0) return -1;
        if (gp_write_u32(w, obj->volume_ptr) != 0) return -1;
        if (gp_write_i32(w, obj->parent_subobject) != 0) return -1;
        for (int j = 0; j < 3; ++j) {
            if (gp_write_i32(w, obj->reserved[j]) != 0) return -1;
        }
        for (int j = 0; j < 3; ++j) {
            if (gp_write_i32(w, obj->translation[j]) != 0) return -1;
        }
        if (gp_write_i32(w, obj->bbox_min_x) != 0) return -1;
        if (gp_write_i32(w, obj->bbox_max_x) != 0) return -1;
        if (gp_write_i32(w, obj->bbox_min_y) != 0) return -1;
        if (gp_write_i32(w, obj->bbox_max_y) != 0) return -1;
        if (gp_write_i32(w, obj->bbox_min_z) != 0) return -1;
        if (gp_write_i32(w, obj->bbox_max_z) != 0) return -1;
        for (int j = 0; j < 3; ++j) {
            if (gp_write_i32(w, obj->center[j]) != 0) return -1;
        }
        if (gp_write_i32(w, obj->bounding_sphere_radius) != 0) return -1;
        if (gp_write_i32(w, obj->bounding_cylinder_radius) != 0) return -1;
        if (gp_write_i32(w, obj->bbox_height) != 0) return -1;
        for (int j = 0; j < 4; ++j) {
            if (gp_write_i32(w, obj->reserved2[j]) != 0) return -1;
        }
    }

    // Translations (12 bytes each)
    for (int32_t i = 0; i < col->translation_count; ++i) {
        if (gp_write_i32(w, col->translations[i].x) != 0) return -1;
        if (gp_write_i32(w, col->translations[i].y) != 0) return -1;
        if (gp_write_i32(w, col->translations[i].z) != 0) return -1;
    }

    // Planes (16 bytes each)
    for (int32_t i = 0; i < col->plane_count; ++i) {
        if (gp_write_i32(w, col->planes[i].a) != 0) return -1;
        if (gp_write_i32(w, col->planes[i].b) != 0) return -1;
        if (gp_write_i32(w, col->planes[i].c) != 0) return -1;
        if (gp_write_i32(w, col->planes[i].d) != 0) return -1;
    }

    // Volumes (96 bytes each)
    for (int32_t i = 0; i < col->volume_count; ++i) {
        const ThreediGpCollisionVolume *vol = &col->volumes[i];
        if (gp_write_i32(w, vol->type) != 0) return -1;
        if (gp_write_i32(w, vol->flags) != 0) return -1;
        if (gp_write_i32(w, vol->min_x) != 0) return -1;
        if (gp_write_i32(w, vol->max_x) != 0) return -1;
        if (gp_write_i32(w, vol->min_y) != 0) return -1;
        if (gp_write_i32(w, vol->max_y) != 0) return -1;
        if (gp_write_i32(w, vol->min_z) != 0) return -1;
        if (gp_write_i32(w, vol->max_z) != 0) return -1;
        for (int j = 0; j < 3; ++j) {
            if (gp_write_i32(w, vol->extent[j]) != 0) return -1;
        }
        if (gp_write_i32(w, vol->reserved1) != 0) return -1;
        if (gp_write_i32(w, vol->bbox_min_x) != 0) return -1;
        if (gp_write_i32(w, vol->bbox_max_x) != 0) return -1;
        if (gp_write_i32(w, vol->bbox_min_y) != 0) return -1;
        if (gp_write_i32(w, vol->bbox_max_y) != 0) return -1;
        if (gp_write_i32(w, vol->bbox_min_z) != 0) return -1;
        if (gp_write_i32(w, vol->bbox_max_z) != 0) return -1;
        if (gp_write_i32(w, vol->plane_count) != 0) return -1;
        if (gp_write_u32(w, vol->plane_ptr) != 0) return -1;
        for (int j = 0; j < 4; ++j) {
            if (gp_write_i32(w, vol->reserved2[j]) != 0) return -1;
        }
    }

    return 0;
}

// Write render vertices
static int write_rverts(GpWriter *w, const ThreediGpFile *gp) {
    ThreediGpMeshType mesh_type = gp->header.mesh_type;
    for (size_t i = 0; i < gp->rvert_count; ++i) {
        const ThreediGpRVert *rv = &gp->rverts[i];

        // Position (common to all types)
        if (gp_write_f32(w, rv->position[0]) != 0) return -1;
        if (gp_write_f32(w, rv->position[1]) != 0) return -1;
        if (gp_write_f32(w, rv->position[2]) != 0) return -1;

        if (mesh_type == THREEDI_GP_MESH_BASIC) {
            // GPM: normal(3f) + packed_color
            if (gp_write_f32(w, rv->normal[0]) != 0) return -1;
            if (gp_write_f32(w, rv->normal[1]) != 0) return -1;
            if (gp_write_f32(w, rv->normal[2]) != 0) return -1;
            if (gp_write_u32(w, rv->packed_color) != 0) return -1;
        } else if (mesh_type == THREEDI_GP_MESH_STATIC) {
            // GPS: normal(3f) + extra(1f) + packed_color
            if (gp_write_f32(w, rv->normal[0]) != 0) return -1;
            if (gp_write_f32(w, rv->normal[1]) != 0) return -1;
            if (gp_write_f32(w, rv->normal[2]) != 0) return -1;
            if (gp_write_f32(w, 0.0f) != 0) return -1; // Extra float (tangent sign / padding)
            if (gp_write_u32(w, rv->packed_color) != 0) return -1;
        } else {
            // GPP: weights(3f) + indices(4u8) + normal(3f) + packed_color
            if (gp_write_f32(w, rv->bone_weights[0]) != 0) return -1;
            if (gp_write_f32(w, rv->bone_weights[1]) != 0) return -1;
            if (gp_write_f32(w, rv->bone_weights[2]) != 0) return -1;
            if (gp_write_u8(w, rv->bone_indices[0]) != 0) return -1;
            if (gp_write_u8(w, rv->bone_indices[1]) != 0) return -1;
            if (gp_write_u8(w, rv->bone_indices[2]) != 0) return -1;
            if (gp_write_u8(w, rv->bone_indices[3]) != 0) return -1;
            if (gp_write_f32(w, rv->normal[0]) != 0) return -1;
            if (gp_write_f32(w, rv->normal[1]) != 0) return -1;
            if (gp_write_f32(w, rv->normal[2]) != 0) return -1;
            if (gp_write_u32(w, rv->packed_color) != 0) return -1;
        }

        // UV coordinates (common to all types)
        if (gp_write_f32(w, rv->uv0[0]) != 0) return -1;
        if (gp_write_f32(w, rv->uv0[1]) != 0) return -1;
        if (gp_write_f32(w, rv->uv1[0]) != 0) return -1;
        if (gp_write_f32(w, rv->uv1[1]) != 0) return -1;
    }
    return 0;
}

// Write material transform (8 bytes)
static int write_material_transform(GpWriter *w, const ThreediGpMaterialTransform *t) {
    if (gp_write_u8(w, t->style) != 0) return -1;
    if (gp_write_u8(w, t->param) != 0) return -1;
    if (gp_write_i16(w, t->rate) != 0) return -1;
    if (gp_write_i16(w, t->start) != 0) return -1;
    if (gp_write_i16(w, t->end) != 0) return -1;
    return 0;
}

// Write anim transform (8 bytes)
static int write_anim_transform(GpWriter *w, const ThreediGpAnimTransform *t) {
    if (gp_write_u8(w, t->control) != 0) return -1;
    if (gp_write_u8(w, t->param) != 0) return -1;
    if (gp_write_i16(w, t->rate) != 0) return -1;
    if (gp_write_i16(w, t->start) != 0) return -1;
    if (gp_write_i16(w, t->end) != 0) return -1;
    return 0;
}

// Write RModel data blob (returns size written)
static int write_rmodel_blob(GpWriter *w, const ThreediGpRModel *rm, int local_batch, size_t *out_size) {
    size_t start = w->len;

    // Strip triplets (12 bytes each)
    for (size_t i = 0; i < rm->strip_triplet_count; ++i) {
        if (gp_write_u32(w, rm->strip_triplets[i].a) != 0) return -1;
        if (gp_write_u32(w, rm->strip_triplets[i].b) != 0) return -1;
        if (gp_write_u32(w, rm->strip_triplets[i].c) != 0) return -1;
    }

    // Subobjects (72 bytes each)
    for (size_t i = 0; i < rm->subobject_count; ++i) {
        const ThreediGpSubObject *sub = &rm->subobjects[i];
        if (gp_write_u32(w, sub->unk_00) != 0) return -1;
        if (gp_write_i32(w, sub->batch_count) != 0) return -1;
        if (gp_write_u32(w, sub->unk_08) != 0) return -1;
        if (gp_write_i32(w, sub->parent) != 0) return -1;
        for (int j = 0; j < 3; ++j) {
            if (gp_write_f32(w, sub->rel[j]) != 0) return -1;
        }
        for (int j = 0; j < 3; ++j) {
            if (gp_write_f32(w, sub->abs[j]) != 0) return -1;
        }
        for (int j = 0; j < 3; ++j) {
            if (gp_write_f32(w, sub->bounding_min[j]) != 0) return -1;
        }
        if (gp_write_u32(w, sub->bounding_radius) != 0) return -1;
        for (int j = 0; j < 3; ++j) {
            if (gp_write_f32(w, sub->bounding_max[j]) != 0) return -1;
        }
        if (gp_write_u8(w, sub->visible) != 0) return -1;
        if (gp_write_u8(w, sub->special_flag) != 0) return -1;
        if (gp_write_u8(w, sub->pad[0]) != 0) return -1;
        if (gp_write_u8(w, sub->pad[1]) != 0) return -1;
    }

    // Batches and polys
    if (local_batch) {
        // Local batch mode: batches are per-subobject, polys follow each batch set
        size_t poly_idx = 0;
        for (size_t sub_idx = 0; sub_idx < rm->subobject_count; ++sub_idx) {
            int bc = rm->subobjects[sub_idx].batch_count;
            // Write batch headers for this subobject
            for (int b = 0; b < bc; ++b) {
                // Count polys for this batch
                uint32_t opaque_count = 0, transparent_count = 0;
                size_t start_poly = poly_idx;
                while (poly_idx < rm->poly_count && rm->polys[poly_idx].subobject_index == (int32_t)sub_idx) {
                    // For now, treat all as opaque (we don't track opaque vs transparent in parse)
                    opaque_count++;
                    poly_idx++;
                }
                // Write batch (32 bytes)
                if (gp_write_u32(w, 0) != 0) return -1; // opaque_ptr (runtime)
                if (gp_write_u32(w, opaque_count) != 0) return -1;
                if (gp_write_u32(w, 0) != 0) return -1; // transparent_ptr (runtime)
                if (gp_write_u32(w, transparent_count) != 0) return -1;
                if (gp_write_u32(w, 0) != 0) return -1; // reserved
                if (gp_write_u32(w, 0) != 0) return -1;
                if (gp_write_u32(w, 0) != 0) return -1;
                if (gp_write_u32(w, 0) != 0) return -1;

                // Write polys for this batch
                for (size_t pi = start_poly; pi < poly_idx; ++pi) {
                    const ThreediGpVariablePoly *p = &rm->polys[pi];
                    if (gp_write_i32(w, p->material_index) != 0) return -1;
                    if (gp_write_u32(w, 0x72646441) != 0) return -1; // "Addr"
                    if (gp_write_u16(w, (uint16_t)p->index_count) != 0) return -1;
                    if (gp_write_u16(w, 0) != 0) return -1; // triangle_count
                    if (gp_write_i32(w, p->topology) != 0) return -1;
                    if (gp_write_i32(w, p->first_vertex) != 0) return -1;
                    if (gp_write_i32(w, p->max_vertex_index) != 0) return -1;
                    // Bone table (16 bytes)
                    if (gp_write_bytes(w, p->bone_table, 16) != 0) return -1;
                    // Indices
                    for (size_t ii = 0; ii < p->index_count; ++ii) {
                        if (gp_write_u16(w, p->indices[ii]) != 0) return -1;
                    }
                }
            }
        }
    } else {
        // Global batch mode
        // First write shared batch header (28 bytes)
        if (gp_write_u32(w, 0) != 0) return -1; // shared_ptr
        uint32_t total_batch = 1; // Simplified: one batch
        if (gp_write_u32(w, total_batch) != 0) return -1;
        for (int i = 0; i < 5; ++i) {
            if (gp_write_u32(w, 0) != 0) return -1;
        }

        // Write single batch with all polys
        if (gp_write_u32(w, 0) != 0) return -1; // opaque_ptr
        if (gp_write_u32(w, (uint32_t)rm->poly_count) != 0) return -1;
        if (gp_write_u32(w, 0) != 0) return -1; // transparent_ptr
        if (gp_write_u32(w, 0) != 0) return -1; // transparent_count
        if (gp_write_u32(w, 0) != 0) return -1;
        if (gp_write_u32(w, 0) != 0) return -1;
        if (gp_write_u32(w, 0) != 0) return -1;
        if (gp_write_u32(w, 0) != 0) return -1;

        // Write all polys
        for (size_t pi = 0; pi < rm->poly_count; ++pi) {
            const ThreediGpVariablePoly *p = &rm->polys[pi];
            if (gp_write_i32(w, p->material_index) != 0) return -1;
            if (gp_write_u32(w, 0x72646441) != 0) return -1; // "Addr"
            if (gp_write_u16(w, (uint16_t)p->index_count) != 0) return -1;
            if (gp_write_u16(w, 0) != 0) return -1; // triangle_count
            if (gp_write_i32(w, p->topology) != 0) return -1;
            if (gp_write_i32(w, p->first_vertex) != 0) return -1;
            if (gp_write_i32(w, p->max_vertex_index) != 0) return -1;
            // Bone table (16 bytes) + padding (7) + length (1) = 24 bytes
            if (gp_write_bytes(w, p->bone_table, 16) != 0) return -1;
            for (int i = 0; i < 7; ++i) {
                if (gp_write_u8(w, 0) != 0) return -1;
            }
            if (gp_write_u8(w, (uint8_t)p->bone_table_length) != 0) return -1;
            // Indices
            for (size_t ii = 0; ii < p->index_count; ++ii) {
                if (gp_write_u16(w, p->indices[ii]) != 0) return -1;
            }
        }
    }

    // Materials (152 bytes each)
    for (size_t i = 0; i < rm->material_count; ++i) {
        const ThreediGpMaterial *mat = &rm->materials[i];
        if (gp_write_padded(w, mat->texture_name, 16) != 0) return -1;
        if (gp_write_u32(w, mat->render_attributes) != 0) return -1;
        if (gp_write_u32(w, mat->physical_attributes) != 0) return -1;
        if (gp_write_u32(w, mat->use_alpha_pcx) != 0) return -1;
        if (gp_write_u32(w, mat->color_rgb[0]) != 0) return -1;
        if (gp_write_u32(w, mat->color_rgb[1]) != 0) return -1;
        if (gp_write_u32(w, mat->color_rgb[2]) != 0) return -1;
        if (gp_write_u32(w, mat->render_lookup) != 0) return -1;
        if (gp_write_u32(w, mat->luminosity) != 0) return -1;
        if (gp_write_u32(w, mat->specular_intensity) != 0) return -1;
        if (gp_write_u32(w, mat->specular_sharpness) != 0) return -1;
        if (gp_write_u32(w, mat->shader_flags) != 0) return -1;
        if (gp_write_u32(w, mat->tex_addressing_mode) != 0) return -1;
        if (gp_write_u32(w, mat->reserved_40) != 0) return -1;
        if (gp_write_u32(w, mat->reserved_44) != 0) return -1;
        if (gp_write_f32(w, mat->u_tiling) != 0) return -1;
        if (gp_write_f32(w, mat->v_tiling) != 0) return -1;
        if (write_material_transform(w, &mat->mapfunc_u) != 0) return -1;
        if (write_material_transform(w, &mat->mapfunc_v) != 0) return -1;
        if (write_material_transform(w, &mat->rgbgen) != 0) return -1;
        if (gp_write_u32(w, mat->emissive_color) != 0) return -1;
        if (write_material_transform(w, &mat->alphagen) != 0) return -1;
        // Tail (36 bytes)
        if (gp_write_u32(w, mat->runtime_ptr) != 0) return -1;
        if (gp_write_f32(w, mat->reflect_r) != 0) return -1;
        if (gp_write_f32(w, mat->reflect_g) != 0) return -1;
        if (gp_write_f32(w, mat->reflect_b) != 0) return -1;
        if (gp_write_u32(w, mat->reflect_type) != 0) return -1;
        if (gp_write_f32(w, mat->reflect_alpha) != 0) return -1;
        if (gp_write_u32(w, mat->actionplane_type) != 0) return -1;
        if (gp_write_u32(w, mat->projector_type) != 0) return -1;
        if (gp_write_u32(w, mat->projector_no_receive) != 0) return -1;
    }

    // Part animations (92 bytes each) if flag 2 is set
    if ((rm->flags & 2u) != 0) {
        for (size_t i = 0; i < rm->part_animation_count; ++i) {
            const ThreediGpPartAnimation *pa = &rm->part_animations[i];
            if (gp_write_u32(w, pa->flags) != 0) return -1;
            if (gp_write_u8(w, pa->parent_subobject) != 0) return -1;
            if (gp_write_u8(w, pa->subobject_index) != 0) return -1;
            if (gp_write_u8(w, pa->matrix_index) != 0) return -1;
            if (gp_write_u8(w, pa->matrix_offset) != 0) return -1;
            if (gp_write_i32(w, pa->bind_matrix_index) != 0) return -1;
            // 7 transforms (56 bytes)
            if (write_anim_transform(w, &pa->scale_x) != 0) return -1;
            if (write_anim_transform(w, &pa->scale_y) != 0) return -1;
            if (write_anim_transform(w, &pa->scale_z) != 0) return -1;
            if (write_anim_transform(w, &pa->rot_x) != 0) return -1;
            if (write_anim_transform(w, &pa->rot_y) != 0) return -1;
            if (write_anim_transform(w, &pa->rot_z) != 0) return -1;
            if (write_anim_transform(w, &pa->translate) != 0) return -1;
            // Tail
            if (gp_write_u32(w, pa->rotate_type) != 0) return -1;
            if (gp_write_u32(w, pa->scale_type) != 0) return -1;
            if (gp_write_u32(w, pa->transform_as) != 0) return -1;
            if (gp_write_f32(w, pa->yaw_rate) != 0) return -1;
            if (gp_write_f32(w, pa->pitch_rate) != 0) return -1;
            if (gp_write_f32(w, pa->roll_rate) != 0) return -1;
        }
    }

    // Extra polys (88 bytes each)
    for (uint32_t i = 0; i < rm->extra_poly_count; ++i) {
        const ThreediGpExtraPoly *ep = &rm->extra_polys[i];
        // VariablePoly1 (40 bytes)
        if (gp_write_i32(w, ep->vp1.material_index) != 0) return -1;
        if (gp_write_u32(w, ep->vp1.indices_tag) != 0) return -1;
        if (gp_write_u16(w, ep->vp1.index_count) != 0) return -1;
        if (gp_write_u16(w, ep->vp1.triangle_count) != 0) return -1;
        if (gp_write_i32(w, ep->vp1.topology) != 0) return -1;
        if (gp_write_i32(w, ep->vp1.first_vertex) != 0) return -1;
        if (gp_write_i32(w, ep->vp1.vertex_count) != 0) return -1;
        for (int j = 0; j < 4; ++j) {
            if (gp_write_u32(w, ep->vp1_reserved[j]) != 0) return -1;
        }
        // VariablePoly2 (48 bytes)
        if (gp_write_i32(w, ep->vp2.material_index) != 0) return -1;
        if (gp_write_u32(w, ep->vp2.indices_tag) != 0) return -1;
        if (gp_write_u16(w, ep->vp2.index_count) != 0) return -1;
        if (gp_write_u16(w, ep->vp2.triangle_count) != 0) return -1;
        if (gp_write_i32(w, ep->vp2.topology) != 0) return -1;
        if (gp_write_i32(w, ep->vp2.first_vertex) != 0) return -1;
        if (gp_write_i32(w, ep->vp2.vertex_count) != 0) return -1;
        for (int j = 0; j < 3; ++j) {
            if (gp_write_f32(w, ep->bbox_min[j]) != 0) return -1;
        }
        for (int j = 0; j < 3; ++j) {
            if (gp_write_f32(w, ep->bbox_max[j]) != 0) return -1;
        }
    }

    *out_size = w->len - start;
    return 0;
}

// Write an RModel (header + blob)
static int write_rmodel(GpWriter *w, const ThreediGpRModel *rm) {
    // If we have a raw blob from parsing, use it directly for byte-perfect roundtrip
    if (rm->raw_blob && rm->raw_blob_len > 0) {
        // Write raw header (data_size is already correct in raw_header)
        if (gp_write_bytes(w, rm->raw_header, THREEDI_GP_RMODEL_HEADER_SIZE) != 0) return -1;
        // Write raw blob
        if (gp_write_bytes(w, rm->raw_blob, rm->raw_blob_len) != 0) return -1;
        return 0;
    }

    // Otherwise, reconstruct from parsed data (may not match byte-for-byte)
    GpWriter blob;
    gp_writer_init(&blob);

    int local_batch = (rm->flags & 1u) == 0;
    size_t blob_size = 0;
    if (write_rmodel_blob(&blob, rm, local_batch, &blob_size) != 0) {
        gp_writer_free(&blob);
        return -1;
    }

    // Copy raw header and patch data_size at offset 0
    uint8_t hdr[THREEDI_GP_RMODEL_HEADER_SIZE];
    memcpy(hdr, rm->raw_header, THREEDI_GP_RMODEL_HEADER_SIZE);
    // Patch data_size (little-endian u32 at offset 0)
    hdr[0] = (uint8_t)blob_size;
    hdr[1] = (uint8_t)(blob_size >> 8);
    hdr[2] = (uint8_t)(blob_size >> 16);
    hdr[3] = (uint8_t)(blob_size >> 24);

    // Write header
    if (gp_write_bytes(w, hdr, THREEDI_GP_RMODEL_HEADER_SIZE) != 0) {
        gp_writer_free(&blob);
        return -1;
    }

    // Write blob
    if (gp_write_bytes(w, blob.data, blob.len) != 0) {
        gp_writer_free(&blob);
        return -1;
    }

    gp_writer_free(&blob);
    return 0;
}

// Write control registers (44 bytes each)
static int write_control_registers(GpWriter *w, const ThreediGpFile *gp) {
    for (size_t i = 0; i < gp->control_register_count; ++i) {
        const ThreediGpControlRegister *cr = &gp->control_registers[i];
        if (gp_write_padded(w, cr->name, 16) != 0) return -1;
        if (gp_write_u32(w, cr->name_index) != 0) return -1;
        for (int j = 0; j < 6; ++j) {
            if (gp_write_u32(w, cr->param[j]) != 0) return -1;
        }
    }
    return 0;
}

// Write matrices (64 bytes each)
static int write_matrices(GpWriter *w, const ThreediGpFile *gp) {
    for (size_t i = 0; i < gp->matrix_count; ++i) {
        for (int j = 0; j < 16; ++j) {
            if (gp_write_f32(w, gp->matrices[i].m[j]) != 0) return -1;
        }
    }
    return 0;
}

// Write light info section
static int write_light_info(GpWriter *w, const ThreediGpFile *gp) {
    // Outer header: version(u32) + payload_size(u32)
    // Inner header: 60 bytes (ambient + light_count + padding)
    // Lights: 48 bytes each

    size_t payload_size = 60 + gp->light_count * 48;

    if (gp_write_u32(w, gp->light_version) != 0) return -1;
    if (gp_write_u32(w, (uint32_t)payload_size) != 0) return -1;

    // Ambient light (36 bytes)
    if (gp_write_u32(w, gp->ambient_light.colorgen_style) != 0) return -1;
    if (gp_write_f32(w, gp->ambient_light.colorgen_rate) != 0) return -1;
    if (gp_write_f32(w, gp->ambient_light.colorgen_phase) != 0) return -1;
    for (int i = 0; i < 3; ++i) {
        if (gp_write_f32(w, gp->ambient_light.color_start[i]) != 0) return -1;
    }
    for (int i = 0; i < 3; ++i) {
        if (gp_write_f32(w, gp->ambient_light.color_end[i]) != 0) return -1;
    }

    // Light count + inner header tail (4 + 5*4 = 24 bytes)
    if (gp_write_u32(w, (uint32_t)gp->light_count) != 0) return -1;
    if (gp_write_u32(w, gp->light_entries_ptr) != 0) return -1;
    if (gp_write_u32(w, gp->light_reserved_2C) != 0) return -1;
    if (gp_write_u32(w, gp->light_reserved_30) != 0) return -1;
    if (gp_write_u32(w, gp->light_reserved_34) != 0) return -1;
    if (gp_write_u32(w, gp->light_reserved_38) != 0) return -1;

    // Lights (48 bytes each)
    for (size_t i = 0; i < gp->light_count; ++i) {
        const ThreediGpLight *l = &gp->lights[i];
        if (gp_write_u8(w, l->style) != 0) return -1;
        if (gp_write_u8(w, l->phase) != 0) return -1;
        if (gp_write_u16(w, l->rate) != 0) return -1;
        // color_start RGB + pad
        if (gp_write_u8(w, l->color_start[0]) != 0) return -1;
        if (gp_write_u8(w, l->color_start[1]) != 0) return -1;
        if (gp_write_u8(w, l->color_start[2]) != 0) return -1;
        if (gp_write_u8(w, 0) != 0) return -1;
        // color_end RGB + pad
        if (gp_write_u8(w, l->color_end[0]) != 0) return -1;
        if (gp_write_u8(w, l->color_end[1]) != 0) return -1;
        if (gp_write_u8(w, l->color_end[2]) != 0) return -1;
        if (gp_write_u8(w, 0) != 0) return -1;
        // position
        for (int j = 0; j < 3; ++j) {
            if (gp_write_f32(w, l->position[j]) != 0) return -1;
        }
        // attenuation
        if (gp_write_f32(w, l->attenuation_start) != 0) return -1;
        if (gp_write_f32(w, l->attenuation_end) != 0) return -1;
        // part_index
        if (gp_write_i32(w, l->part_index) != 0) return -1;
        // tail (12 bytes)
        if (gp_write_u32(w, l->unk_36) != 0) return -1;
        if (gp_write_u32(w, l->unk_40) != 0) return -1;
        if (gp_write_u32(w, l->unk_44) != 0) return -1;
    }

    return 0;
}

// Write VStream section
static int write_vstream(GpWriter *w, const ThreediGpFile *gp) {
    const ThreediGpVStream *vs = gp->vstream;
    if (!vs) return -1;

    if (gp_write_u32(w, vs->buffer_ptr) != 0) return -1;
    if (gp_write_u32(w, vs->data_size) != 0) return -1;
    if (gp_write_u32(w, vs->unk_08) != 0) return -1;
    if (gp_write_u32(w, vs->unk_0C) != 0) return -1;
    if (gp_write_bytes(w, vs->data, vs->data_len) != 0) return -1;

    return 0;
}

// Write occlusion section
static int write_occlusion(GpWriter *w, const ThreediGpFile *gp) {
    const ThreediGpOcclusion *occ = gp->occlusion;
    if (!occ) return 0;

    // Write object headers (60 bytes each)
    // Layout: type/parent/connecting/pad, center[3], radius, counts+ptrs, reserved
    for (size_t i = 0; i < occ->object_count; ++i) {
        const ThreediGpOcclusionObject *obj = &occ->objects[i];
        if (gp_write_u8(w, obj->type) != 0) return -1;
        if (gp_write_u8(w, obj->parent_subobject_index) != 0) return -1;
        if (gp_write_u8(w, obj->connecting_subobject) != 0) return -1;
        if (gp_write_u8(w, obj->pad) != 0) return -1;
        for (int j = 0; j < 3; ++j) {
            if (gp_write_f32(w, obj->center[j]) != 0) return -1;
        }
        if (gp_write_f32(w, obj->radius) != 0) return -1;
        if (gp_write_i32(w, obj->num_vertices) != 0) return -1;
        if (gp_write_u32(w, obj->vertex_ptr) != 0) return -1;
        if (gp_write_i32(w, obj->num_planes) != 0) return -1;
        if (gp_write_u32(w, obj->plane_ptr) != 0) return -1;
        if (gp_write_i32(w, obj->num_faces) != 0) return -1;
        if (gp_write_u32(w, obj->face_ptr) != 0) return -1;
        for (int j = 0; j < 4; ++j) {
            if (gp_write_i32(w, obj->reserved[j]) != 0) return -1;
        }
    }

    // Write per-object payload
    size_t vert_cursor = 0, plane_cursor = 0, face_cursor = 0;
    for (size_t i = 0; i < occ->object_count; ++i) {
        const ThreediGpOcclusionObject *obj = &occ->objects[i];

        // Vertices (12 bytes each)
        for (int v = 0; v < obj->num_vertices; ++v) {
            for (int j = 0; j < 3; ++j) {
                if (gp_write_f32(w, occ->vertices[vert_cursor + v].position[j]) != 0) return -1;
            }
        }
        vert_cursor += (size_t)obj->num_vertices;

        // Planes (16 bytes each)
        for (int p = 0; p < obj->num_planes; ++p) {
            for (int j = 0; j < 3; ++j) {
                if (gp_write_f32(w, occ->planes[plane_cursor + p].normal[j]) != 0) return -1;
            }
            if (gp_write_f32(w, occ->planes[plane_cursor + p].radius) != 0) return -1;
        }
        plane_cursor += (size_t)obj->num_planes;

        // Faces (12 bytes each)
        for (int f = 0; f < obj->num_faces; ++f) {
            if (gp_write_u32(w, occ->faces[face_cursor + f].raw_indices) != 0) return -1;
            if (gp_write_u32(w, occ->faces[face_cursor + f].edge_data) != 0) return -1;
            if (gp_write_u32(w, occ->faces[face_cursor + f].other_edge_data) != 0) return -1;
        }
        face_cursor += (size_t)obj->num_faces;
    }

    return 0;
}

int threedi_gp_write_buffer(const ThreediGpFile *gp, uint8_t **out_data, size_t *out_len) {
    if (!gp || !out_data || !out_len) return -1;

    GpWriter w;
    gp_writer_init(&w);

    // 1. Header (raw, 0xEC bytes)
    if (gp_write_bytes(&w, gp->header.raw, THREEDI_GP_HEADER_SIZE) != 0) goto error;

    // 2. UserPoints (48 bytes each)
    if (write_userpoints(&w, gp) != 0) goto error;

    // 3. MaterialLookup (count + 60 bytes each)
    if (write_material_lookup(&w, gp) != 0) goto error;

    // 4. Collision
    if (write_collision(&w, gp) != 0) goto error;

    // 5. RVerts
    if (write_rverts(&w, gp) != 0) goto error;

    // 6. RModels
    for (size_t i = 0; i < gp->rmodel_count; ++i) {
        if (write_rmodel(&w, &gp->rmodels[i]) != 0) goto error;
    }

    // 7. Control registers
    if (write_control_registers(&w, gp) != 0) goto error;

    // 8. Matrices
    if (write_matrices(&w, gp) != 0) goto error;

    // 9. Light info (if flag set)
    if ((gp->header.flags & 1) != 0) {
        if (write_light_info(&w, gp) != 0) goto error;
    }

    // 10. VStream (if flag set)
    if ((gp->header.flags & 2) != 0) {
        if (write_vstream(&w, gp) != 0) goto error;
    }

    // 11. Occlusion (if flag set)
    if ((gp->header.flags & 4) != 0) {
        if (write_occlusion(&w, gp) != 0) goto error;
    }

    *out_data = w.data;
    *out_len = w.len;
    return 0;

error:
    gp_writer_free(&w);
    return -1;
}

int threedi_gp_write(const char *path, const ThreediGpFile *gp) {
    if (!path || !gp) return -1;

    uint8_t *data = NULL;
    size_t len = 0;

    if (threedi_gp_write_buffer(gp, &data, &len) != 0) {
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(data);
        return -1;
    }

    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    free(data);

    return (written == len) ? 0 : -1;
}
