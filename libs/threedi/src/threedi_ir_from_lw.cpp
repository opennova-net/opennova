// Convert a Land Warrior v10 model (ThreediLwFile) to ThreediModelIR.
//
// LW stores separate vertex/normal arrays indexed per-face-corner plus per-face
// UVs; the IR uses unified vertices + an index buffer. We emit three IR vertices
// per triangle (the importer's scene builder dedupes coincident vertices), laid
// out grouped by part/material. Large groups are split so uint16 indices remain
// local to each primitive.
//
// Per-face material is resolved face -> surface_index -> surface.material_index.

#include "threedi/threedi_ir.h"
#include "threedi/threedi_lw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Provisional fixed-point scales (tunable against rendered output, see
// notes/3di-lw/lw-3di-format.md open items). Positions match the GP collision
// vertex scale (int / 256). Normals are int16 directions; UVs assumed 16.16.
static const float LW_POS_SCALE = 1.0f / 256.0f;
static const float LW_UV_SCALE  = 1.0f / 65536.0f;

static int resolve_face_material(const ThreediLwLod *lod, const ThreediLwFace *f,
                                 uint32_t material_count, uint32_t *out) {
    if (material_count == 0) {
        *out = 0;
        return 0;
    }
    if (!lod->surfaces || lod->surface_count == 0 ||
        f->surface_index < 0 || (uint32_t)f->surface_index >= lod->surface_count) {
        return -1;
    }
    uint32_t mat = (uint32_t)lod->surfaces[f->surface_index].material_index;
    *out = mat < material_count ? mat : 0;
    return 0;
}

static void set_ir_vertex(ThreediIRVertex *dv, const ThreediLwLod *lod,
                          uint32_t vidx, uint32_t nidx, int has_normal,
                          int32_t u, int32_t v) {
    const ThreediLwVertex *sv = &lod->vertices[vidx];
    dv->position[0] = (float)sv->x * LW_POS_SCALE;
    dv->position[1] = (float)sv->y * LW_POS_SCALE;
    dv->position[2] = (float)sv->z * LW_POS_SCALE;

    if (has_normal && lod->normals && lod->normal_count) {
        const ThreediLwVertex *sn = &lod->normals[nidx];
        float nx = (float)sn->x, ny = (float)sn->y, nz = (float)sn->z;
        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len > 0.0f) { dv->normal[0] = nx / len; dv->normal[1] = ny / len; dv->normal[2] = nz / len; }
        else            { dv->normal[2] = 1.0f; }
    } else {
        dv->normal[2] = 1.0f;
    }

    dv->uv0[0] = (float)u * LW_UV_SCALE;
    dv->uv0[1] = (float)v * LW_UV_SCALE;
    dv->bone_weights[0] = 1.0f;
}

// Build IR parts from the LW sub-objects (the rigid-part skeleton). Sets
// parent_index, abs_position (rest pose), and rel_position (parent-relative).
static int build_parts(const ThreediLwLod *src, ThreediIRLod *dst, uint32_t *out_part_count) {
    uint32_t part_count = src->subobject_count ? src->subobject_count : 1;
    dst->part_count = part_count;
    dst->declared_part_count = (int32_t)part_count;
    dst->parts = (ThreediIRPart *)calloc(part_count, sizeof(ThreediIRPart));
    if (!dst->parts) return -1;

    for (uint32_t s = 0; s < part_count; ++s) {
        ThreediIRPart *p = &dst->parts[s];
        p->parent_index = -1;
        if (src->subobject_count) {
            const ThreediLwSubObject *so = &src->subobjects[s];
            int pp = so->parent;
            if (pp >= 0 && (uint32_t)pp < part_count && (uint32_t)pp != s) p->parent_index = pp;
            p->abs_position[0] = (float)so->pos[0] * LW_POS_SCALE;
            p->abs_position[1] = (float)so->pos[1] * LW_POS_SCALE;
            p->abs_position[2] = (float)so->pos[2] * LW_POS_SCALE;
        }
    }
    // rel_position = abs - parent_abs (parents always precede children here, but
    // we read parent abs directly so order does not matter).
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
                               size_t vertex_count, uint32_t material, uint32_t part) {
    ThreediIRPrimitive *p = &dst->primitives[prim_idx];
    p->material_index = (int32_t)material;
    p->part_index = (int32_t)part;
    p->index_offset = (uint32_t)vertex_start;
    p->index_count = (uint32_t)vertex_count;
    p->vertex_offset = (uint32_t)vertex_start;
    p->vertex_count = (uint32_t)vertex_count;
    p->topology = THREEDI_IR_TOPOLOGY_TRIANGLES;
    p->bone_table[0] = (uint8_t)part;
    p->bone_table_length = 1;

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

    uint32_t mat_count = lw->material_count ? lw->material_count : 1;
    uint32_t part_count = 0;
    if (build_parts(src, dst, &part_count) != 0) return -1;
    if (part_count > 255) return -1;

    size_t corner_total = (size_t)src->faceref_count * 3;
    if (corner_total == 0) return 0;  // parts only (empty geometry)
    if (corner_total > UINT32_MAX) return -1;
    if (!src->vertices || !src->faces) return -1;

    dst->vertices = (ThreediIRVertex *)calloc(corner_total, sizeof(ThreediIRVertex));
    dst->indices = (uint16_t *)malloc(corner_total * sizeof(uint16_t));
    // Worst case one emitted primitive per source face.
    dst->primitives = (ThreediIRPrimitive *)calloc(src->faceref_count, sizeof(ThreediIRPrimitive));
    if (!dst->vertices || !dst->indices || !dst->primitives) return -1;

    size_t cursor = 0, prim = 0, face_base = 0, vertex_base = 0, normal_base = 0;
    for (uint32_t s = 0; s < part_count; ++s) {
        uint32_t pv = src->subobject_count ? src->subobjects[s].vertex_count : src->vertex_count;
        uint32_t pn = src->subobject_count ? src->subobjects[s].normal_count : src->normal_count;
        uint32_t pf = src->subobject_count ? src->subobjects[s].face_count : src->faceref_count;
        if (vertex_base + pv > src->vertex_count) return -1;
        if (normal_base + pn > src->normal_count) return -1;
        if (face_base + pf > src->faceref_count) return -1;

        size_t fstart = face_base, fend = face_base + pf;
        dst->parts[s].primitive_start = (int32_t)prim;

        for (uint32_t m = 0; m < mat_count; ++m) {
            size_t pstart = cursor;
            uint32_t local_index = 0;
            for (size_t fi = fstart; fi < fend; ++fi) {
                const ThreediLwFace *f = &src->faces[fi];
                uint32_t face_mat = 0;
                if (resolve_face_material(src, f, lw->material_count, &face_mat) != 0) return -1;
                if (face_mat != m) continue;

                if (local_index + 3 > 65535u) {
                    finalize_primitive(dst, prim++, pstart, local_index, m, s);
                    pstart = cursor;
                    local_index = 0;
                }

                for (int k = 0; k < 3; ++k) {
                    if (f->vertex[k] < 0 || (uint32_t)f->vertex[k] >= pv) return -1;
                    uint32_t nidx = 0;
                    int has_normal = 0;
                    if (pn > 0) {
                        if (f->normal[k] < 0 || (uint32_t)f->normal[k] >= pn) return -1;
                        nidx = (uint32_t)(normal_base + (uint32_t)f->normal[k]);
                        has_normal = 1;
                    }
                    uint32_t vidx = (uint32_t)(vertex_base + (uint32_t)f->vertex[k]);
                    set_ir_vertex(&dst->vertices[cursor], src, vidx, nidx, has_normal, f->u[k], f->v[k]);
                    dst->indices[cursor] = (uint16_t)local_index;
                    cursor++;
                    local_index++;
                }
            }
            if (local_index > 0) {
                finalize_primitive(dst, prim++, pstart, local_index, m, s);
            }
        }
        dst->parts[s].primitive_count = (int32_t)prim - dst->parts[s].primitive_start;
        dst->parts[s].opaque_count = dst->parts[s].primitive_count;
        face_base += pf;
        vertex_base += pv;
        normal_base += pn;
    }
    if (face_base != src->faceref_count || vertex_base != src->vertex_count ||
        normal_base != src->normal_count) return -1;

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
        ThreediIRMaterial *dm = &ir->materials[i];
        dm->index = (int32_t)i;
        dm->blend_mode = THREEDI_IR_BLEND_OPAQUE;
        snprintf(dm->shader_name, sizeof(dm->shader_name), "FF_ST_OP");
        dm->surface_type = 0x01;
        if (i < n && lw->materials[i].tex_name_0[0] != '\0') {
            ThreediIRMaterialTexture *tex = &dm->textures[dm->texture_count++];
            strncpy((char *)tex->name, lw->materials[i].tex_name_0, sizeof(tex->name) - 1);
            tex->slot = THREEDI_IR_TEX_SLOT_DIFFUSE;
        }
        if (i < n && lw->materials[i].tex_name_1[0] != '\0' &&
            dm->texture_count < sizeof(dm->textures) / sizeof(dm->textures[0])) {
            ThreediIRMaterialTexture *tex = &dm->textures[dm->texture_count++];
            strncpy((char *)tex->name, lw->materials[i].tex_name_1, sizeof(tex->name) - 1);
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
    out->source_format = THREEDI_IR_SOURCE_LW10;
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

    // Multi-part models become SKINNED so the importer builds a rigid-part
    // armature (one bone per sub-object) and binds each part's mesh to its bone,
    // enabling SAF1 animation. Single-part models stay BASIC (no armature).
    out->mesh_type = (out->lod_count > 0 && out->lods[0].part_count > 1)
                         ? THREEDI_IR_MESH_SKINNED : THREEDI_IR_MESH_BASIC;

    return 0;
error:
    threedi_ir_free(out);
    return -1;
}
