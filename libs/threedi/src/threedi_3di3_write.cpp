#include "threedi/threedi_3di3.h"

// Split out of threedi_3di3.cpp (quality campaign W3-3). Motion only — every body
// is unchanged. (The original file carried no original-code citations; libs/threedi
// is built from the format records in docs/threedi/, not from decompiled functions.)
//
// The write half: the buffer/chunk builders and one emitter per 3DI3 chunk id,
// ending at threedi_3di3_serialize/write. A parity writer — it builds output from
// scratch, never by passing input bytes through (ADR 0003).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREEDI_DEFAULT_VERSION 259u

typedef struct ChunkBuilder ChunkBuilder;

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
    if (content_len > THREEDI_3DI3_LENGTH_MASK) {
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
    uint32_t flags = content_len & THREEDI_3DI3_LENGTH_MASK;
    if (chunk->child_count > 0 || chunk->is_parent) {
        flags |= THREEDI_3DI3_PARENT_FLAG;
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
            buffer_append_f32_le(&out->payload, o->glow_scale) != 0 ||
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
            buffer_append_s32_le(&out->payload, o->num_normals) != 0 ||
            buffer_append_s32_le(&out->payload, o->num_bounding_volumes) != 0 ||
            buffer_append_s32_le(&out->payload, o->parent_subobject_index) != 0 ||
            buffer_append_s32_le(&out->payload, o->unk3) != 0 ||
            buffer_append_s32_le(&out->payload, o->unk4) != 0 ||
            buffer_append_s32_le(&out->payload, o->unk5) != 0 ||
            buffer_append_s32_le(&out->payload, o->offset[0]) != 0 ||
            buffer_append_s32_le(&out->payload, o->offset[1]) != 0 ||
            buffer_append_s32_le(&out->payload, o->offset[2]) != 0 ||
            buffer_append_s32_le(&out->payload, o->min[0]) != 0 ||
            buffer_append_s32_le(&out->payload, o->min[1]) != 0 ||
            buffer_append_s32_le(&out->payload, o->min[2]) != 0 ||
            buffer_append_s32_le(&out->payload, o->max[0]) != 0 ||
            buffer_append_s32_le(&out->payload, o->max[1]) != 0 ||
            buffer_append_s32_le(&out->payload, o->max[2]) != 0 ||
            buffer_append_s32_le(&out->payload, o->med[0]) != 0 ||
            buffer_append_s32_le(&out->payload, o->med[1]) != 0 ||
            buffer_append_s32_le(&out->payload, o->med[2]) != 0 ||
            buffer_append_s32_le(&out->payload, o->radius) != 0) {
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
        if (buffer_append_s32_le(&out->payload, t->translation[0]) != 0 ||
            buffer_append_s32_le(&out->payload, t->translation[1]) != 0 ||
            buffer_append_s32_le(&out->payload, t->translation[2]) != 0) {
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
