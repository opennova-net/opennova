#include "threedi/threedi_3di3.h"
#include "threedi/threedi_gp.h"
#include "threedi/threedi_material_class.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREEDI_AUTO_VERSION 259u

static void copy_padded(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    memset(dst, 0, dst_size);
    if (!src) {
        return;
    }
    size_t len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
}

static float read_f32_unaligned(const uint8_t *p)
{
    float v = 0.0f;
    memcpy(&v, p, sizeof(v));
    return v;
}

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static int triangle_opposes_authored_normals(const ThreediGpRVert *rverts,
                                             size_t rvert_count,
                                             int32_t first_vertex,
                                             uint16_t a,
                                             uint16_t b,
                                             uint16_t c)
{
    int64_t ia = (int64_t)first_vertex + (int64_t)a;
    int64_t ib = (int64_t)first_vertex + (int64_t)b;
    int64_t ic = (int64_t)first_vertex + (int64_t)c;
    if (ia < 0 || ib < 0 || ic < 0 ||
        (size_t)ia >= rvert_count || (size_t)ib >= rvert_count || (size_t)ic >= rvert_count) {
        return 0;
    }

    const ThreediGpRVert *va = &rverts[ia];
    const ThreediGpRVert *vb = &rverts[ib];
    const ThreediGpRVert *vc = &rverts[ic];
    if (!va->has_normal && !vb->has_normal && !vc->has_normal) {
        return 0;
    }

    float e1[3] = {
        vb->position[0] - va->position[0],
        vb->position[1] - va->position[1],
        vb->position[2] - va->position[2],
    };
    float e2[3] = {
        vc->position[0] - va->position[0],
        vc->position[1] - va->position[1],
        vc->position[2] - va->position[2],
    };
    float geom[3] = {
        e1[1] * e2[2] - e1[2] * e2[1],
        e1[2] * e2[0] - e1[0] * e2[2],
        e1[0] * e2[1] - e1[1] * e2[0],
    };
    float normal[3] = {
        va->normal[0] + vb->normal[0] + vc->normal[0],
        va->normal[1] + vb->normal[1] + vc->normal[1],
        va->normal[2] + vb->normal[2] + vc->normal[2],
    };

    float geom_len2 = dot3(geom, geom);
    float normal_len2 = dot3(normal, normal);
    if (geom_len2 <= 1.0e-12f || normal_len2 <= 1.0e-8f) {
        return 0;
    }
    float d = dot3(geom, normal);
    return d < 0.0f && (d * d) > (geom_len2 * normal_len2 * 1.0e-4f);
}

static void emit_triangle(const ThreediGpRVert *rverts,
                          size_t rvert_count,
                          int32_t first_vertex,
                          uint16_t a,
                          uint16_t b,
                          uint16_t c,
                          uint16_t *out,
                          size_t *out_count)
{
    if (triangle_opposes_authored_normals(rverts, rvert_count, first_vertex, a, b, c)) {
        uint16_t tmp = b;
        b = c;
        c = tmp;
    }
    out[(*out_count)++] = a;
    out[(*out_count)++] = b;
    out[(*out_count)++] = c;
}

static size_t triangulated_index_capacity(const ThreediGpVariablePoly *poly)
{
    if (!poly) {
        return 0;
    }
    if (poly->topology == 0) {
        return (poly->index_count / 3u) * 3u;
    }
    return poly->index_count >= 3u ? (poly->index_count - 2u) * 3u : 0u;
}

static size_t triangulate_poly(const ThreediGpFile *gp,
                               const ThreediGpVariablePoly *poly,
                               uint16_t *out)
{
    size_t out_count = 0;
    if (!gp || !poly || !out || !poly->indices) {
        return 0;
    }
    if (poly->topology == 0) {
        size_t tri_count = poly->index_count / 3u;
        for (size_t i = 0; i < tri_count; ++i) {
            emit_triangle(gp->rverts, gp->rvert_count, poly->first_vertex,
                          poly->indices[i * 3u + 0u],
                          poly->indices[i * 3u + 1u],
                          poly->indices[i * 3u + 2u],
                          out, &out_count);
        }
        return out_count;
    }

    int flip = 0;
    for (size_t i = 0; i + 2u < poly->index_count; ++i) {
        uint16_t a = poly->indices[i + 0u];
        uint16_t b = poly->indices[i + 1u];
        uint16_t c = poly->indices[i + 2u];
        if (a == b || b == c || a == c) {
            flip = 0;
            continue;
        }
        if (flip) {
            emit_triangle(gp->rverts, gp->rvert_count, poly->first_vertex, a, c, b, out, &out_count);
        } else {
            emit_triangle(gp->rverts, gp->rvert_count, poly->first_vertex, a, b, c, out, &out_count);
        }
        flip = !flip;
    }
    return out_count;
}

static int normalized_part_index(const ThreediGpRModel *rm, int32_t part_index)
{
    if (!rm || rm->subobject_count == 0) {
        return 0;
    }
    if (part_index < 0 || (size_t)part_index >= rm->subobject_count) {
        return 0;
    }
    return (int)part_index;
}

static void decode_phase_reg(uint8_t style, uint8_t param, float *phase, int32_t *reg)
{
    if (style <= 112u) {
        *phase = (float)param / 256.0f;
        *reg = -1;
    } else {
        *phase = 0.0f;
        *reg = (int32_t)param;
    }
}

static void convert_gp_uv_params(const ThreediGpMaterialTransform *src, ThreediUvParams *dst)
{
    dst->style = src->style;
    decode_phase_reg(src->style, src->param, &dst->phase, &dst->reg);
    dst->gen_rate = (float)src->rate / 256.0f;
    dst->start = (float)src->start / 256.0f;
    dst->end = (float)src->end / 256.0f;
}

static void convert_gp_alpha_gen(const ThreediGpMaterialTransform *src, ThreediAlphaGen *dst)
{
    dst->style = src->style;
    decode_phase_reg(src->style, src->param, &dst->phase, &dst->reg);
    dst->rate = (float)src->rate / 256.0f;
    dst->start = src->start;
    dst->end = src->end;
}

static void convert_gp_rgb_gen(const ThreediGpMaterialTransform *src,
                               const uint32_t *color_rgb,
                               ThreediRgbGen *dst)
{
    dst->style = src->style;
    decode_phase_reg(src->style, src->param, &dst->phase, &dst->reg);
    dst->rate = (float)src->rate / 256.0f;
    dst->start_color[0] = (float)(color_rgb[0] & 0xFFu) / 255.0f;
    dst->start_color[1] = (float)((color_rgb[0] >> 8) & 0xFFu) / 255.0f;
    dst->start_color[2] = (float)((color_rgb[0] >> 16) & 0xFFu) / 255.0f;
    dst->start_color[3] = 1.0f;
    dst->end_color[0] = (float)(color_rgb[1] & 0xFFu) / 255.0f;
    dst->end_color[1] = (float)((color_rgb[1] >> 8) & 0xFFu) / 255.0f;
    dst->end_color[2] = (float)((color_rgb[1] >> 16) & 0xFFu) / 255.0f;
    dst->end_color[3] = 1.0f;
}

static uint8_t texture_type_for_name(const char *name, uint8_t slot)
{
    if (slot == THREEDI_TEX_SLOT_NORMAL || slot == THREEDI_TEX_SLOT_NORMAL_B) {
        size_t len = name ? strlen(name) : 0u;
        if (len >= 4u && (strcmp(name + len - 4u, ".tga") == 0 || strcmp(name + len - 4u, ".TGA") == 0)) {
            return THREEDI_TEX_TYPE_NORMAL_TGA;
        }
        return THREEDI_TEX_TYPE_NORMAL_MDT;
    }
    return THREEDI_TEX_TYPE_DIFFUSE;
}

static int append_texture(ThreediMaterial *dst, const char *name, uint8_t slot, uint8_t flags)
{
    if (!dst || !name || name[0] == '\0') {
        return 0;
    }
    const uint32_t max_textures = (uint32_t)(sizeof(dst->textures) / sizeof(dst->textures[0]));
    if (dst->texture_count >= max_textures) {
        return -1;
    }
    ThreediMaterialTexture *tex = &dst->textures[dst->texture_count++];
    copy_padded(tex->name, sizeof(tex->name), name);
    tex->slot = slot;
    tex->type = texture_type_for_name(tex->name, slot);
    tex->flags = flags;
    tex->frame = 0;
    return 0;
}

static int synthesize_mdt_name(const char *diffuse, char out[17])
{
    if (!diffuse || diffuse[0] == '\0') {
        return -1;
    }
    memset(out, 0, 17);
    size_t len = strlen(diffuse);
    if (len > 12u) {
        len = 12u;
    }
    memcpy(out, diffuse, len);
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = '\0';
    }
    size_t base_len = strlen(out);
    if (base_len + 4u >= 17u) {
        base_len = 12u;
        out[base_len] = '\0';
    }
    memcpy(out + strlen(out), ".mdt", 5);
    return 0;
}

static int blend_mode_from_gp_flags(uint32_t shader_flags)
{
    if (shader_flags & 0x200u) {
        return THREEDI_CLASS_BLEND_ALPHA;
    }
    if (shader_flags & 0x80000000u) {
        return THREEDI_CLASS_BLEND_ADDITIVE;
    }
    return THREEDI_CLASS_BLEND_OPAQUE;
}

static int convert_materials(const ThreediGpFile *gp, Threedi3di3 *model, size_t *material_offsets)
{
    size_t total = 0;
    for (size_t li = 0; li < gp->rmodel_count; ++li) {
        material_offsets[li] = total;
        total += gp->rmodels[li].material_count;
    }
    model->material_count = (uint32_t)total;
    model->material_record_size = 584u;
    if (total == 0) {
        return 0;
    }
    model->materials = (ThreediMaterial *)calloc(total, sizeof(ThreediMaterial));
    if (!model->materials) {
        return -1;
    }

    size_t out_idx = 0;
    for (size_t li = 0; li < gp->rmodel_count; ++li) {
        const ThreediGpRModel *rm = &gp->rmodels[li];
        for (size_t mi = 0; mi < rm->material_count; ++mi, ++out_idx) {
            const ThreediGpMaterial *sm = &rm->materials[mi];
            ThreediMaterial *dm = &model->materials[out_idx];
            dm->index = (int32_t)out_idx;

            uint32_t lookup_idx = sm->render_lookup & 0xFFu;
            uint8_t tex_flags = (sm->tex_addressing_mode & 0x0101u) ? THREEDI_TEX_FLAG_CLAMPED : 0u;
            if (append_texture(dm, sm->texture_name, THREEDI_TEX_SLOT_DIFFUSE, tex_flags) != 0) {
                return -1;
            }
            if (lookup_idx < gp->material_lookup_count) {
                const ThreediGpMaterialLookup *primary = &gp->material_lookups[lookup_idx];
                for (size_t lj = 0; lj < gp->material_lookup_count; ++lj) {
                    const ThreediGpMaterialLookup *entry = &gp->material_lookups[lj];
                    if (lj == lookup_idx || entry->texture_name[0] == '\0') {
                        continue;
                    }
                    if (entry->slot_type == 0x04u && entry->seq_index == (uint8_t)(primary->seq_index + 1u)) {
                        if (append_texture(dm, entry->texture_name, THREEDI_TEX_SLOT_DETAIL, tex_flags) != 0) {
                            return -1;
                        }
                        break;
                    }
                }
            }

            uint32_t shader_type = sm->shader_flags & 0xFFu;
            if (shader_type >= 8u && shader_type <= 12u && dm->texture_count > 0) {
                char normal_name[17];
                if (synthesize_mdt_name(dm->textures[0].name, normal_name) == 0) {
                    if (append_texture(dm, normal_name, THREEDI_TEX_SLOT_NORMAL, 0u) != 0) {
                        return -1;
                    }
                }
            }

            int blend_mode = blend_mode_from_gp_flags(sm->shader_flags);
            uint32_t rattrib_3da = sm->render_attributes;
            if (rattrib_3da & 0x2u) {
                rattrib_3da = (rattrib_3da & ~0x2u) | 0x1u;
            }

            ThreediMaterialClass cls;
            classify_from_bhd_shader_type(shader_type,
                                          rattrib_3da,
                                          (uint8_t)sm->use_alpha_pcx,
                                          blend_mode != THREEDI_CLASS_BLEND_OPAQUE ? 1u : 0u,
                                          blend_mode == THREEDI_CLASS_BLEND_ALPHA ? 2u : 0u,
                                          &cls);
            if (dm->texture_count >= 2u) {
                cls.has_detail = 1u;
            }
            if (sm->emissive_color != 0) {
                cls.luminance = 1u;
            }
            if (gp->header.mesh_type == THREEDI_GP_MESH_SKINNED) {
                cls.is_skinned = 1u;
            }

            uint8_t is_glass = 0u;
            if (sm->reflect_type != 0 ||
                (sm->texture_name[0] == '\0' && (sm->render_lookup & 0xFFFFFFu) == 0xFFFFFFu)) {
                is_glass = 1u;
                cls.is_glass = 1u;
            }

            synthesize_jo_shader_tag(&cls, dm->shader_name, sizeof(dm->shader_name));
            if (dm->shader_name[0] == '\0') {
                copy_padded(dm->shader_name, sizeof(dm->shader_name), dm->texture_count >= 2u ? "FF_MT_OP" : "FF_ST_OP");
            }

            dm->material_flags = 0u;
            if (cls.two_sided) {
                dm->material_flags |= THREEDI_MATERIAL_FLAG_TWO_SIDED;
            }
            if (cls.alpha_test || blend_mode == THREEDI_CLASS_BLEND_ALPHA) {
                dm->material_flags |= THREEDI_MATERIAL_FLAG_ALPHA_TEST;
                dm->alpha_test_value_byte = 128u;
            }
            if (cls.alpha_test_invert) {
                dm->material_flags |= THREEDI_MATERIAL_FLAG_ALPHA_INVERT;
            }

            convert_gp_alpha_gen(&sm->alphagen, &dm->alpha_gen);
            convert_gp_rgb_gen(&sm->rgbgen, sm->color_rgb, &dm->rgb_gen);
            convert_gp_uv_params(&sm->mapfunc_u, &dm->u_params);
            convert_gp_uv_params(&sm->mapfunc_v, &dm->v_params);

            dm->reflect_color[0] = sm->reflect_r;
            dm->reflect_color[1] = sm->reflect_g;
            dm->reflect_color[2] = sm->reflect_b;
            dm->reflect_color[3] = sm->reflect_alpha;
            dm->emissive_type = sm->emissive_color != 0 ? THREEDI_EMISSIVE_FULL : THREEDI_EMISSIVE_NONE;
            dm->is_glass = is_glass;
        }
    }
    return 0;
}

static int material_content_equal(const ThreediMaterial *a, const ThreediMaterial *b)
{
    ThreediMaterial ca = *a;
    ThreediMaterial cb = *b;
    ca.index = 0;
    cb.index = 0;
    return memcmp(&ca, &cb, sizeof(ca)) == 0;
}

static int dedupe_materials(Threedi3di3 *model)
{
    if (!model || model->material_count == 0 || !model->materials) {
        return 0;
    }
    uint32_t n = model->material_count;
    int32_t *remap = (int32_t *)calloc(n, sizeof(int32_t));
    if (!remap) {
        return -1;
    }

    uint32_t unique = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t found = unique;
        for (uint32_t j = 0; j < unique; ++j) {
            if (material_content_equal(&model->materials[i], &model->materials[j])) {
                found = j;
                break;
            }
        }
        if (found < unique) {
            remap[i] = (int32_t)found;
        } else {
            if (unique != i) {
                model->materials[unique] = model->materials[i];
            }
            model->materials[unique].index = (int32_t)unique;
            remap[i] = (int32_t)unique;
            unique++;
        }
    }

    for (size_t li = 0; li < model->lod_count; ++li) {
        ThreediLod *lod = &model->lods[li];
        for (size_t si = 0; si < lod->strip_count; ++si) {
            int32_t old_idx = lod->strips[si].material_index;
            if (old_idx >= 0 && (uint32_t)old_idx < n) {
                lod->strips[si].material_index = remap[old_idx];
            }
        }
    }

    if (unique < n) {
        memset(&model->materials[unique], 0, (size_t)(n - unique) * sizeof(ThreediMaterial));
        model->material_count = unique;
    }
    free(remap);
    return 0;
}

static int convert_vertices(const ThreediGpFile *gp, ThreediVertexBuffer *out)
{
    uint32_t count = (uint32_t)gp->rvert_count;
    int is_skinned = gp->header.mesh_type == THREEDI_GP_MESH_SKINNED;
    int has_tangents = gp->header.mesh_type == THREEDI_GP_MESH_STATIC || gp->vstream != NULL;
    uint32_t flags = 0u;
    uint32_t stride = 40u;
    if (is_skinned) {
        flags |= THREEDI_VERTEX_FLAG_SKINNED;
        stride += 16u;
    }
    if (has_tangents) {
        flags |= THREEDI_VERTEX_FLAG_TANGENTS;
        stride += 24u;
    }

    out->count = count;
    out->stride = stride;
    out->flags = flags;
    if (count == 0) {
        return 0;
    }
    out->items = (ThreediVertex *)calloc(count, sizeof(ThreediVertex));
    if (!out->items) {
        return -1;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const ThreediGpRVert *sv = &gp->rverts[i];
        ThreediVertex *dv = &out->items[i];
        dv->flags = flags;
        dv->has_tangents = has_tangents;
        dv->is_skinned = is_skinned;
        memcpy(dv->position, sv->position, sizeof(dv->position));
        memcpy(dv->normal, sv->normal, sizeof(dv->normal));
        memcpy(dv->uv0, sv->uv0, sizeof(dv->uv0));
        memcpy(dv->uv1, sv->uv1, sizeof(dv->uv1));
        if (is_skinned) {
            float sum = sv->bone_weights[0] + sv->bone_weights[1] + sv->bone_weights[2];
            if (sum > 0.0f) {
                dv->bone_weights[0] = sv->bone_weights[0] / sum;
                dv->bone_weights[1] = sv->bone_weights[1] / sum;
                dv->bone_weights[2] = sv->bone_weights[2] / sum;
            } else {
                dv->bone_weights[0] = 1.0f;
            }
            memcpy(dv->bone_indices, sv->bone_indices, sizeof(dv->bone_indices));
        }
        if (has_tangents) {
            memcpy(dv->tangent, sv->tangent, sizeof(dv->tangent));
            if (gp->vstream && gp->vstream->data && gp->vstream->data_len >= (size_t)(i + 1u) * 24u) {
                const uint8_t *base = gp->vstream->data + (size_t)i * 24u;
                dv->tangent[0] = read_f32_unaligned(base + 0);
                dv->tangent[1] = read_f32_unaligned(base + 4);
                dv->tangent[2] = read_f32_unaligned(base + 8);
                dv->bitangent[0] = read_f32_unaligned(base + 12);
                dv->bitangent[1] = read_f32_unaligned(base + 16);
                dv->bitangent[2] = read_f32_unaligned(base + 20);
            }
        }
    }
    return 0;
}

static void fill_render_objects(const ThreediGpRModel *rm, ThreediLod *lod, const int32_t *part_counts)
{
    size_t part_count = rm->subobject_count > 0 ? rm->subobject_count : 1u;
    lod->render_object_count = part_count;
    lod->rmdl_render_object_count = (int32_t)part_count;
    for (size_t i = 0; i < part_count; ++i) {
        ThreediRenderObject *dst = &lod->render_objects[i];
        dst->num_strips = part_counts ? part_counts[i] : 0;
        dst->num_alpha_strips = 0;
        dst->parent_index = -1;
        if (i < rm->subobject_count) {
            const ThreediGpSubObject *src = &rm->subobjects[i];
            dst->parent_index = src->parent;
            memcpy(dst->abs, src->abs, sizeof(dst->abs));
            if (src->parent >= 0 && (size_t)src->parent < rm->subobject_count) {
                const ThreediGpSubObject *parent = &rm->subobjects[src->parent];
                dst->rel[0] = src->abs[0] - parent->abs[0];
                dst->rel[1] = src->abs[1] - parent->abs[1];
                dst->rel[2] = src->abs[2] - parent->abs[2];
            } else {
                memcpy(dst->rel, src->abs, sizeof(dst->rel));
            }
            dst->bounding_center[0] = (src->bounding_min[0] + src->bounding_max[0]) * 0.5f;
            dst->bounding_center[1] = (src->bounding_min[1] + src->bounding_max[1]) * 0.5f;
            dst->bounding_center[2] = (src->bounding_min[2] + src->bounding_max[2]) * 0.5f;
            dst->bounding_radius = (float)src->bounding_radius / 65536.0f;
        }
    }
}

static int append_lod_strip(const ThreediGpFile *gp,
                            const ThreediGpVariablePoly *poly,
                            size_t material_offset,
                            ThreediLod *lod,
                            size_t *strip_cursor,
                            size_t *index_cursor)
{
    ThreediTriangleStrip *strip = &lod->strips[*strip_cursor];
    strip->material_index = (int32_t)((int64_t)poly->material_index + (int64_t)material_offset);
    strip->index_offset = (int32_t)*index_cursor;
    strip->is_strip = 0;
    strip->start_vertex = poly->first_vertex;
    memcpy(strip->bone_table, poly->bone_table, sizeof(strip->bone_table));
    strip->bone_table_length = poly->bone_table_length;

    size_t written = triangulate_poly(gp, poly, lod->indices.indices + *index_cursor);
    strip->num_indices = (uint16_t)written;
    strip->num_triangles = (uint16_t)(written / 3u);

    uint32_t max_vert = 0;
    for (size_t i = 0; i < written; ++i) {
        uint32_t v = lod->indices.indices[*index_cursor + i];
        if (v > max_vert) {
            max_vert = v;
        }
    }
    strip->num_vertices = written > 0 ? (int32_t)(max_vert + 1u) : poly->max_vertex_index + 1;
    *index_cursor += written;
    *strip_cursor += 1u;
    return 0;
}

static int convert_lod(const ThreediGpFile *gp, size_t lod_index, size_t material_offset, ThreediLod *lod)
{
    const ThreediGpRModel *rm = &gp->rmodels[lod_index];
    memset(lod, 0, sizeof(*lod));
    copy_padded(lod->model_type, sizeof(lod->model_type), gp->header.model_tag[0] ? gp->header.model_tag : "MESH");
    lod->lod_threshold = lod_index < 3u ? (int32_t)(gp->header.lod_thresholds[lod_index] >> 16u) : 0;

    if (convert_vertices(gp, &lod->vertices) != 0) {
        return -1;
    }

    size_t total_indices = 0;
    for (size_t p = 0; p < rm->poly_count; ++p) {
        total_indices += triangulated_index_capacity(&rm->polys[p]);
    }
    lod->indices.count = (uint32_t)total_indices;
    if (total_indices > 0) {
        lod->indices.indices = (uint16_t *)calloc(total_indices, sizeof(uint16_t));
        if (!lod->indices.indices) {
            return -1;
        }
    }

    lod->strip_count = rm->poly_count;
    lod->strip_record_size = gp->header.mesh_type == THREEDI_GP_MESH_SKINNED ? 68u : 48u;
    if (lod->strip_count > 0) {
        lod->strips = (ThreediTriangleStrip *)calloc(lod->strip_count, sizeof(ThreediTriangleStrip));
        if (!lod->strips) {
            return -1;
        }
    }

    size_t part_count = rm->subobject_count > 0 ? rm->subobject_count : 1u;
    int32_t *part_counts = (int32_t *)calloc(part_count, sizeof(int32_t));
    if (!part_counts) {
        return -1;
    }
    for (size_t p = 0; p < rm->poly_count; ++p) {
        part_counts[normalized_part_index(rm, rm->polys[p].subobject_index)]++;
    }
    lod->render_objects = (ThreediRenderObject *)calloc(part_count, sizeof(ThreediRenderObject));
    if (!lod->render_objects) {
        free(part_counts);
        return -1;
    }
    fill_render_objects(rm, lod, part_counts);

    size_t strip_cursor = 0;
    size_t index_cursor = 0;
    for (size_t part = 0; part < part_count; ++part) {
        for (size_t p = 0; p < rm->poly_count; ++p) {
            if ((size_t)normalized_part_index(rm, rm->polys[p].subobject_index) != part) {
                continue;
            }
            if (append_lod_strip(gp, &rm->polys[p], material_offset, lod, &strip_cursor, &index_cursor) != 0) {
                free(part_counts);
                return -1;
            }
        }
    }
    free(part_counts);
    lod->strip_count = strip_cursor;
    lod->indices.count = (uint32_t)index_cursor;

    if (rm->part_animation_count > 0) {
        lod->part_animation_count = rm->part_animation_count;
        lod->part_animation_record_size = 68u;
        lod->part_animations = (ThreediPartAnimation *)calloc(rm->part_animation_count, sizeof(ThreediPartAnimation));
        if (!lod->part_animations) {
            return -1;
        }
        for (size_t i = 0; i < rm->part_animation_count; ++i) {
            const ThreediGpPartAnimation *src = &rm->part_animations[i];
            ThreediPartAnimation *dst = &lod->part_animations[i];
            dst->flags = src->flags;
            dst->parent_subobject = src->parent_subobject;
            dst->subobject_index = src->subobject_index;
            dst->matrix_index = src->matrix_index;
            dst->matrix_offset = src->matrix_offset;
            dst->bind_matrix_index = src->bind_matrix_index;
#define COPY_XFORM(d, s) do { (d).control = (s).control; (d).control_param = (s).param; (d).rate = (s).rate; (d).start = (s).start; (d).end = (s).end; } while (0)
            COPY_XFORM(dst->rotation_x, src->rot_x);
            COPY_XFORM(dst->rotation_y, src->rot_y);
            COPY_XFORM(dst->rotation_z, src->rot_z);
            COPY_XFORM(dst->scale_x, src->scale_x);
            COPY_XFORM(dst->scale_y, src->scale_y);
            COPY_XFORM(dst->scale_z, src->scale_z);
            COPY_XFORM(dst->translation, src->translate);
#undef COPY_XFORM
        }
    }
    return 0;
}

static int convert_lods(const ThreediGpFile *gp, Threedi3di3 *model, const size_t *material_offsets)
{
    model->lod_count = gp->rmodel_count;
    if (model->lod_count == 0) {
        return 0;
    }
    model->lods = (ThreediLod *)calloc(model->lod_count, sizeof(ThreediLod));
    if (!model->lods) {
        return -1;
    }
    for (size_t i = 0; i < model->lod_count; ++i) {
        if (convert_lod(gp, i, material_offsets[i], &model->lods[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int convert_userpoints(const ThreediGpFile *gp, Threedi3di3 *model)
{
    model->user_point_count = gp->userpoint_count;
    if (model->user_point_count == 0) {
        return 0;
    }
    model->user_points = (ThreediUserPoint *)calloc(model->user_point_count, sizeof(ThreediUserPoint));
    if (!model->user_points) {
        return -1;
    }
    for (size_t i = 0; i < gp->userpoint_count; ++i) {
        const ThreediGpUserPoint *src = &gp->userpoints[i];
        ThreediUserPoint *dst = &model->user_points[i];
        dst->x = src->x;
        dst->y = src->y;
        dst->z = src->z;
        dst->rot_x = src->rot_x;
        dst->rot_y = src->rot_y;
        dst->rot_z = src->rot_z;
        dst->subobject_index = src->parent_subobject;
        dst->userpoint_type = src->type_code;
        copy_padded(dst->name, sizeof(dst->name), src->name);
    }
    return 0;
}

static int convert_ctrl_and_mtrx(const ThreediGpFile *gp, Threedi3di3 *model)
{
    model->ctrl.count = (uint32_t)gp->control_register_count;
    model->ctrl.record_size = gp->control_register_count > 0 ? 24u : 0u;
    if (gp->control_register_count > 0) {
        model->ctrl.registers = (ThreediControlRegister *)calloc(gp->control_register_count, sizeof(ThreediControlRegister));
        if (!model->ctrl.registers) {
            return -1;
        }
        for (size_t i = 0; i < gp->control_register_count; ++i) {
            copy_padded(model->ctrl.registers[i].name, sizeof(model->ctrl.registers[i].name), gp->control_registers[i].name);
        }
    }

    model->mtrx.count = (uint32_t)gp->matrix_count;
    model->mtrx.record_size = gp->matrix_count > 0 ? 64u : 0u;
    if (gp->matrix_count > 0) {
        model->mtrx.matrices = (ThreediMatrix4x4 *)calloc(gp->matrix_count, sizeof(ThreediMatrix4x4));
        if (!model->mtrx.matrices) {
            return -1;
        }
        for (size_t i = 0; i < gp->matrix_count; ++i) {
            memcpy(model->mtrx.matrices[i].m, gp->matrices[i].m, sizeof(model->mtrx.matrices[i].m));
        }
    }
    return 0;
}

static int convert_lights(const ThreediGpFile *gp, Threedi3di3 *model)
{
    model->light_count = gp->light_count;
    if (model->light_count == 0) {
        return 0;
    }
    model->lights = (ThreediLight *)calloc(model->light_count, sizeof(ThreediLight));
    if (!model->lights) {
        return -1;
    }
    for (size_t i = 0; i < gp->light_count; ++i) {
        const ThreediGpLight *src = &gp->lights[i];
        ThreediLight *dst = &model->lights[i];
        memcpy(dst->offset, src->position, sizeof(dst->offset));
        dst->atten_start = src->attenuation_start;
        dst->atten_end = src->attenuation_end;
        dst->style = src->style;
        dst->phase = src->phase;
        dst->rate = src->rate;
        dst->color_start[0] = src->color_start[0];
        dst->color_start[1] = src->color_start[1];
        dst->color_start[2] = src->color_start[2];
        dst->color_end[0] = src->color_end[0];
        dst->color_end[1] = src->color_end[1];
        dst->color_end[2] = src->color_end[2];
        dst->subobj_index = (uint8_t)src->part_index;
    }
    return 0;
}

static int convert_collision(const ThreediGpFile *gp, Threedi3di3 *model)
{
    const ThreediGpCollision *src = gp->collision;
    if (!src) {
        return 0;
    }
    model->collision = (ThreediCollisionModel *)calloc(1, sizeof(ThreediCollisionModel));
    if (!model->collision) {
        return -1;
    }
    ThreediCollisionModel *dst = model->collision;
    memcpy(&dst->model_data.bbox[0], src->min, sizeof(float) * 3u);
    memcpy(&dst->model_data.bbox[3], src->max, sizeof(float) * 3u);
    dst->model_data.radii[0] = src->max[0] - src->min[0];
    dst->model_data.radii[1] = src->max[1] - src->min[1];
    dst->model_data.radii[2] = src->max[2] - src->min[2];
    dst->model_data.num_vertices = src->vertex_count;
    dst->model_data.num_normals = src->normal_count;
    dst->model_data.num_faces = src->face_count;
    dst->model_data.num_objects = src->object_count;
    dst->model_data.num_transforms = src->translation_count;
    dst->model_data.num_bounding_planes = src->plane_count;
    dst->model_data.num_bounding_volumes = src->volume_count;

    dst->vertex_count = (size_t)src->vertex_count;
    if (dst->vertex_count > 0) {
        dst->vertices = (ThreediCollisionVertex *)calloc(dst->vertex_count, sizeof(ThreediCollisionVertex));
        if (!dst->vertices) return -1;
        for (size_t i = 0; i < dst->vertex_count; ++i) {
            dst->vertices[i].position[0] = (float)src->vertices[i].x / 256.0f;
            dst->vertices[i].position[1] = (float)src->vertices[i].y / 256.0f;
            dst->vertices[i].position[2] = (float)src->vertices[i].z / 256.0f;
        }
    }

    dst->normal_count = (size_t)src->normal_count;
    if (dst->normal_count > 0) {
        dst->normals = (ThreediCollisionNormal *)calloc(dst->normal_count, sizeof(ThreediCollisionNormal));
        if (!dst->normals) return -1;
        for (size_t i = 0; i < dst->normal_count; ++i) {
            dst->normals[i].normal[0] = (float)src->normals[i].nx / 16384.0f;
            dst->normals[i].normal[1] = (float)src->normals[i].ny / 16384.0f;
            dst->normals[i].normal[2] = (float)src->normals[i].nz / 16384.0f;
            dst->normals[i].dominate_axis = src->normals[i].dominant_axis;
        }
    }

    dst->face_count = (size_t)src->face_count;
    if (dst->face_count > 0) {
        dst->faces = (ThreediCollisionFace *)calloc(dst->face_count, sizeof(ThreediCollisionFace));
        if (!dst->faces) return -1;
        for (size_t i = 0; i < dst->face_count; ++i) {
            const ThreediGpCollisionFace *sf = &src->faces[i];
            ThreediCollisionFace *df = &dst->faces[i];
            df->vert_index[0] = (int16_t)sf->vertex_indices[0];
            df->vert_index[1] = (int16_t)sf->vertex_indices[1];
            df->vert_index[2] = (int16_t)sf->vertex_indices[2];
            df->normal_index = sf->normal_index;
            df->plane_dist_fp16 = sf->plane_d;
            df->min_x_fp16 = sf->bbox_min_x;
            df->min_y_fp16 = sf->bbox_min_y;
            df->min_z_fp16 = sf->bbox_min_z;
            df->max_x_fp16 = sf->bbox_max_x;
            df->max_y_fp16 = sf->bbox_max_y;
            df->max_z_fp16 = sf->bbox_max_z;
            df->material_flags = (uint32_t)sf->surface_flags;
            df->poly_type = sf->surface_type;
        }
    }

    dst->object_count = (size_t)src->object_count;
    if (dst->object_count > 0) {
        dst->objects = (ThreediCollisionObject *)calloc(dst->object_count, sizeof(ThreediCollisionObject));
        if (!dst->objects) return -1;
        for (size_t i = 0; i < dst->object_count; ++i) {
            const ThreediGpCollisionObject *so = &src->objects[i];
            ThreediCollisionObject *do_ = &dst->objects[i];
            do_->unk0 = so->flags;
            do_->num_vertices = so->vertex_count;
            do_->num_faces = so->face_count;
            do_->num_planes = so->normal_count;
            do_->num_bounding_volumes = so->volume_count;
            do_->parent_subobject_index = so->parent_subobject;
            do_->offset[0] = (float)so->translation[0] / 65536.0f;
            do_->offset[1] = (float)so->translation[1] / 65536.0f;
            do_->offset[2] = (float)so->translation[2] / 65536.0f;
            do_->min[0] = (float)so->bbox_min_x / 65536.0f;
            do_->min[1] = (float)so->bbox_min_y / 65536.0f;
            do_->min[2] = (float)so->bbox_min_z / 65536.0f;
            do_->max[0] = (float)so->bbox_max_x / 65536.0f;
            do_->max[1] = (float)so->bbox_max_y / 65536.0f;
            do_->max[2] = (float)so->bbox_max_z / 65536.0f;
            do_->med[0] = (float)so->center[0] / 65536.0f;
            do_->med[1] = (float)so->center[1] / 65536.0f;
            do_->med[2] = (float)so->center[2] / 65536.0f;
            do_->radius = (float)so->bounding_sphere_radius / 65536.0f;
        }
    }

    dst->translation_count = (size_t)src->translation_count;
    if (dst->translation_count > 0) {
        dst->translations = (ThreediCollisionTranslation *)calloc(dst->translation_count, sizeof(ThreediCollisionTranslation));
        if (!dst->translations) return -1;
        for (size_t i = 0; i < dst->translation_count; ++i) {
            dst->translations[i].translation[0] = (float)src->translations[i].x / 65536.0f;
            dst->translations[i].translation[1] = (float)src->translations[i].y / 65536.0f;
            dst->translations[i].translation[2] = (float)src->translations[i].z / 65536.0f;
        }
    }

    dst->plane_count = (size_t)src->plane_count;
    if (dst->plane_count > 0) {
        dst->planes = (ThreediBoundingPlane *)calloc(dst->plane_count, sizeof(ThreediBoundingPlane));
        if (!dst->planes) return -1;
        for (size_t i = 0; i < dst->plane_count; ++i) {
            dst->planes[i].normal[0] = (float)src->planes[i].a / 65536.0f;
            dst->planes[i].normal[1] = (float)src->planes[i].b / 65536.0f;
            dst->planes[i].normal[2] = (float)src->planes[i].c / 65536.0f;
            dst->planes[i].radius = (float)src->planes[i].d / 65536.0f;
        }
    }

    dst->volume_count = (size_t)src->volume_count;
    if (dst->volume_count > 0) {
        dst->volumes = (ThreediBoundingVolume *)calloc(dst->volume_count, sizeof(ThreediBoundingVolume));
        if (!dst->volumes) return -1;
        for (size_t i = 0; i < dst->volume_count; ++i) {
            const ThreediGpCollisionVolume *sv = &src->volumes[i];
            ThreediBoundingVolume *dv = &dst->volumes[i];
            dv->collidable_type = sv->type;
            dv->flags = sv->flags;
            dv->min_x_fp16 = sv->bbox_min_x;
            dv->min_y_fp16 = sv->bbox_min_y;
            dv->min_z_fp16 = sv->bbox_min_z;
            dv->max_x_fp16 = sv->bbox_max_x;
            dv->max_y_fp16 = sv->bbox_max_y;
            dv->max_z_fp16 = sv->bbox_max_z;
            dv->plane_count = sv->plane_count;
        }
    }
    return 0;
}

static int convert_occlusion(const ThreediGpFile *gp, Threedi3di3 *model)
{
    const ThreediGpOcclusion *src = gp->occlusion;
    if (!src) {
        return 0;
    }
    model->occlusion_vertex_count = src->vertex_count;
    model->occlusion_vertex_record_size = src->vertex_count > 0 ? 12u : 0u;
    if (src->vertex_count > 0) {
        model->occlusion_vertices = (ThreediOcclusionVertex *)calloc(src->vertex_count, sizeof(ThreediOcclusionVertex));
        if (!model->occlusion_vertices) return -1;
        for (size_t i = 0; i < src->vertex_count; ++i) {
            memcpy(model->occlusion_vertices[i].position, src->vertices[i].position, sizeof(model->occlusion_vertices[i].position));
        }
    }

    model->occlusion_face_count = src->face_count;
    model->occlusion_face_record_size = src->face_count > 0 ? 12u : 0u;
    if (src->face_count > 0) {
        model->occlusion_faces = (ThreediOcclusionFace *)calloc(src->face_count, sizeof(ThreediOcclusionFace));
        if (!model->occlusion_faces) return -1;
        for (size_t i = 0; i < src->face_count; ++i) {
            model->occlusion_faces[i].raw_indices = src->faces[i].raw_indices;
            model->occlusion_faces[i].edge_data = src->faces[i].edge_data;
            model->occlusion_faces[i].other_edge_data = src->faces[i].other_edge_data;
        }
    }

    model->occlusion_plane_count = src->plane_count;
    model->occlusion_plane_record_size = src->plane_count > 0 ? 16u : 0u;
    if (src->plane_count > 0) {
        model->occlusion_planes = (ThreediOcclusionPlane *)calloc(src->plane_count, sizeof(ThreediOcclusionPlane));
        if (!model->occlusion_planes) return -1;
        for (size_t i = 0; i < src->plane_count; ++i) {
            memcpy(model->occlusion_planes[i].normal, src->planes[i].normal, sizeof(model->occlusion_planes[i].normal));
            model->occlusion_planes[i].radius = src->planes[i].radius;
        }
    }

    model->occlusion_object_count = src->object_count;
    model->occlusion_object_record_size = src->object_count > 0 ? 36u : 0u;
    if (src->object_count > 0) {
        model->occlusion_objects = (ThreediOcclusionObject *)calloc(src->object_count, sizeof(ThreediOcclusionObject));
        if (!model->occlusion_objects) return -1;
        for (size_t i = 0; i < src->object_count; ++i) {
            const ThreediGpOcclusionObject *so = &src->objects[i];
            ThreediOcclusionObject *do_ = &model->occlusion_objects[i];
            do_->type = so->type;
            do_->parent_subobject_index = so->parent_subobject_index;
            do_->connecting_subobject = so->connecting_subobject;
            memcpy(do_->position, so->center, sizeof(do_->position));
            do_->radius = so->radius;
            do_->num_vertices = so->num_vertices;
            do_->num_planes = so->num_planes;
            do_->face_count = so->num_faces;
        }
    }
    return 0;
}

int threedi_gp_to_3di3(const ThreediGpFile *gp, Threedi3di3 *out_model)
{
    if (!gp || !out_model) {
        return -1;
    }
    memset(out_model, 0, sizeof(*out_model));
    out_model->version = THREEDI_AUTO_VERSION;
    out_model->header.has_header = 1;
    copy_padded(out_model->header.name, sizeof(out_model->header.name), gp->header.name);
    out_model->header.mesh_type = gp->header.mesh_type == THREEDI_GP_MESH_SKINNED
        ? THREEDI_MESH_SKINNED
        : THREEDI_MESH_BASIC;
    out_model->header.lod_count_decl = (int32_t)gp->rmodel_count;
    out_model->header.lod_distance = gp->header.num_lods > 0 ? (int32_t)(gp->header.lod_thresholds[0] >> 16u) : 0;

    size_t *material_offsets = NULL;
    if (gp->rmodel_count > 0) {
        material_offsets = (size_t *)calloc(gp->rmodel_count, sizeof(size_t));
        if (!material_offsets) {
            return -1;
        }
    }

    if (convert_materials(gp, out_model, material_offsets) != 0 ||
        convert_lods(gp, out_model, material_offsets) != 0 ||
        convert_userpoints(gp, out_model) != 0 ||
        convert_ctrl_and_mtrx(gp, out_model) != 0 ||
        convert_lights(gp, out_model) != 0 ||
        convert_collision(gp, out_model) != 0 ||
        convert_occlusion(gp, out_model) != 0 ||
        dedupe_materials(out_model) != 0) {
        free(material_offsets);
        threedi_3di3_free(out_model);
        return -1;
    }
    free(material_offsets);
    return 0;
}

int threedi_read_model_auto(const char *path, Threedi3di3 *out_model)
{
    if (!path || !out_model) {
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    uint8_t magic[4] = {0, 0, 0, 0};
    size_t read = fread(magic, 1, sizeof(magic), f);
    fclose(f);
    if (read < 4u) {
        return -1;
    }
    if (memcmp(magic, "3DI3", 4) == 0) {
        return threedi_3di3_read(path, out_model);
    }
    if (threedi_gp_detect(magic, sizeof(magic)) != THREEDI_GP_MESH_UNKNOWN) {
        ThreediGpFile gp;
        threedi_gp_init(&gp);
        if (threedi_gp_read(path, &gp) != 0) {
            return -1;
        }
        int rc = threedi_gp_to_3di3(&gp, out_model);
        threedi_gp_free(&gp);
        return rc;
    }
    return -1;
}
