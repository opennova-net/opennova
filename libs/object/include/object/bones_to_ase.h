// libs/object/include/object/bones_to_ase.h
//
// Port of pyopennova/ase_from_3di3.py:
//   _bone_objects   (lines 603-648)  — both no-BAD path (lod0.parts) and
//                                       BAD path (bad_file.bones + root_motion)
//   _bone_mesh()    (lines 1040-1062) — 9 verts, 14 faces bone marker shape
//   _bone_export_name (lines 1065-1068)
//   _bone_node_id     (lines 1071-1075)
#pragma once

struct Threedi3di3;
struct ase_Object;
struct BadFile;

#ifdef __cplusplus
extern "C" {
#endif

// Emit BN01..BNNN bone-marker AseObjects for lod0's render objects.
// Equivalent to object_emit_bone_objects_with_bad(ir, NULL, ...).
int object_emit_bone_objects(const struct Threedi3di3* ir,
                             struct ase_Object* out_objects, int out_capacity);

// Emit bone-marker AseObjects from either:
//   - bad_file->bones (when bad_file != NULL && bad_file->num_bones > 0):
//     N BAD-bone objects plus one "root_motion" bone at the end (parent="").
//     BAD bones with parent_index < 0 get parent_name="root_motion" (Python
//     ase_from_3di3.py:638-639 fallback).
//   - ir->lods[0].render_objects (otherwise): synthesised BN01..BNNN.
// Returns total bone count emitted (= bad_file->num_bones + 1 in BAD case;
// = lod0.render_object_count in no-BAD case). out_capacity must be at
// least that value.
int object_emit_bone_objects_with_bad(const struct Threedi3di3* ir,
                                       const struct BadFile* bad_file,
                                       struct ase_Object* out_objects,
                                       int out_capacity);

#ifdef __cplusplus
}
#endif
