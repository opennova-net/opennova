#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Engine render-mesh coords (Y-up) -> Z-up RH.
// Mapping: (x, y, z) -> (-x, -z, y).
// Mirror of pyopennova/coords.py::render_space.
void object_render_space(float x, float y, float z, float out[3]);

// BAD bone position (Y-up, identity adm transform) to Z-up RH.
// Mapping: (x, y, z) -> (x, -z, y).
// Mirror of pyopennova/coords.py::bone_space.
void object_bone_space(float x, float y, float z, float out[3]);

#ifdef __cplusplus
}
#endif
