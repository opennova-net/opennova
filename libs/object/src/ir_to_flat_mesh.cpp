#include "object/ir_to_flat_mesh.h"
#include "object/coords.h"
#include "object/flat_mesh.h"
#include "object/smoothing_groups.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int object_ir_to_flat_meshes_v2(const Threedi3di3* ir, int lod_index,
                                int include_empty_parts, int track_bone_data,
                                int preserve_source_indexing, FlatMeshArray* out) {
    if (!ir || !out) return -1;
    if (lod_index < 0 || static_cast<size_t>(lod_index) >= ir->lod_count) return -1;

    const ThreediLod* lod = &ir->lods[lod_index];

    // Build one FlatMesh per render-object part. The synthetic quad has
    // 1 render object; production fixtures have N. Skip parts that own
    // no strips (unless include_empty_parts is set).
    //
    // mesh_build.py:_flatten_part is the Python reference; nlascexp.dle ::
    // ASE_GetExportTriObject @ 0x1000a6f0 confirms no welding/splitting.

    int part_count = 0;
    for (size_t i = 0; i < lod->render_object_count; ++i) {
        const ThreediRenderObject* ro = &lod->render_objects[i];
        if (include_empty_parts || (ro->num_strips + ro->num_alpha_strips) > 0) {
            ++part_count;
        }
    }
    if (object_flat_mesh_array_alloc(part_count, out) != 0) return -1;

    int out_index = 0;
    for (size_t pi = 0; pi < lod->render_object_count; ++pi) {
        const ThreediRenderObject* ro = &lod->render_objects[pi];
        if (!include_empty_parts && (ro->num_strips + ro->num_alpha_strips) == 0) continue;

        // Determine the part's strip range. Strips are stored linearly in
        // lod->strips; consecutive strips with the same parent render-object
        // index belong to one part.
        // Per-RO strip count = non-alpha + alpha; writer emits them adjacent
        // [non-alpha][alpha] per RO (see libs/object/src/rdta.cpp).
        int strip_begin = 0;
        for (size_t prior_pi = 0; prior_pi < pi; ++prior_pi) {
            const ThreediRenderObject* prior_ro = &lod->render_objects[prior_pi];
            strip_begin += static_cast<int>(prior_ro->num_strips + prior_ro->num_alpha_strips);
        }
        int strip_count = static_cast<int>(ro->num_strips + ro->num_alpha_strips);

        // Pass 1: Walk all strips and collect raw face corners (absolute source
        // indices, after winding swap) plus material id per face.
        struct RawCorner { int abs_src; };
        std::vector<RawCorner> face_corners;  // size = num_tris * 3
        std::vector<int> face_mat;            // size = num_tris

        int num_tris_total = 0;
        for (int si = 0; si < strip_count; ++si) {
            const ThreediTriangleStrip* s = &lod->strips[strip_begin + si];
            const uint16_t* idx = lod->indices.indices + s->index_offset;
            if (s->is_strip == 0) {
                for (int t = 0; t < static_cast<int>(s->num_triangles); ++t) {
                    // Winding swap (0, 2, 1) matches Python mesh_build.py:197-200.
                    face_corners.push_back({static_cast<int>(idx[t * 3 + 0]) + s->start_vertex});
                    face_corners.push_back({static_cast<int>(idx[t * 3 + 2]) + s->start_vertex});
                    face_corners.push_back({static_cast<int>(idx[t * 3 + 1]) + s->start_vertex});
                    face_mat.push_back(static_cast<int>(s->material_index));
                    ++num_tris_total;
                }
            } else {
                // Triangle strip: every consecutive (i, i+1, i+2) with alternating winding.
                for (int t = 0; t < static_cast<int>(s->num_triangles); ++t) {
                    int a = static_cast<int>(idx[t + 0]) + s->start_vertex;
                    int b = static_cast<int>(idx[t + 1]) + s->start_vertex;
                    int c = static_cast<int>(idx[t + 2]) + s->start_vertex;
                    if ((t & 1) == 0) {
                        face_corners.push_back({a});
                        face_corners.push_back({b});
                        face_corners.push_back({c});
                    } else {
                        face_corners.push_back({a});
                        face_corners.push_back({c});
                        face_corners.push_back({b});
                    }
                    face_mat.push_back(static_cast<int>(s->material_index));
                    ++num_tris_total;
                }
            }
        }

        int face_count = num_tris_total;

        // Pass 2: Build vert assignment based on preserve_source_indexing.
        // source_indices[local] = absolute source index.
        // face_local[corner_i] = local index for that corner.
        std::vector<int> source_indices;
        std::vector<int> face_local;

        if (preserve_source_indexing) {
            // First-seen-wins dedup by absolute source index.
            int total_verts = static_cast<int>(lod->vertices.count);
            std::vector<int> seen_map(static_cast<size_t>(total_verts), -1);
            for (size_t i = 0; i < face_corners.size(); ++i) {
                int abs_src = face_corners[i].abs_src;
                int local;
                if (abs_src >= 0 && abs_src < total_verts && seen_map[abs_src] >= 0) {
                    local = seen_map[abs_src];
                } else {
                    local = static_cast<int>(source_indices.size());
                    if (abs_src >= 0 && abs_src < total_verts) {
                        seen_map[abs_src] = local;
                    }
                    source_indices.push_back(abs_src);
                }
                face_local.push_back(local);
            }
        } else {
            // Slice-based (existing behavior): vert_count = max_end - min_start.
            int min_start = lod->vertices.count > 0
                ? static_cast<int>(lod->vertices.count) : 0;
            int max_end = 0;
            for (int si = 0; si < strip_count; ++si) {
                const ThreediTriangleStrip* s = &lod->strips[strip_begin + si];
                int s_end = s->start_vertex + s->num_vertices;
                if (s->start_vertex < min_start) min_start = s->start_vertex;
                if (s_end > max_end) max_end = s_end;
            }
            int vert_count_slice = (max_end > min_start) ? (max_end - min_start) : 0;
            for (int v = 0; v < vert_count_slice; ++v) {
                source_indices.push_back(min_start + v);
            }
            // Offset each corner from the slice base; mirrors old (abs - min_start).
            for (size_t i = 0; i < face_corners.size(); ++i) {
                face_local.push_back(face_corners[i].abs_src - min_start);
            }
        }

        int vert_count = static_cast<int>(source_indices.size());

        // Pass 3: Bone influence pre-count.
        // Python mesh_build.py:163-181: iterates min(4, len(v.bone_weights)) = 3
        // (ThreediVertex.bone_weights has length 3). No implicit 4th weight.
        // Falls back to (part_idx, 1.0) when no positive weights exist.
        int total_influences = 0;
        if (track_bone_data) {
            for (int v = 0; v < vert_count; ++v) {
                const ThreediVertex* src = &lod->vertices.items[source_indices[v]];
                bool any = false;
                if (src->is_skinned) {
                    for (int k = 0; k < 3; ++k) {
                        if (src->bone_weights[k] > 0.0f) { ++total_influences; any = true; }
                    }
                }
                if (!any) ++total_influences;  // Python fallback: (part_idx, 1.0)
            }
        }

        FlatMesh* fm = &out->meshes[out_index];
        if (object_flat_mesh_resize(fm, vert_count, face_count, 0, total_influences) != 0) {
            object_flat_mesh_array_free(out);
            return -1;
        }

        // Pass 4: Copy positions.
        //
        // Python computes world_verts = origin + flat_local where:
        //   origin = render_space(abs)   -- in double (Python float from ctypes)
        //   flat_local = render_space(pos - abs)  -- in double (double subtraction)
        //   world = origin + flat_local  -- in double (_vec_add uses float64)
        //
        // Algebraically: render_space(abs) + render_space(pos - abs) = render_space(pos).
        // Python's double arithmetic gives the same result as render_space(double(pos))
        // because all values come from float32 struct fields accessed as double-precision
        // Python floats; no extra rounding is introduced by the double subtraction.
        //
        // We store world-space (absolute render_space) coordinates in fm->vertices,
        // computed as (float)render_space_double(pos). The origin in fm->origin is
        // preserved for the TM matrix only. flat_mesh_to_ase adds NO origin to
        // fm->vertices — it uses them as already world-space.
        for (int v = 0; v < vert_count; ++v) {
            const ThreediVertex* src = &lod->vertices.items[source_indices[v]];
            // render_space(pos) in double: (-px, -pz, py)
            fm->vertices[v].x = (float)(-(double)src->position[0]);
            fm->vertices[v].y = (float)(-(double)src->position[2]);
            fm->vertices[v].z = (float)( (double)src->position[1]);
        }

        // Pass 5: Copy face triplets + material_id.
        // smoothing_group_mask is filled after Pass 6 once normals are known.
        for (int f = 0; f < face_count; ++f) {
            fm->faces[f].v[0] = face_local[f * 3 + 0];
            fm->faces[f].v[1] = face_local[f * 3 + 1];
            fm->faces[f].v[2] = face_local[f * 3 + 2];
            fm->faces[f].material_id = face_mat[f];
            fm->faces[f].smoothing_group_mask = 0;
        }

        // Pass 6: Per-corner UVs (3 entries per face, in face order) + render_space normals.
        // UV V-flip: source has V=0 at top, target DCCs use V=0 at bottom.
        // Mirrors pyopennova/mesh_build.py:241-255.
        for (int f = 0; f < face_count; ++f) {
            for (int corner = 0; corner < 3; ++corner) {
                int v = fm->faces[f].v[corner];
                const ThreediVertex* src = &lod->vertices.items[source_indices[v]];
                FlatMeshFaceCornerData* cd = &fm->corners[f * 3 + corner];
                cd->u0 = src->uv0[0];
                cd->v0 = 1.0f - src->uv0[1];
                cd->u1 = src->uv1[0];
                cd->v1 = 1.0f - src->uv1[1];
                float n[3];
                object_render_space(src->normal[0], src->normal[1], src->normal[2], n);
                cd->nx = n[0];
                cd->ny = n[1];
                cd->nz = n[2];
            }
        }

        // Smoothing groups from per-face-corner normals (mesh_build.py:260-263).
        if (face_count > 0) {
            std::vector<int> face_indices_flat(face_count * 3);
            std::vector<float> normals_flat(face_count * 3 * 3);
            for (int f = 0; f < face_count; ++f) {
                face_indices_flat[f * 3 + 0] = fm->faces[f].v[0];
                face_indices_flat[f * 3 + 1] = fm->faces[f].v[1];
                face_indices_flat[f * 3 + 2] = fm->faces[f].v[2];
                for (int corner = 0; corner < 3; ++corner) {
                    FlatMeshFaceCornerData* cd = &fm->corners[f * 3 + corner];
                    normals_flat[(f * 3 + corner) * 3 + 0] = cd->nx;
                    normals_flat[(f * 3 + corner) * 3 + 1] = cd->ny;
                    normals_flat[(f * 3 + corner) * 3 + 2] = cd->nz;
                }
            }
            std::vector<uint32_t> groups(face_count, 0);
            object_compute_smoothing_groups(face_count, face_indices_flat.data(),
                                            normals_flat.data(), groups.data());
            for (int f = 0; f < face_count; ++f) {
                fm->faces[f].smoothing_group_mask = groups[f];
            }
        }

        // Pass 7: Bone CSR fill.
        // Python mesh_build.py:163-181: bone_table resolution + fallback.
        // Python then calls _pack_weights which sorts entries by bone_index ascending.
        // We must match this sort to produce identical MESH_WEIGHTSVERTEX output.
        if (track_bone_data && total_influences > 0) {
            struct InflEntry { int bone_index; float weight; };
            int infl_cursor = 0;
            for (int v = 0; v < vert_count; ++v) {
                fm->vertex_influence_offsets[v] = infl_cursor;
                int abs_v = source_indices[v];
                const ThreediVertex* src = &lod->vertices.items[abs_v];

                // Find the strip that owns this absolute vertex.
                // All strips in this RO share the same bone_table.
                // Python mesh_build.py:170-175: skel_idx = bone_table[local_idx]
                // when table_len > 0 and local_idx < table_len, else 0.
                const ThreediTriangleStrip* owning_strip = nullptr;
                for (int si = 0; si < strip_count; ++si) {
                    const ThreediTriangleStrip* s = &lod->strips[strip_begin + si];
                    if (abs_v >= s->start_vertex &&
                        abs_v < s->start_vertex + s->num_vertices) {
                        owning_strip = s;
                        break;
                    }
                }
                int table_len = owning_strip
                    ? static_cast<int>(owning_strip->bone_table_length) : 0;

                // Collect influences for this vertex.
                std::vector<InflEntry> per_vert;
                if (src->is_skinned) {
                    for (int k = 0; k < 3; ++k) {
                        if (src->bone_weights[k] <= 0.0f) continue;
                        int local_idx = static_cast<int>(src->bone_indices[k]);
                        int skel_idx = 0;
                        if (owning_strip && table_len > 0 && local_idx < table_len) {
                            skel_idx = static_cast<int>(owning_strip->bone_table[local_idx]);
                        }
                        per_vert.push_back({skel_idx, src->bone_weights[k]});
                    }
                }
                if (per_vert.empty()) {
                    // Fallback: (part_idx, 1.0) -- mirrors Python mesh_build.py:178-179.
                    per_vert.push_back({fm->part_index, 1.0f});
                }

                // Blender and Max skin modifiers cannot represent duplicate
                // influences for the same bone. Merge duplicates here so the
                // direct ASE path matches what a DCC scene can faithfully store.
                std::sort(per_vert.begin(), per_vert.end(),
                          [](const InflEntry& a, const InflEntry& b) {
                              return a.bone_index < b.bone_index;
                          });
                std::vector<InflEntry> merged;
                for (const InflEntry& e : per_vert) {
                    if (!merged.empty() && merged.back().bone_index == e.bone_index) {
                        merged.back().weight += e.weight;
                    } else {
                        merged.push_back(e);
                    }
                }
                per_vert.swap(merged);

                // Cap at 4 (Python _pack_weights[:4]).
                if ((int)per_vert.size() > 4) per_vert.resize(4);

                // Store into CSR.
                for (const auto& e : per_vert) {
                    fm->vertex_influences[infl_cursor].bone_index = e.bone_index;
                    fm->vertex_influences[infl_cursor].weight = e.weight;
                    ++infl_cursor;
                }
            }
            fm->vertex_influence_offsets[vert_count] = infl_cursor;
        }

        // material_id_set: ordered unique material ids (first-seen wins).
        {
            // Worst case: each face has a unique material.
            int* tmp = static_cast<int*>(std::calloc(static_cast<size_t>(face_count), sizeof(int)));
            int tmp_count = 0;
            if (face_count > 0 && !tmp) {
                object_flat_mesh_array_free(out);
                return -1;
            }
            for (int f = 0; f < face_count; ++f) {
                int mid = fm->faces[f].material_id;
                bool seen = false;
                for (int j = 0; j < tmp_count; ++j) {
                    if (tmp[j] == mid) { seen = true; break; }
                }
                if (!seen) tmp[tmp_count++] = mid;
            }
            // object_flat_mesh_resize already allocated material_id_set,
            // but we sized it to 0. Re-allocate to actual count.
            std::free(fm->material_id_set);
            fm->material_id_set_count = tmp_count;
            fm->material_id_set = tmp_count > 0
                ? static_cast<int32_t*>(std::calloc(static_cast<size_t>(tmp_count), sizeof(int32_t)))
                : nullptr;
            for (int j = 0; j < tmp_count; ++j) {
                fm->material_id_set[j] = static_cast<int32_t>(tmp[j]);
            }
            std::free(tmp);
        }

        // name: Python mesh_build.py line 86: "{part_idx + 1:02d} Mesh0"
        std::snprintf(fm->name, sizeof(fm->name), "%02d Mesh0", static_cast<int>(pi) + 1);
        fm->part_index = static_cast<int>(pi);
        // Store render-space origin (swizzled): mirrors coords.render_space(abs).
        // Python coords.render_space: (x,y,z) -> (-x, -z, y)
        fm->origin[0] = -ro->abs[0];
        fm->origin[1] = -ro->abs[2];
        fm->origin[2] =  ro->abs[1];

        ++out_index;
    }
    return 0;
}

// V0 wrapper: preserve_source_indexing=0 (slice-based, existing behavior).
int object_ir_to_flat_meshes(const Threedi3di3* ir, int lod_index,
                             int include_empty_parts, int track_bone_data,
                             FlatMeshArray* out) {
    return object_ir_to_flat_meshes_v2(ir, lod_index, include_empty_parts,
                                       track_bone_data, /*preserve_source_indexing=*/0,
                                       out);
}
