// libs/object/include/object/material_list_to_ase.h
//
// Port of pyopennova/ase_from_3di3.py::_populate_materials (lines 679-707) and
// populate_ase_submaterial (lines 65-130). Handles the slim material list
// variant: only the global material indices actually referenced by this LOD's
// objects are included, packed into Multi/Sub-Object slots (SUBS_PER_SLOT subs
// per slot), in sorted-global-index order.
//
// Public API:
//   object_build_global_to_local_map   — collect unique IDs + build remap table
//   object_populate_material_list      — allocate slots + populate submaterials
#pragma once

struct Threedi3di3;
struct ase_Document;

#ifdef __cplusplus
extern "C" {
#endif

// Collect the sorted unique global material IDs referenced by this LOD's
// objects, and build the global->local index remap table.
//
// per_spec_material_id_sets       flat array of all spec material_id_set values
// per_spec_material_id_set_counts length of each spec's material_id_set slice
// per_spec_face_material_ids      flat array of all spec face_material_ids
// per_spec_face_material_id_counts length of each spec's face_material_ids slice
// spec_count                      number of specs
// material_count_upper_bound      exclusive upper bound for valid global IDs
// out_sorted_ids                  caller buffer; receives sorted unique IDs
// out_sorted_ids_capacity         max entries in out_sorted_ids
// out_global_to_local             caller buffer of length material_count_upper_bound;
//                                 entry [gid] = local index, or -1 if not used
// out_global_to_local_capacity    must be >= material_count_upper_bound
//
// Returns the number of unique global IDs written to out_sorted_ids, or -1 on
// error (capacity exceeded or invalid argument).
int object_build_global_to_local_map(
    const int* per_spec_material_id_sets,
    const int* per_spec_material_id_set_counts,
    const int* per_spec_face_material_ids,
    const int* per_spec_face_material_id_counts,
    int spec_count,
    int material_count_upper_bound,
    int* out_sorted_ids,
    int out_sorted_ids_capacity,
    int* out_global_to_local,
    int out_global_to_local_capacity);

// Given an IR + sorted_global_ids, allocate doc->materials slots and populate
// submaterials per the slim layout (SUBS_PER_SLOT subs per slot).
// doc->material_count must already be set to ceil(sorted_count / SUBS_PER_SLOT)
// by the caller (via ase_alloc).
// Returns the number of slot entries used, or -1 on error.
int object_populate_material_list(
    const struct Threedi3di3* ir,
    const int* sorted_global_ids,
    int sorted_count,
    struct ase_Document* doc);

#ifdef __cplusplus
}
#endif
