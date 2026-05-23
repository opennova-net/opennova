// libs/object/src/flat_mesh_to_ase.cpp
//
// Port of pyopennova/ase_from_3di3.py::_lod_mesh_objects (lines 250-346) and
// _populate_object (lines 709-798) into a single C++ function.
//
// Design: caller feeds FlatMeshes already built by object_ir_to_flat_meshes_v2
// (or flatten_collision). This function maps each FlatMesh mechanically to one
// ase_Object; LOD/collision selection logic lives in the Task 9 orchestrator.

#include "object/flat_mesh_to_ase.h"

#include "object/coords.h"          // object_render_space
#include "object/ase_vec.h"         // object_tm_identity
#include "object/nlascexp_quirks.h" // object_fixup_name

#include "ase/ase.h"                // ase_alloc_object
#include "ase/types.h"              // ase_Object, ase_Face, ase_UV, ase_Weight

#include "threedi/threedi_3di3.h"   // Threedi3di3, ThreediLod, ThreediRenderObject

#include <array>
#include <vector>
#include <cstring>
#include <cstdio>

static const int SUBS_PER_SLOT = 64;  // matches pyopennova SUBS_PER_SLOT

extern "C" int object_emit_main_mesh_objects(
    const FlatMeshArray* meshes,
    const Threedi3di3* ir,
    int lod_index,
    const NlascexpOptions* /*opts*/,
    const int* global_to_local,
    int global_to_local_count,
    ase_Object* out_objects,
    int* out_count)
{
    if (!meshes || !ir || !out_objects || !out_count)
        return -1;
    if (lod_index < 0 || (size_t)lod_index >= ir->lod_count)
        return -1;

    const ThreediLod* lod = &ir->lods[lod_index];
    *out_count = 0;

    // part_abs is no longer used; fm->origin stores render-space origin directly.
    // (ir_to_flat_meshes_v2 stores render_space(ro->abs) in fm->origin;
    //  build_collision_flat_meshes stores (0,0,0) for the collision LOD.)
    const size_t render_object_count = lod->render_object_count;
    (void)render_object_count;
    // Keep part_abs as empty placeholder; all origin reading now via fm->origin.
    std::vector<std::array<float, 3>> part_abs;  // Empty — not used

    for (int m = 0; m < meshes->count; ++m) {
        const FlatMesh* fm = &meshes->meshes[m];
        ase_Object* obj = &out_objects[*out_count];

        const int part_idx = fm->part_index;
        const bool has_weights = (fm->vertex_influence_offsets != nullptr
                                  && fm->vertex_influences != nullptr);
        const int weight_alloc = has_weights ? fm->vertex_count : 0;

        // UV count = face_count * 3 (one per face-corner, Python line 716 + 773-777).
        // When fm->corners is null (e.g., collision meshes), no UVs are emitted.
        const int uv_count = (fm->corners != nullptr) ? fm->face_count * 3 : 0;

        ase_alloc_object(obj, fm->vertex_count, uv_count, fm->face_count, 0, weight_alloc);

        // name: fixup then copy (Python line 722).
        char name_clean[64];
        object_fixup_name(fm->name, name_clean, sizeof(name_clean));
        std::strncpy(obj->name, name_clean, 63);
        obj->name[63] = '\0';

        // parent_name: Python line 324 calls _export_name(f"PN{i + 1:02d}") which
        // strips the "PN" prefix → just the two-digit index (e.g., "01").
        // _export_name: "if name.startswith('PN') and name[2:4].isdigit(): return name[2:]"
        std::snprintf(obj->parent_name, sizeof(obj->parent_name), "%02d", part_idx + 1);

        // node_id: -1 by default (Python line 726).
        obj->node_id = -1;

        // material_ref from material_id_set[0] // SUBS_PER_SLOT (Python lines 735-739).
        obj->material_ref = 0;
        if (fm->material_id_set_count > 0) {
            int first = fm->material_id_set[0];
            if (global_to_local && global_to_local_count > 0
                && first >= 0 && first < global_to_local_count) {
                first = global_to_local[first];
            }
            obj->material_ref = first / SUBS_PER_SLOT;
        }

        // origin for TM translation and vertex world position.
        // fm->origin stores the render-space origin (already swizzled):
        //   - ir_to_flat_meshes_v2: stores render_space(ro->abs)
        //   - build_collision_flat_meshes: stores (0.0f, 0.0f, 0.0f) (positive zeros,
        //     matching Python's explicit `origin = (0.0, 0.0, 0.0)` for collision LOD)
        // Python lines 328-332: use_collision_mesh → origin=(0.0,0.0,0.0).
        float origin_render[3] = {fm->origin[0], fm->origin[1], fm->origin[2]};

        // TM: identity rotation + translation = origin (Python _tm(origin)).
        float tm[4][3];
        object_tm_identity(origin_render, tm);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c)
                obj->tm_row[r][c] = tm[r][c];

        // Vertices: fm->vertices[i] are already world-space (absolute render_space
        // coordinates, not part-relative). ir_to_flat_meshes_v2 stores world-space
        // positions computed in double to match Python's _vec_add(origin, flat_local)
        // double-precision arithmetic. No origin offset added here.
        for (int v = 0; v < fm->vertex_count; ++v) {
            obj->verts[v * 3 + 0] = fm->vertices[v].x;
            obj->verts[v * 3 + 1] = fm->vertices[v].y;
            obj->verts[v * 3 + 2] = fm->vertices[v].z;
        }

        // Faces (Python lines 748-766).
        for (int f = 0; f < fm->face_count; ++f) {
            ase_Face* af = &obj->faces[f];
            af->vert[0] = fm->faces[f].v[0];
            af->vert[1] = fm->faces[f].v[1];
            af->vert[2] = fm->faces[f].v[2];
            af->vert[3] = 0;
            af->edge_visibility[0] = 1;
            af->edge_visibility[1] = 1;
            af->edge_visibility[2] = 1;
            af->edge_visibility[3] = 0;
            af->smoothing_mask = fm->faces[f].smoothing_group_mask;

            // material_id: remap via global_to_local then % SUBS_PER_SLOT (Python lines 757-764).
            int mid = fm->faces[f].material_id;
            if (global_to_local && global_to_local_count > 0
                && mid >= 0 && mid < global_to_local_count) {
                mid = global_to_local[mid];
            }
            af->material_id = mid % SUBS_PER_SLOT;

            // UV indices per corner: base = f*3, uv[c] = base + c (Python lines 773-777).
            // Only set when there are UVs (corners not null).
            if (uv_count > 0) {
                af->uv[0] = f * 3 + 0;
                af->uv[1] = f * 3 + 1;
                af->uv[2] = f * 3 + 2;
                af->uv[3] = 0;
            }
        }

        // UVs: one per face-corner (Python lines 768-771).
        // corners[] is sized face_count*3; corners[f*3+c].{u0,v0} maps to uvs[f*3+c].
        // Skip when corners is null (collision meshes have no UVs).
        if (fm->corners != nullptr) {
            for (int f = 0; f < fm->face_count; ++f) {
                for (int c = 0; c < 3; ++c) {
                    ase_UV* uv = &obj->uvs[f * 3 + c];
                    uv->u = fm->corners[f * 3 + c].u0;
                    uv->v = fm->corners[f * 3 + c].v0;
                    uv->w = 0.0f;
                }
            }
        }

        // Weights (Python lines 779-786). CSR walk: vertex_influence_offsets[v..v+1].
        if (has_weights) {
            obj->skinned = 1;
            for (int v = 0; v < fm->vertex_count; ++v) {
                const int begin = fm->vertex_influence_offsets[v];
                const int end   = fm->vertex_influence_offsets[v + 1];
                int count = end - begin;
                if (count > 4) count = 4;  // cap at 4 per Python _pack_weights
                ase_Weight* w = &obj->weights[v];
                for (int k = 0; k < 4; ++k) {
                    if (k < count) {
                        w->bone_index[k] = fm->vertex_influences[begin + k].bone_index;
                        w->weight[k]     = fm->vertex_influences[begin + k].weight;
                    } else {
                        w->bone_index[k] = -1;
                        w->weight[k]     = 0.0f;
                    }
                }
            }
        }

        // face_normals: populate from fm->corners[].{nx,ny,nz} so the ASE writer
        // uses the pre-computed normals path (has_pre=true → hp3 format %.9g)
        // which matches the Python path (ase_from_3di3.py lines 788-798).
        // Memory is allocated with new[] so ase_free (which uses delete[]) can clean it up.
        // Skip for collision meshes (corners is null; no pre-computed normals).
        if (fm->corners != nullptr && fm->face_count > 0) {
            float* fn_arr = new float[static_cast<size_t>(fm->face_count) * 9]();
            for (int f = 0; f < fm->face_count; ++f) {
                for (int c = 0; c < 3; ++c) {
                    const FlatMeshFaceCornerData* cd = &fm->corners[f * 3 + c];
                    fn_arr[f * 9 + c * 3 + 0] = cd->nx;
                    fn_arr[f * 9 + c * 3 + 1] = cd->ny;
                    fn_arr[f * 9 + c * 3 + 2] = cd->nz;
                }
            }
            obj->face_normals = fn_arr;
            obj->face_normal_count = fm->face_count;
        }

        ++(*out_count);
    }

    return 0;
}
