// Fixed-point <-> float conversions used across the NovaLogic formats.
//
// 16.16 is the engine-wide fixed-point convention (docs/engine-primer.md);
// 2.14 appears in the 3DI3 normal/quaternion payloads.

#ifndef OPENNOVA_IO_FIXED_H
#define OPENNOVA_IO_FIXED_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include <base/io/le.h>

namespace opennova {
namespace io {

// The 16.16 and 2.14 scales, spelled once per type so a site keeps the exact
// arithmetic it had (float, double and integer forms are NOT interchangeable:
// a float divide and a double divide round differently).
inline constexpr float kFp16One = 65536.0f;
inline constexpr float kInvFp16One = 1.0f / 65536.0f;
inline constexpr double kFp16OneD = 65536.0;
inline constexpr int32_t kFp16OneInt = 65536;
inline constexpr float kFp14One = 16384.0f;

inline float fp16_16_to_float(int32_t raw)
{
    return (float)raw / 65536.0f;
}

inline int32_t float_to_fp16_16(float f)
{
    return (int32_t)(f * 65536.0f);
}

// The saturating world-units -> 16.16 forms the Godot bindings convert their
// float positions with. Binding policies, not witnessed arithmetic: retail
// never held these values as floats. NaN yields 0; out-of-range saturates.
namespace detail {
inline int32_t saturate_fp16_16(double scaled)
{
    if (std::isnan(scaled))
        return 0;
    const double lo = (double)std::numeric_limits<int32_t>::min();
    const double hi = (double)std::numeric_limits<int32_t>::max();
    return (int32_t)std::clamp(scaled, lo, hi);
}
} // namespace detail

// Truncating (the default float -> int the original compiles to).
inline int32_t float_to_fp16_16_sat(double units)
{
    return detail::saturate_fp16_16(units * 65536.0);
}

// Round-to-nearest.
inline int32_t float_to_fp16_16_round_sat(double units)
{
    return detail::saturate_fp16_16(std::round(units * 65536.0));
}

// Non-negative, truncating, unsaturated: the env fog-distance form.
inline uint32_t float_to_fp16_16_nonneg(float units)
{
    if (units <= 0.0f)
        return 0u;
    return (uint32_t)(units * 65536.0f);
}

inline float fp14_to_float(int16_t raw)
{
    return (float)raw / 16384.0f;
}

inline float read_fp_16_16(const uint8_t *p)
{
    return fp16_16_to_float(read_s32_le(p));
}

inline float read_fp_14(const uint8_t *p)
{
    return fp14_to_float(read_s16_le(p));
}

// Q22 (10.22): the world-space fixed-point scale of the collision/entity
// matrices and the sin/cos tables — 2^22 as one double, so the ~40 sites that
// multiply or divide by it alias a single spelling.
constexpr double kQ22One = 4194304.0;
constexpr double kInvQ22One = 1.0 / 4194304.0;

} // namespace io
} // namespace opennova

#endif // OPENNOVA_IO_FIXED_H
