// libs/object/src/occlusion_to_ase.cpp
//
// Port of pyopennova/ase_from_3di3.py::_occlusion_helper_objects (lines 532-601).
//
// Supporting helpers:
//   build_occlusion_name  (pyopennova/scene_naming.py::build_occlusion_name lines 71-82)
//   format_dup_suffix     (pyopennova/scene_naming.py::format_duplicate_suffix lines 98-107)
//
// IR field mapping:
//   Python                                  C (Threedi3di3)
//   occlusion_access(ir)["object_count"]    ir->occlusion_object_count
//   occ["objects"][i]                       ir->occlusion_objects[i]  (ThreediOcclusionObject)
//   occ_obj.num_vertices                    obj->num_vertices
//   occ_obj.face_count                      obj->face_count
//   occ_obj.vertex_start                    (cursor-computed; no explicit field)
//   occ_obj.face_start                      (cursor-computed; no explicit field)
//   occ_obj.parent_subobject_index          obj->parent_subobject_index  (uint8_t)
//   occ_obj.connecting_subobject            obj->connecting_subobject    (uint8_t)
//   occ_obj.type                            obj->type                    (uint8_t)
//   occ["vertices"][j].position             ir->occlusion_vertices[j].position
//   occ["faces"][j].raw_indices             ir->occlusion_faces[j].raw_indices
//   coords.render_space(v.position)         object_render_space(x,y,z,out) -> (-x,-z,y)
//   lod0.parts[i].abs_position             lod->render_objects[i].abs[]
//
// The occlusion geometry is already stored in render-space; apply render_space() to vertices.
// part_abs uses render_space on lod0.parts[i].abs_position.

#include "object/occlusion_to_ase.h"

#include "object/ase_vec.h"    // object_tm_identity
#include "object/coords.h"     // object_render_space

#include "ase/ase.h"           // ase_alloc_object
#include "ase/types.h"         // ase_Object, ase_Face

#include "threedi/threedi_3di3.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <map>
#include <string>
#include <tuple>
#include <array>

// ---------------------------------------------------------------------------
// Name-building helpers — port of pyopennova/scene_naming.py
// ---------------------------------------------------------------------------

// OcclusionType prefixes (type 0=OB, 1=OS, 2=OP, 3=OP).
static const char* occlusion_prefix(int type_code) {
    switch (type_code) {
        case 0: return "OB";
        case 1: return "OS";
        case 2: return "OP";
        case 3: return "OP";
        default: return "OX";
    }
}

// format_duplicate_suffix(occurrence) — "" for <=1, "a","b",... for 2+.
static std::string format_dup_suffix(int occurrence) {
    if (occurrence <= 1) return "";
    int index = occurrence - 1;
    std::string out;
    while (index > 0) {
        index -= 1;
        out = char('a' + (index % 26)) + out;
        index /= 26;
    }
    return out;
}

// build_occlusion_name(type_code, parent_subobject, connecting_subobject)
// Port of scene_naming.py lines 71-82.
static std::string build_occlusion_name(int type_code, int parent_sub, int connecting_sub) {
    int display = (parent_sub >= 0) ? parent_sub : 0;
    const char* prefix = occlusion_prefix(type_code);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s%02d", prefix, display + 1);
    std::string name = buf;
    // types 2 and 3 (OP, OP2) append connecting subobject.
    if ((type_code == 2 || type_code == 3) && connecting_sub >= 0) {
        char sfx[16];
        std::snprintf(sfx, sizeof(sfx), "-%02d", connecting_sub + 1);
        name += sfx;
    }
    return name;
}

// ---------------------------------------------------------------------------
// object_emit_occlusion_helpers
// Port of _occlusion_helper_objects (ase_from_3di3.py lines 532-601).
// ---------------------------------------------------------------------------

extern "C" int object_emit_occlusion_helpers(
    const Threedi3di3* ir,
    ase_Object* out_objects,
    int out_capacity)
{
    if (!ir || !out_objects || out_capacity <= 0)
        return 0;
    if (ir->occlusion_object_count == 0 || ir->lod_count == 0)
        return 0;

    const ThreediLod* lod0 = &ir->lods[0];
    int part_count = (int)lod0->render_object_count;

    // counters keyed by (type, parent_subobject_index, connecting_subobject)
    std::map<std::tuple<int,int,int>, int> counters;

    int emitted = 0;
    int vertex_cursor = 0;
    int face_cursor   = 0;

    for (int i = 0; i < (int)ir->occlusion_object_count; ++i) {
        const ThreediOcclusionObject* occ_obj = &ir->occlusion_objects[i];
        int vert_count = (int)occ_obj->num_vertices;
        int face_count = (int)occ_obj->face_count;

        if (vert_count <= 0 || face_count <= 0) {
            if (vert_count > 0) vertex_cursor += vert_count;
            if (face_count > 0) face_cursor   += face_count;
            continue;
        }

        int vert_start = vertex_cursor;
        int face_start = face_cursor;
        vertex_cursor += vert_count;
        face_cursor   += face_count;

        if (vert_start + vert_count > (int)ir->occlusion_vertex_count)
            continue;
        if (face_start + face_count > (int)ir->occlusion_face_count)
            continue;

        int occ_parent     = (int)occ_obj->parent_subobject_index;
        int occ_connecting = (int)occ_obj->connecting_subobject;
        int occ_type       = (int)occ_obj->type;

        // part_abs and parent name.
        float part_abs[3] = { 0.0f, 0.0f, 0.0f };
        char parent[64] = "";
        if (occ_parent >= 0 && occ_parent < part_count) {
            float raw[3] = {
                lod0->render_objects[occ_parent].abs[0],
                lod0->render_objects[occ_parent].abs[1],
                lod0->render_objects[occ_parent].abs[2],
            };
            object_render_space(raw[0], raw[1], raw[2], part_abs);
            // _export_name("PN{occ_parent+1:02d}") -> "{occ_parent+1:02d}"
            std::snprintf(parent, sizeof(parent), "%02d", occ_parent + 1);
        }

        // Convert vertices: coords.render_space(v.position)
        std::vector<std::array<float, 3>> verts(vert_count);
        for (int j = 0; j < vert_count; ++j) {
            const ThreediOcclusionVertex* v = &ir->occlusion_vertices[vert_start + j];
            float out[3];
            object_render_space(v->position[0], v->position[1], v->position[2], out);
            verts[j][0] = out[0];
            verts[j][1] = out[1];
            verts[j][2] = out[2];
        }

        // Decode faces from raw_indices: v1=raw&0xFF, v2=(raw>>8)&0xFF, v3=(raw>>16)&0xFF
        // smoothing group = ((raw>>24)&0xFF) + 1
        std::vector<std::array<int, 3>> faces;
        std::vector<int> sg_list;
        for (int j = 0; j < face_count; ++j) {
            const ThreediOcclusionFace* face = &ir->occlusion_faces[face_start + j];
            uint32_t raw = face->raw_indices;
            int v1 = (int)(raw & 0xFF);
            int v2 = (int)((raw >> 8) & 0xFF);
            int v3 = (int)((raw >> 16) & 0xFF);
            if (v1 < vert_count && v2 < vert_count && v3 < vert_count) {
                faces.push_back({ v1, v2, v3 });
                sg_list.push_back((int)((raw >> 24) & 0xFF) + 1);
            }
        }

        if (verts.empty() || faces.empty())
            continue;

        // Name and duplicate suffix.
        std::string base_name = build_occlusion_name(occ_type, occ_parent, occ_connecting);
        auto ckey = std::make_tuple(occ_type, occ_parent, occ_connecting);
        counters[ckey]++;
        std::string obj_name = base_name + format_dup_suffix(counters[ckey]) + "-occonly";

        // Emit ase_Object.
        if (emitted >= out_capacity) break;
        ase_Object* obj = &out_objects[emitted];

        int vcount = (int)verts.size();
        int fcount = (int)faces.size();
        ase_alloc_object(obj, vcount, 0, fcount, 0, 0);

        std::strncpy(obj->name, obj_name.c_str(), 63);
        obj->name[63] = '\0';
        std::strncpy(obj->parent_name, parent, 63);
        obj->parent_name[63] = '\0';

        float tm[4][3];
        object_tm_identity(part_abs, tm);
        std::memcpy(obj->tm_row, tm, sizeof(tm));

        obj->node_id = -1;       // Python default spec.get("node_id", -1) = -1
        obj->material_ref = -1;
        obj->skinned = 0;
        obj->weight_count = 0;
        obj->uv_count = 0;

        for (int v = 0; v < vcount; ++v) {
            obj->verts[v * 3 + 0] = verts[v][0];
            obj->verts[v * 3 + 1] = verts[v][1];
            obj->verts[v * 3 + 2] = verts[v][2];
        }

        for (int f = 0; f < fcount; ++f) {
            ase_Face* af = &obj->faces[f];
            std::memset(af, 0, sizeof(ase_Face));
            af->vert[0] = faces[f][0];
            af->vert[1] = faces[f][1];
            af->vert[2] = faces[f][2];
            af->vert[3] = 0;
            af->edge_visibility[0] = 1;
            af->edge_visibility[1] = 1;
            af->edge_visibility[2] = 1;
            af->smoothing_mask = (uint32_t)sg_list[f];
            af->material_id = 0;
            af->material_index = 0;
        }

        ++emitted;
    }

    return emitted;
}
