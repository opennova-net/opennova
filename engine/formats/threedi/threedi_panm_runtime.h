// Runtime PANM helpers: sampling packed PANM tracks using the original wave table.
// These functions are pure (no allocations); caller provides time and optional control table.

#pragma once

#include <stdint.h>
#include <stddef.h>

#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm.h>

namespace opennova::threedi {

// Sample a PANM transform and return the raw 24.8 fixed-point value used by the
// original animation code. The caller supplies:
// - time_ms: the frame-shared GetTickCount DWORD
//   [orig: g_RenderShaderTickMs @ 0x2721A40].
// - ctrl_values: optional view of the 96 signed int32 value dwords in retail's
//   global register bus, indexed by the already-resolved global ordinal in
//   control_param. The adjacent state dwords are omitted because PANM never
//   reads them. If NULL, code 113 uses 0. Codes 114..117 use waveform lookup.
// Return value is signed 32-bit fixed (<<8).
// [orig: PANM_SampleTrack @ 0x5B2270]
int32_t threedi_panm_sample_track_raw(const ThreediTransform *track,
                                      uint32_t time_ms,
                                      const int32_t *ctrl_values);

// What the builder seeds output slot i with before node i is built. Retail
// copies the whole input row there, so a node naming ITSELF as its parent
// carries its pivot through its own posed input
// [orig: Model_TransformBoneMatrices @ 0x58E451..0x58E46B].
enum class ThreediPanmInputs : uint8_t {
    // Model-space rest frames whose translation is each part's abs pivot (the
    // model-level callers' base pose). Retail's input there is the entity
    // matrix, identity in model space, so the seed keeps the orientation only.
    kRestFrames,
    // Posed per-part frames: a clip's skinning matrices, the input of every
    // clip-driven submit (the first-person gun and arms). The seed is the
    // whole input row, and every slot starts from its own part's input, so a
    // parent numbered after its child reads that parent's posed frame where
    // retail reads what the previous model left in the shared array
    // (docs/anim/adm-bad-format-re.md, "A parent numbered after its child").
    kPosedFrames,
};

// Build PANM node matrices (approximate port of PANM_BuildNodeMatrices).
// - inputs: how in_matrices are framed (ThreediPanmInputs above).
// - nodes: every style >0x70 must already have its control_param rewritten from
//   the file-local CTRL index to the retail global catalog ordinal. Runtime
//   sampling reads that slot only for style 113; styles 114..117 use the
//   resolved ordinal as their waveform phase byte.
// - pivots: Per-subobject pivot points (size = max subobject_index + 1)
// - animation_frames is the model MTRX table. PANM matrix_index (disk +6)
//   selects a frame when nonzero; zero bypasses the table.
// - view_inverse is optional; if NULL, identity is used (IDA uses flt_1604CC8).
// - mul_override is optional 4x4 to post-multiply outputs (NULL to skip).
// Returns 0 on success, -1 on invalid args.
int threedi_panm_build_node_matrices(const ThreediPartAnimation *nodes,
                                     size_t node_count,
                                     const ThreediVec3 *pivots,
                                     const ThreediMatrixTable *animation_frames,
                                     const ThreediMatrix4x4 *view_inverse,
                                     const ThreediMatrix4x4 *in_matrices,
                                     ThreediPanmInputs inputs,
                                     const ThreediMatrix4x4 *mul_override,
                                     uint32_t time_ms,
                                     const int32_t *ctrl_values,
                                     ThreediMatrix4x4 *out_matrices);

} // namespace opennova::threedi
