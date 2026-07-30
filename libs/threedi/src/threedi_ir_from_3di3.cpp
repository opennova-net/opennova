// Convert Modern (3DI3) Threedi3di3 to ThreediModelIR

#include "threedi/threedi_ir.h"
#include "threedi/threedi_3di3.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int convert_lod(const ThreediLod *src, const Threedi3di3 *model, ThreediIRLod *dst) {
    // Copy threshold
    dst->threshold = src->lod_threshold;

    // Convert vertices
    dst->vertex_count = src->vertices.count;
    if (dst->vertex_count > 0) {
        dst->vertices = (ThreediIRVertex *)calloc(dst->vertex_count, sizeof(ThreediIRVertex));
        if (!dst->vertices) return -1;

        for (size_t i = 0; i < dst->vertex_count; ++i) {
            const ThreediVertex *sv = &src->vertices.items[i];
            ThreediIRVertex *dv = &dst->vertices[i];

            memcpy(dv->position, sv->position, sizeof(float) * 3);
            memcpy(dv->normal, sv->normal, sizeof(float) * 3);
            memcpy(dv->uv0, sv->uv0, sizeof(float) * 2);
            memcpy(dv->uv1, sv->uv1, sizeof(float) * 2);
            memcpy(dv->tangent, sv->tangent, sizeof(float) * 3);
            memcpy(dv->bitangent, sv->bitangent, sizeof(float) * 3);

            // Bone weights - normalize to 4 weights summing to 1.0
            if (sv->is_skinned) {
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

            dv->flags = sv->flags;
        }
    }

    // Copy indices
    dst->index_count = src->indices.count;
    if (dst->index_count > 0) {
        dst->indices = (uint16_t *)malloc(dst->index_count * sizeof(uint16_t));
        if (!dst->indices) return -1;
        memcpy(dst->indices, src->indices.indices, dst->index_count * sizeof(uint16_t));
    }

    // Convert parts (render objects)
    dst->part_count = src->render_object_count;
    dst->declared_part_count = src->rmdl_render_object_count;
    if (dst->part_count > 0) {
        dst->parts = (ThreediIRPart *)calloc(dst->part_count, sizeof(ThreediIRPart));
        if (!dst->parts) return -1;

        // Build strip-to-part mapping
        size_t current_strip = 0;
        for (size_t i = 0; i < src->render_object_count; ++i) {
            const ThreediRenderObject *ro = &src->render_objects[i];
            ThreediIRPart *part = &dst->parts[i];

            part->parent_index = ro->parent_index;
            memcpy(part->rel_position, ro->rel, sizeof(float) * 3);
            memcpy(part->abs_position, ro->abs, sizeof(float) * 3);
            memcpy(part->bounding_center, ro->bounding_center, sizeof(float) * 3);
            part->bounding_radius = ro->bounding_radius;
            part->primitive_start = (int32_t)current_strip;
            part->opaque_count = ro->num_strips;
            part->alpha_count = ro->num_alpha_strips;
            part->primitive_count = part->opaque_count + part->alpha_count;
            current_strip += (size_t)part->primitive_count;
        }
    }

    // Convert primitives (triangle strips)
    dst->primitive_count = src->strip_count;
    if (dst->primitive_count > 0) {
        dst->primitives = (ThreediIRPrimitive *)calloc(dst->primitive_count, sizeof(ThreediIRPrimitive));
        if (!dst->primitives) return -1;

        // Find which part each strip belongs to
        size_t current_strip = 0;
        for (size_t part_idx = 0; part_idx < src->render_object_count; ++part_idx) {
            const ThreediRenderObject *ro = &src->render_objects[part_idx];
            size_t part_strip_count = (size_t)(ro->num_strips + ro->num_alpha_strips);

            for (size_t s = 0; s < part_strip_count && current_strip < src->strip_count; ++s, ++current_strip) {
                const ThreediTriangleStrip *strip = &src->strips[current_strip];
                ThreediIRPrimitive *prim = &dst->primitives[current_strip];

                prim->material_index = strip->material_index;
                prim->part_index = (int32_t)part_idx;
                prim->index_offset = (uint32_t)strip->index_offset;
                prim->index_count = strip->num_indices;
                prim->vertex_offset = (uint32_t)strip->start_vertex;
                prim->vertex_count = (uint32_t)strip->num_vertices;
                prim->topology = strip->is_strip ? THREEDI_IR_TOPOLOGY_STRIP : THREEDI_IR_TOPOLOGY_TRIANGLES;
                memcpy(prim->min, strip->min, sizeof(float) * 3);
                memcpy(prim->max, strip->max, sizeof(float) * 3);
                memcpy(prim->bone_table, strip->bone_table, 16);
                prim->bone_table_length = (uint8_t)strip->bone_table_length;
            }
        }
    }

    return 0;
}

static ThreediIRBlendMode blend_mode_from_shader_name(const char *name) {
    if (strstr(name, "_AD")) return THREEDI_IR_BLEND_ADD;
    if (strstr(name, "_AB")) return THREEDI_IR_BLEND_ALPHA;
    return THREEDI_IR_BLEND_OPAQUE;
}

static int convert_materials(const Threedi3di3 *model, ThreediModelIR *ir) {
    ir->material_count = model->material_count;
    if (ir->material_count == 0) return 0;

    ir->materials = (ThreediIRMaterial *)calloc(ir->material_count, sizeof(ThreediIRMaterial));
    if (!ir->materials) return -1;

    for (size_t i = 0; i < ir->material_count; ++i) {
        const ThreediMaterial *sm = &model->materials[i];
        ThreediIRMaterial *dm = &ir->materials[i];

        dm->index = sm->index;
        memcpy(dm->shader_name, sm->shader_name, sizeof(dm->shader_name) - 1);
        dm->blend_mode = blend_mode_from_shader_name(sm->shader_name);

        // Convert texture slots
        dm->texture_count = sm->texture_count;
        if (dm->texture_count > 8) dm->texture_count = 8;
        for (size_t t = 0; t < dm->texture_count; ++t) {
            const ThreediMaterialTexture *st = &sm->textures[t];
            ThreediIRMaterialTexture *dt = &dm->textures[t];
            memcpy(dt->name, st->name, sizeof(dt->name) - 1);
            dt->slot = st->slot;
            dt->type = st->type;
            dt->flags = st->flags;
            dt->frame = st->frame;
        }

        // Convert flags
        dm->flags = 0;
        if (sm->material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST)
            dm->flags |= THREEDI_IR_MATERIAL_FLAG_ALPHA_TEST;
        if (sm->material_flags & THREEDI_MATERIAL_FLAG_ALPHA_INVERT)
            dm->flags |= THREEDI_IR_MATERIAL_FLAG_ALPHA_INVERT;
        if (sm->material_flags & THREEDI_MATERIAL_FLAG_TWO_SIDED)
            dm->flags |= THREEDI_IR_MATERIAL_FLAG_TWO_SIDED;
        if (sm->emissive_type == THREEDI_EMISSIVE_FULL)
            dm->flags |= THREEDI_IR_MATERIAL_FLAG_EMISSIVE;

        dm->alpha_threshold = (float)sm->alpha_test_value_byte / 255.0f;

        // Copy UV animation parameters
        dm->u_params.style = sm->u_params.style;
        dm->u_params.phase = sm->u_params.phase;
        dm->u_params.reg = sm->u_params.reg;
        dm->u_params.gen_rate = sm->u_params.gen_rate;
        dm->u_params.start = sm->u_params.start;
        dm->u_params.end = sm->u_params.end;

        dm->v_params.style = sm->v_params.style;
        dm->v_params.phase = sm->v_params.phase;
        dm->v_params.reg = sm->v_params.reg;
        dm->v_params.gen_rate = sm->v_params.gen_rate;
        dm->v_params.start = sm->v_params.start;
        dm->v_params.end = sm->v_params.end;

        // Copy alpha gen
        dm->alpha_gen.style = sm->alpha_gen.style;
        dm->alpha_gen.phase = sm->alpha_gen.phase;
        dm->alpha_gen.reg = sm->alpha_gen.reg;
        dm->alpha_gen.rate = sm->alpha_gen.rate;
        dm->alpha_gen.start = sm->alpha_gen.start;
        dm->alpha_gen.end = sm->alpha_gen.end;

        // Copy RGB gen
        dm->rgb_gen.style = sm->rgb_gen.style;
        dm->rgb_gen.phase = sm->rgb_gen.phase;
        dm->rgb_gen.reg = sm->rgb_gen.reg;
        dm->rgb_gen.rate = sm->rgb_gen.rate;
        memcpy(dm->rgb_gen.start_color, sm->rgb_gen.start_color, sizeof(float) * 4);
        memcpy(dm->rgb_gen.end_color, sm->rgb_gen.end_color, sizeof(float) * 4);

        // Copy texture animation
        dm->animation.num_frames = sm->animation.num_frames;
        dm->animation.animation_type = sm->animation.animation_type;
        dm->animation.cycle_frame_time = sm->animation.cycle_frame_time;

        // Copy reflection / glass
        memcpy(dm->reflect_color, sm->reflect_color, sizeof(float) * 4);
        dm->is_glass = sm->is_glass;

        // Copy material properties
        dm->emissive_type = sm->emissive_type;
        dm->emissive_color = 0;  // 3DI3 doesn't have a raw emissive_color field
        dm->specular_intensity = 0;
        dm->luminosity = 0;
        dm->u_tiling = 0.0f;
        dm->v_tiling = 0.0f;
    }

    return 0;
}

static int convert_lights(const Threedi3di3 *model, ThreediModelIR *ir) {
    ir->light_count = model->light_count;
    if (ir->light_count == 0) return 0;

    ir->lights = (ThreediIRLight *)calloc(ir->light_count, sizeof(ThreediIRLight));
    if (!ir->lights) return -1;

    for (size_t i = 0; i < ir->light_count; ++i) {
        const ThreediLight *sl = &model->lights[i];
        ThreediIRLight *dl = &ir->lights[i];

        memcpy(dl->offset, sl->offset, sizeof(float) * 3);
        dl->attenuation_start = sl->atten_start;
        dl->attenuation_end = sl->atten_end;

        // Convert BGR packed color to RGB float
        dl->color_start[0] = (float)sl->color_start[2] / 255.0f;
        dl->color_start[1] = (float)sl->color_start[1] / 255.0f;
        dl->color_start[2] = (float)sl->color_start[0] / 255.0f;
        dl->color_end[0] = (float)sl->color_end[2] / 255.0f;
        dl->color_end[1] = (float)sl->color_end[1] / 255.0f;
        dl->color_end[2] = (float)sl->color_end[0] / 255.0f;

        dl->style = sl->style;
        dl->phase = sl->phase;
        dl->rate = sl->rate;
        dl->part_index = sl->subobj_index;
        dl->flags = sl->flags;

        // Falloff angle (byte → degrees)
        dl->falloff = (float)sl->falloff_byte;

        // Light direction (-rotY, rotZ, rotX)
        dl->rotation[0] = sl->rotation[0];
        dl->rotation[1] = sl->rotation[1];
        dl->rotation[2] = sl->rotation[2];

        // Light type from flags bit 3
        dl->light_type = (sl->flags & THREEDI_IR_LIGHT_FLAG_TYPE_TARGET) ? 1 : 0;
    }

    return 0;
}

static int convert_userpoints(const Threedi3di3 *model, ThreediModelIR *ir) {
    ir->userpoint_count = model->user_point_count;
    if (ir->userpoint_count == 0) return 0;

    ir->userpoints = (ThreediIRUserPoint *)calloc(ir->userpoint_count, sizeof(ThreediIRUserPoint));
    if (!ir->userpoints) return -1;

    for (size_t i = 0; i < ir->userpoint_count; ++i) {
        const ThreediUserPoint *su = &model->user_points[i];
        ThreediIRUserPoint *du = &ir->userpoints[i];

        memcpy(du->name, su->name, sizeof(du->name) - 1);

        // Convert from fixed-point (16.16) to float, with coordinate swizzle.
        // Original: x->z, y->x, z->y. Userpoints need the side axis mirrored
        // here so NovaObjectData's render-space -X transform preserves the
        // authored driver/passenger side.
        du->position[0] = -(float)su->y / 65536.0f; // x coord
        du->position[1] = (float)su->z / 65536.0f;  // y coord
        du->position[2] = (float)su->x / 65536.0f;  // z coord

        // rot_x/y/z are the local Z-axis direction vector (unit vector),
        // stored as fixed-point with same swizzle as position.
        du->direction[0] = -(float)su->rot_y / 65536.0f;
        du->direction[1] = (float)su->rot_z / 65536.0f;
        du->direction[2] = (float)su->rot_x / 65536.0f;

        du->part_index = su->subobject_index;
        du->type_code = su->userpoint_type;
    }

    return 0;
}

// Map collision face material_flags to pattrib bits (from IDA RE of WriteCFAC).
static uint32_t collision_flags_to_pattrib(uint32_t coll_flags) {
    uint32_t pa = 0;
    if (coll_flags & 0x100) pa |= 0x100;
    if (coll_flags & 0x400) pa |= 0x1000;
    if (coll_flags & 0x800) pa |= 0x2000;
    return pa;
}

// Extract a 3-bit index from collision material_flags for histogram voting.
// bit 0: 0x100, bit 1: 0x400, bit 2: 0x800
static uint32_t collision_flags_index(uint32_t coll_flags) {
    uint32_t idx = 0;
    if (coll_flags & 0x100) idx |= 1;
    if (coll_flags & 0x400) idx |= 2;
    if (coll_flags & 0x800) idx |= 4;
    return idx;
}

// Reconstruct collision flags from a 3-bit index (inverse of collision_flags_index).
static uint32_t collision_flags_from_index(uint32_t idx) {
    uint32_t flags = 0;
    if (idx & 1) flags |= 0x100;
    if (idx & 2) flags |= 0x400;
    if (idx & 4) flags |= 0x800;
    return flags;
}

// Recursive backtracker for optimal material-to-face-group assignment.
// Sorts materials by descending triangle count and prunes branches where
// any group's remaining capacity goes negative.
struct AssignCtx {
    int32_t group_remaining[16];
    int assign[16];
    int best_assign[16];
    uint32_t best_residual;
    int fg_count;
    int mt_count;
    uint32_t *tri_counts;
    int strict;   // 1 = prune overcommit, 0 = allow overcommit
};

static void backtrack_assign(AssignCtx *ctx, int m) {
    if (m == ctx->mt_count) {
        uint32_t residual = 0;
        for (int g = 0; g < ctx->fg_count; ++g) {
            int32_t r = ctx->group_remaining[g];
            residual += (uint32_t)(r > 0 ? r : -r);
        }
        if (residual < ctx->best_residual) {
            ctx->best_residual = residual;
            memcpy(ctx->best_assign, ctx->assign, ctx->mt_count * sizeof(int));
        }
        return;
    }
    for (int g = 0; g < ctx->fg_count; ++g) {
        int32_t new_rem = ctx->group_remaining[g] - (int32_t)ctx->tri_counts[m];
        if (ctx->strict && new_rem < 0) continue;  // prune: would overcommit
        ctx->assign[m] = g;
        ctx->group_remaining[g] = new_rem;
        backtrack_assign(ctx, m + 1);
        ctx->group_remaining[g] = new_rem + (int32_t)ctx->tri_counts[m];
        if (ctx->best_residual == 0) return;  // perfect match found
    }
}

static void assign_surface_types(const Threedi3di3 *model, ThreediModelIR *ir) {
    if (!model->collision || model->collision->object_count == 0 ||
        model->collision->face_count == 0 || ir->material_count == 0)
        return;
    if (model->lod_count == 0 || ir->lod_count == 0) return;

    const ThreediCollisionModel *col = model->collision;

    // Resolve collision LOD: use collision_lod if set, otherwise last LOD.
    size_t lod_idx = (ir->collision_lod >= 0 && (size_t)ir->collision_lod < ir->lod_count)
                   ? (size_t)ir->collision_lod
                   : 0;
    const ThreediIRLod *ir_lod = &ir->lods[lod_idx];

    // Set default surface_type (0x01 = Dirt) for all materials
    for (size_t i = 0; i < ir->material_count; ++i)
        ir->materials[i].surface_type = 0x01;

    // Per-material vote histograms for poly_type and material_flags.
    // For single-material subobjects, all faces vote directly.
    // For multi-material subobjects, face groups (unique poly_type +
    // material_flags) are assigned to materials by matching collision face
    // counts to render triangle counts per material.
    // Max 256 distinct poly_type values (uint8_t).
    size_t mat_count = ir->material_count;
    uint32_t (*votes)[256] = (uint32_t (*)[256])calloc(mat_count, sizeof(uint32_t[256]));
    // Per-material histogram over the 3 pattrib-related bits of material_flags (8 buckets)
    uint32_t (*flag_hist)[8] = (uint32_t (*)[8])calloc(mat_count, sizeof(uint32_t[8]));
    if (!votes || !flag_hist) {
        free(votes); free(flag_hist);
        return;
    }

    // WriteCOBJ creates one COBJ per subobject in order (confirmed via IDA RE).
    // Collision face vert_index[] are LOCAL to the collision object (0-based).

    size_t face_cursor = 0;
    size_t vert_cursor = 0;

    for (size_t obj_idx = 0; obj_idx < col->object_count; ++obj_idx) {
        const ThreediCollisionObject *obj = &col->objects[obj_idx];
        if (obj->num_faces <= 0 || face_cursor >= col->face_count) {
            face_cursor += (size_t)(obj->num_faces > 0 ? obj->num_faces : 0);
            vert_cursor += (size_t)(obj->num_vertices > 0 ? obj->num_vertices : 0);
            continue;
        }

        // Collision object index i = subobject/part index i
        int32_t part_idx = (int32_t)obj_idx;

        // Collect unique materials for this subobject
        int32_t unique_mats[64];
        size_t unique_mat_count = 0;
        for (size_t p = 0; p < ir_lod->primitive_count; ++p) {
            const ThreediIRPrimitive *prim = &ir_lod->primitives[p];
            if (prim->part_index != part_idx) continue;
            bool found = false;
            for (size_t m = 0; m < unique_mat_count; ++m) {
                if (unique_mats[m] == prim->material_index) { found = true; break; }
            }
            if (!found && unique_mat_count < 64)
                unique_mats[unique_mat_count++] = prim->material_index;
        }

        if (unique_mat_count == 0) {
            face_cursor += (size_t)obj->num_faces;
            vert_cursor += (size_t)(obj->num_vertices > 0 ? obj->num_vertices : 0);
            continue;
        }

        // Single material: all faces belong to it — vote directly
        if (unique_mat_count == 1) {
            int32_t mi = unique_mats[0];
            if ((size_t)mi < mat_count) {
                for (int32_t f = 0; f < obj->num_faces; ++f) {
                    size_t fi = face_cursor + (size_t)f;
                    if (fi >= col->face_count) break;
                    votes[mi][col->faces[fi].poly_type]++;
                    flag_hist[mi][collision_flags_index(col->faces[fi].material_flags)]++;
                }
            }
            face_cursor += (size_t)obj->num_faces;
            vert_cursor += (size_t)(obj->num_vertices > 0 ? obj->num_vertices : 0);
            continue;
        }

        // Multiple materials: count-based assignment.
        // Since the engine generates one collision face per render triangle,
        // we match face groups (unique poly_type + material_flags) to materials
        // by comparing collision face counts to render triangle counts.

        // Step 1: count render triangles per material for this part
        struct { int32_t mat; uint32_t count; } mat_tris[64];
        size_t mt_count = 0;
        for (size_t p = 0; p < ir_lod->primitive_count; ++p) {
            const ThreediIRPrimitive *prim = &ir_lod->primitives[p];
            if (prim->part_index != part_idx) continue;

            uint32_t tris = 0;
            if (prim->topology == THREEDI_IR_TOPOLOGY_STRIP) {
                for (uint32_t i = 0; i + 2 < prim->index_count; ++i) {
                    uint16_t i0 = ir_lod->indices[prim->index_offset + i];
                    uint16_t i1 = ir_lod->indices[prim->index_offset + i + 1];
                    uint16_t i2 = ir_lod->indices[prim->index_offset + i + 2];
                    if (i0 != i1 && i1 != i2 && i0 != i2) tris++;
                }
            } else {
                tris = prim->index_count / 3;
            }

            bool found = false;
            for (size_t m = 0; m < mt_count; ++m) {
                if (mat_tris[m].mat == prim->material_index) {
                    mat_tris[m].count += tris;
                    found = true;
                    break;
                }
            }
            if (!found && mt_count < 64) {
                mat_tris[mt_count].mat = prim->material_index;
                mat_tris[mt_count].count = tris;
                mt_count++;
            }
        }

        // Step 2: identify face groups (unique poly_type + material_flags)
        struct { uint8_t poly_type; uint32_t material_flags; uint32_t count; } fgroups[16];
        int fg_count = 0;
        for (int32_t f = 0; f < obj->num_faces; ++f) {
            size_t fi = face_cursor + (size_t)f;
            if (fi >= col->face_count) break;
            uint8_t pt = col->faces[fi].poly_type;
            uint32_t mf = col->faces[fi].material_flags;
            bool found = false;
            for (int g = 0; g < fg_count; ++g) {
                if (fgroups[g].poly_type == pt && fgroups[g].material_flags == mf) {
                    fgroups[g].count++;
                    found = true;
                    break;
                }
            }
            if (!found && fg_count < 16) {
                fgroups[fg_count].poly_type = pt;
                fgroups[fg_count].material_flags = mf;
                fgroups[fg_count].count = 1;
                fg_count++;
            }
        }

        // Step 3: optimal assignment of materials to face groups.
        // Multiple materials may share the same group (same values).
        // Find the assignment that minimizes the total residual:
        //   residual = sum over groups of |face_count - sum(tri_count for assigned mats)|
        // Use recursive backtracker with pruning (handles all practical sizes).
        if (fg_count > 0 && mt_count > 0 && mt_count <= 16) {
            int best_assign[16] = {};

            // Sort mat_tris by descending count for better pruning
            for (size_t i = 0; i < mt_count; ++i) {
                for (size_t j = i + 1; j < mt_count; ++j) {
                    if (mat_tris[j].count > mat_tris[i].count) {
                        int32_t tmp_mat = mat_tris[i].mat;
                        uint32_t tmp_count = mat_tris[i].count;
                        mat_tris[i].mat = mat_tris[j].mat;
                        mat_tris[i].count = mat_tris[j].count;
                        mat_tris[j].mat = tmp_mat;
                        mat_tris[j].count = tmp_count;
                    }
                }
            }

            // Set up backtracker context
            uint32_t tri_counts[16];
            AssignCtx ctx = {};
            ctx.fg_count = fg_count;
            ctx.mt_count = (int)mt_count;
            ctx.tri_counts = tri_counts;
            ctx.best_residual = UINT32_MAX;
            ctx.strict = 1;
            for (size_t m = 0; m < mt_count; ++m)
                tri_counts[m] = mat_tris[m].count;
            for (int g = 0; g < fg_count; ++g)
                ctx.group_remaining[g] = (int32_t)fgroups[g].count;

            backtrack_assign(&ctx, 0);

            // If strict pruning found no valid assignment, retry relaxed.
            if (ctx.best_residual == UINT32_MAX) {
                for (int g = 0; g < fg_count; ++g)
                    ctx.group_remaining[g] = (int32_t)fgroups[g].count;
                ctx.strict = 0;
                backtrack_assign(&ctx, 0);
            }

            memcpy(best_assign, ctx.best_assign, mt_count * sizeof(int));

            // Apply the best assignment
            for (size_t m = 0; m < mt_count; ++m) {
                int32_t mi = mat_tris[m].mat;
                int g = best_assign[m];
                if ((size_t)mi < mat_count && g >= 0 && g < fg_count) {
                    votes[mi][fgroups[g].poly_type] += fgroups[g].count;
                    flag_hist[mi][collision_flags_index(fgroups[g].material_flags)] += fgroups[g].count;
                }
            }
        }

        face_cursor += (size_t)obj->num_faces;
        vert_cursor += (size_t)(obj->num_vertices > 0 ? obj->num_vertices : 0);
    }

    // If no material received any vote (can happen on skinned assets where
    // collision objects don't map cleanly to render part indices), fall back
    // to the dominant collision face group globally so we do not lose
    // collision surface metadata when generating a .3dp from IR.
    bool any_votes = false;
    for (size_t mi = 0; mi < mat_count && !any_votes; ++mi) {
        for (int pt = 0; pt < 256; ++pt) {
            if (votes[mi][pt] != 0) {
                any_votes = true;
                break;
            }
        }
    }

    if (!any_votes && col->face_count > 0) {
        uint32_t global_poly_hist[256] = {};
        uint32_t global_flag_hist[8] = {};
        for (size_t fi = 0; fi < col->face_count; ++fi) {
            const ThreediCollisionFace *cf = &col->faces[fi];
            global_poly_hist[cf->poly_type]++;
            global_flag_hist[collision_flags_index(cf->material_flags)]++;
        }

        uint8_t dominant_pt = 0x01;
        uint32_t dominant_pt_count = 0;
        for (int pt = 0; pt < 256; ++pt) {
            if (global_poly_hist[pt] > dominant_pt_count) {
                dominant_pt_count = global_poly_hist[pt];
                dominant_pt = (uint8_t)pt;
            }
        }

        uint32_t dominant_fi = 0;
        uint32_t dominant_fi_count = 0;
        for (int fi = 0; fi < 8; ++fi) {
            if (global_flag_hist[fi] > dominant_fi_count) {
                dominant_fi_count = global_flag_hist[fi];
                dominant_fi = (uint32_t)fi;
            }
        }
        const uint32_t dominant_pattrib =
            collision_flags_to_pattrib(collision_flags_from_index(dominant_fi));

        for (size_t mi = 0; mi < mat_count; ++mi) {
            ir->materials[mi].surface_type = dominant_pt;
            ir->materials[mi].pattrib = dominant_pattrib;
        }
    }

    // Resolve: for each material, pick the poly_type and flag index with the most votes
    for (size_t mi = 0; mi < mat_count; ++mi) {
        uint32_t best_count = 0;
        uint8_t best_pt = 0x01; // default
        for (int pt = 0; pt < 256; ++pt) {
            if (votes[mi][pt] > best_count) {
                best_count = votes[mi][pt];
                best_pt = (uint8_t)pt;
            }
        }
        if (best_count > 0) {
            ir->materials[mi].surface_type = best_pt;

            // Pick the flag index with the most votes
            uint32_t best_fi_count = 0;
            uint32_t best_fi = 0;
            for (int fi = 0; fi < 8; ++fi) {
                if (flag_hist[mi][fi] > best_fi_count) {
                    best_fi_count = flag_hist[mi][fi];
                    best_fi = (uint32_t)fi;
                }
            }
            ir->materials[mi].pattrib = collision_flags_to_pattrib(collision_flags_from_index(best_fi));
        }
    }

    free(votes);
    free(flag_hist);
}

static int convert_collision(const Threedi3di3 *model, ThreediModelIR *ir) {
    if (!model->collision) return 0;
    const ThreediCollisionModel *col = model->collision;

    ir->collision = (ThreediIRCollision *)calloc(1, sizeof(ThreediIRCollision));
    if (!ir->collision) return -1;

    // bbox: {minX, minY, minZ, maxX, maxY, maxZ}
    ir->collision->model_min[0] = col->model_data.bbox[0];
    ir->collision->model_min[1] = col->model_data.bbox[1];
    ir->collision->model_min[2] = col->model_data.bbox[2];
    ir->collision->model_max[0] = col->model_data.bbox[3];
    ir->collision->model_max[1] = col->model_data.bbox[4];
    ir->collision->model_max[2] = col->model_data.bbox[5];
    memcpy(ir->collision->model_center, col->model_data.radii, sizeof(float) * 3);

    // Convert vertices
    ir->collision->vertex_count = col->vertex_count;
    if (ir->collision->vertex_count > 0) {
        ir->collision->vertices = (ThreediIRCollisionVertex *)calloc(
            ir->collision->vertex_count, sizeof(ThreediIRCollisionVertex));
        if (!ir->collision->vertices) return -1;

        for (size_t i = 0; i < ir->collision->vertex_count; ++i) {
            memcpy(ir->collision->vertices[i].position,
                   col->vertices[i].position, sizeof(float) * 3);
        }
    }

    // Preserve CNRM in its exact signed Q14 representation. The raw parser
    // divided these values by 16384.0f, so multiplying by that power of two is
    // an exact recovery rather than a lossy re-quantization.
    ir->collision->normal_count = col->normal_count;
    if (ir->collision->normal_count > 0) {
        ir->collision->normals = (ThreediIRCollisionNormal *)calloc(
            ir->collision->normal_count, sizeof(ThreediIRCollisionNormal));
        if (!ir->collision->normals) return -1;
        for (size_t i = 0; i < ir->collision->normal_count; ++i) {
            const ThreediCollisionNormal *sn = &col->normals[i];
            ThreediIRCollisionNormal *dn = &ir->collision->normals[i];
            for (int k = 0; k < 3; ++k)
                dn->normal_q14[k] = (int16_t)(sn->normal[k] * 16384.0f);
            dn->dominant_axis = sn->dominate_axis;
        }
    }

    // Convert planes
    ir->collision->plane_count = col->plane_count;
    if (ir->collision->plane_count > 0) {
        ir->collision->planes = (ThreediIRCollisionPlane *)calloc(
            ir->collision->plane_count, sizeof(ThreediIRCollisionPlane));
        if (!ir->collision->planes) return -1;

        for (size_t i = 0; i < ir->collision->plane_count; ++i) {
            const ThreediBoundingPlane *sp = &col->planes[i];
            ThreediIRCollisionPlane *dp = &ir->collision->planes[i];
            memcpy(dp->normal, sp->normal, sizeof(float) * 3);
            dp->distance = sp->radius;
            dp->flags = (uint16_t)sp->flags;
        }
    }

    // Convert volumes
    ir->collision->volume_count = col->volume_count;
    if (ir->collision->volume_count > 0) {
        ir->collision->volumes = (ThreediIRCollisionVolume *)calloc(
            ir->collision->volume_count, sizeof(ThreediIRCollisionVolume));
        if (!ir->collision->volumes) return -1;

        for (size_t i = 0; i < ir->collision->volume_count; ++i) {
            const ThreediBoundingVolume *sv = &col->volumes[i];
            ThreediIRCollisionVolume *dv = &ir->collision->volumes[i];

            dv->type = sv->collidable_type;
            dv->flags = sv->flags;
            dv->object_index = -1;

            // Convert from 16.16 fixed point to float
            dv->min[0] = (float)sv->min_x_fp16 / 65536.0f;
            dv->min[1] = (float)sv->min_y_fp16 / 65536.0f;
            dv->min[2] = (float)sv->min_z_fp16 / 65536.0f;
            dv->max[0] = (float)sv->max_x_fp16 / 65536.0f;
            dv->max[1] = (float)sv->max_y_fp16 / 65536.0f;
            dv->max[2] = (float)sv->max_z_fp16 / 65536.0f;

            dv->plane_count = sv->plane_count;
        }
    }

    // Volumes are sequential per object — walk objects, consume volumes
    size_t vol_cursor = 0;
    size_t plane_cursor = 0;
    for (size_t obj_idx = 0; obj_idx < col->object_count; ++obj_idx) {
        const ThreediCollisionObject *obj = &col->objects[obj_idx];
        for (int32_t v = 0; v < obj->num_bounding_volumes; ++v) {
            if (vol_cursor < ir->collision->volume_count) {
                ir->collision->volumes[vol_cursor].part_index = obj->parent_subobject_index;
                ir->collision->volumes[vol_cursor].object_index = (int32_t)obj_idx;
                ir->collision->volumes[vol_cursor].plane_start = (int32_t)plane_cursor;
                plane_cursor += ir->collision->volumes[vol_cursor].plane_count;
                ++vol_cursor;
            }
        }
    }

    // Convert faces (BulletLOD reconstruction + the round-raycast fields).
    // Faces/normals are sequential per object; the face's normal_index is
    // local to its object's CNRM run, resolved here so the IR face is
    // self-contained [orig: the per-COBJ normal-run fixup in the collision
    // builder @ 0x5b3bf0].
    ir->collision->face_count = col->face_count;
    if (col->face_count > 0 && col->faces) {
        ir->collision->faces = (ThreediIRCollisionFace *)calloc(
            col->face_count, sizeof(ThreediIRCollisionFace));
        if (!ir->collision->faces) return -1;

        for (size_t i = 0; i < col->face_count; ++i) {
            const ThreediCollisionFace *sf = &col->faces[i];
            ThreediIRCollisionFace *df = &ir->collision->faces[i];
            df->vert_index[0] = sf->vert_index[0];
            df->vert_index[1] = sf->vert_index[1];
            df->vert_index[2] = sf->vert_index[2];
            df->normal_index = sf->normal_index;
            df->plane_dist_fp16 = sf->plane_dist_fp16;
            df->min_fp16[0] = sf->min_x_fp16;
            df->min_fp16[1] = sf->min_y_fp16;
            df->min_fp16[2] = sf->min_z_fp16;
            df->max_fp16[0] = sf->max_x_fp16;
            df->max_fp16[1] = sf->max_y_fp16;
            df->max_fp16[2] = sf->max_z_fp16;
            df->material_flags = sf->material_flags;
            df->poly_type = sf->poly_type;
        }
        size_t face_cursor = 0;
        size_t normal_base = 0;
        for (size_t obj_idx = 0; obj_idx < col->object_count; ++obj_idx) {
            const ThreediCollisionObject *obj = &col->objects[obj_idx];
            for (int32_t f = 0; f < obj->num_faces && face_cursor < col->face_count;
                 ++f, ++face_cursor) {
                const ThreediCollisionFace *sf = &col->faces[face_cursor];
                ThreediIRCollisionFace *df = &ir->collision->faces[face_cursor];
                const size_t ni = normal_base + (size_t)sf->normal_index;
                if (sf->normal_index >= 0 && ni < ir->collision->normal_count) {
                    const ThreediIRCollisionNormal *sn = &ir->collision->normals[ni];
                    memcpy(df->normal, sn->normal_q14, sizeof(df->normal));
                    df->dominate_axis = sn->dominant_axis;
                }
            }
            normal_base += (size_t)obj->num_normals;
        }
    }

    // Convert objects (COBJ — one per subobject)
    ir->collision->object_count = col->object_count;
    if (col->object_count > 0 && col->objects) {
        ir->collision->objects = (ThreediIRCollisionObject *)calloc(
            col->object_count, sizeof(ThreediIRCollisionObject));
        if (!ir->collision->objects) return -1;

        for (size_t i = 0; i < col->object_count; ++i) {
            const ThreediCollisionObject *so = &col->objects[i];
            ThreediIRCollisionObject *d = &ir->collision->objects[i];
            d->num_vertices = so->num_vertices;
            d->num_faces = so->num_faces;
            d->num_planes = so->num_normals;
            d->num_bounding_volumes = so->num_bounding_volumes;
            d->parent_subobject_index = so->parent_subobject_index;
            d->offset[0] = so->offset[0];
            d->offset[1] = so->offset[1];
            d->offset[2] = so->offset[2];
            memcpy(d->min, so->min, sizeof(d->min));
            memcpy(d->max, so->max, sizeof(d->max));
            memcpy(d->mid, so->med, sizeof(d->mid));
            d->radius = so->radius;
        }
    }

    // Convert translations (CXLT — attachment points)
    ir->collision->translation_count = col->translation_count;
    if (col->translation_count > 0 && col->translations) {
        ir->collision->translations = (ThreediIRCollisionTranslation *)calloc(
            col->translation_count, sizeof(ThreediIRCollisionTranslation));
        if (!ir->collision->translations) return -1;

        for (size_t i = 0; i < col->translation_count; ++i) {
            memcpy(ir->collision->translations[i].translation,
                   col->translations[i].translation,
                   sizeof(ir->collision->translations[i].translation));
        }
    }

    return 0;
}

static int convert_occlusion(const Threedi3di3 *model, ThreediModelIR *ir) {
    if (!model || model->occlusion_object_count == 0) return 0;

    ir->occlusion = (ThreediIROcclusion *)calloc(1, sizeof(ThreediIROcclusion));
    if (!ir->occlusion) return -1;

    // Convert vertices
    ir->occlusion->vertex_count = model->occlusion_vertex_count;
    if (ir->occlusion->vertex_count > 0) {
        if (!model->occlusion_vertices) return -1;
        ir->occlusion->vertices = (ThreediIROcclusionVertex *)calloc(
            ir->occlusion->vertex_count, sizeof(ThreediIROcclusionVertex));
        if (!ir->occlusion->vertices) return -1;

        for (size_t i = 0; i < ir->occlusion->vertex_count; ++i) {
            memcpy(ir->occlusion->vertices[i].position,
                   model->occlusion_vertices[i].position, sizeof(float) * 3);
        }
    }

    // Convert faces
    ir->occlusion->face_count = model->occlusion_face_count;
    if (ir->occlusion->face_count > 0) {
        if (!model->occlusion_faces) return -1;
        ir->occlusion->faces = (ThreediIROcclusionFace *)calloc(
            ir->occlusion->face_count, sizeof(ThreediIROcclusionFace));
        if (!ir->occlusion->faces) return -1;

        for (size_t i = 0; i < ir->occlusion->face_count; ++i) {
            const ThreediOcclusionFace *sf = &model->occlusion_faces[i];
            ThreediIROcclusionFace *df = &ir->occlusion->faces[i];
            df->raw_indices = sf->raw_indices;
            df->edge_data = sf->edge_data;
            df->other_edge_data = sf->other_edge_data;
        }
    }

    // Convert planes
    ir->occlusion->plane_count = model->occlusion_plane_count;
    if (ir->occlusion->plane_count > 0) {
        if (!model->occlusion_planes) return -1;
        ir->occlusion->planes = (ThreediIROcclusionPlane *)calloc(
            ir->occlusion->plane_count, sizeof(ThreediIROcclusionPlane));
        if (!ir->occlusion->planes) return -1;

        for (size_t i = 0; i < ir->occlusion->plane_count; ++i) {
            const ThreediOcclusionPlane *sp = &model->occlusion_planes[i];
            ThreediIROcclusionPlane *dp = &ir->occlusion->planes[i];
            memcpy(dp->normal, sp->normal, sizeof(float) * 3);
            dp->radius = sp->radius;
        }
    }

    // Convert objects
    ir->occlusion->object_count = model->occlusion_object_count;
    if (ir->occlusion->object_count > 0) {
        if (!model->occlusion_objects) return -1;
        ir->occlusion->objects = (ThreediIROcclusionObject *)calloc(
            ir->occlusion->object_count, sizeof(ThreediIROcclusionObject));
        if (!ir->occlusion->objects) return -1;

        size_t vert_cursor = 0;
        size_t plane_cursor = 0;
        size_t face_cursor = 0;

        for (size_t i = 0; i < ir->occlusion->object_count; ++i) {
            const ThreediOcclusionObject *so = &model->occlusion_objects[i];
            ThreediIROcclusionObject *doj = &ir->occlusion->objects[i];

            doj->type = (int32_t)so->type;
            doj->parent_subobject_index = (int32_t)so->parent_subobject_index;
            doj->connecting_subobject = (int32_t)so->connecting_subobject;
            memcpy(doj->position, so->position, sizeof(float) * 3);
            doj->radius = so->radius;
            doj->glow_scale = so->glow_scale;
            doj->num_vertices = so->num_vertices;
            doj->num_planes = so->num_planes;
            doj->face_count = so->face_count;
            doj->vertex_start = (int32_t)vert_cursor;
            doj->plane_start = (int32_t)plane_cursor;
            doj->face_start = (int32_t)face_cursor;

            if (so->num_vertices > 0) {
                vert_cursor += (size_t)so->num_vertices;
            }
            if (so->num_planes > 0) {
                plane_cursor += (size_t)so->num_planes;
            }
            if (so->face_count > 0) {
                face_cursor += (size_t)so->face_count;
            }
        }
    }

    return 0;
}

static int convert_part_animations(const ThreediLod *src_lod, ThreediIRLod *dst_lod) {
    // Only store PANM if this LOD actually has its own PANM chunk.
    // Fallback to model-level PANM (for LODs without their own) is handled at write-time.
    const ThreediPartAnimation *src_anims = src_lod->part_animations;
    size_t src_count = src_lod->part_animation_count;

    dst_lod->part_animation_count = src_count;
    if (src_count == 0) return 0;

    dst_lod->part_animations = (ThreediIRPartAnimation *)calloc(
        src_count, sizeof(ThreediIRPartAnimation));
    if (!dst_lod->part_animations) return -1;

    for (size_t i = 0; i < src_count; ++i) {
        const ThreediPartAnimation *sp = &src_anims[i];
        ThreediIRPartAnimation *dp = &dst_lod->part_animations[i];

        dp->flags = sp->flags;
        dp->parent_part = sp->parent_subobject;
        dp->part_index = sp->subobject_index;
        dp->matrix_index = sp->matrix_index;
        dp->matrix_offset = sp->matrix_offset;
        dp->bind_matrix_index = sp->bind_matrix_index;

        // Copy transform data
        #define COPY_TRANSFORM(name) do { \
            dp->name.control = sp->name.control; \
            dp->name.control_param = sp->name.control_param; \
            dp->name.rate = sp->name.rate; \
            dp->name.start = sp->name.start; \
            dp->name.end = sp->name.end; \
        } while(0)

        COPY_TRANSFORM(rotation_x);
        COPY_TRANSFORM(rotation_y);
        COPY_TRANSFORM(rotation_z);
        COPY_TRANSFORM(scale_x);
        COPY_TRANSFORM(scale_y);
        COPY_TRANSFORM(scale_z);
        COPY_TRANSFORM(translation);

        #undef COPY_TRANSFORM
    }

    return 0;
}

static int convert_control_registers(const Threedi3di3 *model, ThreediModelIR *ir) {
    ir->control_register_count = model->ctrl.count;
    if (ir->control_register_count == 0) return 0;

    ir->control_registers = (ThreediIRControlRegister *)calloc(
        ir->control_register_count, sizeof(ThreediIRControlRegister));
    if (!ir->control_registers) return -1;

    for (size_t i = 0; i < ir->control_register_count; ++i) {
        memcpy(ir->control_registers[i].name,
               model->ctrl.registers[i].name,
               sizeof(ir->control_registers[i].name) - 1);
    }

    return 0;
}

static int convert_matrices(const Threedi3di3 *model, ThreediModelIR *ir) {
    ir->matrix_count = model->mtrx.count;
    if (ir->matrix_count == 0) return 0;

    ir->matrices = (ThreediIRMatrix *)calloc(ir->matrix_count, sizeof(ThreediIRMatrix));
    if (!ir->matrices) return -1;

    for (size_t i = 0; i < ir->matrix_count; ++i) {
        memcpy(ir->matrices[i].m, model->mtrx.matrices[i].m, sizeof(float) * 16);
    }

    return 0;
}

int threedi_ir_from_3di3(const Threedi3di3 *model, ThreediModelIR *out) {
    if (!model || !out) return -1;

    threedi_ir_init(out);

    // Copy header info
    memcpy(out->name, model->header.name, sizeof(out->name) - 1);
    out->source_format = THREEDI_IR_SOURCE_3DI3;

    // Copy render function from first LOD's RMDL model_type
    if (model->lod_count > 0 && model->lods[0].model_type[0]) {
        memcpy(out->render_function, model->lods[0].model_type, 4);
        out->render_function[4] = '\0';
    }

    switch (model->header.mesh_type) {
        case THREEDI_MESH_BASIC:
            out->mesh_type = THREEDI_IR_MESH_BASIC;
            break;
        case THREEDI_MESH_SKINNED:
            out->mesh_type = THREEDI_IR_MESH_SKINNED;
            break;
        default:
            out->mesh_type = THREEDI_IR_MESH_STATIC;
            break;
    }

    // Convert LODs
    out->lod_count = model->lod_count;
    if (out->lod_count > 0) {
        out->lods = (ThreediIRLod *)calloc(out->lod_count, sizeof(ThreediIRLod));
        if (!out->lods) goto error;

        for (size_t i = 0; i < out->lod_count; ++i) {
            if (convert_lod(&model->lods[i], model, &out->lods[i]) != 0) goto error;
            if (convert_part_animations(&model->lods[i], &out->lods[i]) != 0) goto error;
        }
    }

    // Convert materials
    if (convert_materials(model, out) != 0) goto error;

    // Convert lights
    if (convert_lights(model, out) != 0) goto error;

    // Convert userpoints
    if (convert_userpoints(model, out) != 0) goto error;

    // Convert collision
    if (convert_collision(model, out) != 0) goto error;

    // Assign collision surface types to materials
    assign_surface_types(model, out);

    // Convert occlusion
    if (convert_occlusion(model, out) != 0) goto error;

    // Convert control registers
    if (convert_control_registers(model, out) != 0) goto error;

    // Convert matrices
    if (convert_matrices(model, out) != 0) goto error;

    return 0;

error:
    threedi_ir_free(out);
    return -1;
}
