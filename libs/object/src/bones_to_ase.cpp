// libs/object/src/bones_to_ase.cpp
//
// Port of pyopennova/ase_from_3di3.py:
//   _bone_objects     (lines 603-648)  — both no-BAD and BAD branches
//   _bone_mesh()      (lines 1040-1062) — 9 verts, 14 faces bone marker shape
//   _bone_export_name (lines 1065-1068)
//   _bone_node_id     (lines 1071-1075)
//
// IR field mapping vs Python attribute names:
//   ir->lods[0]                           = self.ir.lods[0]
//   lods[0].render_objects[i].abs[]       = lod0.parts[i].abs_position
//   lods[0].render_objects[i].parent_index = lod0.parts[i].parent_index
//   lods[0].render_object_count           = lod0.part_count
//
// _bone_mesh() geometry (verbatim from Python lines 1040-1062):
//   base = 0.01, tip = 0.001, base_z = 0.01, tip_z = 0.10
//   9 verts, 14 faces
//
// BAD path (Python 604-619):
//   - For each BAD bone: name = _decode(bone.name).replace(":", "") or "BN##"
//   - parent = pi if 0 <= pi < bone_count and pi != i else -1
//   - position = coords.bone_space(bone.position)
//   - Append synthetic "root_motion" bone with parent=-1, pos=(0,0,0)
//   - Per-bone parent_name resolution (Python 633-639):
//       if parents[i] >= 0: parent = _bone_export_name(names[parents[i]])
//       elif name != "root_motion" and "root_motion" in names: parent = "root_motion"
//       else: parent = ""

#include "object/bones_to_ase.h"

#include "object/coords.h"    // object_render_space, object_bone_space
#include "object/ase_vec.h"   // object_tm_identity

#include "ase/ase.h"          // ase_alloc_object
#include "ase/types.h"        // ase_Object, ase_Face

#include "threedi/threedi_3di3.h"  // Threedi3di3, ThreediLod, ThreediRenderObject
#include "bad/bad.h"          // BadFile, BadBone

#include <cctype>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// _bone_mesh() — verbatim port from Python lines 1040-1062.
// 9 verts (local, relative to bone origin), 14 triangular faces.
// ---------------------------------------------------------------------------

// Bone vertex offsets: stored as double to preserve the exact Python float literal values.
// Python _bone_mesh() returns (0.01, 0.01, 0.01) etc. as Python float (float64 = 0.01 exactly).
// If stored as float (float32), 0.01f becomes 0.00999999978... which gives different
// world-space positions when added to the part origin (also double from ctypes).
static const double k_bone_verts[9][3] = {
    // vert 0..3: base square at base_z = 0.01
    { 0.01,  0.01,  0.01},  // 0
    { 0.01, -0.01,  0.01},  // 1
    {-0.01, -0.01,  0.01},  // 2
    {-0.01,  0.01,  0.01},  // 3
    // vert 4..7: tip square at tip_z = 0.10
    { 0.001,  0.001,  0.10}, // 4
    { 0.001, -0.001,  0.10}, // 5
    {-0.001, -0.001,  0.10}, // 6
    {-0.001,  0.001,  0.10}, // 7
    // vert 8: base apex at origin
    { 0.0,   0.0,   0.0},  // 8
};

static const int k_bone_faces[14][3] = {
    // bottom cap (apex to base ring)
    {8, 0, 1}, {8, 1, 2}, {8, 2, 3}, {8, 3, 0},
    // side walls (base ring to tip ring)
    {0, 1, 5}, {0, 5, 4},
    {1, 2, 6}, {1, 6, 5},
    {2, 3, 7}, {2, 7, 6},
    {3, 0, 4}, {3, 4, 7},
    // tip cap
    {4, 5, 6}, {4, 6, 7},
};

// ---------------------------------------------------------------------------
// Fill 14 ase_Face entries from the bone face table.
// ---------------------------------------------------------------------------
static void fill_bone_faces(ase_Face* faces)
{
    for (int f = 0; f < 14; ++f) {
        ase_Face* af = &faces[f];
        af->vert[0] = k_bone_faces[f][0];
        af->vert[1] = k_bone_faces[f][1];
        af->vert[2] = k_bone_faces[f][2];
        af->vert[3] = 0;
        af->edge_visibility[0] = 1;
        af->edge_visibility[1] = 1;
        af->edge_visibility[2] = 1;
        af->edge_visibility[3] = 0;
        af->smoothing_mask  = 0;
        af->material_id     = 0;
        af->material_index  = 0;
        af->uv[0] = af->uv[1] = af->uv[2] = af->uv[3] = 0;
        af->color[0] = af->color[1] = af->color[2] = 0;
        af->reserved1 = 0;
        af->reserved2 = 0;
    }
}

// Fill 9 world-space bone verts: local + origin offset.
// out_verts[] must be size 9*3.
// Python's _bone_objects uses _vec_add(positions[i], v) which adds float64+float64.
// We use double arithmetic to match Python's precision.
static void fill_bone_verts(float* out_verts, const float origin[3])
{
    for (int v = 0; v < 9; ++v) {
        out_verts[v * 3 + 0] = (float)((double)origin[0] + (double)k_bone_verts[v][0]);
        out_verts[v * 3 + 1] = (float)((double)origin[1] + (double)k_bone_verts[v][1]);
        out_verts[v * 3 + 2] = (float)((double)origin[2] + (double)k_bone_verts[v][2]);
    }
}

// ---------------------------------------------------------------------------
// _bone_export_name — Python lines 1065-1068:
//   if name starts with "BN" and len >= 4 and name[2:4] are digits => name[:4]
//   else return name.
// For synthesised "BN{i+1:02d}" names this is always identity.
// For BAD bone names (e.g. "Spine", "SubBone") this is identity as well.
// ---------------------------------------------------------------------------
static void bone_export_name(const char* name, char out[33])
{
    // Default: identity copy (truncated to 32 chars + null).
    std::strncpy(out, name, 32);
    out[32] = '\0';
    if (name[0] == 'B' && name[1] == 'N' &&
        std::isdigit((unsigned char)name[2]) &&
        std::isdigit((unsigned char)name[3])) {
        out[2] = name[2];
        out[3] = name[3];
        out[4] = '\0';
    }
}

// _bone_node_id — Python lines 1071-1075 (via _bone_export_name):
//   trimmed = _bone_export_name(name)
//   if trimmed starts with "BN" and len >= 4 and [2:4] are digits =>
//       int(trimmed[2:4]) - 1
//   else: -1
static int bone_node_id(const char* name)
{
    if (name[0] == 'B' && name[1] == 'N' &&
        std::isdigit((unsigned char)name[2]) &&
        std::isdigit((unsigned char)name[3])) {
        int tens = name[2] - '0';
        int ones = name[3] - '0';
        return tens * 10 + ones - 1;
    }
    return -1;
}

// Decode a BAD bone name (char[33], null-terminated, may contain ':' chars).
// Python: _decode(bone.name).replace(":", "") or f"BN{i + 1:02d}"
// We treat the raw bytes as already-decoded ASCII (BAD names are stored as
// ASCII bytes); strip any ':' characters; if the result is empty, fall back
// to the synthesized "BN##" name.
static void bone_name_decode(const char raw[33], int fallback_idx, char out[33])
{
    int oi = 0;
    for (int i = 0; i < 32 && raw[i] != '\0'; ++i) {
        if (raw[i] != ':') {
            out[oi++] = raw[i];
        }
    }
    out[oi] = '\0';
    if (oi == 0) {
        std::snprintf(out, 33, "BN%02d", fallback_idx + 1);
    }
}

// ---------------------------------------------------------------------------
// emit_one_bone — fills one ase_Object for a bone marker. The body is the
// per-bone slice lifted from the previous monolithic loop (verts, faces,
// tm, name, parent, node_id, material_ref, skinned).
// `origin` must already be in Z-up RH space.
// `node_id_override` is used so callers can pass -1 (e.g. for BAD-derived
// names where _bone_node_id returns -1).
// ---------------------------------------------------------------------------
static void emit_one_bone(const char* name,
                          const char* parent_name,
                          const float origin[3],
                          int node_id_override,
                          ase_Object* obj)
{
    // Alloc: 9 verts, 0 weights, 14 faces, 0 UVs, 0 colors.
    ase_alloc_object(obj, 9, 0, 14, 0, 0);

    // Name & parent: _bone_export_name(name) and _bone_export_name(names[parents[i]]).
    // _bone_export_name truncates "BN##*" to "BN##" but is identity for
    // non-BN names (including "root_motion").
    char exported_name[33];
    bone_export_name(name, exported_name);
    std::snprintf(obj->name, sizeof(obj->name), "%s", exported_name);

    if (parent_name && parent_name[0] != '\0') {
        char exported_parent[33];
        bone_export_name(parent_name, exported_parent);
        std::snprintf(obj->parent_name, sizeof(obj->parent_name), "%s", exported_parent);
    } else {
        obj->parent_name[0] = '\0';
    }

    obj->node_id      = node_id_override;
    obj->material_ref = -1;
    obj->skinned      = 0;

    // TM: identity at origin (Python uses _tm(positions[i]) with no rot).
    float tm[4][3];
    object_tm_identity(origin, tm);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            obj->tm_row[r][c] = tm[r][c];

    // Verts: local bone shape offset by origin (Python: _vec_add(pos, v)).
    fill_bone_verts(obj->verts, origin);
    fill_bone_faces(obj->faces);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" int object_emit_bone_objects_with_bad(
    const Threedi3di3* ir,
    const BadFile* bad_file,
    ase_Object* out_objects,
    int out_capacity)
{
    if (!out_objects || out_capacity <= 0)
        return 0;

    // ---- BAD path ---------------------------------------------------------
    // Python lines 604-619: when bad_file is present and has bones, emit
    // num_bones BAD bones + one synthetic "root_motion" at the end.
    if (bad_file && bad_file->num_bones > 0 && bad_file->bones) {
        const int n_bad = (int)bad_file->num_bones;
        const int total = n_bad + 1;  // +1 for root_motion
        if (total > out_capacity)
            return 0;

        // Buffer names so we can look up parents by index. char[33] each.
        // Stack allocation bound: BAD typically has <= 64 bones; clamp to 256.
        // For safety fall back to dynamic alloc if larger.
        if (total > 256) {
            // Defensive: refuse extreme bone counts rather than overflow stack.
            return 0;
        }
        char names[256][33];

        for (int i = 0; i < n_bad; ++i) {
            bone_name_decode(bad_file->bones[i].name, i, names[i]);
        }
        std::snprintf(names[n_bad], 33, "root_motion");

        // Per-bone parent indices, mirroring Python:
        //   pi if 0 <= pi < bone_count and pi != i else -1
        // bone_count here is n_bad (the BAD count, not total) — Python's check
        // `0 <= pi < bone_count` uses bone_count = num_bones at append time.
        for (int i = 0; i < n_bad; ++i) {
            const BadBone* bone = &bad_file->bones[i];
            int pi = (int)bone->parent_index;
            const char* parent_name = "";

            if (pi >= 0 && pi < n_bad && pi != i) {
                parent_name = names[pi];
            } else {
                // Python 638-639: elif name != "root_motion" and
                // "root_motion" in names: parent = "root_motion".
                // Names != "root_motion" here (only the trailing synthetic
                // entry uses that name), and "root_motion" is always present
                // in the BAD path, so fall back to "root_motion".
                parent_name = "root_motion";
            }

            float origin[3];
            object_bone_space(bone->position[0],
                              bone->position[1],
                              bone->position[2],
                              origin);

            emit_one_bone(names[i],
                          parent_name,
                          origin,
                          bone_node_id(names[i]),
                          &out_objects[i]);
        }

        // root_motion: parent="" (Python 636-639: name == "root_motion" skips
        // the elif branch), position (0, 0, 0) in bone-space (still (0,0,0)).
        float root_origin[3];
        object_bone_space(0.0f, 0.0f, 0.0f, root_origin);
        emit_one_bone("root_motion",
                      "",
                      root_origin,
                      bone_node_id("root_motion"),  // returns -1
                      &out_objects[n_bad]);

        return total;
    }

    // ---- No-BAD path ------------------------------------------------------
    // Python lines 620-628: synthesised BN01..BNNN from lod0.parts.
    if (!ir || ir->lod_count == 0)
        return 0;

    const ThreediLod* lod0 = &ir->lods[0];
    const int n = (int)lod0->render_object_count;
    if (n <= 0 || n > out_capacity)
        return 0;

    if (n > 256)
        return 0;
    char names[256][33];
    for (int i = 0; i < n; ++i) {
        std::snprintf(names[i], 33, "BN%02d", i + 1);
    }

    for (int i = 0; i < n; ++i) {
        const ThreediRenderObject* ro = &lod0->render_objects[i];

        int pi = ro->parent_index;
        const char* parent_name = "";
        if (pi >= 0 && pi < n && pi != i) {
            parent_name = names[pi];
        }
        // No-BAD path: no "root_motion" fallback (Python 638 requires
        // "root_motion" in names, which is only true in the BAD branch).

        float origin[3];
        object_render_space(ro->abs[0], ro->abs[1], ro->abs[2], origin);

        emit_one_bone(names[i],
                      parent_name,
                      origin,
                      bone_node_id(names[i]),  // returns i for "BN{i+1:02d}"
                      &out_objects[i]);
    }

    return n;
}

extern "C" int object_emit_bone_objects(
    const Threedi3di3* ir, ase_Object* out_objects, int out_capacity)
{
    return object_emit_bone_objects_with_bad(ir, /*bad_file=*/nullptr,
                                             out_objects, out_capacity);
}
