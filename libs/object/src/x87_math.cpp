#include "x87_math.h"

#include <cmath>

namespace opennova::object::x87 {

namespace {

inline float round_f32(double value) {
  return static_cast<float>(value);
}

}  // namespace

void ensure_x87_default_state() {
  // The external emulator state is gone. The public hook remains as a no-op so
  // callers do not need conditional build paths.
}

float fmul_x87(float a, float b) {
  return round_f32(static_cast<double>(a) * static_cast<double>(b));
}

float fsub_x87(float a, float b) {
  return round_f32(static_cast<double>(a) - static_cast<double>(b));
}

float fadd_x87(float a, float b) {
  return round_f32(static_cast<double>(a) + static_cast<double>(b));
}

float fdiv_x87(float a, float b) {
  return round_f32(static_cast<double>(a) / static_cast<double>(b));
}

float msub_x87(float a, float b, float c, float d) {
  const float ab = fmul_x87(a, b);
  const float cd = fmul_x87(c, d);
  return fsub_x87(ab, cd);
}

float neg_div_msub_x87(float a, float b, float c, float d, float divisor) {
  return -fdiv_x87(msub_x87(a, b, c, d), divisor);
}

void cross3_x87(const float a[3], const float b[3], float out[3]) {
  out[0] = msub_x87(a[1], b[2], a[2], b[1]);
  out[1] = msub_x87(a[2], b[0], a[0], b[2]);
  out[2] = msub_x87(a[0], b[1], a[1], b[0]);
}

float length3_x87(const float v[3]) {
  const float xx = fmul_x87(v[0], v[0]);
  const float yy = fmul_x87(v[1], v[1]);
  const float zz = fmul_x87(v[2], v[2]);
  const float sum = fadd_x87(fadd_x87(xx, yy), zz);
  return round_f32(std::sqrt(static_cast<double>(sum)));
}

void normalize_vec3_x87(float v[3]) {
  const float len = length3_x87(v);
  if (len == 0.0f) {
    v[0] = v[1] = v[2] = 0.0f;
    return;
  }
  const float inv = fdiv_x87(1.0f, len);
  v[0] = fmul_x87(inv, v[0]);
  v[1] = fmul_x87(inv, v[1]);
  v[2] = fmul_x87(inv, v[2]);
}

}  // namespace opennova::object::x87
