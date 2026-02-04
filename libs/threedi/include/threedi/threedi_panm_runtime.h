// Runtime PANM helpers: sampling packed PANM tracks using the original wave table.
// These functions are pure (no allocations); caller provides time and optional control table.

#ifndef THREEDI_PANM_RUNTIME_H
#define THREEDI_PANM_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

#include "threedi/threedi_panm.h"

#ifdef __cplusplus
extern "C" {
#endif

// Sample a PANM transform and return the raw 24.8 fixed-point value used by the
// original animation code. The caller supplies:
// - time_ms: tick time in milliseconds (matches dword_18B42A4 usage).
// - ctrl_table: optional control register lookup table (512 entries). If NULL,
//   control-based tracks will use 0.
// Return value is signed 32-bit fixed (<<8), matching PANM_SampleTrack.
int32_t threedi_panm_sample_track_raw(const ThreediTransform *track,
                                      uint32_t time_ms,
                                      const uint16_t *ctrl_table);

// Build PANM node matrices (approximate port of PANM_BuildNodeMatrices).
// - pivots: Per-subobject pivot points (size = max subobject_index + 1)
// - view_inverse is optional; if NULL, identity is used (IDA uses flt_1604CC8).
// - mul_override is optional 4x4 to post-multiply outputs (NULL to skip).
// Returns 0 on success, -1 on invalid args.
int threedi_panm_build_node_matrices(const ThreediPartAnimation *nodes,
                                     size_t node_count,
                                     const ThreediVec3 *pivots,
                                     const ThreediMatrix4x4 *view_inverse,
                                     const ThreediMatrix4x4 *in_matrices,
                                     const ThreediMatrix4x4 *mul_override,
                                     uint32_t time_ms,
                                     const uint16_t *ctrl_table,
                                     ThreediMatrix4x4 *out_matrices);

#ifdef __cplusplus
}
#endif

#endif // THREEDI_PANM_RUNTIME_H
