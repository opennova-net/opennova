// ASE format — C API for FFI (Python ctypes, etc.)
#ifndef ASE_H
#define ASE_H

#include "ase/types.h"

#include <io/export.h>
#define ASE_EXPORT OPENNOVA_API

#ifdef __cplusplus
extern "C" {
#endif

// Parse an ASE file into a caller-provided Document. Returns 0 on success.
ASE_EXPORT int ase_parse(const char* path, ase_Document* out);

// Write a Document to an ASE file. Returns 0 on success.
ASE_EXPORT int ase_write(const char* path, const ase_Document* doc);

// Free all allocations inside a Document.
ASE_EXPORT void ase_free(ase_Document* doc);

// Create an empty Document with allocated arrays.
ASE_EXPORT void ase_alloc(ase_Document* doc, int objects, int materials, int lights);

// Allocate arrays within an Object.
ASE_EXPORT void ase_alloc_object(ase_Object* obj, int verts, int uvs, int faces,
                                 int colors, int weights);

// Allocate sub-materials within a Material (for Multi/Sub-Object).
ASE_EXPORT void ase_alloc_submaterials(ase_Material* mat, int count);

#ifdef __cplusplus
}
#endif

#endif // ASE_H
