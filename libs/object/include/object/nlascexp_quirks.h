// libs/object/include/object/nlascexp_quirks.h
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 if det(tm 3x3 basis) < 0 (mirrored TM); else 0.
// Mirror of ASE_NodeTMHasNegativeHandedness @ 0x1000a650 (Phase A audit
// cross-cutting finding 3). Predicate is dot(row2, cross(row0, row1)) < 0.0f.
int object_node_tm_has_negative_handedness(const float tm[4][3]);

// Sanitize an .ase object name: trim trailing whitespace, replace path-unsafe
// chars. Mirror of pyopennova/ase_from_3di3.py::fixup_name (line 1084).
// Writes result into `out` (max `out_size` bytes including null terminator).
// Returns number of bytes written (excluding null).
size_t object_fixup_name(const char* name, char* out, size_t out_size);

// Decode an IR string field (bytes with trailing NULs) into a clean UTF-8
// C-string. Mirror of pyopennova/ase_from_3di3.py::_decode (line 876).
// Writes result into `out` (max `out_size` bytes including null terminator).
size_t object_decode_ir_string(const char* raw, size_t raw_len, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif
