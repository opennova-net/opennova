#include "threedi/threedi_3di3.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <io/fixed.h>

#define LENGTH_MASK 0x00FFFFFFu
#define PARENT_FLAG 0x80000000u
#define THREEDI_DEFAULT_VERSION 259u

typedef struct ChunkBuilder ChunkBuilder;

using opennova::io::read_u32_le;
using opennova::io::read_s32_le;
using opennova::io::read_u16_le;
using opennova::io::read_s16_le;
using opennova::io::read_u8;
using opennova::io::read_f32_le;
using opennova::io::read_fp_16_16;
using opennova::io::read_fp_14;

static const ThreediChunk *find_first_chunk(const ThreediChunk *chunk, const char id[4])
{
    if (!chunk) {
        return NULL;
    }
    if (memcmp(chunk->id, id, 4) == 0) {
        return chunk;
    }
    for (size_t i = 0; i < chunk->child_count; ++i) {
        const ThreediChunk *found = find_first_chunk(&chunk->children[i], id);
        if (found) {
            return found;
        }
    }
    return NULL;
}

static int append_chunk(const ThreediChunk ***list, size_t *count, size_t *cap, const ThreediChunk *chunk)
{
    if (*count == *cap) {
        size_t new_cap = *cap == 0 ? 4 : (*cap * 2);
        const ThreediChunk **tmp = (const ThreediChunk **)realloc(*list, new_cap * sizeof(*tmp));
        if (!tmp) {
            return -1;
        }
        *list = tmp;
        *cap = new_cap;
    }
    (*list)[*count] = chunk;
    (*count)++;
    return 0;
}

static int collect_chunks(const ThreediChunk *chunk, const char id[4], const ThreediChunk ***out_list, size_t *out_count, size_t *out_cap)
{
    if (!chunk) {
        return 0;
    }
    if (memcmp(chunk->id, id, 4) == 0) {
        if (append_chunk(out_list, out_count, out_cap, chunk) != 0) {
            return -1;
        }
    }
    for (size_t i = 0; i < chunk->child_count; ++i) {
        if (collect_chunks(&chunk->children[i], id, out_list, out_count, out_cap) != 0) {
            return -1;
        }
    }
    return 0;
}

static void free_model_arrays(Threedi3di3 *model)
{
    if (!model) {
        return;
    }
    if (model->lods) {
        for (size_t i = 0; i < model->lod_count; ++i) {
            ThreediLod *lod = &model->lods[i];
            free(lod->vertices.items);
            free(lod->indices.indices);
            free(lod->strips);
            free(lod->render_objects);
            free(lod->part_animations);
        }
        free(model->lods);
    }
    if (model->materials) {
        free(model->materials);
    }
    if (model->lights) {
        free(model->lights);
    }
    if (model->user_points) {
        free(model->user_points);
    }
    if (model->collision) {
        free(model->collision->planes);
        free(model->collision->volumes);
        free(model->collision->vertices);
        free(model->collision->normals);
        free(model->collision->faces);
        free(model->collision->objects);
        free(model->collision->translations);
        free(model->collision);
    }
    free(model->ovrt.data);
    free(model->mtrx.matrices);
    free(model->occlusion_vertices);
    free(model->occlusion_faces);
    free(model->occlusion_objects);
    free(model->occlusion_planes);
    free(model->part_animations);
    free(model->ctrl.registers);
    free(model->info.data);
    model->ctrl.registers = NULL;
    model->version = 0;
    model->lods = NULL;
    model->lod_count = 0;
    model->materials = NULL;
    model->material_count = 0;
    model->material_record_size = 0;
    model->lights = NULL;
    model->light_count = 0;
    model->user_points = NULL;
    model->user_point_count = 0;
    model->collision = NULL;
    memset(&model->ctrl, 0, sizeof(model->ctrl));
    memset(&model->ovrt, 0, sizeof(model->ovrt));
    memset(&model->mtrx, 0, sizeof(model->mtrx));
    model->occlusion_vertices = NULL;
    model->occlusion_faces = NULL;
    model->occlusion_objects = NULL;
    model->occlusion_vertex_count = 0;
    model->occlusion_face_count = 0;
    model->occlusion_object_count = 0;
    model->occlusion_vertex_record_size = 0;
    model->occlusion_face_record_size = 0;
    model->occlusion_object_record_size = 0;
    model->occlusion_planes = NULL;
    model->occlusion_plane_count = 0;
    model->occlusion_plane_record_size = 0;
    model->part_animations = NULL;
    model->part_animation_count = 0;
    model->part_animation_record_size = 0;
    memset(&model->header, 0, sizeof(model->header));
    memset(&model->info, 0, sizeof(model->info));
}

static int parse_header_chunk(const ThreediChunk *chunk, ThreediHeader *out)
{
    if (!chunk || !out) {
        return -1;
    }
    if (chunk->data_len < 28) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    size_t copy_len = chunk->data_len < 16 ? chunk->data_len : 16;
    memcpy(out->name, chunk->data, copy_len);
    out->name[16] = '\0';

    out->mesh_type = (ThreediMeshType)read_s32_le(chunk->data + 16);
    out->lod_count_decl = read_s32_le(chunk->data + 20);
    out->lod_distance = read_s32_le(chunk->data + 24);
    out->has_header = 1;
    return 0;
}

static int parse_info_chunk(const ThreediChunk *chunk, ThreediInfo *out)
{
    if (!chunk || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (chunk->data_len > 0) {
        out->data = (uint8_t *)malloc(chunk->data_len);
        if (!out->data) {
            return -1;
        }
        memcpy(out->data, chunk->data, chunk->data_len);
        out->data_len = chunk->data_len;
    }
    return 0;
}

static const ThreediChunk *find_child(const ThreediChunk *parent, const char id[4])
{
    if (!parent) {
        return NULL;
    }
    for (size_t i = 0; i < parent->child_count; ++i) {
        if (memcmp(parent->children[i].id, id, 4) == 0) {
            return &parent->children[i];
        }
    }
    return NULL;
}

static int parse_rmdl_chunk(const ThreediChunk *chunk, char out_model_type[5], int32_t *out_render_obj_count, int32_t *out_lod_threshold)
{
    if (!chunk || chunk->data_len < 12) {
        return -1;
    }
    memcpy(out_model_type, chunk->data, 4);
    out_model_type[4] = '\0';
    *out_lod_threshold = read_s32_le(chunk->data + 4);
    *out_render_obj_count = read_s32_le(chunk->data + 8);
    return 0;
}

static int parse_vert_chunk(const ThreediChunk *chunk, ThreediVertexBuffer *out)
{
    if (!chunk || !out) {
        return -1;
    }
    assert(chunk->data_len >= 12);

    uint32_t count = read_u32_le(chunk->data);
    uint32_t stride = read_u32_le(chunk->data + 4);
    uint32_t flags = read_u32_le(chunk->data + 8);

    size_t expected_size = 12u + (size_t)count * (size_t)stride;
    assert(chunk->data_len == expected_size);

    int has_tangents = (flags & THREEDI_VERTEX_FLAG_TANGENTS) != 0;
    int is_skinned = (flags & THREEDI_VERTEX_FLAG_SKINNED) != 0;
    uint32_t expected_stride = 40;
    if (is_skinned) {
        expected_stride += 16; // weights + indices
    }
    if (has_tangents) {
        expected_stride += 24; // tangent + bitangent
    }
    if (stride != expected_stride) {
        return -1;
    }

    ThreediVertex *verts = (ThreediVertex *)calloc(count, sizeof(ThreediVertex));
    if (!verts && count > 0) {
        return -1;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 12 + (size_t)i * stride;
        size_t cursor = 0;
        ThreediVertex *v = &verts[i];
        v->flags = flags;
        v->has_tangents = has_tangents;
        v->is_skinned = is_skinned;

        v->position[0] = read_f32_le(base + cursor); cursor += 4;
        v->position[1] = read_f32_le(base + cursor); cursor += 4;
        v->position[2] = read_f32_le(base + cursor); cursor += 4;

        if (is_skinned) {
            v->bone_weights[0] = read_f32_le(base + cursor); cursor += 4;
            v->bone_weights[1] = read_f32_le(base + cursor); cursor += 4;
            v->bone_weights[2] = read_f32_le(base + cursor); cursor += 4;
            memcpy(v->bone_indices, base + cursor, 4);
            cursor += 4;
        }

        v->normal[0] = read_f32_le(base + cursor); cursor += 4;
        v->normal[1] = read_f32_le(base + cursor); cursor += 4;
        v->normal[2] = read_f32_le(base + cursor); cursor += 4;

        v->uv0[0] = read_f32_le(base + cursor); cursor += 4;
        v->uv0[1] = read_f32_le(base + cursor); cursor += 4;
        v->uv1[0] = read_f32_le(base + cursor); cursor += 4;
        v->uv1[1] = read_f32_le(base + cursor); cursor += 4;

        if (has_tangents) {
            v->tangent[0] = read_f32_le(base + cursor); cursor += 4;
            v->tangent[1] = read_f32_le(base + cursor); cursor += 4;
            v->tangent[2] = read_f32_le(base + cursor); cursor += 4;

            v->bitangent[0] = read_f32_le(base + cursor); cursor += 4;
            v->bitangent[1] = read_f32_le(base + cursor); cursor += 4;
            v->bitangent[2] = read_f32_le(base + cursor); cursor += 4;
        }
    }

    out->count = count;
    out->stride = stride;
    out->flags = flags;
    out->items = verts;
    return 0;
}

static int parse_indx_chunk(const ThreediChunk *chunk, ThreediIndexBuffer *out)
{
    if (!chunk || !out) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 2);
    size_t expected = 8u + (size_t)count * 2u;
    assert(chunk->data_len == expected);
    uint16_t *indices = (uint16_t *)calloc(count, sizeof(uint16_t));
    if (!indices && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        size_t off = 8 + (size_t)i * 2u;
        indices[i] = read_u16_le(chunk->data + off);
    }
    out->count = count;
    out->indices = indices;
    return 0;
}

static int parse_strp_chunk(const ThreediChunk *chunk, ThreediTriangleStrip **out_strips, size_t *out_count, uint32_t *out_record_size)
{
    if (!chunk || !out_strips || !out_count) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 48 || record_size == 68);
    size_t expected = 8u + (size_t)count * (size_t)record_size;
    assert(chunk->data_len == expected);
    ThreediTriangleStrip *strips = (ThreediTriangleStrip *)calloc(count, sizeof(ThreediTriangleStrip));
    if (!strips && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        ThreediTriangleStrip *s = &strips[i];
        s->material_index = read_s32_le(base + 0);
        s->index_offset = read_s32_le(base + 4);
        s->num_indices = read_u16_le(base + 8);
        s->num_triangles = read_u16_le(base + 10);
        s->is_strip = read_s32_le(base + 12);
        s->start_vertex = read_s32_le(base + 16);
        s->num_vertices = read_s32_le(base + 20);
        s->min[0] = read_f32_le(base + 24);
        s->min[1] = read_f32_le(base + 28);
        s->min[2] = read_f32_le(base + 32);
        s->max[0] = read_f32_le(base + 36);
        s->max[1] = read_f32_le(base + 40);
        s->max[2] = read_f32_le(base + 44);
        if (record_size == 68) {
            memcpy(s->bone_table, base + 48, 16);
            s->bone_table_length = read_s32_le(base + 64);
            if (s->bone_table_length < 0 || s->bone_table_length > 16) {
                free(strips);
                return -1;
            }
        } else {
            s->bone_table_length = 0;
        }
    }
    *out_strips = strips;
    *out_count = count;
    if (out_record_size) {
        *out_record_size = record_size;
    }
    return 0;
}

static int parse_robj_chunk(const ThreediChunk *chunk, ThreediRenderObject **out_objs, size_t *out_count)
{
    if (!chunk || !out_objs || !out_count) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 52);
    size_t expected = 8u + (size_t)count * 52u;
    assert(chunk->data_len == expected);
    ThreediRenderObject *objs = (ThreediRenderObject *)calloc(count, sizeof(ThreediRenderObject));
    if (!objs && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * 52u;
        ThreediRenderObject *o = &objs[i];
        o->num_strips = read_s32_le(base + 0);
        o->num_alpha_strips = read_s32_le(base + 4);
        o->parent_index = read_s32_le(base + 8);
        o->rel[0] = read_f32_le(base + 12);
        o->rel[1] = read_f32_le(base + 16);
        o->rel[2] = read_f32_le(base + 20);
        o->abs[0] = read_f32_le(base + 24);
        o->abs[1] = read_f32_le(base + 28);
        o->abs[2] = read_f32_le(base + 32);
        o->bounding_center[0] = read_f32_le(base + 36);
        o->bounding_center[1] = read_f32_le(base + 40);
        o->bounding_center[2] = read_f32_le(base + 44);
        o->bounding_radius = read_f32_le(base + 48);
    }
    *out_objs = objs;
    *out_count = count;
    return 0;
}

static int parse_panm(const ThreediChunk *chunk, ThreediPartAnimation **out_anims, size_t *out_count, uint32_t *out_record_size);

static int parse_rlod(const ThreediChunk *rlod_chunk, ThreediLod *out_lod)
{
    if (!rlod_chunk || !out_lod) {
        return -1;
    }
    if (!rlod_chunk->is_parent) {
        return -1;
    }
    memset(out_lod, 0, sizeof(*out_lod));

    const ThreediChunk *rmdl = find_child(rlod_chunk, "RMDL");
    if (rmdl) {
        if (parse_rmdl_chunk(rmdl, out_lod->model_type, &out_lod->rmdl_render_object_count, &out_lod->lod_threshold) != 0) {
            return -1;
        }
    }

    const ThreediChunk *vert = find_child(rlod_chunk, "VERT");
    const ThreediChunk *indx = find_child(rlod_chunk, "INDX");
    if (!vert || !indx) {
        return -1;
    }
    if (parse_vert_chunk(vert, &out_lod->vertices) != 0) {
        return -1;
    }
    if (parse_indx_chunk(indx, &out_lod->indices) != 0) {
        free(out_lod->vertices.items);
        memset(&out_lod->vertices, 0, sizeof(out_lod->vertices));
        return -1;
    }

    const ThreediChunk *strp = find_child(rlod_chunk, "STRP");
    if (strp) {
        if (parse_strp_chunk(strp, &out_lod->strips, &out_lod->strip_count, &out_lod->strip_record_size) != 0) {
            free(out_lod->vertices.items);
            free(out_lod->indices.indices);
            memset(&out_lod->vertices, 0, sizeof(out_lod->vertices));
            memset(&out_lod->indices, 0, sizeof(out_lod->indices));
            return -1;
        }
    }

    const ThreediChunk *robj = find_child(rlod_chunk, "ROBJ");
    if (robj) {
        if (parse_robj_chunk(robj, &out_lod->render_objects, &out_lod->render_object_count) != 0) {
            free(out_lod->vertices.items);
            free(out_lod->indices.indices);
            free(out_lod->strips);
            memset(&out_lod->vertices, 0, sizeof(out_lod->vertices));
            memset(&out_lod->indices, 0, sizeof(out_lod->indices));
            out_lod->strip_count = 0;
            out_lod->strips = NULL;
            return -1;
        }
    }

    const ThreediChunk *panm = find_child(rlod_chunk, "PANM");
    if (panm) {
        if (parse_panm(panm, &out_lod->part_animations, &out_lod->part_animation_count, &out_lod->part_animation_record_size) != 0) {
            free(out_lod->vertices.items);
            free(out_lod->indices.indices);
            free(out_lod->strips);
            free(out_lod->render_objects);
            memset(out_lod, 0, sizeof(*out_lod));
            return -1;
        }
    }

    return 0;
}

static int parse_cmdl(const ThreediChunk *chunk, ThreediCollisionModelData *out)
{
    if (!chunk || !out) {
        return -1;
    }
    assert(chunk->data_len == 64);
    // bbox {minX, minY, minZ, maxX, maxY, maxZ}, radii, 7 counts.
    for (int i = 0; i < 6; ++i)
        out->bbox[i] = read_fp_16_16(chunk->data + i * 4);
    out->radii[0] = read_fp_16_16(chunk->data + 24);  // max_radius
    out->radii[1] = read_fp_16_16(chunk->data + 28);  // max_radius_xy
    out->radii[2] = read_fp_16_16(chunk->data + 32);  // max_radius_z
    out->num_vertices         = read_s32_le(chunk->data + 36);
    out->num_normals          = read_s32_le(chunk->data + 40);
    out->num_faces            = read_s32_le(chunk->data + 44);
    out->num_objects          = read_s32_le(chunk->data + 48);
    out->num_transforms       = read_s32_le(chunk->data + 52);
    out->num_bounding_planes  = read_s32_le(chunk->data + 56);
    out->num_bounding_volumes = read_s32_le(chunk->data + 60);
    return 0;
}

static int parse_bpln(const ThreediChunk *chunk, ThreediBoundingPlane **out_planes, size_t *out_count)
{
    if (!chunk || !out_planes || !out_count) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 12);
    size_t expected = 8u + (size_t)count * (size_t)record_size;
    assert(chunk->data_len == expected);
    if (chunk->data_len != expected) {
        return -1;
    }
    ThreediBoundingPlane *planes = (ThreediBoundingPlane *)calloc(count, sizeof(ThreediBoundingPlane));
    if (!planes && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        planes[i].flags = read_s16_le(base + 0);
        planes[i].normal[0] = read_fp_14(base + 2);
        planes[i].normal[1] = read_fp_14(base + 4);
        planes[i].normal[2] = read_fp_14(base + 6);
        planes[i].radius = read_fp_16_16(base + 8);
    }
    *out_planes = planes;
    *out_count = count;
    return 0;
}

static int parse_bvol(const ThreediChunk *chunk, ThreediBoundingVolume **out_vols, size_t *out_count)
{
    if (!chunk || !out_vols || !out_count) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 36);
    size_t expected = 8u + (size_t)count * (size_t)record_size;
    assert(chunk->data_len == expected);
    if (chunk->data_len != expected) {
        return -1;
    }
    ThreediBoundingVolume *vols = (ThreediBoundingVolume *)calloc(count, sizeof(ThreediBoundingVolume));
    if (!vols && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        vols[i].collidable_type = read_s32_le(base + 0);
        vols[i].flags = read_s32_le(base + 4);
        vols[i].min_x_fp16 = read_s32_le(base + 8);
        vols[i].min_y_fp16 = read_s32_le(base + 12);
        vols[i].min_z_fp16 = read_s32_le(base + 16);
        vols[i].max_x_fp16 = read_s32_le(base + 20);
        vols[i].max_y_fp16 = read_s32_le(base + 24);
        vols[i].max_z_fp16 = read_s32_le(base + 28);
        vols[i].plane_count = read_s32_le(base + 32);
    }
    *out_vols = vols;
    *out_count = count;
    return 0;
}

static int parse_cvrt(const ThreediChunk *chunk, ThreediCollisionVertex **out_vertices, size_t *out_count)
{
    if (!chunk || !out_vertices || !out_count) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(count == 0 || record_size == 8);  // record_size can be 0 when count is 0
    size_t expected = 8u + (size_t)count * (size_t)record_size;
    // Don't assert on data_len - some files have padding; runtime check below handles mismatch
    if (chunk->data_len < expected) {
        return -1;
    }
    ThreediCollisionVertex *verts = (ThreediCollisionVertex *)calloc(count, sizeof(ThreediCollisionVertex));
    if (!verts && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        float x = (float)read_s16_le(base + 0) / 256.0f;
        float y = (float)read_s16_le(base + 2) / 256.0f;
        float z = (float)read_s16_le(base + 4) / 256.0f;
        int16_t unknown = read_s16_le(base + 6);
        // assert(unknown == 0);  // Some models have non-zero values here
        (void)unknown;
        verts[i].position[0] = x;
        verts[i].position[1] = y;
        verts[i].position[2] = z;
    }
    *out_vertices = verts;
    *out_count = count;
    return 0;
}

static int parse_cnrm(const ThreediChunk *chunk, ThreediCollisionNormal **out_normals, size_t *out_count)
{
    if (!chunk || !out_normals || !out_count) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(count == 0 || record_size == 8);  // record_size can be 0 when count is 0
    size_t expected = 8u + (size_t)count * (size_t)record_size;
    // Don't assert on data_len - some files have padding; runtime check below handles mismatch
    if (chunk->data_len < expected) {
        return -1;
    }
    ThreediCollisionNormal *normals = (ThreediCollisionNormal *)calloc(count, sizeof(ThreediCollisionNormal));
    if (!normals && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        const int16_t raw_x = read_s16_le(base + 0);
        const int16_t raw_y = read_s16_le(base + 2);
        const int16_t raw_z = read_s16_le(base + 4);
        normals[i].normal[0] = (float)raw_x / 16384.0f;
        normals[i].normal[1] = (float)raw_y / 16384.0f;
        normals[i].normal[2] = (float)raw_z / 16384.0f;
        normals[i].dominate_axis = read_s16_le(base + 6);
        assert(normals[i].dominate_axis == 1 || normals[i].dominate_axis == 2 || normals[i].dominate_axis == 4);
    }
    *out_normals = normals;
    *out_count = count;
    return 0;
}

static int parse_cfac(const ThreediChunk *chunk, ThreediCollisionFace **out_faces, size_t *out_count)
{
    if (!chunk || !out_faces || !out_count) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 44);
    size_t expected = 8u + (size_t)count * 44u;
    assert(chunk->data_len == expected);
    if (chunk->data_len != expected) {
        return -1;
    }
    ThreediCollisionFace *faces = (ThreediCollisionFace *)calloc(count, sizeof(ThreediCollisionFace));
    if (!faces && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        faces[i].vert_index[0] = read_s16_le(base + 0);
        faces[i].vert_index[1] = read_s16_le(base + 2);
        faces[i].vert_index[2] = read_s16_le(base + 4);
        faces[i].normal_index = read_s16_le(base + 6);
        faces[i].plane_dist_fp16 = read_s32_le(base + 8);
        faces[i].min_x_fp16 = read_s32_le(base + 12);
        faces[i].min_y_fp16 = read_s32_le(base + 16);
        faces[i].min_z_fp16 = read_s32_le(base + 20);
        faces[i].max_x_fp16 = read_s32_le(base + 24);
        faces[i].max_y_fp16 = read_s32_le(base + 28);
        faces[i].max_z_fp16 = read_s32_le(base + 32);
        faces[i].material_flags = read_u32_le(base + 36);
        faces[i].poly_type = read_u8(base + 40);
        faces[i].pad[0] = read_u8(base + 41);
        faces[i].pad[1] = read_u8(base + 42);
        faces[i].pad[2] = read_u8(base + 43);
    }
    *out_faces = faces;
    *out_count = count;
    return 0;
}

static int parse_cobj(const ThreediChunk *chunk, ThreediCollisionObject **out_objs, size_t *out_count)
{
    if (!chunk || !out_objs || !out_count) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 88);
    size_t expected = 8u + (size_t)count * 88u;
    assert(chunk->data_len == expected);
    if (chunk->data_len != expected) {
        return -1;
    }
    ThreediCollisionObject *objs = (ThreediCollisionObject *)calloc(count, sizeof(ThreediCollisionObject));
    if (!objs && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        objs[i].unk0 = read_s32_le(base + 0);
        assert(objs[i].unk0 == 0);
        objs[i].num_vertices = read_s32_le(base + 4);
        objs[i].num_faces = read_s32_le(base + 8);
        objs[i].num_planes = read_s32_le(base + 12);
        objs[i].num_bounding_volumes = read_s32_le(base + 16);
        objs[i].parent_subobject_index = read_s32_le(base + 20);
        objs[i].unk3 = read_s32_le(base + 24);
        objs[i].unk4 = read_s32_le(base + 28);
        objs[i].unk5 = read_s32_le(base + 32);
        assert(objs[i].unk3 == 0 && objs[i].unk4 == 0 && objs[i].unk5 == 0);
        objs[i].offset[0] = (float)read_s32_le(base + 36);
        objs[i].offset[1] = (float)read_s32_le(base + 40);
        objs[i].offset[2] = (float)read_s32_le(base + 44);
        objs[i].min[0] = (float)read_s32_le(base + 48);
        objs[i].min[1] = (float)read_s32_le(base + 52);
        objs[i].min[2] = (float)read_s32_le(base + 56);
        objs[i].max[0] = (float)read_s32_le(base + 60);
        objs[i].max[1] = (float)read_s32_le(base + 64);
        objs[i].max[2] = (float)read_s32_le(base + 68);
        objs[i].med[0] = (float)read_s32_le(base + 72);
        objs[i].med[1] = (float)read_s32_le(base + 76);
        objs[i].med[2] = (float)read_s32_le(base + 80);
        objs[i].radius = (float)read_s32_le(base + 84);
    }
    *out_objs = objs;
    *out_count = count;
    return 0;
}

static int parse_cxlt(const ThreediChunk *chunk, ThreediCollisionTranslation **out_trans, size_t *out_count)
{
    if (!chunk || !out_trans || !out_count) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 12);
    size_t expected = 8u + (size_t)count * 12u;
    assert(chunk->data_len == expected);
    if (chunk->data_len != expected) {
        return -1;
    }
    ThreediCollisionTranslation *trans = (ThreediCollisionTranslation *)calloc(count, sizeof(ThreediCollisionTranslation));
    if (!trans && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        trans[i].translation[0] = (float)read_s32_le(base + 0);
        trans[i].translation[1] = (float)read_s32_le(base + 4);
        trans[i].translation[2] = (float)read_s32_le(base + 8);
    }
    *out_trans = trans;
    *out_count = count;
    return 0;
}

static int parse_material_texture(const uint8_t *p, ThreediMaterialTexture *out)
{
    memcpy(out->name, p, 16);
    out->name[16] = '\0';
    out->slot = read_u8(p + 16);
    out->type = read_u8(p + 17);
    out->flags = read_u8(p + 18);
    out->frame = read_u8(p + 19);
    return 0;
}

static int parse_material(const uint8_t *base, uint32_t record_size, ThreediMaterial *out, int index)
{
    assert(record_size == 584);
    memset(out, 0, sizeof(*out));
    out->index = index;
    size_t cursor = 0;

    assert(cursor + 32 <= record_size);
    memcpy(out->shader_name, base + cursor, 32);
    out->shader_name[32] = '\0';
    cursor += 32;

    assert(cursor + 4 <= record_size);
    uint32_t tex_count = read_u32_le(base + cursor);
    assert(tex_count <= 24);
    out->texture_count = tex_count;
    cursor += 4;

    size_t tex_block = 24 * 20;
    assert(cursor + tex_block <= record_size);
    for (uint32_t i = 0; i < tex_count && i < 24; ++i) {
        parse_material_texture(base + cursor + i * 20, &out->textures[i]);
        assert(out->textures[i].slot >= 1 && out->textures[i].slot <= 4);
        assert(out->textures[i].type == 0 || out->textures[i].type == 4 || out->textures[i].type == 5);
    }
    cursor += tex_block;

    assert(cursor + 8 <= record_size);
    out->alpha_gen.style = read_u8(base + cursor);
    uint8_t alpha_phase_or_reg = read_u8(base + cursor + 1);
    if (out->alpha_gen.style <= 112) {
        out->alpha_gen.phase = (float)alpha_phase_or_reg / 256.0f;
        out->alpha_gen.reg = -1;
    } else {
        out->alpha_gen.reg = alpha_phase_or_reg;
        out->alpha_gen.phase = 0.0f;
    }
    out->alpha_gen.rate = (float)read_s16_le(base + cursor + 2) / 256.0f;
    out->alpha_gen.start = read_s16_le(base + cursor + 4);
    out->alpha_gen.end = read_s16_le(base + cursor + 6);
    cursor += 8;

    assert(cursor + 12 <= record_size);
    out->rgb_gen.style = read_u8(base + cursor);
    uint8_t rgb_phase_or_reg = read_u8(base + cursor + 1);
    if (out->rgb_gen.style <= 112) {
        out->rgb_gen.phase = (float)rgb_phase_or_reg / 256.0f;
        out->rgb_gen.reg = -1;
    } else {
        out->rgb_gen.reg = rgb_phase_or_reg;
        out->rgb_gen.phase = 0.0f;
    }
    out->rgb_gen.rate = (float)read_s16_le(base + cursor + 2) / 256.0f;
    out->rgb_gen.start_color[2] = (float)read_u8(base + cursor + 4) / 255.0f;
    out->rgb_gen.start_color[1] = (float)read_u8(base + cursor + 5) / 255.0f;
    out->rgb_gen.start_color[0] = (float)read_u8(base + cursor + 6) / 255.0f;
    out->rgb_gen.start_color[3] = (float)read_u8(base + cursor + 7) / 255.0f;
    out->rgb_gen.end_color[2] = (float)read_u8(base + cursor + 8) / 255.0f;
    out->rgb_gen.end_color[1] = (float)read_u8(base + cursor + 9) / 255.0f;
    out->rgb_gen.end_color[0] = (float)read_u8(base + cursor + 10) / 255.0f;
    out->rgb_gen.end_color[3] = (float)read_u8(base + cursor + 11) / 255.0f;
    cursor += 12;

    // Second RGB generator (same 12-byte compact format, always zero in practice)
    assert(cursor + 12 <= record_size);
    out->rgb_gen2.style = read_u8(base + cursor);
    uint8_t rgb2_phase_or_reg = read_u8(base + cursor + 1);
    if (out->rgb_gen2.style <= 112) {
        out->rgb_gen2.phase = (float)rgb2_phase_or_reg / 256.0f;
        out->rgb_gen2.reg = -1;
    } else {
        out->rgb_gen2.reg = rgb2_phase_or_reg;
        out->rgb_gen2.phase = 0.0f;
    }
    out->rgb_gen2.rate = (float)read_s16_le(base + cursor + 2) / 256.0f;
    out->rgb_gen2.start_color[2] = (float)read_u8(base + cursor + 4) / 255.0f;
    out->rgb_gen2.start_color[1] = (float)read_u8(base + cursor + 5) / 255.0f;
    out->rgb_gen2.start_color[0] = (float)read_u8(base + cursor + 6) / 255.0f;
    out->rgb_gen2.start_color[3] = (float)read_u8(base + cursor + 7) / 255.0f;
    out->rgb_gen2.end_color[2] = (float)read_u8(base + cursor + 8) / 255.0f;
    out->rgb_gen2.end_color[1] = (float)read_u8(base + cursor + 9) / 255.0f;
    out->rgb_gen2.end_color[0] = (float)read_u8(base + cursor + 10) / 255.0f;
    out->rgb_gen2.end_color[3] = (float)read_u8(base + cursor + 11) / 255.0f;
    cursor += 12;

    assert(cursor + 8 <= record_size);
    out->u_params.style = read_u8(base + cursor);
    uint8_t up_phase_or_reg = read_u8(base + cursor + 1);
    if (out->u_params.style <= 112) {
        out->u_params.phase = (float)up_phase_or_reg / 256.0f;
        out->u_params.reg = -1;
    } else {
        out->u_params.reg = up_phase_or_reg;
        out->u_params.phase = 0.0f;
    }
    out->u_params.gen_rate = (float)read_s16_le(base + cursor + 2) / 256.0f;
    out->u_params.start = (float)read_s16_le(base + cursor + 4) / 256.0f;
    out->u_params.end = (float)read_s16_le(base + cursor + 6) / 256.0f;
    cursor += 8;

    assert(cursor + 8 <= record_size);
    out->v_params.style = read_u8(base + cursor);
    uint8_t vp_phase_or_reg = read_u8(base + cursor + 1);
    if (out->v_params.style <= 112) {
        out->v_params.phase = (float)vp_phase_or_reg / 256.0f;
        out->v_params.reg = -1;
    } else {
        out->v_params.reg = vp_phase_or_reg;
        out->v_params.phase = 0.0f;
    }
    out->v_params.gen_rate = (float)read_s16_le(base + cursor + 2) / 256.0f;
    out->v_params.start = (float)read_s16_le(base + cursor + 4) / 256.0f;
    out->v_params.end = (float)read_s16_le(base + cursor + 6) / 256.0f;
    cursor += 8;

    assert(cursor + 4 <= record_size);
    out->reflect_color[2] = (float)read_u8(base + cursor + 0) / 255.0f;
    out->reflect_color[1] = (float)read_u8(base + cursor + 1) / 255.0f;
    out->reflect_color[0] = (float)read_u8(base + cursor + 2) / 255.0f;
    out->reflect_color[3] = (float)read_u8(base + cursor + 3) / 255.0f;
    cursor += 4;

    // Second reflect color (always zero in practice — WriteMTRL only populates channel 0)
    assert(cursor + 4 <= record_size);
    out->reflect_color2[2] = (float)read_u8(base + cursor + 0) / 255.0f;
    out->reflect_color2[1] = (float)read_u8(base + cursor + 1) / 255.0f;
    out->reflect_color2[0] = (float)read_u8(base + cursor + 2) / 255.0f;
    out->reflect_color2[3] = (float)read_u8(base + cursor + 3) / 255.0f;
    cursor += 4;
    assert(cursor + 4 <= record_size);
    out->emissive_type = read_u8(base + cursor);
    assert(out->emissive_type == 0 || out->emissive_type == 2);
    cursor += 1;
    out->emissive_type2 = read_u8(base + cursor);
    cursor += 1;
    out->is_glass = read_u8(base + cursor);
    assert(out->is_glass == 0 || out->is_glass == 1);
    cursor += 1;
    out->glass_type2 = read_u8(base + cursor);
    cursor += 1;
    assert(cursor + 4 <= record_size);
    out->material_flags = read_u8(base + cursor);
    cursor += 1;
    out->alpha_test_value_byte = read_u8(base + cursor);
    cursor += 1;
    out->pad[0] = read_u8(base + cursor);
    out->pad[1] = read_u8(base + cursor + 1);
    cursor += 2;
    assert(cursor + 4 <= record_size);
    out->animation.num_frames = read_u8(base + cursor);
    out->animation.animation_type = read_u8(base + cursor + 1);
    out->animation.cycle_frame_time = read_s16_le(base + cursor + 2);
    assert(out->animation.animation_type == 0 || out->animation.animation_type == 1);

    if (out->emissive_type == 2) {
        size_t name_len = strlen(out->shader_name);
        int has_lum_suffix = 0;
        if (name_len >= 4 && strcmp(out->shader_name + name_len - 4, "_LUM") == 0) {
            has_lum_suffix = 1;
        } else if (name_len >= 7 && strcmp(out->shader_name + name_len - 7, "_LUM#UV") == 0) {
            has_lum_suffix = 1;
        }
        assert(has_lum_suffix);
    }

    (void)record_size; // reserved for future validation
    return 0;
}

static int parse_mtrl(const ThreediChunk *chunk, ThreediMaterial **out_mats, uint32_t *out_count, uint32_t *out_record_size)
{
    if (!chunk || !out_mats || !out_count || !out_record_size) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 584);
    size_t expected = 8u + (size_t)count * (size_t)record_size;
    assert(chunk->data_len == expected);
    ThreediMaterial *mats = (ThreediMaterial *)calloc(count, sizeof(ThreediMaterial));
    if (!mats && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        if (parse_material(base, record_size, &mats[i], (int)i) != 0) {
            free(mats);
            return -1;
        }
    }
    *out_mats = mats;
    *out_count = count;
    *out_record_size = record_size;
    return 0;
}

static int parse_lght(const ThreediChunk *chunk, ThreediLight **out_lights, size_t *out_count)
{
    if (!chunk || !out_lights || !out_count) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 116);
    size_t expected = 8u + (size_t)count * 116u;
    assert(chunk->data_len == expected);
    ThreediLight *lights = (ThreediLight *)calloc(count, sizeof(ThreediLight));
    if (!lights && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        ThreediLight *l = &lights[i];
        l->offset[0] = read_f32_le(base + 0);
        l->offset[1] = read_f32_le(base + 4);
        l->offset[2] = read_f32_le(base + 8);
        l->atten_start = read_f32_le(base + 12);
        l->atten_end = read_f32_le(base + 16);
        l->style = read_u8(base + 20);
        l->phase = read_u8(base + 21);
        l->rate = read_u16_le(base + 22);
        l->color_start[0] = read_u8(base + 24);
        l->color_start[1] = read_u8(base + 25);
        l->color_start[2] = read_u8(base + 26);
        l->color_start[3] = read_u8(base + 27);
        l->color_end[0] = read_u8(base + 28);
        l->color_end[1] = read_u8(base + 29);
        l->color_end[2] = read_u8(base + 30);
        l->color_end[3] = read_u8(base + 31);
        l->subobj_index = read_u8(base + 32);
        l->flags = read_u8(base + 33);
        l->unknown1 = read_u8(base + 34);
        l->falloff_byte = read_u8(base + 35);
        l->rotation[0] = read_f32_le(base + 36);
        l->rotation[1] = read_f32_le(base + 40);
        l->rotation[2] = read_f32_le(base + 44);
        l->rotation[3] = read_f32_le(base + 48);
        const uint8_t *mat = base + 52;
        for (int m = 0; m < 16; ++m) {
            l->view_proj[m] = read_f32_le(mat + m * 4);
        }
    }
    *out_lights = lights;
    *out_count = count;
    return 0;
}

static int parse_usrp(const ThreediChunk *chunk, ThreediUserPoint **out_points, size_t *out_count)
{
    if (!chunk || !out_points || !out_count) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 48);
    size_t expected = 8u + (size_t)count * 48u;
    assert(chunk->data_len == expected);
    ThreediUserPoint *pts = (ThreediUserPoint *)calloc(count, sizeof(ThreediUserPoint));
    if (!pts && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        pts[i].x = read_s32_le(base + 0);
        pts[i].y = read_s32_le(base + 4);
        pts[i].z = read_s32_le(base + 8);
        pts[i].rot_x = read_s32_le(base + 12);
        pts[i].rot_y = read_s32_le(base + 16);
        pts[i].rot_z = read_s32_le(base + 20);
        pts[i].subobject_index = read_s32_le(base + 24);
        pts[i].userpoint_type = read_s32_le(base + 28);
        memcpy(pts[i].name, base + 32, 16);
        pts[i].name[16] = '\0';
    }
    *out_points = pts;
    *out_count = count;
    return 0;
}

static int parse_ovrt(const ThreediChunk *chunk, ThreediOcclusionVertex **out_vertices, size_t *out_count, uint32_t *out_record_size)
{
    if (!chunk || !out_vertices || !out_count || !out_record_size) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 12);
    size_t expected = 8u + (size_t)count * (size_t)record_size;
    assert(chunk->data_len == expected);
    if (chunk->data_len != expected) {
        return -1;
    }
    ThreediOcclusionVertex *verts = (ThreediOcclusionVertex *)calloc(count, sizeof(ThreediOcclusionVertex));
    if (!verts && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        verts[i].position[0] = read_f32_le(base + 0);
        verts[i].position[1] = read_f32_le(base + 4);
        verts[i].position[2] = read_f32_le(base + 8);
    }
    *out_vertices = verts;
    *out_count = count;
    *out_record_size = record_size;
    return 0;
}

static ThreediTransform parse_transform(const uint8_t *p)
{
    ThreediTransform t;
    t.control = read_u8(p + 0);
    t.control_param = read_u8(p + 1);
    t.rate = read_s16_le(p + 2);
    t.start = read_s16_le(p + 4);
    t.end = read_s16_le(p + 6);
    return t;
}

static int parse_panm(const ThreediChunk *chunk, ThreediPartAnimation **out_anims, size_t *out_count, uint32_t *out_record_size)
{
    if (!chunk || !out_anims || !out_count || !out_record_size) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    size_t expected = 8u + (size_t)count * (size_t)record_size;
    if (chunk->data_len != expected || record_size != 68u) {
        return -1;
    }
    ThreediPartAnimation *anims = (ThreediPartAnimation *)calloc(count, sizeof(ThreediPartAnimation));
    if (!anims && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        ThreediPartAnimation *a = &anims[i];
        a->flags = read_u32_le(base + 0);
        a->parent_subobject = read_u8(base + 4);
        a->subobject_index = read_u8(base + 5);
        a->matrix_index = read_u8(base + 6);
        a->matrix_offset = read_u8(base + 7);
        a->bind_matrix_index = (int32_t)read_u32_le(base + 8);
        // assert(a->bind_matrix_index == 0 && "bind_matrix_index expected to always be 0");
        a->rotation_x = parse_transform(base + 12);
        a->rotation_y = parse_transform(base + 20);
        a->rotation_z = parse_transform(base + 28);
        a->scale_x = parse_transform(base + 36);
        a->scale_y = parse_transform(base + 44);
        a->scale_z = parse_transform(base + 52);
        a->translation = parse_transform(base + 60);
    }
    *out_anims = anims;
    *out_count = count;
    *out_record_size = record_size;
    return 0;
}

static int parse_ofac(const ThreediChunk *chunk, ThreediOcclusionFace **out_faces, size_t *out_count, uint32_t *out_record_size)
{
    if (!chunk || !out_faces || !out_count || !out_record_size) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 12);
    size_t expected = 8u + (size_t)count * (size_t)record_size;
    assert(chunk->data_len == expected);
    if (chunk->data_len != expected) {
        return -1;
    }
    ThreediOcclusionFace *faces = (ThreediOcclusionFace *)calloc(count, sizeof(ThreediOcclusionFace));
    if (!faces && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        faces[i].raw_indices = read_u32_le(base + 0);
        faces[i].edge_data = read_u32_le(base + 4);
        faces[i].other_edge_data = read_u32_le(base + 8);
    }
    *out_faces = faces;
    *out_count = count;
    *out_record_size = record_size;
    return 0;
}

static int parse_oobj(const ThreediChunk *chunk, ThreediOcclusionObject **out_objs, size_t *out_count, uint32_t *out_record_size)
{
    if (!chunk || !out_objs || !out_count || !out_record_size) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 36);
    size_t expected = 8u + (size_t)count * (size_t)record_size;
    assert(chunk->data_len == expected);
    if (chunk->data_len != expected) {
        return -1;
    }
    ThreediOcclusionObject *objs = (ThreediOcclusionObject *)calloc(count, sizeof(ThreediOcclusionObject));
    if (!objs && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        objs[i].type = read_u8(base + 0);
        objs[i].parent_subobject_index = read_u8(base + 1);
        objs[i].connecting_subobject = read_u8(base + 2);
        objs[i].unused0 = read_u8(base + 3);
        objs[i].position[0] = read_f32_le(base + 4);
        objs[i].position[1] = read_f32_le(base + 8);
        objs[i].position[2] = read_f32_le(base + 12);
        objs[i].radius = read_f32_le(base + 16);
        objs[i].unk1 = read_s32_le(base + 20);
        assert(objs[i].unk1 == 0);
        objs[i].num_vertices = read_s32_le(base + 24);
        objs[i].num_planes = read_s32_le(base + 28);
        objs[i].face_count = read_s32_le(base + 32);
    }
    *out_objs = objs;
    *out_count = count;
    *out_record_size = record_size;
    return 0;
}

static int parse_opln_typed(const ThreediChunk *chunk, ThreediOcclusionPlane **out_planes, size_t *out_count, uint32_t *out_record_size)
{
    if (!chunk || !out_planes || !out_count || !out_record_size) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 16);
    size_t expected = 8u + (size_t)count * (size_t)record_size;
    assert(chunk->data_len == expected);
    if (chunk->data_len != expected) {
        return -1;
    }
    ThreediOcclusionPlane *planes = (ThreediOcclusionPlane *)calloc(count, sizeof(ThreediOcclusionPlane));
    if (!planes && count > 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        planes[i].normal[0] = read_f32_le(base + 0);
        planes[i].normal[1] = read_f32_le(base + 4);
        planes[i].normal[2] = read_f32_le(base + 8);
        planes[i].radius = read_f32_le(base + 12);
    }
    *out_planes = planes;
    *out_count = count;
    *out_record_size = record_size;
    return 0;
}
static int parse_ctrl(const ThreediChunk *chunk, ThreediCtrl *out_ctrl)
{
    if (!chunk || !out_ctrl) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data + 0);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 24);
    size_t expected = 8u + (size_t)count * (size_t)record_size;
    assert(chunk->data_len == expected);
    if (chunk->data_len != expected) {
        return -1;
    }
    ThreediControlRegister *regs = NULL;
    if (count > 0) {
        regs = (ThreediControlRegister *)calloc(count, sizeof(ThreediControlRegister));
        if (!regs) {
            return -1;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
        memcpy(regs[i].name, base, 24);
        regs[i].name[24] = '\0';
    }
    out_ctrl->count = count;
    out_ctrl->record_size = record_size;
    out_ctrl->registers = regs;
    return 0;
}

static int parse_raw_table(const ThreediChunk *chunk, ThreediRawTable *out_table)
{
    if (!chunk || !out_table) {
        return -1;
    }
    if (chunk->data_len < 8) {
        return -1;
    }
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    size_t expected = 8u + (size_t)count * (size_t)record_size;
    if (chunk->data_len < expected) {
        return -1;
    }
    uint8_t *copy = NULL;
    if (expected > 8) {
        copy = (uint8_t *)malloc(expected - 8);
        if (!copy) {
            return -1;
        }
        memcpy(copy, chunk->data + 8, expected - 8);
    }
    out_table->count = count;
    out_table->record_size = record_size;
    out_table->data = copy;
    out_table->data_len = expected - 8;
    return 0;
}

static int parse_mtrx(const ThreediChunk *chunk, ThreediMatrixTable *out)
{
    if (!chunk || !out) {
        return -1;
    }
    assert(chunk->data_len >= 8);
    uint32_t count = read_u32_le(chunk->data);
    uint32_t record_size = read_u32_le(chunk->data + 4);
    assert(record_size == 64);
    size_t expected = 8u + (size_t)count * (size_t)record_size;
    assert(chunk->data_len == expected);
    if (chunk->data_len != expected) {
        return -1;
    }
    ThreediMatrix4x4 *mats = NULL;
    if (record_size >= 64 && count > 0) {
        mats = (ThreediMatrix4x4 *)calloc(count, sizeof(ThreediMatrix4x4));
        if (!mats) {
            return -1;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t *base = chunk->data + 8 + (size_t)i * record_size;
            for (int j = 0; j < 16; ++j) {
                mats[i].m[j] = read_f32_le(base + j * 4);
            }
        }
    }
    out->count = count;
    out->record_size = record_size;
    out->matrices = mats;
    return 0;
}

int threedi_3di3_parse(const ThreediFile *file, Threedi3di3 *out_model)
{
    if (!file || !file->root || !out_model) {
        return -1;
    }
    memset(out_model, 0, sizeof(*out_model));
    out_model->version = file->version;

    const ThreediChunk *ghdr = find_first_chunk(file->root, "GHDR");
    if (ghdr) {
        if (parse_header_chunk(ghdr, &out_model->header) != 0) {
            free_model_arrays(out_model);
            return -1;
        }
    }

    const ThreediChunk **rlod_list = NULL;
    size_t rlod_count = 0;
    size_t rlod_cap = 0;
    if (collect_chunks(file->root, "RLOD", &rlod_list, &rlod_count, &rlod_cap) != 0) {
        free(rlod_list);
        return -1;
    }

    if (rlod_count == 0) {
        free(rlod_list);
        return -1;
    }

    out_model->lods = (ThreediLod *)calloc(rlod_count, sizeof(ThreediLod));
    if (!out_model->lods) {
        free(rlod_list);
        return -1;
    }
    out_model->lod_count = rlod_count;

    for (size_t i = 0; i < rlod_count; ++i) {
        if (parse_rlod(rlod_list[i], &out_model->lods[i]) != 0) {
            free(rlod_list);
            threedi_3di3_free(out_model);
            return -1;
        }
    }

    free(rlod_list);

    const ThreediChunk *info = find_first_chunk(file->root, "INFO");
    if (info) {
        if (parse_info_chunk(info, &out_model->info) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
    }

    const ThreediChunk *ctrl_chunk = find_first_chunk(file->root, "CTRL");
    if (ctrl_chunk) {
        if (parse_ctrl(ctrl_chunk, &out_model->ctrl) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
    }

    const ThreediChunk *mtrl = find_first_chunk(file->root, "MTRL");
    if (mtrl) {
        if (parse_mtrl(mtrl, &out_model->materials, &out_model->material_count, &out_model->material_record_size) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
    }

    const ThreediChunk *lght = find_first_chunk(file->root, "LGHT");
    if (lght) {
        if (parse_lght(lght, &out_model->lights, &out_model->light_count) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
    }

    const ThreediChunk *usrp = find_first_chunk(file->root, "USRP");
    if (usrp) {
        if (parse_usrp(usrp, &out_model->user_points, &out_model->user_point_count) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
    }

    const ThreediChunk *cmdl = find_first_chunk(file->root, "CMDL");
    const ThreediChunk *bpln = find_first_chunk(file->root, "BPLN");
    const ThreediChunk *bvol = find_first_chunk(file->root, "BVOL");
    const ThreediChunk *cvrt = find_first_chunk(file->root, "CVRT");
    const ThreediChunk *cnrm = find_first_chunk(file->root, "CNRM");
    const ThreediChunk *cfac = find_first_chunk(file->root, "CFAC");
    const ThreediChunk *cobj = find_first_chunk(file->root, "COBJ");
    const ThreediChunk *cxlt = find_first_chunk(file->root, "CXLT");
    if (cmdl || bpln || bvol || cvrt || cnrm || cfac || cobj || cxlt) {
        out_model->collision = (ThreediCollisionModel *)calloc(1, sizeof(ThreediCollisionModel));
        if (!out_model->collision) {
            threedi_3di3_free(out_model);
            return -1;
        }
        if (cmdl && parse_cmdl(cmdl, &out_model->collision->model_data) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
        if (bpln && parse_bpln(bpln, &out_model->collision->planes, &out_model->collision->plane_count) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
        if (bvol && parse_bvol(bvol, &out_model->collision->volumes, &out_model->collision->volume_count) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
        if (cvrt && parse_cvrt(cvrt, &out_model->collision->vertices, &out_model->collision->vertex_count) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
        if (cnrm && parse_cnrm(cnrm, &out_model->collision->normals, &out_model->collision->normal_count) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
        if (cfac && parse_cfac(cfac, &out_model->collision->faces, &out_model->collision->face_count) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
        if (cobj && parse_cobj(cobj, &out_model->collision->objects, &out_model->collision->object_count) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
        if (cxlt && parse_cxlt(cxlt, &out_model->collision->translations, &out_model->collision->translation_count) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
    }

    const ThreediChunk *opln = find_first_chunk(file->root, "OPLN");
    if (opln) {
        if (parse_opln_typed(opln, &out_model->occlusion_planes, &out_model->occlusion_plane_count, &out_model->occlusion_plane_record_size) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
        // Keep raw as well for completeness.
    }

    const ThreediChunk *ofac = find_first_chunk(file->root, "OFAC");
    if (ofac) {
        if (parse_ofac(ofac, &out_model->occlusion_faces, &out_model->occlusion_face_count, &out_model->occlusion_face_record_size) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
    }

    const ThreediChunk *oobj = find_first_chunk(file->root, "OOBJ");
    if (oobj) {
        if (parse_oobj(oobj, &out_model->occlusion_objects, &out_model->occlusion_object_count, &out_model->occlusion_object_record_size) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
    }

    const ThreediChunk *mtrx = find_first_chunk(file->root, "MTRX");
    if (mtrx) {
        if (parse_mtrx(mtrx, &out_model->mtrx) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
    }
    const ThreediChunk *ovrt = find_first_chunk(file->root, "OVRT");
    if (ovrt) {
        if (parse_ovrt(ovrt, &out_model->occlusion_vertices, &out_model->occlusion_vertex_count, &out_model->occlusion_vertex_record_size) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
    }
    const ThreediChunk *panm = find_first_chunk(file->root, "PANM");
    if (panm) {
        if (parse_panm(panm, &out_model->part_animations, &out_model->part_animation_count, &out_model->part_animation_record_size) != 0) {
            threedi_3di3_free(out_model);
            return -1;
        }
    }

    return 0;
}

void threedi_3di3_free(Threedi3di3 *model)
{
    if (!model) {
        return;
    }
    free_model_arrays(model);
}

int threedi_3di3_read(const char *path, Threedi3di3 *out_model)
{
    if (!path || !out_model) {
        return -1;
    }
    ThreediFile file = {0};
    int rc = threedi_read_file(path, &file);
    if (rc != 0) {
        return rc;
    }
    rc = threedi_3di3_parse(&file, out_model);
    threedi_free_file(&file);
    return rc;
}

int threedi_3di3_read_memory(const uint8_t *data, size_t size, Threedi3di3 *out_model)
{
    if (!data || size == 0 || !out_model) {
        return -1;
    }
    ThreediFile file = {0};
    int rc = threedi_read_memory(data, size, &file);
    if (rc != 0) {
        return rc;
    }
    rc = threedi_3di3_parse(&file, out_model);
    threedi_free_file(&file);
    return rc;
}

typedef struct BufferBuilder {
    uint8_t *data;
    size_t len;
    size_t cap;
} BufferBuilder;

static void buffer_builder_free(BufferBuilder *buf)
{
    if (!buf) {
        return;
    }
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static int buffer_reserve(BufferBuilder *buf, size_t additional)
{
    if (!buf) {
        return -1;
    }
    if (additional == 0) {
        return 0;
    }
    size_t needed = buf->len + additional;
    if (needed < buf->len) {
        return -1;
    }
    if (needed <= buf->cap) {
        return 0;
    }
    size_t new_cap = buf->cap == 0 ? 256u : buf->cap;
    while (new_cap < needed) {
        if (new_cap > (SIZE_MAX / 2u)) {
            new_cap = needed;
            break;
        }
        new_cap *= 2u;
    }
    uint8_t *tmp = (uint8_t *)realloc(buf->data, new_cap);
    if (!tmp) {
        return -1;
    }
    buf->data = tmp;
    buf->cap = new_cap;
    return 0;
}

static int buffer_append(BufferBuilder *buf, const void *src, size_t len)
{
    if (!buf || (len > 0 && !src)) {
        return -1;
    }
    if (buffer_reserve(buf, len) != 0) {
        return -1;
    }
    if (len > 0) {
        memcpy(buf->data + buf->len, src, len);
        buf->len += len;
    }
    return 0;
}

static int buffer_append_zeroes(BufferBuilder *buf, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (buffer_reserve(buf, len) != 0) {
        return -1;
    }
    memset(buf->data + buf->len, 0, len);
    buf->len += len;
    return 0;
}

static int buffer_append_u8(BufferBuilder *buf, uint8_t v)
{
    return buffer_append(buf, &v, 1);
}

static int buffer_append_u16_le(BufferBuilder *buf, uint16_t v)
{
    uint8_t tmp[2] = {
        (uint8_t)(v & 0xFFu),
        (uint8_t)((v >> 8) & 0xFFu)
    };
    return buffer_append(buf, tmp, sizeof(tmp));
}

static int buffer_append_s16_le(BufferBuilder *buf, int16_t v)
{
    return buffer_append_u16_le(buf, (uint16_t)v);
}

static int buffer_append_u32_le(BufferBuilder *buf, uint32_t v)
{
    uint8_t tmp[4] = {
        (uint8_t)(v & 0xFFu),
        (uint8_t)((v >> 8) & 0xFFu),
        (uint8_t)((v >> 16) & 0xFFu),
        (uint8_t)((v >> 24) & 0xFFu)
    };
    return buffer_append(buf, tmp, sizeof(tmp));
}

static int buffer_append_s32_le(BufferBuilder *buf, int32_t v)
{
    return buffer_append_u32_le(buf, (uint32_t)v);
}

static int buffer_append_f32_le(BufferBuilder *buf, float v)
{
    uint8_t tmp[4];
    memcpy(tmp, &v, sizeof(float));
    return buffer_append(buf, tmp, sizeof(tmp));
}

static int buffer_append_padded(BufferBuilder *buf, const char *src, size_t width)
{
    if (!buf || !src) {
        return -1;
    }
    size_t len = strlen(src);
    if (len > width) {
        len = width;
    }
    if (buffer_append(buf, src, len) != 0) {
        return -1;
    }
    return buffer_append_zeroes(buf, width - len);
}

static int64_t round_nearest(double value)
{
    return value >= 0.0 ? (int64_t)(value + 0.5) : (int64_t)(value - 0.5);
}

static uint8_t float_to_byte(float value, float scale)
{
    long v = (long)round_nearest((double)value * (double)scale);
    if (v < 0) {
        v = 0;
    }
    if (v > 255) {
        v = 255;
    }
    return (uint8_t)v;
}

static int buffer_append_fixed_16_16(BufferBuilder *buf, float value)
{
    int32_t raw = (int32_t)round_nearest((double)value * 65536.0);
    return buffer_append_s32_le(buf, raw);
}

static int buffer_append_fixed_14(BufferBuilder *buf, float value)
{
    int16_t raw = (int16_t)round_nearest((double)value * 16384.0);
    return buffer_append_s16_le(buf, raw);
}

static int buffer_append_scaled_s16(BufferBuilder *buf, float value, float scale)
{
    int16_t raw = (int16_t)round_nearest((double)value * (double)scale);
    return buffer_append_s16_le(buf, raw);
}

static int buffer_append_scaled_s32(BufferBuilder *buf, double value, double scale)
{
    int32_t raw = (int32_t)round_nearest(value * scale);
    return buffer_append_s32_le(buf, raw);
}

typedef struct ChunkBuilder {
    char id[5];
    int is_parent;
    BufferBuilder payload;
    struct ChunkBuilder *children;
    size_t child_count;
    size_t child_cap;
} ChunkBuilder;

static void chunk_builder_init(ChunkBuilder *chunk, const char id[4], int is_parent)
{
    memset(chunk, 0, sizeof(*chunk));
    memcpy(chunk->id, id, 4);
    chunk->id[4] = '\0';
    chunk->is_parent = is_parent;
}

static void chunk_builder_free(ChunkBuilder *chunk)
{
    if (!chunk) {
        return;
    }
    for (size_t i = 0; i < chunk->child_count; ++i) {
        chunk_builder_free(&chunk->children[i]);
    }
    free(chunk->children);
    chunk->children = NULL;
    chunk->child_count = 0;
    chunk->child_cap = 0;
    buffer_builder_free(&chunk->payload);
    memset(chunk->id, 0, sizeof(chunk->id));
    chunk->is_parent = 0;
}

static int chunk_builder_add_child(ChunkBuilder *parent, ChunkBuilder *child)
{
    if (!parent || !child) {
        return -1;
    }
    if (parent->child_count == parent->child_cap) {
        size_t new_cap = parent->child_cap == 0 ? 4u : parent->child_cap * 2u;
        ChunkBuilder *tmp = (ChunkBuilder *)realloc(parent->children, new_cap * sizeof(ChunkBuilder));
        if (!tmp) {
            return -1;
        }
        parent->children = tmp;
        parent->child_cap = new_cap;
    }
    parent->children[parent->child_count] = *child;
    memset(child, 0, sizeof(*child));
    parent->child_count++;
    parent->is_parent = 1;
    return 0;
}

static int chunk_total_length(const ChunkBuilder *chunk, size_t *out_total)
{
    if (!chunk || !out_total) {
        return -1;
    }
    size_t content_len = 0;
    if (chunk->child_count == 0) {
        content_len = chunk->payload.len;
    } else {
        for (size_t i = 0; i < chunk->child_count; ++i) {
            size_t child_total = 0;
            if (chunk_total_length(&chunk->children[i], &child_total) != 0) {
                return -1;
            }
            if (SIZE_MAX - content_len < child_total) {
                return -1;
            }
            content_len += child_total;
        }
    }
    if (content_len > LENGTH_MASK) {
        return -1;
    }
    size_t total = content_len + 8u;
    if (total < content_len) {
        return -1;
    }
    *out_total = total;
    return 0;
}

static int chunk_builder_serialize(const ChunkBuilder *chunk, BufferBuilder *out)
{
    if (!chunk || !out) {
        return -1;
    }
    size_t total_len = 0;
    if (chunk_total_length(chunk, &total_len) != 0) {
        return -1;
    }
    uint32_t content_len = (uint32_t)(total_len - 8u);
    uint32_t flags = content_len & LENGTH_MASK;
    if (chunk->child_count > 0 || chunk->is_parent) {
        flags |= PARENT_FLAG;
    }
    if (buffer_append(out, chunk->id, 4) != 0) {
        return -1;
    }
    if (buffer_append_u32_le(out, flags) != 0) {
        return -1;
    }
    if (chunk->child_count == 0) {
        return buffer_append(out, chunk->payload.data, chunk->payload.len);
    }
    for (size_t i = 0; i < chunk->child_count; ++i) {
        if (chunk_builder_serialize(&chunk->children[i], out) != 0) {
            return -1;
        }
    }
    return 0;
}

static int build_info_chunk(const ThreediInfo *info, ChunkBuilder *out)
{
    chunk_builder_init(out, "INFO", 0);
    if (info && info->data && info->data_len > 0) {
        if (buffer_append(&out->payload, info->data, info->data_len) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    return 0;
}

static int build_ghdr_chunk(const ThreediHeader *header, ChunkBuilder *out)
{
    if (!header || !header->has_header) {
        return -1;
    }
    chunk_builder_init(out, "GHDR", 0);
    if (buffer_append_padded(&out->payload, header->name, 16) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    if (buffer_append_s32_le(&out->payload, (int32_t)header->mesh_type) != 0 ||
        buffer_append_s32_le(&out->payload, header->lod_count_decl) != 0 ||
        buffer_append_s32_le(&out->payload, header->lod_distance) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_usrp_chunk(const Threedi3di3 *model, ChunkBuilder *out)
{
    if (!model) {
        return -1;
    }
    if (model->user_point_count > 0 && !model->user_points) {
        return -1;
    }
    chunk_builder_init(out, "USRP", 0);
    uint32_t count = (uint32_t)model->user_point_count;
    if (buffer_append_u32_le(&out->payload, count) != 0 ||
        buffer_append_u32_le(&out->payload, 48u) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < model->user_point_count; ++i) {
        const ThreediUserPoint *p = &model->user_points[i];
        if (buffer_append_s32_le(&out->payload, p->x) != 0 ||
            buffer_append_s32_le(&out->payload, p->y) != 0 ||
            buffer_append_s32_le(&out->payload, p->z) != 0 ||
            buffer_append_s32_le(&out->payload, p->rot_x) != 0 ||
            buffer_append_s32_le(&out->payload, p->rot_y) != 0 ||
            buffer_append_s32_le(&out->payload, p->rot_z) != 0 ||
            buffer_append_s32_le(&out->payload, p->subobject_index) != 0 ||
            buffer_append_s32_le(&out->payload, p->userpoint_type) != 0) {
            chunk_builder_free(out);
            return -1;
        }
        if (buffer_append_padded(&out->payload, p->name, 16) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)model->user_point_count * 48u;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_ctrl_chunk(const ThreediCtrl *ctrl, ChunkBuilder *out)
{
    if (!ctrl) {
        return -1;
    }
    uint32_t record_size = ctrl->record_size == 0 ? 24u : ctrl->record_size;
    if (record_size != 24u) {
        return -1;
    }
    if (ctrl->count > 0 && !ctrl->registers) {
        return -1;
    }
    chunk_builder_init(out, "CTRL", 0);
    if (buffer_append_u32_le(&out->payload, ctrl->count) != 0 ||
        buffer_append_u32_le(&out->payload, record_size) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (uint32_t i = 0; i < ctrl->count; ++i) {
        if (buffer_append_padded(&out->payload, ctrl->registers[i].name, 24) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)ctrl->count * (size_t)record_size;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int append_material_texture(BufferBuilder *buf, const ThreediMaterialTexture *tex)
{
    if (buffer_append_padded(buf, tex->name, 16) != 0) {
        return -1;
    }
    if (buffer_append_u8(buf, tex->slot) != 0 ||
        buffer_append_u8(buf, tex->type) != 0 ||
        buffer_append_u8(buf, tex->flags) != 0 ||
        buffer_append_u8(buf, tex->frame) != 0) {
        return -1;
    }
    return 0;
}

static int append_alpha_gen(BufferBuilder *buf, const ThreediAlphaGen *alpha)
{
    uint8_t phase_or_reg = 0;
    if (alpha->style <= 112) {
        phase_or_reg = float_to_byte(alpha->phase, 256.0f);
    } else {
        phase_or_reg = (uint8_t)alpha->reg;
    }
    if (buffer_append_u8(buf, alpha->style) != 0 ||
        buffer_append_u8(buf, phase_or_reg) != 0 ||
        buffer_append_scaled_s16(buf, alpha->rate, 256.0f) != 0 ||
        buffer_append_s16_le(buf, alpha->start) != 0 ||
        buffer_append_s16_le(buf, alpha->end) != 0) {
        return -1;
    }
    return 0;
}

static int append_rgb_gen(BufferBuilder *buf, const ThreediRgbGen *rgb)
{
    uint8_t block[12] = {0};
    block[0] = rgb->style;
    block[1] = rgb->style <= 112 ? float_to_byte(rgb->phase, 256.0f) : (uint8_t)rgb->reg;
    uint16_t rate_raw = (uint16_t)round_nearest((double)rgb->rate * 256.0);
    block[2] = (uint8_t)(rate_raw & 0xFFu);
    block[3] = (uint8_t)((rate_raw >> 8) & 0xFFu);
    block[4] = float_to_byte(rgb->start_color[2], 255.0f);
    block[5] = float_to_byte(rgb->start_color[1], 255.0f);
    block[6] = float_to_byte(rgb->start_color[0], 255.0f);
    block[7] = float_to_byte(rgb->start_color[3], 255.0f);
    block[8] = float_to_byte(rgb->end_color[2], 255.0f);
    block[9] = float_to_byte(rgb->end_color[1], 255.0f);
    block[10] = float_to_byte(rgb->end_color[0], 255.0f);
    block[11] = float_to_byte(rgb->end_color[3], 255.0f);
    if (buffer_append(buf, block, sizeof(block)) != 0) {
        return -1;
    }
    return 0;
}

static int append_uv_params(BufferBuilder *buf, const ThreediUvParams *uv)
{
    uint8_t phase_or_reg = uv->style <= 112 ? float_to_byte(uv->phase, 256.0f) : (uint8_t)uv->reg;
    if (buffer_append_u8(buf, uv->style) != 0 ||
        buffer_append_u8(buf, phase_or_reg) != 0 ||
        buffer_append_scaled_s16(buf, uv->gen_rate, 256.0f) != 0 ||
        buffer_append_scaled_s16(buf, uv->start, 256.0f) != 0 ||
        buffer_append_scaled_s16(buf, uv->end, 256.0f) != 0) {
        return -1;
    }
    return 0;
}

static int append_material(BufferBuilder *buf, const ThreediMaterial *mat, uint32_t record_size)
{
    size_t start = buf->len;
    if (mat->texture_count > 24u) {
        return -1;
    }
    if (buffer_append_padded(buf, mat->shader_name, 32) != 0) {
        return -1;
    }
    if (buffer_append_u32_le(buf, mat->texture_count) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < 24u; ++i) {
        ThreediMaterialTexture zero = {0};
        const ThreediMaterialTexture *tex = i < mat->texture_count ? &mat->textures[i] : &zero;
        if (append_material_texture(buf, tex) != 0) {
            return -1;
        }
    }
    if (append_alpha_gen(buf, &mat->alpha_gen) != 0) {
        return -1;
    }
    if (append_rgb_gen(buf, &mat->rgb_gen) != 0 ||
        append_rgb_gen(buf, &mat->rgb_gen2) != 0) {
        return -1;
    }
    if (append_uv_params(buf, &mat->u_params) != 0 ||
        append_uv_params(buf, &mat->v_params) != 0) {
        return -1;
    }
    if (buffer_append_u8(buf, float_to_byte(mat->reflect_color[2], 255.0f)) != 0 ||
        buffer_append_u8(buf, float_to_byte(mat->reflect_color[1], 255.0f)) != 0 ||
        buffer_append_u8(buf, float_to_byte(mat->reflect_color[0], 255.0f)) != 0 ||
        buffer_append_u8(buf, float_to_byte(mat->reflect_color[3], 255.0f)) != 0) {
        return -1;
    }
    if (buffer_append_u8(buf, float_to_byte(mat->reflect_color2[2], 255.0f)) != 0 ||
        buffer_append_u8(buf, float_to_byte(mat->reflect_color2[1], 255.0f)) != 0 ||
        buffer_append_u8(buf, float_to_byte(mat->reflect_color2[0], 255.0f)) != 0 ||
        buffer_append_u8(buf, float_to_byte(mat->reflect_color2[3], 255.0f)) != 0) {
        return -1;
    }
    if (buffer_append_u8(buf, mat->emissive_type) != 0 ||
        buffer_append_u8(buf, mat->emissive_type2) != 0 ||
        buffer_append_u8(buf, mat->is_glass) != 0 ||
        buffer_append_u8(buf, mat->glass_type2) != 0 ||
        buffer_append_u8(buf, mat->material_flags) != 0 ||
        buffer_append_u8(buf, mat->alpha_test_value_byte) != 0 ||
        buffer_append_u8(buf, mat->pad[0]) != 0 ||
        buffer_append_u8(buf, mat->pad[1]) != 0) {
        return -1;
    }
    if (buffer_append_u8(buf, mat->animation.num_frames) != 0 ||
        buffer_append_u8(buf, mat->animation.animation_type) != 0 ||
        buffer_append_s16_le(buf, mat->animation.cycle_frame_time) != 0) {
        return -1;
    }
    size_t written = buf->len - start;
    if (written != record_size) {
        return -1;
    }
    return 0;
}

static int build_mtrl_chunk(const Threedi3di3 *model, ChunkBuilder *out)
{
    uint32_t record_size = model->material_record_size == 0 ? 584u : model->material_record_size;
    if (record_size != 584u) {
        return -1;
    }
    if (model->material_count > 0 && !model->materials) {
        return -1;
    }
    chunk_builder_init(out, "MTRL", 0);
    if (buffer_append_u32_le(&out->payload, model->material_count) != 0 ||
        buffer_append_u32_le(&out->payload, record_size) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (uint32_t i = 0; i < model->material_count; ++i) {
        if (append_material(&out->payload, &model->materials[i], record_size) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)model->material_count * (size_t)record_size;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_lght_chunk(const Threedi3di3 *model, ChunkBuilder *out)
{
    if (!model) {
        return -1;
    }
    if (model->light_count > 0 && !model->lights) {
        return -1;
    }
    chunk_builder_init(out, "LGHT", 0);
    if (buffer_append_u32_le(&out->payload, (uint32_t)model->light_count) != 0 ||
        buffer_append_u32_le(&out->payload, 116u) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < model->light_count; ++i) {
        const ThreediLight *l = &model->lights[i];
        size_t start = out->payload.len;
        if (buffer_append_f32_le(&out->payload, l->offset[0]) != 0 ||
            buffer_append_f32_le(&out->payload, l->offset[1]) != 0 ||
            buffer_append_f32_le(&out->payload, l->offset[2]) != 0 ||
            buffer_append_f32_le(&out->payload, l->atten_start) != 0 ||
            buffer_append_f32_le(&out->payload, l->atten_end) != 0 ||
            buffer_append_u8(&out->payload, l->style) != 0 ||
            buffer_append_u8(&out->payload, l->phase) != 0 ||
            buffer_append_u16_le(&out->payload, l->rate) != 0 ||
            buffer_append_u8(&out->payload, l->color_start[0]) != 0 ||
            buffer_append_u8(&out->payload, l->color_start[1]) != 0 ||
            buffer_append_u8(&out->payload, l->color_start[2]) != 0 ||
            buffer_append_u8(&out->payload, l->color_start[3]) != 0 ||
            buffer_append_u8(&out->payload, l->color_end[0]) != 0 ||
            buffer_append_u8(&out->payload, l->color_end[1]) != 0 ||
            buffer_append_u8(&out->payload, l->color_end[2]) != 0 ||
            buffer_append_u8(&out->payload, l->color_end[3]) != 0 ||
            buffer_append_u8(&out->payload, l->subobj_index) != 0 ||
            buffer_append_u8(&out->payload, l->flags) != 0 ||
            buffer_append_u8(&out->payload, l->unknown1) != 0 ||
            buffer_append_u8(&out->payload, l->falloff_byte) != 0) {
            chunk_builder_free(out);
            return -1;
        }
        for (int r = 0; r < 4; ++r) {
            if (buffer_append_f32_le(&out->payload, l->rotation[r]) != 0) {
                chunk_builder_free(out);
                return -1;
            }
        }
        for (int m = 0; m < 16; ++m) {
            if (buffer_append_f32_le(&out->payload, l->view_proj[m]) != 0) {
                chunk_builder_free(out);
                return -1;
            }
        }
        if (out->payload.len - start != 116u) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)model->light_count * 116u;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_mtrx_chunk(const ThreediMatrixTable *mtrx, ChunkBuilder *out)
{
    if (!mtrx) {
        return -1;
    }
    uint32_t record_size = mtrx->record_size == 0 ? 64u : mtrx->record_size;
    if (record_size != 64u) {
        return -1;
    }
    if (mtrx->count > 0 && !mtrx->matrices) {
        return -1;
    }
    chunk_builder_init(out, "MTRX", 0);
    if (buffer_append_u32_le(&out->payload, mtrx->count) != 0 ||
        buffer_append_u32_le(&out->payload, record_size) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (uint32_t i = 0; i < mtrx->count; ++i) {
        for (int j = 0; j < 16; ++j) {
            if (buffer_append_f32_le(&out->payload, mtrx->matrices[i].m[j]) != 0) {
                chunk_builder_free(out);
                return -1;
            }
        }
    }
    size_t expected = 8u + (size_t)mtrx->count * (size_t)record_size;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_ovrt_chunk(const Threedi3di3 *model, ChunkBuilder *out)
{
    if (!model) {
        return -1;
    }
    uint32_t record_size = model->occlusion_vertex_record_size == 0 ? 12u : model->occlusion_vertex_record_size;
    if (record_size != 12u) {
        return -1;
    }
    if (model->occlusion_vertex_count > 0 && !model->occlusion_vertices) {
        return -1;
    }
    chunk_builder_init(out, "OVRT", 0);
    if (buffer_append_u32_le(&out->payload, (uint32_t)model->occlusion_vertex_count) != 0 ||
        buffer_append_u32_le(&out->payload, record_size) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < model->occlusion_vertex_count; ++i) {
        const ThreediOcclusionVertex *v = &model->occlusion_vertices[i];
        if (buffer_append_f32_le(&out->payload, v->position[0]) != 0 ||
            buffer_append_f32_le(&out->payload, v->position[1]) != 0 ||
            buffer_append_f32_le(&out->payload, v->position[2]) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)model->occlusion_vertex_count * (size_t)record_size;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_ofac_chunk(const Threedi3di3 *model, ChunkBuilder *out)
{
    if (!model) {
        return -1;
    }
    uint32_t record_size = model->occlusion_face_record_size == 0 ? 12u : model->occlusion_face_record_size;
    if (record_size != 12u) {
        return -1;
    }
    if (model->occlusion_face_count > 0 && !model->occlusion_faces) {
        return -1;
    }
    chunk_builder_init(out, "OFAC", 0);
    if (buffer_append_u32_le(&out->payload, (uint32_t)model->occlusion_face_count) != 0 ||
        buffer_append_u32_le(&out->payload, record_size) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < model->occlusion_face_count; ++i) {
        const ThreediOcclusionFace *f = &model->occlusion_faces[i];
        if (buffer_append_u32_le(&out->payload, f->raw_indices) != 0 ||
            buffer_append_u32_le(&out->payload, f->edge_data) != 0 ||
            buffer_append_u32_le(&out->payload, f->other_edge_data) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)model->occlusion_face_count * (size_t)record_size;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_oobj_chunk(const Threedi3di3 *model, ChunkBuilder *out)
{
    if (!model) {
        return -1;
    }
    uint32_t record_size = model->occlusion_object_record_size == 0 ? 36u : model->occlusion_object_record_size;
    if (record_size != 36u) {
        return -1;
    }
    if (model->occlusion_object_count > 0 && !model->occlusion_objects) {
        return -1;
    }
    chunk_builder_init(out, "OOBJ", 0);
    if (buffer_append_u32_le(&out->payload, (uint32_t)model->occlusion_object_count) != 0 ||
        buffer_append_u32_le(&out->payload, record_size) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < model->occlusion_object_count; ++i) {
        const ThreediOcclusionObject *o = &model->occlusion_objects[i];
        if (buffer_append_u8(&out->payload, o->type) != 0 ||
            buffer_append_u8(&out->payload, o->parent_subobject_index) != 0 ||
            buffer_append_u8(&out->payload, o->connecting_subobject) != 0 ||
            buffer_append_u8(&out->payload, o->unused0) != 0 ||
            buffer_append_f32_le(&out->payload, o->position[0]) != 0 ||
            buffer_append_f32_le(&out->payload, o->position[1]) != 0 ||
            buffer_append_f32_le(&out->payload, o->position[2]) != 0 ||
            buffer_append_f32_le(&out->payload, o->radius) != 0 ||
            buffer_append_s32_le(&out->payload, o->unk1) != 0 ||
            buffer_append_s32_le(&out->payload, o->num_vertices) != 0 ||
            buffer_append_s32_le(&out->payload, o->num_planes) != 0 ||
            buffer_append_s32_le(&out->payload, o->face_count) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)model->occlusion_object_count * (size_t)record_size;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_opln_chunk(const Threedi3di3 *model, ChunkBuilder *out)
{
    if (!model) {
        return -1;
    }
    uint32_t record_size = model->occlusion_plane_record_size == 0 ? 16u : model->occlusion_plane_record_size;
    if (record_size != 16u) {
        return -1;
    }
    if (model->occlusion_plane_count > 0 && !model->occlusion_planes) {
        return -1;
    }
    chunk_builder_init(out, "OPLN", 0);
    if (buffer_append_u32_le(&out->payload, (uint32_t)model->occlusion_plane_count) != 0 ||
        buffer_append_u32_le(&out->payload, record_size) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < model->occlusion_plane_count; ++i) {
        const ThreediOcclusionPlane *p = &model->occlusion_planes[i];
        if (buffer_append_f32_le(&out->payload, p->normal[0]) != 0 ||
            buffer_append_f32_le(&out->payload, p->normal[1]) != 0 ||
            buffer_append_f32_le(&out->payload, p->normal[2]) != 0 ||
            buffer_append_f32_le(&out->payload, p->radius) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)model->occlusion_plane_count * (size_t)record_size;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_cmdl_chunk(const ThreediCollisionModelData *cmdl, ChunkBuilder *out)
{
    if (!cmdl) {
        return -1;
    }
    chunk_builder_init(out, "CMDL", 0);
    // Layout: bbox {minX,minY,minZ,maxX,maxY,maxZ}, radii, 7 counts.
    if (buffer_append_fixed_16_16(&out->payload, cmdl->bbox[0]) != 0 ||
        buffer_append_fixed_16_16(&out->payload, cmdl->bbox[1]) != 0 ||
        buffer_append_fixed_16_16(&out->payload, cmdl->bbox[2]) != 0 ||
        buffer_append_fixed_16_16(&out->payload, cmdl->bbox[3]) != 0 ||
        buffer_append_fixed_16_16(&out->payload, cmdl->bbox[4]) != 0 ||
        buffer_append_fixed_16_16(&out->payload, cmdl->bbox[5]) != 0 ||
        buffer_append_fixed_16_16(&out->payload, cmdl->radii[0]) != 0 ||
        buffer_append_fixed_16_16(&out->payload, cmdl->radii[1]) != 0 ||
        buffer_append_fixed_16_16(&out->payload, cmdl->radii[2]) != 0 ||
        buffer_append_s32_le(&out->payload, cmdl->num_vertices) != 0 ||
        buffer_append_s32_le(&out->payload, cmdl->num_normals) != 0 ||
        buffer_append_s32_le(&out->payload, cmdl->num_faces) != 0 ||
        buffer_append_s32_le(&out->payload, cmdl->num_objects) != 0 ||
        buffer_append_s32_le(&out->payload, cmdl->num_transforms) != 0 ||
        buffer_append_s32_le(&out->payload, cmdl->num_bounding_planes) != 0 ||
        buffer_append_s32_le(&out->payload, cmdl->num_bounding_volumes) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    if (out->payload.len != 64u) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_bpln_chunk(const ThreediCollisionModel *collision, ChunkBuilder *out)
{
    if (!collision) {
        return -1;
    }
    if (collision->plane_count > 0 && !collision->planes) {
        return -1;
    }
    chunk_builder_init(out, "BPLN", 0);
    if (buffer_append_u32_le(&out->payload, (uint32_t)collision->plane_count) != 0 ||
        buffer_append_u32_le(&out->payload, 12u) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < collision->plane_count; ++i) {
        const ThreediBoundingPlane *p = &collision->planes[i];
        if (buffer_append_s16_le(&out->payload, p->flags) != 0 ||
            buffer_append_fixed_14(&out->payload, p->normal[0]) != 0 ||
            buffer_append_fixed_14(&out->payload, p->normal[1]) != 0 ||
            buffer_append_fixed_14(&out->payload, p->normal[2]) != 0 ||
            buffer_append_fixed_16_16(&out->payload, p->radius) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)collision->plane_count * 12u;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_bvol_chunk(const ThreediCollisionModel *collision, ChunkBuilder *out)
{
    if (!collision) {
        return -1;
    }
    if (collision->volume_count > 0 && !collision->volumes) {
        return -1;
    }
    chunk_builder_init(out, "BVOL", 0);
    if (buffer_append_u32_le(&out->payload, (uint32_t)collision->volume_count) != 0 ||
        buffer_append_u32_le(&out->payload, 36u) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < collision->volume_count; ++i) {
        const ThreediBoundingVolume *v = &collision->volumes[i];
        if (buffer_append_s32_le(&out->payload, v->collidable_type) != 0 ||
            buffer_append_s32_le(&out->payload, v->flags) != 0 ||
            buffer_append_s32_le(&out->payload, v->min_x_fp16) != 0 ||
            buffer_append_s32_le(&out->payload, v->min_y_fp16) != 0 ||
            buffer_append_s32_le(&out->payload, v->min_z_fp16) != 0 ||
            buffer_append_s32_le(&out->payload, v->max_x_fp16) != 0 ||
            buffer_append_s32_le(&out->payload, v->max_y_fp16) != 0 ||
            buffer_append_s32_le(&out->payload, v->max_z_fp16) != 0 ||
            buffer_append_s32_le(&out->payload, v->plane_count) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)collision->volume_count * 36u;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_cvrt_chunk(const ThreediCollisionModel *collision, ChunkBuilder *out)
{
    if (!collision) {
        return -1;
    }
    if (collision->vertex_count > 0 && !collision->vertices) {
        return -1;
    }
    chunk_builder_init(out, "CVRT", 0);
    if (buffer_append_u32_le(&out->payload, (uint32_t)collision->vertex_count) != 0 ||
        buffer_append_u32_le(&out->payload, 8u) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < collision->vertex_count; ++i) {
        const ThreediCollisionVertex *v = &collision->vertices[i];
        if (buffer_append_scaled_s16(&out->payload, v->position[0], 256.0f) != 0 ||
            buffer_append_scaled_s16(&out->payload, v->position[1], 256.0f) != 0 ||
            buffer_append_scaled_s16(&out->payload, v->position[2], 256.0f) != 0 ||
            buffer_append_s16_le(&out->payload, 0) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)collision->vertex_count * 8u;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_cnrm_chunk(const ThreediCollisionModel *collision, ChunkBuilder *out)
{
    if (!collision) {
        return -1;
    }
    if (collision->normal_count > 0 && !collision->normals) {
        return -1;
    }
    chunk_builder_init(out, "CNRM", 0);
    if (buffer_append_u32_le(&out->payload, (uint32_t)collision->normal_count) != 0 ||
        buffer_append_u32_le(&out->payload, 8u) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < collision->normal_count; ++i) {
        const ThreediCollisionNormal *n = &collision->normals[i];
        // Truncation toward zero, matching original WriteCNRM's (unsigned __int64) cast.
        int16_t raw_x = (int16_t)(n->normal[0] * 16384.0f);
        int16_t raw_y = (int16_t)(n->normal[1] * 16384.0f);
        int16_t raw_z = (int16_t)(n->normal[2] * 16384.0f);
        if (buffer_append_s16_le(&out->payload, raw_x) != 0 ||
            buffer_append_s16_le(&out->payload, raw_y) != 0 ||
            buffer_append_s16_le(&out->payload, raw_z) != 0 ||
            buffer_append_s16_le(&out->payload, n->dominate_axis) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)collision->normal_count * 8u;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_cfac_chunk(const ThreediCollisionModel *collision, ChunkBuilder *out)
{
    if (!collision) {
        return -1;
    }
    if (collision->face_count > 0 && !collision->faces) {
        return -1;
    }
    chunk_builder_init(out, "CFAC", 0);
    if (buffer_append_u32_le(&out->payload, (uint32_t)collision->face_count) != 0 ||
        buffer_append_u32_le(&out->payload, 44u) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < collision->face_count; ++i) {
        const ThreediCollisionFace *f = &collision->faces[i];
        if (buffer_append_s16_le(&out->payload, f->vert_index[0]) != 0 ||
            buffer_append_s16_le(&out->payload, f->vert_index[1]) != 0 ||
            buffer_append_s16_le(&out->payload, f->vert_index[2]) != 0 ||
            buffer_append_s16_le(&out->payload, f->normal_index) != 0 ||
            buffer_append_s32_le(&out->payload, f->plane_dist_fp16) != 0 ||
            buffer_append_s32_le(&out->payload, f->min_x_fp16) != 0 ||
            buffer_append_s32_le(&out->payload, f->min_y_fp16) != 0 ||
            buffer_append_s32_le(&out->payload, f->min_z_fp16) != 0 ||
        buffer_append_s32_le(&out->payload, f->max_x_fp16) != 0 ||
        buffer_append_s32_le(&out->payload, f->max_y_fp16) != 0 ||
        buffer_append_s32_le(&out->payload, f->max_z_fp16) != 0 ||
        buffer_append_s32_le(&out->payload, (int32_t)f->material_flags) != 0 ||
            buffer_append_u8(&out->payload, f->poly_type) != 0 ||
            buffer_append_u8(&out->payload, f->pad[0]) != 0 ||
            buffer_append_u8(&out->payload, f->pad[1]) != 0 ||
            buffer_append_u8(&out->payload, f->pad[2]) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)collision->face_count * 44u;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_cobj_chunk(const ThreediCollisionModel *collision, ChunkBuilder *out)
{
    if (!collision) {
        return -1;
    }
    if (collision->object_count > 0 && !collision->objects) {
        return -1;
    }
    chunk_builder_init(out, "COBJ", 0);
    if (buffer_append_u32_le(&out->payload, (uint32_t)collision->object_count) != 0 ||
        buffer_append_u32_le(&out->payload, 88u) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < collision->object_count; ++i) {
        const ThreediCollisionObject *o = &collision->objects[i];
        if (buffer_append_s32_le(&out->payload, o->unk0) != 0 ||
            buffer_append_s32_le(&out->payload, o->num_vertices) != 0 ||
            buffer_append_s32_le(&out->payload, o->num_faces) != 0 ||
            buffer_append_s32_le(&out->payload, o->num_planes) != 0 ||
            buffer_append_s32_le(&out->payload, o->num_bounding_volumes) != 0 ||
            buffer_append_s32_le(&out->payload, o->parent_subobject_index) != 0 ||
            buffer_append_s32_le(&out->payload, o->unk3) != 0 ||
            buffer_append_s32_le(&out->payload, o->unk4) != 0 ||
            buffer_append_s32_le(&out->payload, o->unk5) != 0 ||
            buffer_append_s32_le(&out->payload, (int32_t)o->offset[0]) != 0 ||
            buffer_append_s32_le(&out->payload, (int32_t)o->offset[1]) != 0 ||
            buffer_append_s32_le(&out->payload, (int32_t)o->offset[2]) != 0 ||
            buffer_append_s32_le(&out->payload, (int32_t)o->min[0]) != 0 ||
            buffer_append_s32_le(&out->payload, (int32_t)o->min[1]) != 0 ||
            buffer_append_s32_le(&out->payload, (int32_t)o->min[2]) != 0 ||
            buffer_append_s32_le(&out->payload, (int32_t)o->max[0]) != 0 ||
            buffer_append_s32_le(&out->payload, (int32_t)o->max[1]) != 0 ||
            buffer_append_s32_le(&out->payload, (int32_t)o->max[2]) != 0 ||
            buffer_append_s32_le(&out->payload, (int32_t)o->med[0]) != 0 ||
            buffer_append_s32_le(&out->payload, (int32_t)o->med[1]) != 0 ||
            buffer_append_s32_le(&out->payload, (int32_t)o->med[2]) != 0 ||
            buffer_append_s32_le(&out->payload, (int32_t)o->radius) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)collision->object_count * 88u;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_cxlt_chunk(const ThreediCollisionModel *collision, ChunkBuilder *out)
{
    if (!collision) {
        return -1;
    }
    if (collision->translation_count > 0 && !collision->translations) {
        return -1;
    }
    chunk_builder_init(out, "CXLT", 0);
    if (buffer_append_u32_le(&out->payload, (uint32_t)collision->translation_count) != 0 ||
        buffer_append_u32_le(&out->payload, 12u) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < collision->translation_count; ++i) {
        const ThreediCollisionTranslation *t = &collision->translations[i];
        if (buffer_append_s32_le(&out->payload, (int32_t)t->translation[0]) != 0 ||
            buffer_append_s32_le(&out->payload, (int32_t)t->translation[1]) != 0 ||
            buffer_append_s32_le(&out->payload, (int32_t)t->translation[2]) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)collision->translation_count * 12u;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_rmdl_chunk(const ThreediLod *lod, ChunkBuilder *out)
{
    chunk_builder_init(out, "RMDL", 0);
    char model_type[4] = {0};
    memcpy(model_type, lod->model_type, 4);
    if (buffer_append(&out->payload, model_type, 4) != 0 ||
        buffer_append_s32_le(&out->payload, lod->lod_threshold) != 0 ||
        buffer_append_s32_le(&out->payload, lod->rmdl_render_object_count) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_vert_chunk(const ThreediVertexBuffer *verts, ChunkBuilder *out)
{
    if (!verts) {
        return -1;
    }
    if (verts->count > 0 && !verts->items) {
        return -1;
    }
    uint32_t expected_stride = 40u;
    if ((verts->flags & THREEDI_VERTEX_FLAG_SKINNED) != 0) {
        expected_stride += 16u;
    }
    if ((verts->flags & THREEDI_VERTEX_FLAG_TANGENTS) != 0) {
        expected_stride += 24u;
    }
    if (expected_stride != verts->stride) {
        return -1;
    }
    chunk_builder_init(out, "VERT", 0);
    if (buffer_append_u32_le(&out->payload, verts->count) != 0 ||
        buffer_append_u32_le(&out->payload, verts->stride) != 0 ||
        buffer_append_u32_le(&out->payload, verts->flags) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    int is_skinned = (verts->flags & THREEDI_VERTEX_FLAG_SKINNED) != 0;
    int has_tangents = (verts->flags & THREEDI_VERTEX_FLAG_TANGENTS) != 0;
    for (uint32_t i = 0; i < verts->count; ++i) {
        const ThreediVertex *v = &verts->items[i];
        if (buffer_append_f32_le(&out->payload, v->position[0]) != 0 ||
            buffer_append_f32_le(&out->payload, v->position[1]) != 0 ||
            buffer_append_f32_le(&out->payload, v->position[2]) != 0) {
            chunk_builder_free(out);
            return -1;
        }
        if (is_skinned) {
            if (buffer_append_f32_le(&out->payload, v->bone_weights[0]) != 0 ||
                buffer_append_f32_le(&out->payload, v->bone_weights[1]) != 0 ||
                buffer_append_f32_le(&out->payload, v->bone_weights[2]) != 0 ||
                buffer_append(&out->payload, v->bone_indices, 4) != 0) {
                chunk_builder_free(out);
                return -1;
            }
        }
        if (buffer_append_f32_le(&out->payload, v->normal[0]) != 0 ||
            buffer_append_f32_le(&out->payload, v->normal[1]) != 0 ||
            buffer_append_f32_le(&out->payload, v->normal[2]) != 0 ||
            buffer_append_f32_le(&out->payload, v->uv0[0]) != 0 ||
            buffer_append_f32_le(&out->payload, v->uv0[1]) != 0 ||
            buffer_append_f32_le(&out->payload, v->uv1[0]) != 0 ||
            buffer_append_f32_le(&out->payload, v->uv1[1]) != 0) {
            chunk_builder_free(out);
            return -1;
        }
        if (has_tangents) {
            if (buffer_append_f32_le(&out->payload, v->tangent[0]) != 0 ||
                buffer_append_f32_le(&out->payload, v->tangent[1]) != 0 ||
                buffer_append_f32_le(&out->payload, v->tangent[2]) != 0 ||
                buffer_append_f32_le(&out->payload, v->bitangent[0]) != 0 ||
                buffer_append_f32_le(&out->payload, v->bitangent[1]) != 0 ||
                buffer_append_f32_le(&out->payload, v->bitangent[2]) != 0) {
                chunk_builder_free(out);
                return -1;
            }
        }
    }
    size_t expected = 12u + (size_t)verts->count * (size_t)verts->stride;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_indx_chunk(const ThreediIndexBuffer *indices, ChunkBuilder *out)
{
    if (!indices) {
        return -1;
    }
    if (indices->count > 0 && !indices->indices) {
        return -1;
    }
    chunk_builder_init(out, "INDX", 0);
    if (buffer_append_u32_le(&out->payload, indices->count) != 0 ||
        buffer_append_u32_le(&out->payload, 2u) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (uint32_t i = 0; i < indices->count; ++i) {
        if (buffer_append_u16_le(&out->payload, indices->indices[i]) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)indices->count * 2u;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_strp_chunk(const ThreediLod *lod, ChunkBuilder *out)
{
    if (lod->strip_record_size != 48u && lod->strip_record_size != 68u) {
        return -1;
    }
    if (lod->strip_count > 0 && !lod->strips) {
        return -1;
    }
    chunk_builder_init(out, "STRP", 0);
    if (buffer_append_u32_le(&out->payload, (uint32_t)lod->strip_count) != 0 ||
        buffer_append_u32_le(&out->payload, lod->strip_record_size) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < lod->strip_count; ++i) {
        const ThreediTriangleStrip *s = &lod->strips[i];
        if (buffer_append_s32_le(&out->payload, s->material_index) != 0 ||
            buffer_append_s32_le(&out->payload, s->index_offset) != 0 ||
            buffer_append_u16_le(&out->payload, s->num_indices) != 0 ||
            buffer_append_u16_le(&out->payload, s->num_triangles) != 0 ||
            buffer_append_s32_le(&out->payload, s->is_strip) != 0 ||
            buffer_append_s32_le(&out->payload, s->start_vertex) != 0 ||
            buffer_append_s32_le(&out->payload, s->num_vertices) != 0 ||
            buffer_append_f32_le(&out->payload, s->min[0]) != 0 ||
            buffer_append_f32_le(&out->payload, s->min[1]) != 0 ||
            buffer_append_f32_le(&out->payload, s->min[2]) != 0 ||
            buffer_append_f32_le(&out->payload, s->max[0]) != 0 ||
            buffer_append_f32_le(&out->payload, s->max[1]) != 0 ||
            buffer_append_f32_le(&out->payload, s->max[2]) != 0) {
            chunk_builder_free(out);
            return -1;
        }
        if (lod->strip_record_size == 68u) {
            if (buffer_append(&out->payload, s->bone_table, 16) != 0 ||
                buffer_append_s32_le(&out->payload, s->bone_table_length) != 0) {
                chunk_builder_free(out);
                return -1;
            }
        }
    }
    size_t expected = 8u + (size_t)lod->strip_count * (size_t)lod->strip_record_size;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int build_robj_chunk(const ThreediLod *lod, ChunkBuilder *out)
{
    if (lod->render_object_count > 0 && !lod->render_objects) {
        return -1;
    }
    chunk_builder_init(out, "ROBJ", 0);
    if (buffer_append_u32_le(&out->payload, (uint32_t)lod->render_object_count) != 0 ||
        buffer_append_u32_le(&out->payload, 52u) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < lod->render_object_count; ++i) {
        const ThreediRenderObject *o = &lod->render_objects[i];
        if (buffer_append_s32_le(&out->payload, o->num_strips) != 0 ||
            buffer_append_s32_le(&out->payload, o->num_alpha_strips) != 0 ||
            buffer_append_s32_le(&out->payload, o->parent_index) != 0 ||
            buffer_append_f32_le(&out->payload, o->rel[0]) != 0 ||
            buffer_append_f32_le(&out->payload, o->rel[1]) != 0 ||
            buffer_append_f32_le(&out->payload, o->rel[2]) != 0 ||
            buffer_append_f32_le(&out->payload, o->abs[0]) != 0 ||
            buffer_append_f32_le(&out->payload, o->abs[1]) != 0 ||
            buffer_append_f32_le(&out->payload, o->abs[2]) != 0 ||
            buffer_append_f32_le(&out->payload, o->bounding_center[0]) != 0 ||
            buffer_append_f32_le(&out->payload, o->bounding_center[1]) != 0 ||
            buffer_append_f32_le(&out->payload, o->bounding_center[2]) != 0 ||
            buffer_append_f32_le(&out->payload, o->bounding_radius) != 0) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)lod->render_object_count * 52u;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

static int append_transform(BufferBuilder *buf, const ThreediTransform *t)
{
    if (buffer_append_u8(buf, t->control) != 0 ||
        buffer_append_u8(buf, t->control_param) != 0 ||
        buffer_append_s16_le(buf, t->rate) != 0 ||
        buffer_append_s16_le(buf, t->start) != 0 ||
        buffer_append_s16_le(buf, t->end) != 0) {
        return -1;
    }
    return 0;
}

static int build_panm_chunk(const ThreediLod *lod, const Threedi3di3 *model, ChunkBuilder *out)
{
    // Use per-LOD PANM data if available, otherwise fall back to global (for backwards compat).
    const ThreediPartAnimation *panm_data = lod->part_animations ? lod->part_animations : model->part_animations;
    const size_t panm_count = lod->part_animations ? lod->part_animation_count : model->part_animation_count;
    const uint32_t panm_rec_size = lod->part_animations
        ? (lod->part_animation_record_size == 0 ? 68u : lod->part_animation_record_size)
        : (model->part_animation_record_size == 0 ? 68u : model->part_animation_record_size);
    if (panm_rec_size != 68u) {
        return -1;
    }
    if (panm_count > 0 && !panm_data) {
        return -1;
    }
    chunk_builder_init(out, "PANM", 0);
    if (buffer_append_u32_le(&out->payload, (uint32_t)panm_count) != 0 ||
        buffer_append_u32_le(&out->payload, panm_rec_size) != 0) {
        chunk_builder_free(out);
        return -1;
    }
    for (size_t i = 0; i < panm_count; ++i) {
        const ThreediPartAnimation *a = &panm_data[i];
        size_t record_start = out->payload.len;
        if (buffer_append_u32_le(&out->payload, a->flags) != 0 ||
            buffer_append_u8(&out->payload, a->parent_subobject) != 0 ||
            buffer_append_u8(&out->payload, a->subobject_index) != 0 ||
            buffer_append_u8(&out->payload, a->matrix_index) != 0 ||
            buffer_append_u8(&out->payload, a->matrix_offset) != 0 ||
            buffer_append_s32_le(&out->payload, a->bind_matrix_index) != 0 ||
            append_transform(&out->payload, &a->rotation_x) != 0 ||
            append_transform(&out->payload, &a->rotation_y) != 0 ||
            append_transform(&out->payload, &a->rotation_z) != 0 ||
            append_transform(&out->payload, &a->scale_x) != 0 ||
            append_transform(&out->payload, &a->scale_y) != 0 ||
            append_transform(&out->payload, &a->scale_z) != 0 ||
            append_transform(&out->payload, &a->translation) != 0) {
            chunk_builder_free(out);
            return -1;
        }
        size_t record_written = out->payload.len - record_start;
        if (record_written != (size_t)panm_rec_size) {
            chunk_builder_free(out);
            return -1;
        }
    }
    size_t expected = 8u + (size_t)panm_count * (size_t)panm_rec_size;
    if (out->payload.len != expected) {
        chunk_builder_free(out);
        return -1;
    }
    return 0;
}

typedef struct SerializeContext {
    const Threedi3di3 *model;
} SerializeContext;

static int build_rlod_chunk(const Threedi3di3 *model, const ThreediLod *lod, SerializeContext *ctx, ChunkBuilder *out)
{
    (void)ctx;
    if (!model || !lod) {
        return -1;
    }
    chunk_builder_init(out, "RLOD", 1);

    ChunkBuilder rmdl = {0};
    if (build_rmdl_chunk(lod, &rmdl) != 0 || chunk_builder_add_child(out, &rmdl) != 0) {
        chunk_builder_free(&rmdl);
        chunk_builder_free(out);
        return -1;
    }

    ChunkBuilder vert = {0};
    if (build_vert_chunk(&lod->vertices, &vert) != 0 || chunk_builder_add_child(out, &vert) != 0) {
        chunk_builder_free(&vert);
        chunk_builder_free(out);
        return -1;
    }

    ChunkBuilder indx = {0};
    if (build_indx_chunk(&lod->indices, &indx) != 0 || chunk_builder_add_child(out, &indx) != 0) {
        chunk_builder_free(&indx);
        chunk_builder_free(out);
        return -1;
    }

    ChunkBuilder strp = {0};
    if (build_strp_chunk(lod, &strp) != 0 || chunk_builder_add_child(out, &strp) != 0) {
        chunk_builder_free(&strp);
        chunk_builder_free(out);
        return -1;
    }

    ChunkBuilder robj = {0};
    if (build_robj_chunk(lod, &robj) != 0 || chunk_builder_add_child(out, &robj) != 0) {
        chunk_builder_free(&robj);
        chunk_builder_free(out);
        return -1;
    }

    ChunkBuilder panm = {0};
    if (build_panm_chunk(lod, model, &panm) != 0 || chunk_builder_add_child(out, &panm) != 0) {
        chunk_builder_free(&panm);
        chunk_builder_free(out);
        return -1;
    }

    return 0;
}

static int build_rdta_chunk(const Threedi3di3 *model, SerializeContext *ctx, ChunkBuilder *out)
{
    (void)ctx;
    if (!model || !out) {
        return -1;
    }
    if (model->lod_count == 0 || !model->lods) {
        return -1;
    }
    chunk_builder_init(out, "RDTA", 1);
    for (size_t i = 0; i < model->lod_count; ++i) {
        ChunkBuilder rlod = {0};
        if (build_rlod_chunk(model, &model->lods[i], ctx, &rlod) != 0) {
            chunk_builder_free(&rlod);
            chunk_builder_free(out);
            return -1;
        }
        if (chunk_builder_add_child(out, &rlod) != 0) {
            chunk_builder_free(&rlod);
            chunk_builder_free(out);
            return -1;
        }
    }
    return 0;
}

static int build_cdta_chunk(const Threedi3di3 *model, ChunkBuilder *out)
{
    if (!model || !out) {
        return -1;
    }
    const ThreediCollisionModel zero = {0};
    const ThreediCollisionModel *collision = model->collision ? model->collision : &zero;
    chunk_builder_init(out, "CDTA", 1);

    ChunkBuilder cmdl = {0};
    if (build_cmdl_chunk(&collision->model_data, &cmdl) != 0 || chunk_builder_add_child(out, &cmdl) != 0) {
        chunk_builder_free(&cmdl);
        chunk_builder_free(out);
        return -1;
    }

    ChunkBuilder cvrt = {0};
    if (build_cvrt_chunk(collision, &cvrt) != 0 || chunk_builder_add_child(out, &cvrt) != 0) {
        chunk_builder_free(&cvrt);
        chunk_builder_free(out);
        return -1;
    }

    ChunkBuilder cnrm = {0};
    if (build_cnrm_chunk(collision, &cnrm) != 0 || chunk_builder_add_child(out, &cnrm) != 0) {
        chunk_builder_free(&cnrm);
        chunk_builder_free(out);
        return -1;
    }

    ChunkBuilder cfac = {0};
    if (build_cfac_chunk(collision, &cfac) != 0 || chunk_builder_add_child(out, &cfac) != 0) {
        chunk_builder_free(&cfac);
        chunk_builder_free(out);
        return -1;
    }

    ChunkBuilder bpln = {0};
    if (build_bpln_chunk(collision, &bpln) != 0 || chunk_builder_add_child(out, &bpln) != 0) {
        chunk_builder_free(&bpln);
        chunk_builder_free(out);
        return -1;
    }

    ChunkBuilder bvol = {0};
    if (build_bvol_chunk(collision, &bvol) != 0 || chunk_builder_add_child(out, &bvol) != 0) {
        chunk_builder_free(&bvol);
        chunk_builder_free(out);
        return -1;
    }

    ChunkBuilder cobj = {0};
    if (build_cobj_chunk(collision, &cobj) != 0 || chunk_builder_add_child(out, &cobj) != 0) {
        chunk_builder_free(&cobj);
        chunk_builder_free(out);
        return -1;
    }

    ChunkBuilder cxlt = {0};
    if (build_cxlt_chunk(collision, &cxlt) != 0 || chunk_builder_add_child(out, &cxlt) != 0) {
        chunk_builder_free(&cxlt);
        chunk_builder_free(out);
        return -1;
    }

    return 0;
}

static int build_occl_chunk(const Threedi3di3 *model, ChunkBuilder *out)
{
    if (!model || !out) {
        return -1;
    }
    chunk_builder_init(out, "OCCL", 1);

    ChunkBuilder ovrt = {0};
    if (build_ovrt_chunk(model, &ovrt) != 0 || chunk_builder_add_child(out, &ovrt) != 0) {
        chunk_builder_free(&ovrt);
        chunk_builder_free(out);
        return -1;
    }

    ChunkBuilder opln = {0};
    if (build_opln_chunk(model, &opln) != 0 || chunk_builder_add_child(out, &opln) != 0) {
        chunk_builder_free(&opln);
        chunk_builder_free(out);
        return -1;
    }

    ChunkBuilder ofac = {0};
    if (build_ofac_chunk(model, &ofac) != 0 || chunk_builder_add_child(out, &ofac) != 0) {
        chunk_builder_free(&ofac);
        chunk_builder_free(out);
        return -1;
    }

    ChunkBuilder oobj = {0};
    if (build_oobj_chunk(model, &oobj) != 0 || chunk_builder_add_child(out, &oobj) != 0) {
        chunk_builder_free(&oobj);
        chunk_builder_free(out);
        return -1;
    }

    return 0;
}

static int threedi_3di3_serialize(const Threedi3di3 *model, BufferBuilder *out)
{
    if (!model || !out) {
        return -1;
    }
    uint32_t version = model->version != 0 ? model->version : THREEDI_DEFAULT_VERSION;
    BufferBuilder buf = {0};
    if (buffer_append(&buf, "3DI3", 4) != 0 ||
        buffer_append_u32_le(&buf, version) != 0) {
        buffer_builder_free(&buf);
        return -1;
    }

    SerializeContext ctx = {0};
    ctx.model = model;
    ChunkBuilder root = {0};
    chunk_builder_init(&root, "ROOT", 1);

    ChunkBuilder info = {0};
    if (build_info_chunk(&model->info, &info) != 0 || chunk_builder_add_child(&root, &info) != 0) {
        chunk_builder_free(&info);
        chunk_builder_free(&root);
        buffer_builder_free(&buf);
        return -1;
    }

    ChunkBuilder ghdr = {0};
    if (build_ghdr_chunk(&model->header, &ghdr) != 0 || chunk_builder_add_child(&root, &ghdr) != 0) {
        chunk_builder_free(&ghdr);
        chunk_builder_free(&root);
        buffer_builder_free(&buf);
        return -1;
    }

    ChunkBuilder usrp = {0};
    if (build_usrp_chunk(model, &usrp) != 0 || chunk_builder_add_child(&root, &usrp) != 0) {
        chunk_builder_free(&usrp);
        chunk_builder_free(&root);
        buffer_builder_free(&buf);
        return -1;
    }

    ChunkBuilder ctrl = {0};
    if (build_ctrl_chunk(&model->ctrl, &ctrl) != 0 || chunk_builder_add_child(&root, &ctrl) != 0) {
        chunk_builder_free(&ctrl);
        chunk_builder_free(&root);
        buffer_builder_free(&buf);
        return -1;
    }

    ChunkBuilder mtrl = {0};
    if (build_mtrl_chunk(model, &mtrl) != 0 || chunk_builder_add_child(&root, &mtrl) != 0) {
        chunk_builder_free(&mtrl);
        chunk_builder_free(&root);
        buffer_builder_free(&buf);
        return -1;
    }

    ChunkBuilder cdta = {0};
    if (build_cdta_chunk(model, &cdta) != 0 || chunk_builder_add_child(&root, &cdta) != 0) {
        chunk_builder_free(&cdta);
        chunk_builder_free(&root);
        buffer_builder_free(&buf);
        return -1;
    }

    ChunkBuilder rdta = {0};
    if (build_rdta_chunk(model, &ctx, &rdta) != 0 || chunk_builder_add_child(&root, &rdta) != 0) {
        chunk_builder_free(&rdta);
        chunk_builder_free(&root);
        buffer_builder_free(&buf);
        return -1;
    }

    ChunkBuilder occl = {0};
    if (build_occl_chunk(model, &occl) != 0 || chunk_builder_add_child(&root, &occl) != 0) {
        chunk_builder_free(&occl);
        chunk_builder_free(&root);
        buffer_builder_free(&buf);
        return -1;
    }

    ChunkBuilder lght = {0};
    if (build_lght_chunk(model, &lght) != 0 || chunk_builder_add_child(&root, &lght) != 0) {
        chunk_builder_free(&lght);
        chunk_builder_free(&root);
        buffer_builder_free(&buf);
        return -1;
    }

    ChunkBuilder mtrx = {0};
    if (build_mtrx_chunk(&model->mtrx, &mtrx) != 0 || chunk_builder_add_child(&root, &mtrx) != 0) {
        chunk_builder_free(&mtrx);
        chunk_builder_free(&root);
        buffer_builder_free(&buf);
        return -1;
    }

    if (chunk_builder_serialize(&root, &buf) != 0) {
        chunk_builder_free(&root);
        buffer_builder_free(&buf);
        return -1;
    }
    chunk_builder_free(&root);
    *out = buf;
    return 0;
}

int threedi_3di3_write(const char *path, const Threedi3di3 *model)
{
    if (!path || !model) {
        return -1;
    }
    BufferBuilder buf = {0};
    if (threedi_3di3_serialize(model, &buf) != 0) {
        return -1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        buffer_builder_free(&buf);
        return -1;
    }
    size_t written = fwrite(buf.data, 1, buf.len, f);
    fclose(f);
    int rc = written == buf.len ? 0 : -1;
    buffer_builder_free(&buf);
    return rc;
}
