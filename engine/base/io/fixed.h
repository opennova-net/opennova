// Fixed-point <-> float conversions used across the NovaLogic formats.
//
// 16.16 is the engine-wide fixed-point convention (docs/engine-primer.md);
// 2.14 appears in the 3DI3 normal/quaternion payloads.

#ifndef OPENNOVA_IO_FIXED_H
#define OPENNOVA_IO_FIXED_H

#include <cstdint>

#include <base/io/le.h>

namespace opennova {
namespace io {

inline float fp16_16_to_float(int32_t raw)
{
    return (float)raw / 65536.0f;
}

inline int32_t float_to_fp16_16(float f)
{
    return (int32_t)(f * 65536.0f);
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
