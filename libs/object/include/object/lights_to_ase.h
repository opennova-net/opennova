// libs/object/include/object/lights_to_ase.h
//
// Port of pyopennova/ase_from_3di3.py:
//   _lights          (lines 650-677) — walks ir.lights[], builds ase_Light
//   _populate_light  (folded inline) — name/type/pos/color/atten/falloff/row2
//   _light_rgb                       — B,G,R bytes -> float triple (R,G,B)
#pragma once

struct Threedi3di3;
struct ase_Light;

#ifdef __cplusplus
extern "C" {
#endif

// Populate ase_Light entries from ir->lights[].
// Returns count populated; caller must have allocated out_lights.
int object_emit_lights(const struct Threedi3di3* ir,
                       struct ase_Light* out_lights, int out_capacity);

#ifdef __cplusplus
}
#endif
