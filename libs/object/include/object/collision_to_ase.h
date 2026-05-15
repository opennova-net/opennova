#pragma once

struct Threedi3di3;
struct ase_Object;

#ifdef __cplusplus
extern "C" {
#endif

// Emit collision helper AseObjects (BB/OB/LP/CC/etc.) from ir.collision[0].objects[].
// Returns count emitted; caller has allocated out_objects.
int object_emit_collision_helpers(const struct Threedi3di3* ir,
                                   struct ase_Object* out_objects, int out_capacity);

#ifdef __cplusplus
}
#endif
