// libs/object/src/material_list_to_ase.cpp
//
// Port of pyopennova/ase_from_3di3.py::_populate_materials (lines 679-707) and
// populate_ase_submaterial (lines 65-130).
//
// NOTE ON SUBS_PER_SLOT:
//   The canonical Python source (ase_from_3di3.py line 32) defines SUBS_PER_SLOT = 64.
//   The task description says 16, which is incorrect. We use 64 to match the Python
//   source of truth. flat_mesh_to_ase.cpp uses 16 and will need a separate fix.

#include "object/material_list_to_ase.h"

#include "ase/ase.h"           // ase_alloc_submaterials
#include "ase/types.h"         // ase_Material, ase_Document
#include "threedi/threedi_3di3.h" // Threedi3di3, ThreediMaterial, ThreediMaterialTexture

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <vector>

// Must match ase_from_3di3.py line 32.
static const int SUBS_PER_SLOT = 64;

// ---------------------------------------------------------------------------
// Local helpers — mirror of pyopennova/ase_from_3di3.py::populate_ase_submaterial
// ---------------------------------------------------------------------------

// Truncate a null-terminated src into dst[0..max_len-1] (dst has size max_len).
// Result is always null-terminated.
static void copy_truncated(char* dst, int dst_size, const char* src)
{
    if (!dst || dst_size <= 0) return;
    int n = dst_size - 1;
    int i = 0;
    while (i < n && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

// Return the diffuse texture name from an IR material's texture slots.
// Mirrors pyopennova/materials.py::_resolve_textures (lines 1025-1049):
//   for each texture, if role=="diffuse" and slot==DIFFUSE (or no diffuse yet),
//   overwrite diffuse. The LAST matching texture wins.
// This means if there are multiple DIFFUSE-slot textures (tex_anim sequences),
// we return the last one.
static const char* find_diffuse_tex(const ThreediMaterial* mat)
{
    const char* result = nullptr;
    for (uint32_t i = 0; i < mat->texture_count && i < 24; ++i) {
        if (mat->textures[i].slot == THREEDI_TEX_SLOT_DIFFUSE && mat->textures[i].name[0]) {
            result = mat->textures[i].name;  // overwrite; last wins
        }
    }
    return result;
}

static const char* find_detail_tex(const ThreediMaterial* mat)
{
    for (uint32_t i = 0; i < mat->texture_count && i < 24; ++i) {
        if (mat->textures[i].slot == THREEDI_TEX_SLOT_DETAIL) {
            return mat->textures[i].name;
        }
    }
    return nullptr;
}

// Port of populate_ase_submaterial (ase_from_3di3.py lines 65-130).
// We work directly from ThreediMaterial instead of a MaterialDescriptor
// because the full descriptor pipeline (describe_material, derive_uv1_tilings)
// is Python-side and not yet ported to C++. The fields we set here cover the
// common case and match the Python output for standard (non-glass, non-animated)
// materials. The gate test (Task 10) will expose any remaining gaps.
//
// global_mat_index: the 3DI3 model's global material index (= mat->index).
// Used to build the canonical name "Material_{index}_{shader}" matching
// pyopennova.materials.describe_material line 406:
//   name = "Material_%d_%s" % (index, shader)
//
// uv1_u_tiling, uv1_v_tiling: pre-computed channel-1 UV tiling from
// derive_uv1_tilings_cpp (mirroring pyopennova.materials.derive_uv1_tilings).
static void populate_submaterial_from_ir(ase_Material* sub, const ThreediMaterial* mat,
                                          int global_mat_index,
                                          float uv1_u_tiling, float uv1_v_tiling)
{
    // name: Python line 406: "Material_{index}_{shader_name}"
    {
        char name_buf[64];
        const char* sn = mat->shader_name[0] ? mat->shader_name : "FF_ST_OP";
        std::snprintf(name_buf, sizeof(name_buf), "Material_%d_%s", global_mat_index, sn);
        copy_truncated(sub->name, (int)sizeof(sub->name), name_buf);
    }

    // maps[0] = MAP_DIFFUSE; maps[1] = detail; maps[2] = MAP_OPACITY (alpha test)
    const char* diffuse = find_diffuse_tex(mat);
    const char* detail  = find_detail_tex(mat);

    if (diffuse && diffuse[0]) {
        copy_truncated(sub->maps[0], (int)sizeof(sub->maps[0]), diffuse);
    }
    if (detail && detail[0]) {
        copy_truncated(sub->maps[1], (int)sizeof(sub->maps[1]), detail);
    }
    // MAP_OPACITY: alpha test + diffuse present (Python line 85-88)
    if ((mat->material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) && diffuse && diffuse[0]) {
        copy_truncated(sub->maps[2], (int)sizeof(sub->maps[2]), diffuse);
    }

    // UV tiling (Python lines 97-100):
    //   sub.uv_u_tiling[0] = desc.effective_u_tiling  (3DI3 MTRL has no u_tiling field → 1.0)
    //   sub.uv_v_tiling[0] = desc.effective_v_tiling  (same → 1.0)
    //   sub.uv_u_tiling[1] = desc.effective_u1_tiling (from derive_uv1_tilings)
    //   sub.uv_v_tiling[1] = desc.effective_v1_tiling (from derive_uv1_tilings)
    sub->uv_u_tiling[0] = 1.0f;
    sub->uv_v_tiling[0] = 1.0f;
    sub->uv_u_tiling[1] = uv1_u_tiling;
    sub->uv_v_tiling[1] = uv1_v_tiling;

    // Default Max material colors (Python lines 108-116).
    // Glass reflect_color goes into ambient (Python lines 104-107).
    if (mat->is_glass &&
        (mat->reflect_color[0] != 0.0f ||
         mat->reflect_color[1] != 0.0f ||
         mat->reflect_color[2] != 0.0f))
    {
        // reflect_color is BGRA; Python uses desc.reflect_color[:3]
        // which maps to the same underlying array.
        sub->ambient[0] = mat->reflect_color[0];
        sub->ambient[1] = mat->reflect_color[1];
        sub->ambient[2] = mat->reflect_color[2];
    } else {
        sub->ambient[0] = sub->ambient[1] = sub->ambient[2] = 0.0588f;
    }

    sub->diffuse[0]  = sub->diffuse[1]  = sub->diffuse[2]  = 0.5882f;
    sub->specular[0] = sub->specular[1] = sub->specular[2] = 0.9f;
    sub->shine          = 0.1f;
    sub->shine_strength = 0.0f;
    sub->transparency   = 0.0f;
    sub->wiresize       = 1.0f;

    // MATERIAL_TWOSIDED: extra_flags bit 0 (Python lines 121-122).
    if (mat->material_flags & THREEDI_MATERIAL_FLAG_TWO_SIDED) {
        sub->extra_flags |= 1u;
    }

    // Shading: Phong (1) vs Blinn (0).
    // Python line 126-129: phong if phong_shader or shader_family in
    // ("phong", "environment", "glass"). We derive from shader_name directly.
    {
        const char* sn = mat->shader_name;
        bool phong = mat->is_glass != 0 ||
                     (std::strstr(sn, "PHONG") != nullptr) ||
                     (std::strstr(sn, "ENV")   != nullptr);
        sub->shading = phong ? 1 : 0;
    }
}

// ---------------------------------------------------------------------------
// UV1 tiling back-calculation
//
// Port of pyopennova.materials.derive_uv1_tilings (lines 317-355).
// Back-calculates per-material (u1_tiling, v1_tiling) from per-vertex
// (uv0, uv1) ratios: tiling = (uv1 - 0.5) / (uv0 - 0.5).
// EPS mirrors the Python value 1e-4.
// Returns the median of collected ratios, or 1.0 if no usable samples.
// ---------------------------------------------------------------------------

namespace {

static float median_float(std::vector<float>& v) {
    if (v.empty()) return 1.0f;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n % 2 == 1) return v[n / 2];
    return 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

// Fills out_u and out_v arrays (indexed by material index) with the back-calculated
// uv1 tiling for each material. Both arrays must have at least mat_count entries.
// out_u[i] and out_v[i] default to 1.0.
static void derive_uv1_tilings_cpp(const Threedi3di3* ir, float* out_u, float* out_v)
{
    static const float EPS = 1e-4f;
    uint32_t mat_count = ir->material_count;
    for (uint32_t m = 0; m < mat_count; ++m) {
        out_u[m] = 1.0f;
        out_v[m] = 1.0f;
    }

    // Python collects ratios across ALL LODs before taking a single median per material.
    // (Python lines 334-352 have the lod loop INSIDE the mat_idx loop, accumulating
    // u_ratios/v_ratios across all LODs before the final _median call at line 353.)
    // C++ must mirror this: collect all ratios first, then median once per material.
    std::vector<std::vector<float>> u_ratios(mat_count);
    std::vector<std::vector<float>> v_ratios(mat_count);

    for (size_t lod_i = 0; lod_i < ir->lod_count; ++lod_i) {
        const ThreediLod* lod = &ir->lods[lod_i];
        const ThreediVertex* verts = lod->vertices.items;
        size_t total_verts = (size_t)lod->vertices.count;

        // Iterate strips (= primitives). Mirrors Python's lod.primitives.
        for (size_t si = 0; si < lod->strip_count; ++si) {
            const ThreediTriangleStrip* strip = &lod->strips[si];
            int32_t mat_idx = strip->material_index;
            if (mat_idx < 0 || (uint32_t)mat_idx >= mat_count) continue;

            int32_t vstart = strip->start_vertex;
            int32_t vcount = strip->num_vertices;
            if (vstart < 0 || vcount <= 0) continue;
            if ((size_t)(vstart + vcount) > total_verts) continue;

            for (int32_t vi = vstart; vi < vstart + vcount; ++vi) {
                const ThreediVertex* v = &verts[vi];
                float u0 = v->uv0[0] - 0.5f;
                float u1 = v->uv1[0] - 0.5f;
                float v0 = v->uv0[1] - 0.5f;
                float v1 = v->uv1[1] - 0.5f;
                if (std::fabs(u0) > EPS)
                    u_ratios[(size_t)mat_idx].push_back(u1 / u0);
                if (std::fabs(v0) > EPS)
                    v_ratios[(size_t)mat_idx].push_back(v1 / v0);
            }
        }
    }

    // Single median per material across all LODs (matches Python _median call at line 353).
    for (uint32_t m = 0; m < mat_count; ++m) {
        if (!u_ratios[m].empty())
            out_u[m] = median_float(u_ratios[m]);
        if (!v_ratios[m].empty())
            out_v[m] = median_float(v_ratios[m]);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

extern "C" int object_build_global_to_local_map(
    const int* per_spec_material_id_sets,
    const int* per_spec_material_id_set_counts,
    const int* per_spec_face_material_ids,
    const int* per_spec_face_material_id_counts,
    int spec_count,
    int material_count_upper_bound,
    int* out_sorted_ids,
    int out_sorted_ids_capacity,
    int* out_global_to_local,
    int out_global_to_local_capacity)
{
    if (material_count_upper_bound <= 0 || spec_count < 0) return -1;
    if (out_global_to_local_capacity < material_count_upper_bound) return -1;

    // seen[gid] = true if gid appears in any spec's material refs.
    std::vector<bool> seen(static_cast<size_t>(material_count_upper_bound), false);

    // Initialize out_global_to_local to -1 (unseen).
    for (int i = 0; i < out_global_to_local_capacity; ++i) {
        out_global_to_local[i] = -1;
    }

    // Walk per-spec material_id_sets.
    {
        int offset = 0;
        for (int s = 0; s < spec_count; ++s) {
            int cnt = per_spec_material_id_set_counts ? per_spec_material_id_set_counts[s] : 0;
            for (int k = 0; k < cnt; ++k) {
                int gid = per_spec_material_id_sets ? per_spec_material_id_sets[offset + k] : -1;
                if (gid >= 0 && gid < material_count_upper_bound) {
                    seen[static_cast<size_t>(gid)] = true;
                }
            }
            offset += cnt;
        }
    }

    // Walk per-spec face_material_ids.
    {
        int offset = 0;
        for (int s = 0; s < spec_count; ++s) {
            int cnt = per_spec_face_material_id_counts ? per_spec_face_material_id_counts[s] : 0;
            for (int k = 0; k < cnt; ++k) {
                int gid = per_spec_face_material_ids ? per_spec_face_material_ids[offset + k] : -1;
                if (gid >= 0 && gid < material_count_upper_bound) {
                    seen[static_cast<size_t>(gid)] = true;
                }
            }
            offset += cnt;
        }
    }

    // Collect sorted unique IDs.
    int local_idx = 0;
    for (int gid = 0; gid < material_count_upper_bound; ++gid) {
        if (!seen[static_cast<size_t>(gid)]) continue;
        if (local_idx >= out_sorted_ids_capacity) return -1; // overflow
        out_sorted_ids[local_idx]              = gid;
        out_global_to_local[gid]               = local_idx;
        ++local_idx;
    }

    return local_idx; // count of unique IDs
}

extern "C" int object_populate_material_list(
    const struct Threedi3di3* ir,
    const int* sorted_global_ids,
    int sorted_count,
    struct ase_Document* doc)
{
    if (!ir || !doc) return -1;
    if (sorted_count <= 0) return 0;

    int mat_count = (int)doc->material_count;

    // Back-calculate per-material uv1 tilings (Python: derive_uv1_tilings).
    // Port of pyopennova.materials.derive_uv1_tilings (lines 317-355).
    uint32_t total_mats = ir->material_count;
    std::vector<float> uv1_u(total_mats, 1.0f), uv1_v(total_mats, 1.0f);
    if (total_mats > 0) {
        derive_uv1_tilings_cpp(ir, uv1_u.data(), uv1_v.data());
    }

    // Populate each slot.
    for (int slot_idx = 0; slot_idx < mat_count; ++slot_idx) {
        int start        = slot_idx * SUBS_PER_SLOT;
        int end          = start + SUBS_PER_SLOT;
        if (end > sorted_count) end = sorted_count;
        int count_in_slot = end - start;

        ase_Material* slot = &doc->materials[slot_idx];

        // Slot name: Python line 691.
        char slot_name[32];
        std::snprintf(slot_name, sizeof(slot_name), "Scene_Materials_%d", slot_idx);
        copy_truncated(slot->name, (int)sizeof(slot->name), slot_name);

        // Allocate submaterials for this slot.
        ase_alloc_submaterials(slot, count_in_slot);

        // Populate each sub.
        for (int sub_idx = 0; sub_idx < count_in_slot; ++sub_idx) {
            int lid      = start + sub_idx;
            int global_id = sorted_global_ids[lid];
            if (global_id < 0 || (uint32_t)global_id >= ir->material_count) {
                continue; // skip out-of-range
            }
            float eu1 = ((uint32_t)global_id < total_mats) ? uv1_u[(uint32_t)global_id] : 1.0f;
            float ev1 = ((uint32_t)global_id < total_mats) ? uv1_v[(uint32_t)global_id] : 1.0f;
            ase_Material* sub = &slot->submaterials[sub_idx];
            populate_submaterial_from_ir(sub, &ir->materials[global_id], global_id, eu1, ev1);
        }
    }

    return mat_count;
}
