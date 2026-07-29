#include "threedi/threedi_3di3.h"

// Split out of threedi_3di3.cpp (quality campaign W3-3). Motion only — every body
// is unchanged. (The original file carried no original-code citations; libs/threedi
// is built from the format records in docs/threedi/, not from decompiled functions.)
//
// The read half: the chunk walker and one parser per 3DI3 chunk id, ending at
// threedi_3di3_parse/read/read_memory. Everything here is static except those
// entry points; the writer is threedi_3di3_write.cpp and shares nothing with it.

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <io/fixed.h>

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
        objs[i].num_normals = read_s32_le(base + 12);
        objs[i].num_bounding_volumes = read_s32_le(base + 16);
        objs[i].parent_subobject_index = read_s32_le(base + 20);
        objs[i].unk3 = read_s32_le(base + 24);
        objs[i].unk4 = read_s32_le(base + 28);
        objs[i].unk5 = read_s32_le(base + 32);
        assert(objs[i].unk3 == 0 && objs[i].unk4 == 0 && objs[i].unk5 == 0);
        objs[i].offset[0] = read_s32_le(base + 36);
        objs[i].offset[1] = read_s32_le(base + 40);
        objs[i].offset[2] = read_s32_le(base + 44);
        objs[i].min[0] = read_s32_le(base + 48);
        objs[i].min[1] = read_s32_le(base + 52);
        objs[i].min[2] = read_s32_le(base + 56);
        objs[i].max[0] = read_s32_le(base + 60);
        objs[i].max[1] = read_s32_le(base + 64);
        objs[i].max[2] = read_s32_le(base + 68);
        objs[i].med[0] = read_s32_le(base + 72);
        objs[i].med[1] = read_s32_le(base + 76);
        objs[i].med[2] = read_s32_le(base + 80);
        objs[i].radius = read_s32_le(base + 84);
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
        trans[i].translation[0] = read_s32_le(base + 0);
        trans[i].translation[1] = read_s32_le(base + 4);
        trans[i].translation[2] = read_s32_le(base + 8);
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
        objs[i].glow_scale = read_f32_le(base + 20);
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
