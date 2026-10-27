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
// - time_ms: the frame-shared GetTickCount DWORD (the embedder's one clock,
//   world::frame_tick_count_ms) [orig: g_RenderShaderTickMs @ 0x2721A40; the
//   sampler's own copy g_RenderPanmTickMs @0x28E07B4, read @0x5B22C5; all stored
//   from one GetTickCount call, Render_SubmitEntity @0x5DAD85..0x5DADA9].
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
//   selects the row threedi_panm_frame_row names (an Euler node's byte read
//   signed, above zero); zero and 0x80..0xFF bypass the table.
// - view_inverse is the view's inverse the two view modes (rotation types 3 and 4)
//   build from, the camera's axes as rows (right, up, forward) in the inputs' frame:
//   the model's own frame for rest frames (renderer::panm_view_in_model_frame); NULL
//   is identity. Retail reads the global view inverse the current view's setup wrote
//   [orig: matA @ 0x27219C0, read @ 0x58EBC4..0x58EC09 / @ 0x58ECF2..0x58ED1C; the view
//   matrix's inverse (Math_InvertMatrix4x4_Float_ToStatic @ 0x611960) stored by
//   Render_SetViewAndProjectionFromMatrices @ 0x58D74D..0x58D752 and
//   Render_SetViewAndProjectionMatrices @ 0x58D942..0x58D947].
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
