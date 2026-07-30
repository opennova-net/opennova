// Convert GP (GPM/GPS/GPP) ThreediGpFile to ThreediModelIR

#include "threedi/threedi_ir.h"
#include "threedi/threedi_gp.h"
#include "threedi/threedi_3di3.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <io/log.h>

// ============================================================================
// Triangulation helpers
// ============================================================================

// Triangulate a triangle list (already triangles, just copy)
static size_t triangulate_list(const uint16_t *indices, size_t count,
                               int32_t first_vertex, uint16_t *out) {
    size_t tri_count = count / 3;
    for (size_t i = 0; i < tri_count; ++i) {
        out[i * 3 + 0] = indices[i * 3 + 0];
        out[i * 3 + 1] = indices[i * 3 + 1];
        out[i * 3 + 2] = indices[i * 3 + 2];
    }
    (void)first_vertex;
    return tri_count * 3;
}

// Triangulate a triangle strip
static size_t triangulate_strip(const uint16_t *indices, size_t count,
                                int32_t first_vertex, uint16_t *out) {
    if (count < 3) return 0;

    size_t out_count = 0;
    int flip = 0;

    for (size_t i = 0; i + 2 < count; ++i) {
        uint16_t a = indices[i];
        uint16_t b = indices[i + 1];
        uint16_t c = indices[i + 2];

        // Skip degenerate triangles
        if (a == b || b == c || a == c) {
            flip = 0;
            continue;
        }

        if (flip) {
            out[out_count++] = a;
            out[out_count++] = c;
            out[out_count++] = b;
        } else {
            out[out_count++] = a;
            out[out_count++] = b;
            out[out_count++] = c;
        }
        flip = !flip;
    }

    (void)first_vertex;
    return out_count;
}

// ============================================================================
// Shader parameter decode helpers
// ============================================================================

// Shared phase/reg decode logic (matches 3DI3 threedi_3di3.c)
static void decode_phase_reg(uint8_t style, uint8_t param,
                              float *phase, int32_t *reg) {
    if (style <= 112) {
        *phase = (float)param / 256.0f;
        *reg = -1;
    } else {
        *reg = (int32_t)param;
        *phase = 0.0f;
    }
}

static void decode_gp_uv_params(const ThreediGpMaterialTransform *src,
                                 ThreediIRUvParams *dst) {
    dst->style = src->style;
    decode_phase_reg(src->style, src->param, &dst->phase, &dst->reg);
    dst->gen_rate = (float)src->rate / 256.0f;
    dst->start = (float)src->start / 256.0f;
    dst->end = (float)src->end / 256.0f;
}

static void decode_gp_alphagen(const ThreediGpMaterialTransform *src,
                                ThreediIRAlphaGen *dst) {
    dst->style = src->style;
    decode_phase_reg(src->style, src->param, &dst->phase, &dst->reg);
    dst->rate = (float)src->rate / 256.0f;
    dst->start = src->start;  // raw int16
    dst->end = src->end;      // raw int16
}

static void decode_gp_rgbgen(const ThreediGpMaterialTransform *src,
                              const uint32_t *color_rgb,
                              ThreediIRRgbGen *dst) {
    dst->style = src->style;
    decode_phase_reg(src->style, src->param, &dst->phase, &dst->reg);
    dst->rate = (float)src->rate / 256.0f;
    // Colors from color_rgb (low 3 bytes = R,G,B)
    dst->start_color[0] = (float)(color_rgb[0] & 0xFF) / 255.0f;
    dst->start_color[1] = (float)((color_rgb[0] >> 8) & 0xFF) / 255.0f;
    dst->start_color[2] = (float)((color_rgb[0] >> 16) & 0xFF) / 255.0f;
    dst->start_color[3] = 1.0f;
    dst->end_color[0] = (float)(color_rgb[1] & 0xFF) / 255.0f;
    dst->end_color[1] = (float)((color_rgb[1] >> 8) & 0xFF) / 255.0f;
    dst->end_color[2] = (float)((color_rgb[1] >> 16) & 0xFF) / 255.0f;
    dst->end_color[3] = 1.0f;
}

// ============================================================================
// Blend mode + shader name synthesis
// ============================================================================

static ThreediIRBlendMode blend_mode_from_gp_flags(uint32_t shader_flags) {
    if (shader_flags & 0x200)       return THREEDI_IR_BLEND_ALPHA;
    if (shader_flags & 0x80000000)  return THREEDI_IR_BLEND_ADD;
    return THREEDI_IR_BLEND_OPAQUE;
}

static void synthesize_shader_name(const ThreediGpMaterial *sm,
                                    const ThreediIRMaterial *dm,
                                    int is_skinned,
                                    char *out, size_t out_size) {
    uint32_t flags = sm->shader_flags;
    uint32_t stype = flags & 0xFF;
    // Types 1-6: bump from diffuse alpha.  Types 8-12: bump from MDT file.
    // Type 7: phong without bump.  Type 0: no advanced shading.
    int has_bump = (stype >= 1 && stype <= 6) || (stype >= 8);
    int has_mdt = (stype >= 8);
    int has_detail = dm->texture_count >= 2;
    int is_glass = dm->is_glass;
    int is_emissive = (sm->emissive_color != 0);
    int has_uv_anim = (sm->mapfunc_u.style != 0 || sm->mapfunc_v.style != 0);

    // Blend suffix
    const char *blend;
    if (flags & 0x200)            blend = "_AB";
    else if (flags & 0x80000000)  blend = "_AD";
    else                          blend = "_OP";

    if (is_glass && (!has_bump || dm->texture_count == 0)) {
        // Glass with no texture: bump is meaningless (no alpha channel source)
        if (is_skinned) snprintf(out, out_size, "VS_SKGLASS");
        else            snprintf(out, out_size, "FFP_GLASS");
    } else if (is_skinned) {
        if (has_bump && is_glass) {
            if (has_detail) snprintf(out, out_size, "VS_SKBUMPDIFFT2");
            else            snprintf(out, out_size, "VS_SKBUMPDIFFT");
        } else if (has_bump) {
            if (has_detail) snprintf(out, out_size, "VS_SKBUMPDIFFOBJ2");
            else            snprintf(out, out_size, "VS_SKBUMPDIFFOBJ");
        } else {
            snprintf(out, out_size, "VS_SKBASIC");
        }
    } else if (has_bump) {
        if (is_glass)                   snprintf(out, out_size, "VS_BUMPMIRRT");
        else if (stype == 1) {
            if (has_detail)             snprintf(out, out_size, "VS_DOT3DIFF2");
            else                        snprintf(out, out_size, "VS_DOT3DIFF");
        } else if (has_mdt) {
            snprintf(out, out_size, "VS_PHONGT_MDT");
        } else {
            snprintf(out, out_size, "VS_PHONGT");
        }
    } else if (stype == 7) {
        // Phong without bumpmap, diffuse only
        const char *tex = has_detail ? "FF_MT" : "FF_ST";
        snprintf(out, out_size, "%s%s", tex, blend);
    } else {
        // Fixed-function: FF_ST or FF_MT
        const char *tex = has_detail ? "FF_MT" : "FF_ST";
        const char *lum = is_emissive ? "_LUM" : "";
        snprintf(out, out_size, "%s%s%s", tex, blend, lum);
    }

    // Append #UV suffix if UV animation active (only for FF_ shaders)
    if (has_uv_anim && strncmp(out, "FF_", 3) == 0) {
        size_t len = strlen(out);
        if (len + 3 < out_size) strcat(out, "#UV");
    }
}

// ============================================================================
// Conversion
// ============================================================================

static int convert_lod(const ThreediGpFile *gp, size_t lod_idx, ThreediIRLod *dst) {
    if (lod_idx >= gp->rmodel_count) return -1;
    const ThreediGpRModel *rm = &gp->rmodels[lod_idx];

    // Convert Q16.16 LOD threshold to integer distance
    if (lod_idx < 3) {
        dst->threshold = (int32_t)(gp->header.lod_thresholds[lod_idx] >> 16);
    } else {
        dst->threshold = 0;
    }

    // Count total triangulated indices needed
    size_t total_indices = 0;
    for (size_t p = 0; p < rm->poly_count; ++p) {
        const ThreediGpVariablePoly *poly = &rm->polys[p];
        if (poly->topology == 0) {
            // List: indices are already triangles
            total_indices += (poly->index_count / 3) * 3;
        } else {
            // Strip: estimate worst case (each index after first two makes a triangle)
            if (poly->index_count >= 3) {
                total_indices += (poly->index_count - 2) * 3;
            }
        }
    }

    // Allocate indices
    if (total_indices > 0) {
        dst->indices = (uint16_t *)malloc(total_indices * sizeof(uint16_t));
        if (!dst->indices) return -1;
    }

    // Convert vertices
    dst->vertex_count = gp->rvert_count;
    if (dst->vertex_count > 0) {
        dst->vertices = (ThreediIRVertex *)calloc(dst->vertex_count, sizeof(ThreediIRVertex));
        if (!dst->vertices) return -1;

        for (size_t i = 0; i < dst->vertex_count; ++i) {
            const ThreediGpRVert *sv = &gp->rverts[i];
            ThreediIRVertex *dv = &dst->vertices[i];

            memcpy(dv->position, sv->position, sizeof(float) * 3);
            memcpy(dv->normal, sv->normal, sizeof(float) * 3);
            memcpy(dv->uv0, sv->uv0, sizeof(float) * 2);
            memcpy(dv->uv1, sv->uv1, sizeof(float) * 2);
            memcpy(dv->tangent, sv->tangent, sizeof(float) * 3);

            if (sv->is_skinned) {
                // Normalize weights
                float sum = sv->bone_weights[0] + sv->bone_weights[1] + sv->bone_weights[2];
                if (sum > 0.0f) {
                    dv->bone_weights[0] = sv->bone_weights[0] / sum;
                    dv->bone_weights[1] = sv->bone_weights[1] / sum;
                    dv->bone_weights[2] = sv->bone_weights[2] / sum;
                } else {
                    dv->bone_weights[0] = 1.0f;
                }
                dv->bone_weights[3] = 0.0f;
                memcpy(dv->bone_indices, sv->bone_indices, 4);
            } else {
                dv->bone_weights[0] = 1.0f;
            }
        }
    }

    // Convert parts (subobjects)
    dst->part_count = rm->subobject_count;
    if (dst->part_count > 0) {
        dst->parts = (ThreediIRPart *)calloc(dst->part_count, sizeof(ThreediIRPart));
        if (!dst->parts) return -1;

        for (size_t i = 0; i < rm->subobject_count; ++i) {
            const ThreediGpSubObject *ss = &rm->subobjects[i];
            ThreediIRPart *dp = &dst->parts[i];

            dp->parent_index = ss->parent;
            memcpy(dp->abs_position, ss->abs, sizeof(float) * 3);

            // GP rel values are not simple parent-relative offsets like modern,
            // so derive rel from abs positions instead
            if (ss->parent >= 0 && (size_t)ss->parent < rm->subobject_count) {
                const ThreediGpSubObject *ps = &rm->subobjects[ss->parent];
                dp->rel_position[0] = ss->abs[0] - ps->abs[0];
                dp->rel_position[1] = ss->abs[1] - ps->abs[1];
                dp->rel_position[2] = ss->abs[2] - ps->abs[2];
            } else {
                memcpy(dp->rel_position, ss->abs, sizeof(float) * 3);
            }
            // primitive_start and primitive_count will be filled below
        }
    }

    // Convert primitives - triangulate all polys
    dst->primitive_count = rm->poly_count;
    if (dst->primitive_count > 0) {
        dst->primitives = (ThreediIRPrimitive *)calloc(dst->primitive_count, sizeof(ThreediIRPrimitive));
        if (!dst->primitives) return -1;

        // Track primitive counts per part
        int32_t *part_prim_counts = (int32_t *)calloc(dst->part_count > 0 ? dst->part_count : 1, sizeof(int32_t));
        if (!part_prim_counts) return -1;

        size_t index_offset = 0;
        for (size_t p = 0; p < rm->poly_count; ++p) {
            const ThreediGpVariablePoly *sp = &rm->polys[p];
            ThreediIRPrimitive *dp = &dst->primitives[p];

            dp->material_index = sp->material_index;
            dp->part_index = sp->subobject_index;
            dp->vertex_offset = (uint32_t)sp->first_vertex;

            // Triangulate indices
            uint16_t *out_indices = dst->indices + index_offset;
            size_t tri_indices;

            if (sp->topology == 0) {
                tri_indices = triangulate_list(sp->indices, sp->index_count,
                                               sp->first_vertex, out_indices);
            } else {
                tri_indices = triangulate_strip(sp->indices, sp->index_count,
                                                sp->first_vertex, out_indices);
            }

            dp->index_offset = (uint32_t)index_offset;
            dp->index_count = (uint32_t)tri_indices;
            dp->topology = THREEDI_IR_TOPOLOGY_TRIANGLES; // Always triangles after conversion

            // Copy bone table
            memcpy(dp->bone_table, sp->bone_table, 16);
            dp->bone_table_length = (uint8_t)sp->bone_table_length;

            // Track vertex range for vertex_count
            uint32_t max_vert = 0;
            for (size_t i = 0; i < tri_indices; ++i) {
                uint32_t v = out_indices[i];
                if (v > max_vert) max_vert = v;
            }
            dp->vertex_count = max_vert + 1;

            index_offset += tri_indices;

            // Count primitives per part
            if (sp->subobject_index >= 0 && (size_t)sp->subobject_index < dst->part_count) {
                part_prim_counts[sp->subobject_index]++;
            }
        }

        // Assign primitive starts to parts
        int32_t prim_offset = 0;
        for (size_t i = 0; i < dst->part_count; ++i) {
            dst->parts[i].primitive_start = prim_offset;
            dst->parts[i].primitive_count = part_prim_counts[i];
            dst->parts[i].opaque_count = part_prim_counts[i]; // GP doesn't distinguish
            dst->parts[i].alpha_count = 0;
            prim_offset += part_prim_counts[i];
        }

        free(part_prim_counts);
        dst->index_count = index_offset;
    }

    return 0;
}

static int convert_materials(const ThreediGpFile *gp, ThreediModelIR *ir) {
    if (gp->rmodel_count == 0) return 0;
    const ThreediGpRModel *rm = &gp->rmodels[0];

    ir->material_count = rm->material_count;
    if (ir->material_count == 0) return 0;

    ir->materials = (ThreediIRMaterial *)calloc(ir->material_count, sizeof(ThreediIRMaterial));
    if (!ir->materials) return -1;

    for (size_t i = 0; i < ir->material_count; ++i) {
        const ThreediGpMaterial *sm = &rm->materials[i];
        ThreediIRMaterial *dm = &ir->materials[i];

        dm->index = (int32_t)i;

        // Look up texture info from material lookup table via render_lookup index
        // render_lookup is 4 packed byte indices (one per LOD); byte[0] = primary LOD
        uint32_t lookup_idx = sm->render_lookup & 0xFF;
        dm->texture_count = 0;

        // Primary texture from material name
        if (sm->texture_name[0] != '\0') {
            memcpy(dm->textures[0].name, sm->texture_name, sizeof(dm->textures[0].name) - 1);
            dm->textures[0].slot = THREEDI_IR_TEX_SLOT_DIFFUSE;
            dm->texture_count = 1;
        }

        // Check material lookup table for additional texture slots (e.g. lightmap)
        if (lookup_idx < gp->material_lookup_count) {
            const ThreediGpMaterialLookup *ml = &gp->material_lookups[lookup_idx];
            // If the lookup points to a different texture (lightmap/occmap), add as second slot
            if (ml->slot_type == 0x04 && ml->texture_name[0] != '\0') {
                // This is a lightmap entry — the primary diffuse is already set from material name
                // Look for a diffuse entry that might be adjacent
            }
            // Scan all lookups for entries referencing this material's render_lookup
            // to find multi-texture combos (diffuse + lightmap)
            for (size_t li = 0; li < gp->material_lookup_count; ++li) {
                const ThreediGpMaterialLookup *entry = &gp->material_lookups[li];
                // Skip if same as primary or no name
                if (entry->texture_name[0] == '\0') continue;
                if (li == lookup_idx) continue;
                // Check if this entry's seq_index matches and is a lightmap for same material
                if (entry->slot_type == 0x04 && entry->seq_index == ml->seq_index + 1 &&
                    dm->texture_count < 8) {
                    memcpy(dm->textures[dm->texture_count].name, entry->texture_name,
                           sizeof(dm->textures[0].name) - 1);
                    dm->textures[dm->texture_count].slot = THREEDI_IR_TEX_SLOT_DETAIL;
                    dm->texture_count++;
                    break;
                }
            }
        }

        // For MDT bump-enabled materials (shader_type 8-12), synthesize MDT normal map slot.
        // Types 1-6 use the diffuse texture's alpha channel as a bump map instead.
        // The engine derives the MDT filename from the diffuse texture at runtime.
        uint32_t shader_type = sm->shader_flags & 0xFF;
        if (shader_type >= 8 && dm->texture_count > 0 && dm->texture_count < 8) {
            const char *diffuse_name = (const char *)dm->textures[0].name;
            if (diffuse_name[0] != '\0') {
                char mdt_base[13]; // max 12 chars + null so "%s.mdt" fits in 17
                memset(mdt_base, 0, sizeof(mdt_base));
                strncpy(mdt_base, diffuse_name, 12);
                char *dot = strrchr(mdt_base, '.');
                if (dot) *dot = '\0';

                ThreediIRMaterialTexture *nt = &dm->textures[dm->texture_count];
                snprintf((char *)nt->name, sizeof(nt->name), "%s.mdt", mdt_base);
                nt->slot = THREEDI_IR_TEX_SLOT_NORMAL;
                nt->type = THREEDI_TEX_TYPE_NORMAL_MDT;
                nt->flags = 0;
                nt->frame = 0;
                dm->texture_count++;
            }
        }

        // Derive blend mode from GP shader_flags
        dm->blend_mode = blend_mode_from_gp_flags(sm->shader_flags);

        // Convert GP flags to IR flags
        dm->flags = 0;

        // GP alpha blend -> ALPHA_TEST (engine uses clip-style alpha for GP _AB materials)
        if (dm->blend_mode == THREEDI_IR_BLEND_ALPHA) {
            dm->flags |= THREEDI_IR_MATERIAL_FLAG_ALPHA_TEST;
            dm->alpha_threshold = 0.5f;
        }

        // GP render_attributes 0x2 = two-sided
        if ((sm->render_attributes & 0x2u) != 0) {
            dm->flags |= THREEDI_IR_MATERIAL_FLAG_TWO_SIDED;
        }

        // GP emissive_color nonzero = emissive material
        if (sm->emissive_color != 0) {
            dm->flags |= THREEDI_IR_MATERIAL_FLAG_EMISSIVE;
        }

        // GP tex_addressing_mode: bit 0 = U clamp, bit 8 = V clamp
        // If either axis is clamped, mark texture as clamped
        if (dm->texture_count > 0 && (sm->tex_addressing_mode & 0x0101u) != 0) {
            dm->textures[0].flags |= THREEDI_TEX_FLAG_CLAMPED;
        }

        // Decode shader animation parameters
        decode_gp_uv_params(&sm->mapfunc_u, &dm->u_params);
        decode_gp_uv_params(&sm->mapfunc_v, &dm->v_params);
        decode_gp_alphagen(&sm->alphagen, &dm->alpha_gen);
        decode_gp_rgbgen(&sm->rgbgen, sm->color_rgb, &dm->rgb_gen);

        // Material properties
        dm->specular_intensity = sm->specular_intensity;
        dm->luminosity = sm->luminosity;
        dm->emissive_color = sm->emissive_color;
        dm->emissive_type = (sm->emissive_color != 0) ? 2 : 0;
        dm->u_tiling = sm->u_tiling;
        dm->v_tiling = sm->v_tiling;

        // Reflection data
        dm->reflect_color[0] = sm->reflect_r;
        dm->reflect_color[1] = sm->reflect_g;
        dm->reflect_color[2] = sm->reflect_b;
        dm->reflect_color[3] = sm->reflect_alpha;
        // Glass detection: reflect_type set, OR no texture + unused lookup (0xFFFFFF)
        dm->is_glass = (sm->reflect_type != 0) ? 1 :
                        (sm->texture_name[0] == '\0' && (sm->render_lookup & 0xFFFFFF) == 0xFFFFFF) ? 1 : 0;

        // Synthesize equivalent 3DI3 shader name from GP material properties
        // NOTE: must come after is_glass is set, since shader name depends on it
        {
            int is_skinned = (gp->header.mesh_type == THREEDI_GP_MESH_SKINNED);
            synthesize_shader_name(sm, dm, is_skinned, dm->shader_name, sizeof(dm->shader_name));
        }

        // Texture animation: GP doesn't have a dedicated anim struct,
        // but animated textures are flagged via texture flags
        dm->animation.num_frames = 0;
        dm->animation.animation_type = 0;
        dm->animation.cycle_frame_time = 0;
    }

    return 0;
}

static void assign_surface_types(const ThreediGpFile *gp, ThreediModelIR *ir) {
    if (!gp->collision || gp->collision->object_count == 0 ||
        gp->collision->face_count == 0 || gp->rmodel_count == 0)
        return;

    const ThreediGpCollision *col = gp->collision;
    const ThreediGpRModel *rm = &gp->rmodels[0];

    // Set default surface_type (0x01 = Dirt) for all materials
    for (size_t i = 0; i < ir->material_count; ++i)
        ir->materials[i].surface_type = 0x01;

    // Walk collision objects; faces are a flat array consumed in order.
    // Each collision object has a uniform surface_type across all its faces.
    size_t face_cursor = 0;
    for (int32_t obj_idx = 0; obj_idx < col->object_count; ++obj_idx) {
        const ThreediGpCollisionObject *obj = &col->objects[obj_idx];
        if (obj->face_count <= 0 || face_cursor >= (size_t)col->face_count) {
            face_cursor += (size_t)obj->face_count;
            continue;
        }

        uint8_t st = col->faces[face_cursor].surface_type;

        // Assert all faces in this object share the same surface_type
        for (int32_t f = 1; f < obj->face_count; ++f) {
            size_t fi = face_cursor + (size_t)f;
            if (fi < (size_t)col->face_count && col->faces[fi].surface_type != st) {
                opennova::io::logf(opennova::io::LogLevel::kWarn,
		"WARNING: GP collision object %d has mixed surface_types "
                        "(face 0: 0x%02X, face %d: 0x%02X)",
                        obj_idx, st, f, col->faces[fi].surface_type);
                break;
            }
        }
        face_cursor += (size_t)obj->face_count;

        // Assign to all materials used by the parent subobject's render polys
        int32_t sub_idx = obj->parent_subobject;
        for (size_t p = 0; p < rm->poly_count; ++p) {
            if (rm->polys[p].subobject_index == sub_idx) {
                int32_t mi = rm->polys[p].material_index;
                if (mi >= 0 && (size_t)mi < ir->material_count)
                    ir->materials[mi].surface_type = st;
            }
        }
    }
}

static int convert_userpoints(const ThreediGpFile *gp, ThreediModelIR *ir) {
    ir->userpoint_count = gp->userpoint_count;
    if (ir->userpoint_count == 0) return 0;

    ir->userpoints = (ThreediIRUserPoint *)calloc(ir->userpoint_count, sizeof(ThreediIRUserPoint));
    if (!ir->userpoints) return -1;

    for (size_t i = 0; i < ir->userpoint_count; ++i) {
        const ThreediGpUserPoint *su = &gp->userpoints[i];
        ThreediIRUserPoint *du = &ir->userpoints[i];

        memcpy(du->name, su->name, sizeof(du->name) - 1);

        // Convert from fixed-point (16.16) to float with swizzle matching modern.
        // Userpoints need the side axis mirrored here so NovaObjectData's
        // render-space -X transform preserves the authored driver/passenger side.
        du->position[0] = -(float)su->y / 65536.0f;
        du->position[1] = (float)su->z / 65536.0f;
        du->position[2] = (float)su->x / 65536.0f;

        du->direction[0] = -(float)su->rot_y / 65536.0f;
        du->direction[1] = (float)su->rot_z / 65536.0f;
        du->direction[2] = (float)su->rot_x / 65536.0f;

        du->part_index = su->parent_subobject;
        du->type_code = su->type_code;
    }

    return 0;
}

static int convert_lights(const ThreediGpFile *gp, ThreediModelIR *ir) {
    ir->light_count = gp->light_count;
    if (ir->light_count == 0) return 0;

    ir->lights = (ThreediIRLight *)calloc(ir->light_count, sizeof(ThreediIRLight));
    if (!ir->lights) return -1;

    for (size_t i = 0; i < ir->light_count; ++i) {
        const ThreediGpLight *sl = &gp->lights[i];
        ThreediIRLight *dl = &ir->lights[i];

        memcpy(dl->offset, sl->position, sizeof(float) * 3);
        dl->attenuation_start = sl->attenuation_start;
        dl->attenuation_end = sl->attenuation_end;

        /* GP stores the authored RGB color as B,G,R, like 3DI3 LGHT. */
        dl->color_start[0] = (float)sl->color_start[2] / 255.0f;
        dl->color_start[1] = (float)sl->color_start[1] / 255.0f;
        dl->color_start[2] = (float)sl->color_start[0] / 255.0f;

        dl->color_end[0] = (float)sl->color_end[2] / 255.0f;
        dl->color_end[1] = (float)sl->color_end[1] / 255.0f;
        dl->color_end[2] = (float)sl->color_end[0] / 255.0f;

        dl->style = sl->style;
        dl->phase = sl->phase;
        dl->rate = sl->rate;
        dl->part_index = sl->part_index;
        dl->flags = 0;
    }

    return 0;
}

int threedi_ir_from_gp(const ThreediGpFile *gp, ThreediModelIR *out) {
    if (!gp || !out) return -1;

    threedi_ir_init(out);

    // Copy header info
    memcpy(out->name, gp->header.name, sizeof(out->name) - 1);

    switch (gp->header.mesh_type) {
        case THREEDI_GP_MESH_BASIC:
            out->source_format = THREEDI_IR_SOURCE_GPM;
            out->mesh_type = THREEDI_IR_MESH_BASIC;
            break;
        case THREEDI_GP_MESH_STATIC:
            out->source_format = THREEDI_IR_SOURCE_GPS;
            out->mesh_type = THREEDI_IR_MESH_STATIC;
            break;
        case THREEDI_GP_MESH_SKINNED:
            out->source_format = THREEDI_IR_SOURCE_GPP;
            out->mesh_type = THREEDI_IR_MESH_SKINNED;
            break;
        default:
            out->source_format = THREEDI_IR_SOURCE_UNKNOWN;
            out->mesh_type = THREEDI_IR_MESH_INVALID;
            break;
    }

    // Convert LODs
    out->lod_count = gp->rmodel_count;
    if (out->lod_count > 0) {
        out->lods = (ThreediIRLod *)calloc(out->lod_count, sizeof(ThreediIRLod));
        if (!out->lods) goto error;

        for (size_t i = 0; i < out->lod_count; ++i) {
            if (convert_lod(gp, i, &out->lods[i]) != 0) goto error;
        }
    }

    // Convert materials (from first LOD)
    if (convert_materials(gp, out) != 0) goto error;

    // Assign collision surface types to materials
    assign_surface_types(gp, out);

    // Convert userpoints
    if (convert_userpoints(gp, out) != 0) goto error;

    // Convert lights
    if (convert_lights(gp, out) != 0) goto error;

    // Convert occlusion
    if (gp->occlusion) {
        const ThreediGpOcclusion *src = gp->occlusion;
        ThreediIROcclusion *occ = (ThreediIROcclusion *)calloc(1, sizeof(ThreediIROcclusion));
        if (!occ) goto error;
        out->occlusion = occ;

        // Convert vertices
        occ->vertex_count = src->vertex_count;
        if (occ->vertex_count > 0) {
            occ->vertices = (ThreediIROcclusionVertex *)calloc(occ->vertex_count, sizeof(ThreediIROcclusionVertex));
            if (!occ->vertices) goto error;
            for (size_t i = 0; i < occ->vertex_count; ++i) {
                memcpy(occ->vertices[i].position, src->vertices[i].position, sizeof(float) * 3);
            }
        }

        // Convert faces
        occ->face_count = src->face_count;
        if (occ->face_count > 0) {
            occ->faces = (ThreediIROcclusionFace *)calloc(occ->face_count, sizeof(ThreediIROcclusionFace));
            if (!occ->faces) goto error;
            for (size_t i = 0; i < occ->face_count; ++i) {
                occ->faces[i].raw_indices = src->faces[i].raw_indices;
                occ->faces[i].edge_data = src->faces[i].edge_data;
                occ->faces[i].other_edge_data = src->faces[i].other_edge_data;
            }
        }

        // Convert planes
        occ->plane_count = src->plane_count;
        if (occ->plane_count > 0) {
            occ->planes = (ThreediIROcclusionPlane *)calloc(occ->plane_count, sizeof(ThreediIROcclusionPlane));
            if (!occ->planes) goto error;
            for (size_t i = 0; i < occ->plane_count; ++i) {
                memcpy(occ->planes[i].normal, src->planes[i].normal, sizeof(float) * 3);
                occ->planes[i].radius = src->planes[i].radius;
            }
        }

        // Convert objects with cursor-based indexing
        occ->object_count = src->object_count;
        if (occ->object_count > 0) {
            occ->objects = (ThreediIROcclusionObject *)calloc(occ->object_count, sizeof(ThreediIROcclusionObject));
            if (!occ->objects) goto error;

            size_t vert_cursor = 0, plane_cursor = 0, face_cursor = 0;
            for (size_t i = 0; i < occ->object_count; ++i) {
                const ThreediGpOcclusionObject *so = &src->objects[i];
                ThreediIROcclusionObject *d = &occ->objects[i];

                d->type = (int32_t)so->type;
                d->parent_subobject_index = (int32_t)so->parent_subobject_index;
                d->connecting_subobject = (int32_t)so->connecting_subobject;
                memcpy(d->position, so->center, sizeof(float) * 3);
                d->radius = so->radius;
                d->glow_scale = 0.0f;
                d->num_vertices = so->num_vertices;
                d->num_planes = so->num_planes;
                d->face_count = so->num_faces;

                d->vertex_start = (int32_t)vert_cursor;
                d->plane_start = (int32_t)plane_cursor;
                d->face_start = (int32_t)face_cursor;

                vert_cursor += (size_t)so->num_vertices;
                plane_cursor += (size_t)so->num_planes;
                face_cursor += (size_t)so->num_faces;
            }
        }
    }

    // Convert collision
    if (gp->collision) {
        const ThreediGpCollision *src = gp->collision;
        ThreediIRCollision *col = (ThreediIRCollision *)calloc(1, sizeof(ThreediIRCollision));
        if (!col) goto error;
        out->collision = col;
        if (src->object_count < 0 || src->vertex_count < 0 || src->normal_count < 0 ||
            src->face_count < 0 || src->plane_count < 0 || src->volume_count < 0 ||
            src->translation_count < 0 ||
            (src->object_count > 0 && !src->objects) ||
            (src->vertex_count > 0 && !src->vertices) ||
            (src->normal_count > 0 && !src->normals) ||
            (src->face_count > 0 && !src->faces) ||
            (src->plane_count > 0 && !src->planes) ||
            (src->volume_count > 0 && !src->volumes) ||
            (src->translation_count > 0 && !src->translations)) goto error;

        // Copy model bounds from header
        memcpy(col->model_min, src->min, sizeof(float) * 3);
        memcpy(col->model_max, src->max, sizeof(float) * 3);
        memcpy(col->model_center, src->mid, sizeof(float) * 3);

        // Derive int32→float scale from header bounds vs first object AABB
        // The int32 values are multiplied by (1 << shift), so we find the ratio
        float inv_scale = 1.0f / 65536.0f; // default 16.16 fixed-point
        if (src->object_count > 0) {
            const ThreediGpCollisionObject *obj0 = &src->objects[0];
            // Try to derive scale from header max vs object max
            float hmax = src->max[0];
            int32_t imax = obj0->bbox_max_x;
            if (hmax != 0.0f && imax != 0) {
                inv_scale = hmax / (float)imax;
            }
        }

        // Preserve the GP collision-object table in the canonical IR, matching
        // the modern COBJ converter. Runtime sections use the parent subobject
        // to attach collision to the correct animated model part.
        col->object_count = (size_t)src->object_count;
        if (col->object_count > 0) {
            col->objects = (ThreediIRCollisionObject *)calloc(
                col->object_count, sizeof(ThreediIRCollisionObject));
            if (!col->objects) goto error;
            for (size_t i = 0; i < col->object_count; ++i) {
                const ThreediGpCollisionObject *so = &src->objects[i];
                ThreediIRCollisionObject *d = &col->objects[i];
                d->num_vertices = so->vertex_count;
                d->num_faces = so->face_count;
                d->num_planes = so->normal_count;
                d->num_bounding_volumes = so->volume_count;
                d->parent_subobject_index = so->parent_subobject;
                d->offset[0] = so->translation[0];
                d->offset[1] = so->translation[1];
                d->offset[2] = so->translation[2];
                d->min[0] = so->bbox_min_x;
                d->min[1] = so->bbox_min_y;
                d->min[2] = so->bbox_min_z;
                d->max[0] = so->bbox_max_x;
                d->max[1] = so->bbox_max_y;
                d->max[2] = so->bbox_max_z;
                for (int k = 0; k < 3; ++k) d->mid[k] = so->center[k];
                d->radius = so->bounding_sphere_radius;
            }
        }

        // Convert vertices (int16 / 256.0)
        col->vertex_count = (size_t)src->vertex_count;
        if (col->vertex_count > 0) {
            col->vertices = (ThreediIRCollisionVertex *)calloc(col->vertex_count, sizeof(ThreediIRCollisionVertex));
            if (!col->vertices) goto error;
            for (size_t i = 0; i < col->vertex_count; ++i) {
                col->vertices[i].position[0] = (float)src->vertices[i].x / 256.0f;
                col->vertices[i].position[1] = (float)src->vertices[i].y / 256.0f;
                col->vertices[i].position[2] = (float)src->vertices[i].z / 256.0f;
            }
        }

        // GP already stores collision normals as exact signed Q14 integers.
        col->normal_count = (size_t)src->normal_count;
        if (col->normal_count > 0) {
            col->normals = (ThreediIRCollisionNormal *)calloc(
                col->normal_count, sizeof(ThreediIRCollisionNormal));
            if (!col->normals) goto error;
            for (size_t i = 0; i < col->normal_count; ++i) {
                col->normals[i].normal_q14[0] = src->normals[i].nx;
                col->normals[i].normal_q14[1] = src->normals[i].ny;
                col->normals[i].normal_q14[2] = src->normals[i].nz;
                col->normals[i].dominant_axis = src->normals[i].dominant_axis;
            }
        }

        col->face_count = (size_t)src->face_count;
        if (col->face_count > 0) {
            col->faces = (ThreediIRCollisionFace *)calloc(
                col->face_count, sizeof(ThreediIRCollisionFace));
            if (!col->faces) goto error;
            for (size_t i = 0; i < col->face_count; ++i) {
                const ThreediGpCollisionFace *sf = &src->faces[i];
                ThreediIRCollisionFace *df = &col->faces[i];
                for (int k = 0; k < 3; ++k)
                    df->vert_index[k] = (int16_t)sf->vertex_indices[k];
                df->normal_index = sf->normal_index;
                df->plane_dist_fp16 = sf->plane_d;
                df->min_fp16[0] = sf->bbox_min_x;
                df->min_fp16[1] = sf->bbox_min_y;
                df->min_fp16[2] = sf->bbox_min_z;
                df->max_fp16[0] = sf->bbox_max_x;
                df->max_fp16[1] = sf->bbox_max_y;
                df->max_fp16[2] = sf->bbox_max_z;
                df->material_flags = (uint32_t)sf->surface_flags;
                df->poly_type = sf->surface_type;
            }
            size_t face_cursor = 0;
            size_t normal_base = 0;
            for (size_t obj_idx = 0; obj_idx < col->object_count; ++obj_idx) {
                const ThreediGpCollisionObject *obj = &src->objects[obj_idx];
                for (int32_t f = 0; f < obj->face_count && face_cursor < col->face_count;
                     ++f, ++face_cursor) {
                    const ThreediGpCollisionFace *sf = &src->faces[face_cursor];
                    ThreediIRCollisionFace *df = &col->faces[face_cursor];
                    const size_t ni = normal_base + (size_t)sf->normal_index;
                    if (sf->normal_index >= 0 && ni < col->normal_count) {
                        const ThreediIRCollisionNormal *normal = &col->normals[ni];
                        memcpy(df->normal, normal->normal_q14, sizeof(df->normal));
                        df->dominate_axis = normal->dominant_axis;
                    }
                }
                if (obj->normal_count > 0) normal_base += (size_t)obj->normal_count;
            }
        }

        // Convert planes (int32 / 65536.0 for normal components)
        col->plane_count = (size_t)src->plane_count;
        if (col->plane_count > 0) {
            col->planes = (ThreediIRCollisionPlane *)calloc(col->plane_count, sizeof(ThreediIRCollisionPlane));
            if (!col->planes) goto error;
            for (size_t i = 0; i < col->plane_count; ++i) {
                col->planes[i].normal[0] = (float)src->planes[i].a / 65536.0f;
                col->planes[i].normal[1] = (float)src->planes[i].b / 65536.0f;
                col->planes[i].normal[2] = (float)src->planes[i].c / 65536.0f;
                col->planes[i].distance = (float)src->planes[i].d / 65536.0f;
                col->planes[i].flags = 0;
            }
        }

        // Convert volumes with cursor-based object walk (matching modern path)
        col->volume_count = (size_t)src->volume_count;
        if (col->volume_count > 0) {
            col->volumes = (ThreediIRCollisionVolume *)calloc(col->volume_count, sizeof(ThreediIRCollisionVolume));
            if (!col->volumes) goto error;

            // First pass: copy per-volume data
            for (size_t i = 0; i < col->volume_count; ++i) {
                const ThreediGpCollisionVolume *sv = &src->volumes[i];
                ThreediIRCollisionVolume *dv = &col->volumes[i];

                dv->type = sv->type;
                dv->flags = sv->flags;
                dv->min[0] = (float)sv->bbox_min_x * inv_scale;
                dv->min[1] = (float)sv->bbox_min_y * inv_scale;
                dv->min[2] = (float)sv->bbox_min_z * inv_scale;
                dv->max[0] = (float)sv->bbox_max_x * inv_scale;
                dv->max[1] = (float)sv->bbox_max_y * inv_scale;
                dv->max[2] = (float)sv->bbox_max_z * inv_scale;
                dv->plane_count = sv->plane_count;
                dv->object_index = -1;
            }

            // Second pass: walk collision objects, assign part_index + object_index
            size_t vol_cursor = 0;
            size_t plane_cursor = 0;
            for (size_t obj_idx = 0; obj_idx < (size_t)src->object_count; ++obj_idx) {
                const ThreediGpCollisionObject *obj = &src->objects[obj_idx];
                if (obj->volume_count < 0 ||
                    (size_t)obj->volume_count > col->volume_count - vol_cursor) goto error;
                for (int32_t v = 0; v < obj->volume_count; ++v) {
                    ThreediIRCollisionVolume *volume = &col->volumes[vol_cursor];
                    if (volume->plane_count <= 0 ||
                        (size_t)volume->plane_count > col->plane_count - plane_cursor)
                        goto error;
                    volume->part_index = obj->parent_subobject;
                    volume->object_index = (int32_t)obj_idx;
                    volume->plane_start = (int32_t)plane_cursor;
                    plane_cursor += (size_t)volume->plane_count;
                    ++vol_cursor;
                }
            }
            if (vol_cursor != col->volume_count || plane_cursor != col->plane_count)
                goto error;
        }

        col->translation_count = (size_t)src->translation_count;
        if (col->translation_count > 0) {
            col->translations = (ThreediIRCollisionTranslation *)calloc(
                col->translation_count, sizeof(ThreediIRCollisionTranslation));
            if (!col->translations) goto error;
            for (size_t i = 0; i < col->translation_count; ++i) {
                col->translations[i].translation[0] = src->translations[i].x;
                col->translations[i].translation[1] = src->translations[i].y;
                col->translations[i].translation[2] = src->translations[i].z;
            }
        }
    }

    // Convert part animations per-LOD
    #define GP_READ_XFORM(dst, src) do { \
        (dst).control = (src).control; \
        (dst).control_param = (src).param; \
        (dst).rate = (src).rate; \
        (dst).start = (src).start; \
        (dst).end = (src).end; \
    } while(0)

    for (size_t li = 0; li < out->lod_count; ++li) {
        if (li >= gp->rmodel_count) break;
        const ThreediGpRModel *rm = &gp->rmodels[li];
        if (rm->part_animation_count == 0) continue;

        ThreediIRLod *dst_lod = &out->lods[li];
        dst_lod->part_animation_count = rm->part_animation_count;
        dst_lod->part_animations = (ThreediIRPartAnimation *)calloc(dst_lod->part_animation_count,
                                                                      sizeof(ThreediIRPartAnimation));
        if (!dst_lod->part_animations) goto error;

        for (size_t i = 0; i < dst_lod->part_animation_count; ++i) {
            const ThreediGpPartAnimation *sp = &rm->part_animations[i];
            ThreediIRPartAnimation *dp = &dst_lod->part_animations[i];

            dp->flags = sp->flags;
            dp->parent_part = sp->parent_subobject;
            dp->part_index = sp->subobject_index;
            dp->matrix_index = sp->matrix_index;
            dp->matrix_offset = sp->matrix_offset;
            dp->bind_matrix_index = sp->bind_matrix_index;

            GP_READ_XFORM(dp->rotation_x, sp->rot_x);
            GP_READ_XFORM(dp->rotation_y, sp->rot_y);
            GP_READ_XFORM(dp->rotation_z, sp->rot_z);
            GP_READ_XFORM(dp->scale_x, sp->scale_x);
            GP_READ_XFORM(dp->scale_y, sp->scale_y);
            GP_READ_XFORM(dp->scale_z, sp->scale_z);
            GP_READ_XFORM(dp->translation, sp->translate);
        }
    }

    #undef GP_READ_XFORM

    // Convert control registers
    if (gp->control_register_count > 0) {
        out->control_register_count = gp->control_register_count;
        out->control_registers = (ThreediIRControlRegister *)calloc(out->control_register_count,
                                                                      sizeof(ThreediIRControlRegister));
        if (!out->control_registers) goto error;
        for (size_t i = 0; i < out->control_register_count; ++i) {
            memset(out->control_registers[i].name, 0, sizeof(out->control_registers[i].name));
            memcpy(out->control_registers[i].name, gp->control_registers[i].name, 16);
        }
    }

    // Convert matrices
    if (gp->matrix_count > 0) {
        out->matrix_count = gp->matrix_count;
        out->matrices = (ThreediIRMatrix *)calloc(out->matrix_count, sizeof(ThreediIRMatrix));
        if (!out->matrices) goto error;
        for (size_t i = 0; i < out->matrix_count; ++i) {
            memcpy(out->matrices[i].m, gp->matrices[i].m, sizeof(float) * 16);
        }
    }

    return 0;

error:
    threedi_ir_free(out);
    return -1;
}
