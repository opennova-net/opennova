// Convert a Land Warrior model (ThreediLwFile, v8/v10) to ThreediModelIR.
//
// LW stores separate vertex/normal arrays indexed per-face-corner plus per-face
// UVs; the IR uses unified vertices + an index buffer. We emit three IR vertices
// per triangle (the importer's scene builder dedupes coincident vertices), laid
// out grouped by material so each material maps to one contiguous primitive.
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

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Resolve the material index for a face via its surface record.
static int32_t face_material(const ThreediLwLod *lod, const ThreediLwFace *f,
                             uint32_t material_count) {
    if (material_count == 0) return 0;
    int32_t mat = 0;
    if (lod->surfaces && lod->surface_count) {
        int si = clampi(f->surface_index, 0, (int)lod->surface_count - 1);
        mat = (int32_t)lod->surfaces[si].material_index;
    }
    return clampi(mat, 0, (int)material_count - 1);
}

static void set_ir_vertex(ThreediIRVertex *dv, const ThreediLwLod *lod,
                          int16_t vidx, int16_t nidx, int32_t u, int32_t v) {
    const ThreediLwVertex *sv = &lod->vertices[vidx];
    dv->position[0] = (float)sv->x * LW_POS_SCALE;
    dv->position[1] = (float)sv->y * LW_POS_SCALE;
    dv->position[2] = (float)sv->z * LW_POS_SCALE;

    if (lod->normals && lod->normal_count && nidx >= 0 &&
        (uint32_t)nidx < lod->normal_count) {
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

static int convert_lod(const ThreediLwFile *lw, const ThreediLwLod *src,
                       size_t lod_idx, ThreediIRLod *dst) {
    dst->threshold = (lod_idx < 3) ? (int32_t)(lw->lod_thresholds[lod_idx] >> 16) : 0;

    uint32_t mat_count = lw->material_count ? lw->material_count : 1;
    uint32_t part_count = 0;
    if (build_parts(src, dst, &part_count) != 0) return -1;

    size_t corner_total = (size_t)src->faceref_count * 3;
    if (corner_total == 0) return 0;  // parts only (empty geometry)

    dst->vertices = (ThreediIRVertex *)calloc(corner_total, sizeof(ThreediIRVertex));
    dst->indices = (uint16_t *)malloc(corner_total * sizeof(uint16_t));
    // Worst case one primitive per (part, material).
    dst->primitives = (ThreediIRPrimitive *)calloc((size_t)part_count * mat_count + 1,
                                                    sizeof(ThreediIRPrimitive));
    if (!dst->vertices || !dst->indices || !dst->primitives) return -1;

    // Faces are partitioned across parts in sub-object order; cumulative ranges.
    size_t cursor = 0, prim = 0, face_base = 0;
    for (uint32_t s = 0; s < part_count; ++s) {
        uint32_t pf = src->subobject_count ? src->subobjects[s].face_count : src->faceref_count;
        size_t fstart = face_base, fend = face_base + pf;
        if (fend > src->faceref_count) fend = src->faceref_count;  // defensive
        dst->parts[s].primitive_start = (int32_t)prim;

        for (uint32_t m = 0; m < mat_count; ++m) {
            size_t pstart = cursor;
            for (size_t fi = fstart; fi < fend; ++fi) {
                const ThreediLwFace *f = &src->faces[fi];
                if ((uint32_t)face_material(src, f, lw->material_count) != m) continue;
                for (int k = 0; k < 3; ++k) {
                    set_ir_vertex(&dst->vertices[cursor], src, f->vertex[k], f->normal[k],
                                  f->u[k], f->v[k]);
                    dst->indices[cursor] = (uint16_t)cursor;
                    cursor++;
                }
            }
            if (cursor > pstart) {
                ThreediIRPrimitive *p = &dst->primitives[prim++];
                p->material_index = (int32_t)m;
                p->part_index = (int32_t)s;
                p->index_offset = (uint32_t)pstart;
                p->index_count = (uint32_t)(cursor - pstart);
                p->vertex_offset = (uint32_t)pstart;
                p->vertex_count = (uint32_t)(cursor - pstart);
                p->topology = THREEDI_IR_TOPOLOGY_TRIANGLES;
                p->bone_table[0] = (uint8_t)s;  // local bone 0 -> this part
                p->bone_table_length = 1;
            }
        }
        dst->parts[s].primitive_count = (int32_t)prim - dst->parts[s].primitive_start;
        dst->parts[s].opaque_count = dst->parts[s].primitive_count;
        face_base = fend;
    }

    dst->vertex_count = cursor;
    dst->index_count = cursor;
    dst->primitive_count = prim;
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
        if (i < n && lw->materials[i].tex_name_0[0] != '\0') {
            strncpy((char *)dm->textures[0].name, lw->materials[i].tex_name_0,
                    sizeof(dm->textures[0].name) - 1);
            dm->textures[0].slot = THREEDI_IR_TEX_SLOT_DIFFUSE;
            dm->texture_count = 1;
        }
    }
    return 0;
}

int threedi_ir_from_lw(const ThreediLwFile *lw, ThreediModelIR *out) {
    if (!lw || !out) return -1;
    threedi_ir_init(out);

    strncpy(out->name, lw->name, sizeof(out->name) - 1);
    out->source_format = (lw->version == THREEDI_LW_VERSION_8)
                             ? THREEDI_IR_SOURCE_LW8 : THREEDI_IR_SOURCE_LW10;

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
