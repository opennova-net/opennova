#include "threedi/threedi_ir.h"
#include "threedi/threedi_lw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const float LW_POS_SCALE = 1.0f / 256.0f;
static const float LW_PART_POS_SCALE = 1.0f / 65536.0f;
static const float LW_UV_SCALE = 1.0f / 65536.0f;

static void lw_raw_to_ir(float out[3], float x, float y, float z, float scale) {
    out[0] = -y * scale;
    out[1] = z * scale;
    out[2] = x * scale;
}

static int ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c;
}

static int name_ieq(const char *a, const char *b) {
    if (!a || !b || !a[0] || !b[0]) return 0;
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) return 0;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int hidden_surface_name(const char *name) {
    return name_ieq(name, "DONTDRAW.PCX") ||
           name_ieq(name, "DONTDRAW.TGA") ||
           name_ieq(name, "NoName");
}

static int material_matches_surface(const ThreediLwMaterial *mat, const char *surface_name) {
    return name_ieq(surface_name, mat->texture0) || name_ieq(surface_name, mat->texture1);
}

static int resolve_face_material(const ThreediLwFile *lw, const ThreediLwLod *lod,
                                 const ThreediLwFace *f, uint32_t *out) {
    if (!lw || lw->material_count == 0) {
        *out = 0;
        return 0;
    }
    if (!lod->surfaces || lod->surface_count == 0 ||
        f->surface_index < 0 || (uint32_t)f->surface_index >= lod->surface_count) {
        return -1;
    }

    const ThreediLwSurface *surface = &lod->surfaces[f->surface_index];
    if (hidden_surface_name(surface->name)) return 1;

    if ((surface->flags & (0x1u | 0x8000u)) != 0) {
        uint8_t selector = surface->material_selectors[0];
        for (uint32_t i = 0; i < lw->material_count; ++i) {
            if (lw->materials[i].selector_id == (uint16_t)selector) {
                *out = i;
                return 0;
            }
        }
    }

    for (uint32_t i = 0; i < lw->material_count; ++i) {
        if (material_matches_surface(&lw->materials[i], surface->name)) {
            *out = i;
            return 0;
        }
    }

    uint32_t mat = (uint32_t)surface->material_index;
    if (mat >= lw->material_count) return 1;
    *out = mat;
    return 0;
}

static int source_vertex_bone(const ThreediLwLod *lod, uint32_t vidx, uint32_t owning_part, uint8_t *out) {
    if ((lod->flags & 1u) != 0 && lod->subobjects && lod->subobject_count > 0) {
        const ThreediLwVertex *sv = &lod->vertices[vidx];
        if (sv->w < 0 || (uint32_t)sv->w >= lod->subobject_count) return -1;
        *out = (uint8_t)sv->w;
        return 0;
    }
    *out = (uint8_t)owning_part;
    return 0;
}

static int find_bone_slot(const uint8_t *bone_table, uint8_t bone_count, uint8_t bone) {
    for (uint8_t i = 0; i < bone_count; ++i) {
        if (bone_table[i] == bone) return (int)i;
    }
    return -1;
}

static int add_bone_to_table(uint8_t *bone_table, uint8_t *bone_count, uint8_t bone) {
    int slot = find_bone_slot(bone_table, *bone_count, bone);
    if (slot >= 0) return slot;
    if (*bone_count >= 16) return -1;
    slot = (int)*bone_count;
    bone_table[*bone_count] = bone;
    ++(*bone_count);
    return slot;
}

static int set_ir_vertex(ThreediIRVertex *dst, const ThreediLwLod *lod,
                         uint32_t vidx, uint32_t nidx, int has_normal,
                         int32_t u, int32_t v, uint8_t local_bone_index) {
    const ThreediLwVertex *sv = &lod->vertices[vidx];
    if ((lod->flags & 1u) != 0 && lod->subobjects && lod->subobject_count > 0) {
        if (sv->w < 0 || (uint32_t)sv->w >= lod->subobject_count) return -1;
    }
    lw_raw_to_ir(dst->position, (float)sv->x, (float)sv->y, (float)sv->z, LW_POS_SCALE);

    if (has_normal && lod->normals && lod->normal_count) {
        const ThreediLwVertex *sn = &lod->normals[nidx];
        float nx = (float)sn->x, ny = (float)sn->y, nz = (float)sn->z;
        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len > 0.0f) lw_raw_to_ir(dst->normal, nx, ny, nz, 1.0f / len);
        else dst->normal[1] = 1.0f;
    } else {
        dst->normal[1] = 1.0f;
    }

    dst->uv0[0] = (float)u * LW_UV_SCALE;
    dst->uv0[1] = (float)v * LW_UV_SCALE;
    dst->bone_weights[0] = 1.0f;
    dst->bone_indices[0] = local_bone_index;
    return 0;
}

static int build_parts(const ThreediLwLod *src, ThreediIRLod *dst, uint32_t *out_part_count) {
    uint32_t part_count = src->subobject_count ? src->subobject_count : 1;
    int local_space = (src->flags & 1u) != 0;
    dst->part_count = part_count;
    dst->declared_part_count = (int32_t)part_count;
    dst->parts = (ThreediIRPart *)calloc(part_count, sizeof(ThreediIRPart));
    if (!dst->parts) return -1;

    for (uint32_t s = 0; s < part_count; ++s) {
        ThreediIRPart *p = &dst->parts[s];
        p->parent_index = -1;
        if (src->subobject_count) {
            const ThreediLwSubObject *so = &src->subobjects[s];
            int parent = so->parent;
            if (parent >= 0 && (uint32_t)parent < part_count && (uint32_t)parent != s) {
                p->parent_index = parent;
            }
            if (local_space) {
                lw_raw_to_ir(p->abs_position, (float)so->pos[0], (float)so->pos[1], (float)so->pos[2],
                             LW_PART_POS_SCALE);
            }
        }
    }
    for (uint32_t s = 0; s < part_count; ++s) {
        ThreediIRPart *p = &dst->parts[s];
        if (p->parent_index >= 0) {
            const ThreediIRPart *pp = &dst->parts[p->parent_index];
            for (int k = 0; k < 3; ++k) p->rel_position[k] = p->abs_position[k] - pp->abs_position[k];
        } else {
            for (int k = 0; k < 3; ++k) p->rel_position[k] = p->abs_position[k];
        }
    }
    *out_part_count = part_count;
    return 0;
}

static void finalize_primitive(ThreediIRLod *dst, size_t prim_idx, size_t vertex_start,
                               size_t vertex_count, uint32_t material, uint32_t part,
                               const uint8_t *bone_table, uint8_t bone_count) {
    ThreediIRPrimitive *p = &dst->primitives[prim_idx];
    p->material_index = (int32_t)material;
    p->part_index = (int32_t)part;
    p->index_offset = (uint32_t)vertex_start;
    p->index_count = (uint32_t)vertex_count;
    p->vertex_offset = (uint32_t)vertex_start;
    p->vertex_count = (uint32_t)vertex_count;
    p->topology = THREEDI_IR_TOPOLOGY_TRIANGLES;
    if (bone_count > 0 && bone_table) {
        memcpy(p->bone_table, bone_table, bone_count);
        p->bone_table_length = bone_count;
    }
    if (vertex_count == 0) return;

    const ThreediIRVertex *first = &dst->vertices[vertex_start];
    for (int k = 0; k < 3; ++k) {
        p->min[k] = first->position[k];
        p->max[k] = first->position[k];
    }
    for (size_t i = 1; i < vertex_count; ++i) {
        const ThreediIRVertex *v = &dst->vertices[vertex_start + i];
        for (int k = 0; k < 3; ++k) {
            if (v->position[k] < p->min[k]) p->min[k] = v->position[k];
            if (v->position[k] > p->max[k]) p->max[k] = v->position[k];
        }
    }
}

static void compute_part_bounds(ThreediIRLod *dst) {
    for (size_t part_idx = 0; part_idx < dst->part_count; ++part_idx) {
        ThreediIRPart *part = &dst->parts[part_idx];
        if (part->primitive_count <= 0) continue;
        int have = 0;
        float minv[3] = {0.0f, 0.0f, 0.0f};
        float maxv[3] = {0.0f, 0.0f, 0.0f};
        int start = part->primitive_start;
        int end = start + part->primitive_count;
        for (int pi = start; pi < end; ++pi) {
            const ThreediIRPrimitive *pr = &dst->primitives[pi];
            if (!have) {
                for (int k = 0; k < 3; ++k) {
                    minv[k] = pr->min[k];
                    maxv[k] = pr->max[k];
                }
                have = 1;
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                if (pr->min[k] < minv[k]) minv[k] = pr->min[k];
                if (pr->max[k] > maxv[k]) maxv[k] = pr->max[k];
            }
        }
        if (!have) continue;
        for (int k = 0; k < 3; ++k) {
            part->bounding_center[k] = (minv[k] + maxv[k]) * 0.5f;
        }
        float radius = 0.0f;
        for (int pi = start; pi < end; ++pi) {
            const ThreediIRPrimitive *pr = &dst->primitives[pi];
            for (uint32_t vi = 0; vi < pr->vertex_count; ++vi) {
                const ThreediIRVertex *v = &dst->vertices[pr->vertex_offset + vi];
                float dx = v->position[0] - part->bounding_center[0];
                float dy = v->position[1] - part->bounding_center[1];
                float dz = v->position[2] - part->bounding_center[2];
                float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (d > radius) radius = d;
            }
        }
        part->bounding_radius = radius;
    }
}

static int convert_lod(const ThreediLwFile *lw, const ThreediLwLod *src,
                       size_t lod_idx, ThreediIRLod *dst) {
    dst->threshold = (lod_idx < 3) ? (int32_t)(lw->lod_thresholds[lod_idx] >> 16) : 0;
    uint32_t part_count = 0;
    if (build_parts(src, dst, &part_count) != 0) return -1;
    if (part_count > 255) return -1;

    size_t corner_total = (size_t)src->face_count * 3;
    if (corner_total == 0) return 0;
    if (corner_total > UINT32_MAX) return -1;
    if (!src->vertices || !src->faces) return -1;

    uint32_t mat_count = lw->material_count ? lw->material_count : 1;
    int skinned_lod = (src->flags & 1u) != 0;
    dst->vertices = (ThreediIRVertex *)calloc(corner_total, sizeof(ThreediIRVertex));
    dst->indices = (uint16_t *)malloc(corner_total * sizeof(uint16_t));
    dst->primitives = (ThreediIRPrimitive *)calloc(src->face_count, sizeof(ThreediIRPrimitive));
    if (!dst->vertices || !dst->indices || !dst->primitives) return -1;

    size_t cursor = 0;
    size_t prim = 0;
    size_t face_base = 0;
    size_t vertex_base = 0;
    size_t normal_base = 0;

    for (uint32_t s = 0; s < part_count; ++s) {
        uint32_t pv = src->subobject_count ? src->subobjects[s].vertex_count : src->vertex_count;
        uint32_t pn = src->subobject_count ? src->subobjects[s].normal_count : src->normal_count;
        uint32_t pf = src->subobject_count ? src->subobjects[s].face_count : src->face_count;
        if (vertex_base + pv > src->vertex_count) return -1;
        if (normal_base + pn > src->normal_count) return -1;
        if (face_base + pf > src->face_count) return -1;
        size_t fstart = face_base;
        size_t fend = face_base + pf;
        dst->parts[s].primitive_start = (int32_t)prim;

        for (uint32_t m = 0; m < mat_count; ++m) {
            size_t pstart = cursor;
            uint32_t local_index = 0;
            uint8_t bone_table[16];
            uint8_t bone_count = 0;
            memset(bone_table, 0, sizeof(bone_table));

            for (size_t fi = fstart; fi < fend; ++fi) {
                const ThreediLwFace *f = &src->faces[fi];
                uint32_t face_mat = 0;
                int mat_rc = resolve_face_material(lw, src, f, &face_mat);
                if (mat_rc < 0) return -1;
                if (mat_rc > 0 || face_mat != m) continue;

                if (local_index + 3 > 65535u) {
                    finalize_primitive(dst, prim++, pstart, local_index, m, s,
                                       skinned_lod ? bone_table : NULL,
                                       skinned_lod ? bone_count : 0);
                    pstart = cursor;
                    local_index = 0;
                    bone_count = 0;
                    memset(bone_table, 0, sizeof(bone_table));
                }

                const int corner_order[3] = {0, 2, 1};
                uint32_t corner_vidx[3] = {0, 0, 0};
                uint32_t corner_nidx[3] = {0, 0, 0};
                int corner_has_normal[3] = {0, 0, 0};
                uint8_t corner_bone[3] = {0, 0, 0};
                for (int oi = 0; oi < 3; ++oi) {
                    int k = corner_order[oi];
                    if (f->vertex[k] < 0 || (uint32_t)f->vertex[k] >= pv) return -1;
                    corner_vidx[oi] = (uint32_t)(vertex_base + (uint32_t)f->vertex[k]);
                    if (pn > 0) {
                        if (f->normal[k] < 0 || (uint32_t)f->normal[k] >= pn) return -1;
                        corner_nidx[oi] = (uint32_t)(normal_base + (uint32_t)f->normal[k]);
                        corner_has_normal[oi] = 1;
                    }
                    if (source_vertex_bone(src, corner_vidx[oi], s, &corner_bone[oi]) != 0) return -1;
                }

                if (skinned_lod) {
                    uint8_t trial_table[16];
                    uint8_t trial_count = bone_count;
                    memcpy(trial_table, bone_table, sizeof(trial_table));
                    int overflow = 0;
                    for (int oi = 0; oi < 3; ++oi) {
                        if (add_bone_to_table(trial_table, &trial_count, corner_bone[oi]) < 0) {
                            overflow = 1;
                            break;
                        }
                    }
                    if (overflow) {
                        if (local_index == 0) return -1;
                        finalize_primitive(dst, prim++, pstart, local_index, m, s, bone_table, bone_count);
                        pstart = cursor;
                        local_index = 0;
                        bone_count = 0;
                        memset(bone_table, 0, sizeof(bone_table));
                        for (int oi = 0; oi < 3; ++oi) {
                            if (add_bone_to_table(bone_table, &bone_count, corner_bone[oi]) < 0) return -1;
                        }
                    } else {
                        memcpy(bone_table, trial_table, sizeof(bone_table));
                        bone_count = trial_count;
                    }
                }

                for (int oi = 0; oi < 3; ++oi) {
                    int k = corner_order[oi];
                    uint8_t local_bone_index = 0;
                    if (skinned_lod) {
                        int slot = find_bone_slot(bone_table, bone_count, corner_bone[oi]);
                        if (slot < 0) return -1;
                        local_bone_index = (uint8_t)slot;
                    }
                    if (set_ir_vertex(&dst->vertices[cursor], src, corner_vidx[oi], corner_nidx[oi],
                                      corner_has_normal[oi], f->u[k], f->v[k], local_bone_index) != 0) {
                        return -1;
                    }
                    dst->indices[cursor] = (uint16_t)local_index;
                    ++cursor;
                    ++local_index;
                }
            }

            if (local_index > 0) {
                finalize_primitive(dst, prim++, pstart, local_index, m, s,
                                   skinned_lod ? bone_table : NULL,
                                   skinned_lod ? bone_count : 0);
            }
        }
        dst->parts[s].primitive_count = (int32_t)prim - dst->parts[s].primitive_start;
        dst->parts[s].opaque_count = dst->parts[s].primitive_count;
        face_base += pf;
        vertex_base += pv;
        normal_base += pn;
    }

    if (face_base != src->face_count ||
        vertex_base != src->vertex_count ||
        normal_base != src->normal_count) {
        return -1;
    }

    dst->vertex_count = cursor;
    dst->index_count = cursor;
    dst->primitive_count = prim;
    compute_part_bounds(dst);
    return 0;
}

static int convert_materials(const ThreediLwFile *lw, ThreediModelIR *ir) {
    size_t n = lw->material_count;
    ir->material_count = n ? n : 1;
    ir->materials = (ThreediIRMaterial *)calloc(ir->material_count, sizeof(ThreediIRMaterial));
    if (!ir->materials) return -1;

    for (size_t i = 0; i < ir->material_count; ++i) {
        ThreediIRMaterial *mat = &ir->materials[i];
        mat->index = (int32_t)i;
        mat->blend_mode = THREEDI_IR_BLEND_OPAQUE;
        snprintf(mat->shader_name, sizeof(mat->shader_name), "FF_ST_OP");
        mat->surface_type = 0x01;
        if (i < n && lw->materials[i].texture0[0] != '\0') {
            ThreediIRMaterialTexture *tex = &mat->textures[mat->texture_count++];
            strncpy(tex->name, lw->materials[i].texture0, sizeof(tex->name) - 1);
            tex->slot = THREEDI_IR_TEX_SLOT_DIFFUSE;
        }
        if (i < n && lw->materials[i].texture1[0] != '\0' &&
            mat->texture_count < sizeof(mat->textures) / sizeof(mat->textures[0])) {
            ThreediIRMaterialTexture *tex = &mat->textures[mat->texture_count++];
            strncpy(tex->name, lw->materials[i].texture1, sizeof(tex->name) - 1);
            tex->slot = THREEDI_IR_TEX_SLOT_DETAIL;
        }
    }
    return 0;
}

int threedi_ir_from_lw(const ThreediLwFile *lw, ThreediModelIR *out) {
    if (!lw || !out) return -1;
    threedi_ir_init(out);
    if (lw->version != THREEDI_LW_VERSION_10) return -1;

    strncpy(out->name, lw->name, sizeof(out->name) - 1);
    out->source_format = THREEDI_IR_SOURCE_LW;
    if (lw->render_tags[0][0] != '\0') {
        strncpy(out->render_function, lw->render_tags[0], sizeof(out->render_function) - 1);
    } else {
        strncpy(out->render_function, "gnrc", sizeof(out->render_function) - 1);
    }

    if (convert_materials(lw, out) != 0) goto error;

    out->lod_count = lw->lod_count;
    if (out->lod_count > 0) {
        out->lods = (ThreediIRLod *)calloc(out->lod_count, sizeof(ThreediIRLod));
        if (!out->lods) goto error;
        for (size_t i = 0; i < out->lod_count; ++i) {
            if (convert_lod(lw, &lw->lods[i], i, &out->lods[i]) != 0) goto error;
        }
    }

    out->mesh_type = THREEDI_IR_MESH_BASIC;
    return 0;

error:
    threedi_ir_free(out);
    return -1;
}
