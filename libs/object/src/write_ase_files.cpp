// libs/object/src/write_ase_files.cpp
//
// IR-driven top-level: object_write_ase_files_from_3di3 builds the per-LOD
// FlatMeshArrays (with collision-LOD substitution) and delegates the actual
// .ase writing to object_write_ase_files_from_flat_meshes.

#include "object/write_ase_files.h"
#include "object/write_ase_files_from_flat_meshes.h"
#include "object/flat_mesh.h"
#include "object/ir_to_flat_mesh.h"
#include "threedi/threedi_3di3.h"
#include "bad/bad.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// derive_poly_collision_lod — port of pyopennova.project_writer.derive_poly_collision_lod.
int derive_poly_collision_lod(const Threedi3di3* ir) {
    if (!ir || !ir->collision) return -1;
    const ThreediCollisionModel* coll = ir->collision;
    int target_face = (int)coll->face_count;
    int target_vert = (int)coll->vertex_count;
    if (target_face <= 0 || target_vert <= 0) return -1;
    if (ir->lod_count == 1) return 0;

    int best_face = -1;
    int best_vert = -1;
    for (int i = 0; i < (int)ir->lod_count; ++i) {
        const ThreediLod* lod = &ir->lods[i];
        int lv = (int)lod->vertices.count;
        int total_tris = 0;
        for (size_t si = 0; si < lod->strip_count; ++si) {
            total_tris += (int)lod->strips[si].num_triangles;
        }
        if (total_tris == target_face) {
            if (lv == target_vert) {
                best_face = i;
            } else if (best_face < 0) {
                best_face = i;
            }
        }
        if (lv == target_vert && best_vert < i) {
            best_vert = i;
        }
    }
    if (best_face >= 0) return best_face;
    return best_vert;
}

// build_collision_flat_meshes — port of pyopennova.mesh_build.flatten_collision.
int build_collision_flat_meshes(const Threedi3di3* ir, FlatMeshArray* out) {
    if (!ir || !ir->collision || !out) return -1;
    const ThreediCollisionModel* coll = ir->collision;
    int obj_count = (int)coll->object_count;
    if (obj_count <= 0) return 0;

    if (object_flat_mesh_array_alloc(obj_count, out) != 0) return -1;

    int v_cum = 0, f_cum = 0;
    for (int k = 0; k < obj_count; ++k) {
        const ThreediCollisionObject* co = &coll->objects[k];
        int num_v = (int)co->num_vertices;
        int num_f = (int)co->num_faces;

        FlatMesh* fm = &out->meshes[k];
        std::snprintf(fm->name, sizeof(fm->name), "%02d Mesh0", k + 1);
        fm->part_index = k;

        if (object_flat_mesh_resize(fm, num_v, num_f, num_f > 0 ? 1 : 0, 0) != 0) {
            object_flat_mesh_array_free(out);
            return -1;
        }
        std::free(fm->corners);
        fm->corners = nullptr;

        for (int vi = 0; vi < num_v; ++vi) {
            if (v_cum + vi >= (int)coll->vertex_count) break;
            const float* cp = coll->vertices[v_cum + vi].position;
            fm->vertices[vi].x = cp[1];
            fm->vertices[vi].y = -cp[0];
            fm->vertices[vi].z = cp[2];
        }
        for (int fi = 0; fi < num_f; ++fi) {
            if (f_cum + fi >= (int)coll->face_count) break;
            const ThreediCollisionFace* cf = &coll->faces[f_cum + fi];
            fm->faces[fi].v[0] = (int32_t)cf->vert_index[0];
            fm->faces[fi].v[1] = (int32_t)cf->vert_index[1];
            fm->faces[fi].v[2] = (int32_t)cf->vert_index[2];
            fm->faces[fi].material_id = 0;
            fm->faces[fi].smoothing_group_mask = 1;
        }
        if (num_f > 0 && fm->material_id_set_count > 0) {
            fm->material_id_set[0] = 0;
        }
        v_cum += num_v;
        f_cum += num_f;
    }
    return obj_count;
}

bool is_skinned(const Threedi3di3* ir) {
    return ir && (int)ir->header.mesh_type == (int)THREEDI_MESH_SKINNED;
}

// Build per-LOD FlatMeshArrays for the IR with collision-LOD substitution applied.
int build_meshes_for_all_lods(
    const Threedi3di3* ir,
    std::vector<FlatMeshArray>& out_meshes)
{
    int lod_count = (int)ir->lod_count;
    out_meshes.assign(lod_count, FlatMeshArray{});
    const bool skinned = is_skinned(ir);
    int poly_coll_lod = derive_poly_collision_lod(ir);

    for (int lod_idx = 0; lod_idx < lod_count; ++lod_idx) {
        bool use_collision_mesh = false;
        int lod_vert_count = (int)ir->lods[lod_idx].vertices.count;
        int coll_vert_count = ir->collision ? (int)ir->collision->vertex_count : 0;
        use_collision_mesh = (lod_idx == poly_coll_lod && lod_vert_count == coll_vert_count
                              && ir->collision);

        if (use_collision_mesh) {
            int n = build_collision_flat_meshes(ir, &out_meshes[lod_idx]);
            if (n < 0) {
                for (int j = 0; j < lod_idx; ++j) object_flat_mesh_array_free(&out_meshes[j]);
                return -1;
            }

            FlatMeshArray render_meshes = {};
            if (object_ir_to_flat_meshes_v2(ir, lod_idx, 1, 0, 1, &render_meshes) == 0) {
                std::vector<int> shared_mat_set;
                for (int mi = 0; mi < render_meshes.count; ++mi) {
                    const FlatMesh* rfm = &render_meshes.meshes[mi];
                    if (rfm->material_id_set_count > 0) {
                        for (int j = 0; j < rfm->material_id_set_count; ++j) {
                            shared_mat_set.push_back(rfm->material_id_set[j]);
                        }
                        break;
                    }
                }
                object_flat_mesh_array_free(&render_meshes);

                if (!shared_mat_set.empty()) {
                    int mset_n = (int)shared_mat_set.size();
                    for (int mi = 0; mi < out_meshes[lod_idx].count; ++mi) {
                        FlatMesh* fm = &out_meshes[lod_idx].meshes[mi];
                        std::free(fm->material_id_set);
                        fm->material_id_set = (int32_t*)std::malloc(mset_n * sizeof(int32_t));
                        fm->material_id_set_count = mset_n;
                        for (int j = 0; j < mset_n; ++j) {
                            fm->material_id_set[j] = shared_mat_set[j];
                        }
                        if (fm->face_count > 0) {
                            for (int fi = 0; fi < fm->face_count; ++fi) {
                                fm->faces[fi].material_id = shared_mat_set[fi % mset_n];
                            }
                        }
                    }
                }
            }
        } else {
            int rc = object_ir_to_flat_meshes_v2(ir, lod_idx,
                /*include_empty_parts=*/1,
                /*track_bone_data=*/skinned ? 1 : 0,
                /*preserve_source_indexing=*/1,
                &out_meshes[lod_idx]);
            if (rc != 0) {
                for (int j = 0; j < lod_idx; ++j) object_flat_mesh_array_free(&out_meshes[j]);
                return rc;
            }
        }
    }
    return 0;
}

}  // namespace

extern "C" int object_write_ase_files_from_3di3_with_bad(
    const struct Threedi3di3* ir,
    const struct BadFile* bad_file,
    const char* primary_path,
    const NlascexpOptions* opts,
    char** out_paths_array,
    int out_paths_capacity,
    int* out_paths_count)
{
    if (!ir || !primary_path || !out_paths_array || !out_paths_count) return -1;

    std::vector<FlatMeshArray> per_lod;
    int rc = build_meshes_for_all_lods(ir, per_lod);
    if (rc != 0) return rc;

    rc = object_write_ase_files_from_flat_meshes_with_bad(
        per_lod.data(), (int)per_lod.size(),
        ir, bad_file, primary_path, opts,
        out_paths_array, out_paths_capacity, out_paths_count);

    for (auto& arr : per_lod) object_flat_mesh_array_free(&arr);
    return rc;
}

extern "C" int object_write_ase_files_from_3di3(
    const struct Threedi3di3* ir,
    const char* primary_path,
    const NlascexpOptions* opts,
    char** out_paths_array,
    int out_paths_capacity,
    int* out_paths_count)
{
    return object_write_ase_files_from_3di3_with_bad(
        ir, /*bad_file=*/nullptr, primary_path, opts,
        out_paths_array, out_paths_capacity, out_paths_count);
}
