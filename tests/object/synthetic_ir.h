#pragma once

#include "threedi/threedi_3di3.h"

// Build a minimal single-LOD Threedi3di3 in memory for tests.
//
// Layout: 1 render LOD with 1 part containing a 4-vertex quad
// (2 triangles, indices [0,1,2, 0,2,3]). Static-simple vertex layout
// (stride=40, flags=0x01). No skinning, no materials, no occlusion.
//
// The returned struct's inner pointers are owned by the caller; pass to
// `synthetic_ir_free()` (NOT `threedi_3di3_free`) when done.
int synthetic_ir_build_quad(Threedi3di3* out);

// Skinned variant: same quad geometry, but vertex.is_skinned=1 with
// 3 bone influences per vertex summing to 1.0.
int synthetic_ir_build_quad_skinned(Threedi3di3* out);

// Counterpart free.
void synthetic_ir_free(Threedi3di3* ir);

// Build a synthetic IR for source-index dedup testing (Task 6).
// Layout: 1 LOD, 1 part, 1 strip (is_strip=0), 8 verts, 2 triangles.
// strip.indices = [5, 3, 7, 3, 5, 0] (face-corner source indices before winding swap).
// After (0,2,1) winding swap:
//   triangle 0: (5, 7, 3)
//   triangle 1: (3, 0, 5)
// With preserve_source_indexing=1, first-seen order -> 4 unique verts:
//   source 5 -> local 0, source 7 -> local 1, source 3 -> local 2, source 0 -> local 3
// Resulting faces: (0,1,2) and (2,3,0).
Threedi3di3 make_dedup_test_ir(void);
