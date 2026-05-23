// libs/object/src/write_ase_files_from_flat_meshes.cpp
//
// Public entry point: write .ase files from caller-provided FlatMeshes + IR side-data.
// See header for semantics.
//
// Implementation is a refactor of the emission body of write_one_lod_ase (lines 338-529
// of write_ase_files.cpp pre-Phase-E); the only behavioral change is that the FlatMeshes
// come from the caller, not from internal flattening.

#include "object/write_ase_files_from_flat_meshes.h"
#include "object/flat_mesh.h"
#include "object/flat_mesh_to_ase.h"
#include "object/material_list_to_ase.h"
#include "object/markers_to_ase.h"
#include "object/bones_to_ase.h"
#include "bad/bad.h"
#include "object/lights_to_ase.h"
#include "object/collision_to_ase.h"
#include "object/occlusion_to_ase.h"
#include "ase/ase.h"
#include "ase/types.h"
#include "threedi/threedi_3di3.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

namespace {

static const int SUBS_PER_SLOT = 64;  // mirrors pyopennova SUBS_PER_SLOT

bool is_skinned(const Threedi3di3* ir) {
    return ir && (int)ir->header.mesh_type == (int)THREEDI_MESH_SKINNED;
}

void build_lod_filename(const char* primary_path, int lod_idx,
                         char* out, size_t out_size) {
    if (lod_idx == 0) {
        std::snprintf(out, out_size, "%s", primary_path);
        return;
    }
    const char* dot = std::strrchr(primary_path, '.');
    if (!dot) {
        std::snprintf(out, out_size, "%s_lod%d", primary_path, lod_idx);
        return;
    }
    size_t stem_len = (size_t)(dot - primary_path);
    std::snprintf(out, out_size, "%.*s_lod%d%s", (int)stem_len, primary_path,
                  lod_idx, dot);
}

int collect_material_ids_from_meshes(
    const FlatMeshArray* meshes,
    std::vector<int>& flat_id_sets,
    std::vector<int>& id_set_counts,
    std::vector<int>& flat_face_ids,
    std::vector<int>& face_id_counts)
{
    flat_id_sets.clear();
    id_set_counts.clear();
    flat_face_ids.clear();
    face_id_counts.clear();

    for (int mi = 0; mi < meshes->count; ++mi) {
        const FlatMesh* fm = &meshes->meshes[mi];
        id_set_counts.push_back(fm->material_id_set_count);
        for (int i = 0; i < fm->material_id_set_count; ++i) {
            flat_id_sets.push_back(fm->material_id_set[i]);
        }
        face_id_counts.push_back(fm->face_count);
        for (int fi = 0; fi < fm->face_count; ++fi) {
            flat_face_ids.push_back(fm->faces[fi].material_id);
        }
    }
    return 0;
}

int build_slim_material_map(
    const FlatMeshArray* meshes,
    int material_count_upper_bound,
    std::vector<int>& sorted_ids,
    std::vector<int>& g2l_map)
{
    std::vector<int> flat_id_sets, id_set_counts;
    std::vector<int> flat_face_ids, face_id_counts;

    if (collect_material_ids_from_meshes(meshes, flat_id_sets, id_set_counts,
                                          flat_face_ids, face_id_counts) != 0) {
        return -1;
    }

    sorted_ids.resize(material_count_upper_bound);
    g2l_map.resize(material_count_upper_bound, -1);

    int unique_count = object_build_global_to_local_map(
        flat_id_sets.empty() ? nullptr : flat_id_sets.data(),
        id_set_counts.empty() ? nullptr : id_set_counts.data(),
        flat_face_ids.empty() ? nullptr : flat_face_ids.data(),
        face_id_counts.empty() ? nullptr : face_id_counts.data(),
        meshes->count,
        material_count_upper_bound,
        sorted_ids.data(),
        material_count_upper_bound,
        g2l_map.data(),
        material_count_upper_bound);

    if (unique_count < 0) return -1;
    sorted_ids.resize(unique_count);
    return unique_count;
}

// Write one LOD's .ase file using caller-provided meshes.
int write_one_lod_ase_from_meshes(
    const FlatMeshArray* meshes,
    const Threedi3di3* ir,
    int lod_idx,
    const char* path,
    const NlascexpOptions* opts,
    const BadFile* bad_file)
{
    if (lod_idx < 0 || lod_idx >= (int)ir->lod_count) return -1;

    if (lod_idx > 0 && (int)ir->lods[lod_idx].render_object_count == 0) return 0;

    const bool skinned = is_skinned(ir);
    const bool primary = (lod_idx == 0);

    std::vector<int> sorted_ids, g2l_map;
    int unique_mat_count = build_slim_material_map(
        meshes, (int)ir->material_count, sorted_ids, g2l_map);
    if (unique_mat_count < 0) return -1;

    if ((int)ir->lods[lod_idx].render_object_count > 0 &&
        ir->material_count > 0 &&
        (g2l_map.empty() || g2l_map[0] < 0)) {
        sorted_ids.insert(sorted_ids.begin(), 0);
        ++unique_mat_count;
        if ((int)g2l_map.size() < (int)ir->material_count)
            g2l_map.resize(ir->material_count, -1);
        for (int gi = 1; gi < (int)ir->material_count; ++gi) {
            if (g2l_map[gi] >= 0) g2l_map[gi] += 1;
        }
        g2l_map[0] = 0;
    }

    int mat_slots = (unique_mat_count > 0)
        ? (int)std::ceil((double)unique_mat_count / SUBS_PER_SLOT)
        : 0;

    int bone_count_max;
    if (primary && skinned) {
        if (bad_file && bad_file->num_bones > 0) {
            bone_count_max = (int)bad_file->num_bones + 1;  // +1 for root_motion
        } else {
            bone_count_max = (int)ir->lods[0].render_object_count;
        }
    } else {
        bone_count_max = 0;
    }
    int main_count_max = meshes->count;
    int dummy_count_max = (int)ir->lods[lod_idx].render_object_count;  // 1 PN per render_object
    int center_count_max = (int)ir->lods[lod_idx].render_object_count;
    int attach_count_max = (int)ir->lods[lod_idx].render_object_count;
    int up_count_max = primary ? (int)ir->user_point_count : 0;
    int coll_count_max = (primary && opts && opts->include_collisions && ir->collision)
        ? (int)ir->collision->volume_count : 0;
    int occ_count_max = (primary && opts && opts->include_occlusion)
        ? (int)ir->occlusion_object_count : 0;
    int light_count_max = (primary && opts && opts->include_lights)
        ? (int)ir->light_count : 0;

    int total_obj_max = bone_count_max + main_count_max + dummy_count_max
                        + center_count_max + attach_count_max + up_count_max
                        + coll_count_max + occ_count_max;
    if (total_obj_max < 1) total_obj_max = 1;

    std::vector<ase_Object> all_objects(total_obj_max);
    std::memset(all_objects.data(), 0, total_obj_max * sizeof(ase_Object));
    int obj_cursor = 0;

    std::vector<ase_Light> lights;
    int light_count = 0;

    if (primary && skinned) {
        int n = object_emit_bone_objects_with_bad(ir, bad_file,
            &all_objects[obj_cursor], total_obj_max - obj_cursor);
        if (n > 0) obj_cursor += n;
    }

    {
        int main_n = 0;
        int emit_rc = object_emit_main_mesh_objects(
            meshes, ir, lod_idx, opts,
            g2l_map.empty() ? nullptr : g2l_map.data(),
            (int)g2l_map.size(),
            &all_objects[obj_cursor], &main_n);
        if (emit_rc != 0) return emit_rc;
        obj_cursor += main_n;
    }

    // Step 2.5: PN## part hierarchy dummies (Phase G.5 - fixes OED
    // "centers outside range of available subobjects" warning).
    {
        int n = object_emit_part_dummies(ir, lod_idx,
            &all_objects[obj_cursor], total_obj_max - obj_cursor);
        if (n > 0) obj_cursor += n;
    }

    {
        int n = object_emit_center_markers(ir, lod_idx,
            &all_objects[obj_cursor], total_obj_max - obj_cursor);
        if (n > 0) obj_cursor += n;
    }

    {
        int n = object_emit_attach_markers(ir, lod_idx,
            &all_objects[obj_cursor], total_obj_max - obj_cursor);
        if (n > 0) obj_cursor += n;
    }

    if (primary) {
        int up_n = object_emit_userpoint_markers(ir,
            &all_objects[obj_cursor], total_obj_max - obj_cursor);
        if (up_n > 0) obj_cursor += up_n;

        if (opts && opts->include_collisions && ir->collision) {
            int coll_n = object_emit_collision_helpers(ir,
                &all_objects[obj_cursor], total_obj_max - obj_cursor);
            if (coll_n > 0) obj_cursor += coll_n;
        }

        if (opts && opts->include_occlusion) {
            int occ_n = object_emit_occlusion_helpers(ir,
                &all_objects[obj_cursor], total_obj_max - obj_cursor);
            if (occ_n > 0) obj_cursor += occ_n;
        }
    }

    int total_objects = obj_cursor;

    if (primary && opts && opts->include_lights && light_count_max > 0) {
        lights.resize(light_count_max);
        std::memset(lights.data(), 0, light_count_max * sizeof(ase_Light));
        light_count = object_emit_lights(ir, lights.data(), light_count_max);
        if (light_count < 0) light_count = 0;
    }

    ase_Document doc = {};
    ase_alloc(&doc, total_objects, mat_slots, light_count);
    doc.flags = skinned ? 1 : 0;
    doc.skinned_flags = skinned ? 1 : 0;

    if (unique_mat_count > 0) {
        object_populate_material_list(ir, sorted_ids.data(), unique_mat_count, &doc);
    }
    for (int i = 0; i < total_objects; ++i) {
        doc.objects[i] = all_objects[i];
    }
    for (int i = 0; i < light_count; ++i) {
        doc.lights[i] = lights[i];
    }

    int rc = ase_write(path, &doc);
    ase_free(&doc);
    return rc;
}

}  // namespace

extern "C" int object_write_ase_files_from_flat_meshes_with_bad(
    const struct FlatMeshArray* meshes_per_lod,
    int lod_count,
    const struct Threedi3di3* ir,
    const struct BadFile* bad_file,
    const char* primary_path,
    const NlascexpOptions* opts,
    char** out_paths_array,
    int out_paths_capacity,
    int* out_paths_count)
{
    if (!meshes_per_lod || !ir || !primary_path || !out_paths_array || !out_paths_count) return -1;
    if (lod_count != (int)ir->lod_count) return -2;

    NlascexpOptions default_opts;
    if (!opts) {
        object_nlascexp_options_init_defaults(&default_opts);
        opts = &default_opts;
    }

    *out_paths_count = 0;

    for (int lod_idx = 0;
         lod_idx < lod_count && *out_paths_count < out_paths_capacity;
         ++lod_idx) {
        char path[260];
        build_lod_filename(primary_path, lod_idx, path, sizeof(path));

        int rc = write_one_lod_ase_from_meshes(&meshes_per_lod[lod_idx], ir, lod_idx, path, opts, bad_file);
        if (rc != 0) return rc;

        std::strncpy(out_paths_array[*out_paths_count], path, 259);
        out_paths_array[*out_paths_count][259] = '\0';
        ++(*out_paths_count);
    }

    return 0;
}

extern "C" int object_write_ase_files_from_flat_meshes(
    const struct FlatMeshArray* meshes_per_lod,
    int lod_count,
    const struct Threedi3di3* ir,
    const char* primary_path,
    const NlascexpOptions* opts,
    char** out_paths_array,
    int out_paths_capacity,
    int* out_paths_count)
{
    return object_write_ase_files_from_flat_meshes_with_bad(
        meshes_per_lod, lod_count, ir, /*bad_file=*/nullptr,
        primary_path, opts, out_paths_array, out_paths_capacity, out_paths_count);
}
