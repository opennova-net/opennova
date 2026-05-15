// Legacy face-basis arithmetic helpers.
//
// These functions keep the x87-shaped call sites used by the object bake
// pipeline without carrying an external floating-point emulator. Each
// operation uses native double intermediates and rounds back to float at
// the same storage boundaries the previous wrapper exposed.

#pragma once

namespace opennova::object::x87 {

// Retained as a no-op compatibility hook after removing emulator state.
void ensure_x87_default_state();

void cross3_x87(const float a[3], const float b[3], float out[3]);
float length3_x87(const float v[3]);
void normalize_vec3_x87(float v[3]);

float fmul_x87(float a, float b);
float fsub_x87(float a, float b);
float fadd_x87(float a, float b);
float fdiv_x87(float a, float b);
float msub_x87(float a, float b, float c, float d);
float neg_div_msub_x87(float a, float b, float c, float d, float divisor);

}  // namespace opennova::object::x87
