// Convert Land Warrior (.3di v10) ThreediLwFile to ThreediModelIR.
//
// This mirrors threedi_ir_from_gp.cpp: one IR Part per sub-object, faces
// grouped into one primitive per (part, material), and a faithful structural
// translation of how Dflw.exe draws the model. The draw path was reverse
// engineered from Dflw.exe (imagebase 0x400000):
//   - LW3di_LoadLod @ 0x47CF80  (geometry + per-surface material resolution)
//   - sub_4A1080 @ 0x4A1080     (model draw orchestrator)
//   - sub_4911B0 @ 0x4911B0     (per-subobject mesh draw kernel)
//   - sub_48F110 @ 0x48F110     (rigid transform: whole subobject by matrices[subobjIdx])
//   - sub_48F290 @ 0x48F290     (skinned transform: each vertex by matrices[vertex.w])
// The renderer draws ONE subobject at a time and selects a bone matrix per
// vertex by the vertex's 4th int16 (vertex.w). There is NO per-primitive bone
// palette in the engine; the per-primitive bone_table here is purely the IR's
// way of carrying those per-vertex bone indices to the scene builder.

#include "threedi/threedi_ir.h"
#include "threedi/threedi_lw.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// LW geometry is raw Z-up, +X-forward. Vertices are 8.8 fixed-point; sub-object
// translations are 16.16 (LW3di_LoadLod localizes verts by subobj.pos >> 8 @
// 0x47d4b5). The -y/z/x swizzle and {0,2,1} winding are visual-preview-validated
// pending a final render-orientation RE pass.
static const float LW_POS_SCALE = 1.0f / 256.0f;      // 8.8 fixed-point vertex units
static const float LW_PART_POS_SCALE = 1.0f / 65536.0f; // 16.16 sub-object translation
static const float LW_UV_SCALE = 1.0f / 65536.0f;     // provisional; see notes/3di-lw

static void lw_raw_to_ir(float out[3], float x, float y, float z, float scale) {
    out[0] = -y * scale;
    out[1] = z * scale;
    out[2] = x * scale;
}

// Case-insensitive equality for short fixed names.
static int ci_equal(const char *a, const char *b) {
    while (*a && *b) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        ++a; ++b;
    }
    return *a == *b;
}

// Surfaces named DONTDRAW.* or NoName are non-rendering markers (see
// notes/3di-lw/lw-3di-format.md §4c); their faces must not emit geometry.
static int surface_is_hidden(const char *name) {
    return ci_equal(name, "DONTDRAW.PCX") ||
           ci_equal(name, "DONTDRAW.TGA") ||
           ci_equal(name, "NoName");
}

// Resolve every surface to a material index once, returning a malloc'd
// int32_t[surface_count] (entry = material index, or -1 to drop the surface).
//
// LW3di_LoadLod @ 0x47d356 binds a surface to a material by SELECTOR ID, not by
// texture name: for the standalone/default skin (activeSkin = dword_5B752C = 0)
// it matches surface.material_selectors[0] against material.selector_id (+0x28).
// We then fall back to the on-disk surface.material_index (+0x18), which the
// engine also stores, so surfaces that do not match by selector still draw
// rather than being silently dropped.
static int32_t *build_surface_material_map(const ThreediLwFile *lw, const ThreediLwLod *lod) {
    if (!lod->surfaces || lod->surface_count == 0) return NULL;
    int32_t *map = (int32_t *)malloc((size_t)lod->surface_count * sizeof(int32_t));
    if (!map) return NULL;

    uint32_t mat_count = lw->material_count;
    for (uint32_t s = 0; s < lod->surface_count; ++s) {
        const ThreediLwSurface *surf = &lod->surfaces[s];
        if (surface_is_hidden(surf->name)) {
            map[s] = -1;
            continue;
        }
        if (mat_count == 0) {
            // Material-less model: a single synthesized default (index 0).
            map[s] = 0;
            continue;
        }
        int32_t resolved = -1;
        uint8_t selector = surf->material_selectors[0];
        for (uint32_t i = 0; i < mat_count; ++i) {
            if (lw->materials[i].selector_id == (uint16_t)selector) {
                resolved = (int32_t)i;
                break;
            }
        }
        if (resolved < 0 && surf->material_index < mat_count) {
            resolved = (int32_t)surf->material_index; // on-disk fallback (surface +0x18)
        }
        map[s] = resolved; // may stay -1 -> surface has no resolvable material, dropped
    }
    return map;
}

// Resolve a single face's material via the surface map. Returns the material
// index, or -1 if the face should be dropped (hidden / unresolved surface).
static int32_t face_material(const ThreediLwFile *lw, const ThreediLwLod *lod,
                             const int32_t *surf_map, const ThreediLwFace *f) {
    if (!lod->surfaces || lod->surface_count == 0 || !surf_map) {
        return 0; // no surfaces: everything falls into the single default material bucket
    }
    if (f->surface_index < 0 || (uint32_t)f->surface_index >= lod->surface_count) {
        return -1; // malformed reference
    }
    return surf_map[f->surface_index];
}

// LW binds each vertex to exactly one bone: its 4th int16 (vertex.w) for skinned
// LODs, otherwise its owning sub-object. Returns the skeleton (sub-object) index.
static int source_vertex_bone(const ThreediLwLod *lod, uint32_t vidx,
                              uint32_t owning_part, int skinned, uint8_t *out) {
    if (skinned && lod->subobjects && lod->subobject_count > 0) {
        int16_t w = lod->vertices[vidx].w;
        if (w < 0 || (uint32_t)w >= lod->subobject_count) return -1;
        *out = (uint8_t)w;
        return 0;
    }
    *out = (uint8_t)owning_part;
    return 0;
}

// Per-primitive bone table maps a local slot -> skeleton bone index. The corpus
// shows at most 5 distinct bones per primitive (well under the IR's 16), so no
// overflow handling is required; add_bone_to_table fails closed if that ever
// changes.
static int find_bone_slot(const uint8_t *table, uint8_t count, uint8_t bone) {
    for (uint8_t i = 0; i < count; ++i) {
        if (table[i] == bone) return (int)i;
    }
    return -1;
}

static int add_bone_to_table(uint8_t *table, uint8_t *count, uint8_t bone) {
    int slot = find_bone_slot(table, *count, bone);
    if (slot >= 0) return slot;
    if (*count >= 16) return -1;
    slot = (int)*count;
    table[(*count)++] = bone;
    return slot;
}

static int set_ir_vertex(ThreediIRVertex *dst, const ThreediLwLod *lod,
                         uint32_t vidx, uint32_t nidx, int has_normal,
                         int32_t u, int32_t v, uint8_t local_bone_index) {
    const ThreediLwVertex *sv = &lod->vertices[vidx];
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
    dst->bone_weights[0] = 1.0f; // one bone per vertex
    dst->bone_indices[0] = local_bone_index;
    return 0;
}

// Build IR parts from sub-objects (hierarchy + rest positions). Matches the
// loader's rel = pos - parent.pos (LW3di_LoadLod @ 0x47d508).
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
    if (part_count > 255) return -1; // bone indices are stored as uint8_t

    size_t corner_total = (size_t)src->face_count * 3;
    if (corner_total == 0) return 0;
    if (corner_total > UINT32_MAX) return -1;
    if (!src->vertices || !src->faces) return -1;

    uint32_t mat_count = lw->material_count ? lw->material_count : 1;
    int skinned_lod = (src->flags & 1u) != 0;

    int32_t *surf_map = build_surface_material_map(lw, src);
    // surf_map may legitimately be NULL when the LOD has no surfaces.

    dst->vertices = (ThreediIRVertex *)calloc(corner_total, sizeof(ThreediIRVertex));
    dst->indices = (uint16_t *)malloc(corner_total * sizeof(uint16_t));
    dst->primitives = (ThreediIRPrimitive *)calloc(src->face_count, sizeof(ThreediIRPrimitive));
    if (!dst->vertices || !dst->indices || !dst->primitives) {
        free(surf_map);
        return -1;
    }

    // Sub-objects own contiguous, in-order ranges of vertices / normals / faces.
    // The loader addresses these via per-subobject pointer fields (+0x08/+0x20/
    // +0x10), but they are computed by simple count accumulation; we reproduce
    // the accumulation and intentionally ignore the on-disk pointer fields. This
    // sequential layout holds for all 4060 LODs in the validation corpus; a
    // mismatch is treated as a hard error rather than silently misread.
    size_t cursor = 0;
    size_t prim = 0;
    size_t face_base = 0;
    size_t vertex_base = 0;
    size_t normal_base = 0;
    int rc = -1;

    for (uint32_t s = 0; s < part_count; ++s) {
        uint32_t pv = src->subobject_count ? src->subobjects[s].vertex_count : src->vertex_count;
        uint32_t pn = src->subobject_count ? src->subobjects[s].normal_count : src->normal_count;
        uint32_t pf = src->subobject_count ? src->subobjects[s].face_count : src->face_count;
        if (vertex_base + pv > src->vertex_count) goto done;
        if (normal_base + pn > src->normal_count) goto done;
        if (face_base + pf > src->face_count) goto done;
        size_t fstart = face_base;
        size_t fend = face_base + pf;
        dst->parts[s].primitive_start = (int32_t)prim;

        for (uint32_t m = 0; m < mat_count; ++m) {
            size_t pstart = cursor;
            uint32_t local_index = 0;
            uint8_t bone_table[16];
            uint8_t bone_count = 0;

            for (size_t fi = fstart; fi < fend; ++fi) {
                const ThreediLwFace *f = &src->faces[fi];
                int32_t resolved = face_material(lw, src, surf_map, f);
                if (resolved < 0 || (uint32_t)resolved != m) continue; // dropped or other bucket

                // Split if a single (part, material) primitive would exceed the
                // 16-bit index space (a buffer guard; never hit in the corpus).
                if (local_index + 3 > 65535u) {
                    finalize_primitive(dst, prim++, pstart, local_index, m, s,
                                       skinned_lod ? bone_table : NULL,
                                       skinned_lod ? bone_count : 0);
                    pstart = cursor;
                    local_index = 0;
                    bone_count = 0;
                }

                const int corner_order[3] = {0, 2, 1};
                for (int oi = 0; oi < 3; ++oi) {
                    int k = corner_order[oi];
                    if (f->vertex[k] < 0 || (uint32_t)f->vertex[k] >= pv) goto done;
                    uint32_t vidx = vertex_base + (uint32_t)f->vertex[k];

                    uint32_t nidx = 0;
                    int has_normal = 0;
                    if (pn > 0) {
                        if (f->normal[k] < 0 || (uint32_t)f->normal[k] >= pn) goto done;
                        nidx = normal_base + (uint32_t)f->normal[k];
                        has_normal = 1;
                    }

                    uint8_t local_bone = 0;
                    if (skinned_lod) {
                        uint8_t bone;
                        if (source_vertex_bone(src, vidx, s, 1, &bone) != 0) goto done;
                        int slot = add_bone_to_table(bone_table, &bone_count, bone);
                        if (slot < 0) goto done; // > 16 distinct bones: unsupported
                        local_bone = (uint8_t)slot;
                    }

                    if (set_ir_vertex(&dst->vertices[cursor], src, vidx, nidx, has_normal,
                                      f->u[k], f->v[k], local_bone) != 0) {
                        goto done;
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
        goto done; // non-sequential sub-object layout: refuse rather than misread
    }

    dst->vertex_count = cursor;
    dst->index_count = cursor;
    dst->primitive_count = prim;
    compute_part_bounds(dst);
    rc = 0;

done:
    free(surf_map);
    return rc;
}

// LW materials carry only a texture name pair and dimensions. Shading is mapped
// to a minimal opaque fixed-function shader for now; richer material parity
// (blend modes, two-sided, texture animation) is deferred to a later phase
// alongside texture decoding (see notes/3di-lw/lw-3di-format.md §5).
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

    // A model is skinned if any LOD enables per-vertex bone influence (flags&1).
    // The mesh_type drives every downstream skinning gate (tdp.cpp, scene_builder),
    // so it must reflect the bone data the converter emits. Scoped in a block so
    // the goto-based error path does not cross its initialization.
    {
        int any_skinned = 0;
        for (uint32_t i = 0; i < lw->lod_count; ++i) {
            if (lw->lods[i].flags & 1u) { any_skinned = 1; break; }
        }
        out->mesh_type = any_skinned ? THREEDI_IR_MESH_SKINNED : THREEDI_IR_MESH_BASIC;
    }

    out->lod_count = lw->lod_count;
    if (out->lod_count > 0) {
        out->lods = (ThreediIRLod *)calloc(out->lod_count, sizeof(ThreediIRLod));
        if (!out->lods) goto error;
        for (size_t i = 0; i < out->lod_count; ++i) {
            if (convert_lod(lw, &lw->lods[i], i, &out->lods[i]) != 0) goto error;
        }
    }

    return 0;

error:
    threedi_ir_free(out);
    return -1;
}
