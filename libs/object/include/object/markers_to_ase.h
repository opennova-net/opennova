// libs/object/include/object/markers_to_ase.h
//
// Port of pyopennova/ase_from_3di3.py marker emission:
//   _center_marker_objects  (lines 348-380)
//   _attach_marker_objects  (lines 382-406)
//   _userpoint_objects      (lines 408-455)
#pragma once

struct Threedi3di3;
struct ase_Object;

#ifdef __cplusplus
extern "C" {
#endif

// Emit center marker ase_Objects for each part of the given LOD.
// Returns count of objects emitted; caller has pre-allocated out_objects[out_capacity].
int object_emit_center_markers(const struct Threedi3di3* ir, int lod_index,
                                struct ase_Object* out_objects, int out_capacity);

// Emit attach-point marker ase_Objects for the given LOD.
// Returns count; caller has pre-allocated out_objects[out_capacity].
int object_emit_attach_markers(const struct Threedi3di3* ir, int lod_index,
                                struct ase_Object* out_objects, int out_capacity);

// Emit userpoint marker ase_Objects (UP<type-char><index>).
// Returns count; caller has pre-allocated out_objects[out_capacity].
int object_emit_userpoint_markers(const struct Threedi3di3* ir,
                                   struct ase_Object* out_objects, int out_capacity);

// Emit empty "PN01..PN{N:02d}" hierarchy dummy nodes for each render_object
// in lod_index. These exist so OED can resolve center/attach parent_name
// references back to subobjects. Stock artist-authored .ase files included
// PN## dummies; pre-Phase-D Python writer omitted them. OED's parser strips
// "PN" prefix -> "{NN}" -> subobject (NN-1). Each dummy is a 1mm cube at the
// part's render_space origin.
//
// Returns count emitted (= lod->render_object_count); 0 on error/empty.
int object_emit_part_dummies(const struct Threedi3di3* ir,
                              int lod_index,
                              struct ase_Object* out_objects,
                              int out_capacity);

#ifdef __cplusplus
}
#endif
