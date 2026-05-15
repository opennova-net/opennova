#pragma once

struct Threedi3di3;
struct ase_Object;

#ifdef __cplusplus
extern "C" {
#endif

// Emit occlusion helper AseObjects from ir.occlusion_objects[].
// Returns count emitted; caller has allocated out_objects.
int object_emit_occlusion_helpers(const struct Threedi3di3* ir,
                                   struct ase_Object* out_objects, int out_capacity);

#ifdef __cplusplus
}
#endif
