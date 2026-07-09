// 32-bit binary angular measure (BAM) arithmetic: 2^32 units = one full turn.
//
// Retail keeps yaw/pitch/roll in int32 registers and leans on x86 semantics:
// add/sub/shl wrap two's-complement, sar shifts arithmetically, neg maps
// INT32_MIN to itself. In C++ (17) signed overflow is undefined and a negative
// right shift is implementation-defined, so every ported BAM expression routes
// through these helpers — unsigned modular arithmetic with explicit sign
// handling, bit-identical to the retail instructions on every platform.
// The wrap IS the math: a BAM difference is the shortest-arc delta because the
// seam wraps (docs/engine-primer.md).

#ifndef OPENNOVA_IO_BAM_H
#define OPENNOVA_IO_BAM_H

#include <cstdint>

namespace opennova {
namespace io {

// x86 add: two's-complement wrap.
constexpr int32_t bam_add(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a + (uint32_t)b);
}

// x86 sub: the wrapping difference — across the +/-180 deg seam this is the
// shortest arc, which is exactly why the original never normalizes.
constexpr int32_t bam_sub(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a - (uint32_t)b);
}

// x86 shl/lea doubling (2*x): wraps like the add it is.
constexpr int32_t bam_dbl(int32_t a)
{
    return bam_add(a, a);
}

// x86 sar: arithmetic right shift, defined for negative inputs. (Signed >> is
// implementation-defined until C++20; this pins the retail behavior.)
constexpr int32_t bam_sar(int32_t a, int shift)
{
    return a < 0 ? (int32_t)~(~(uint32_t)a >> shift)
                 : (int32_t)((uint32_t)a >> shift);
}

// x86 neg-based magnitude: INT32_MIN stays INT32_MIN (still "negative"),
// exactly as the retail register math has it — never UB.
constexpr int32_t bam_abs(int32_t v)
{
    return v < 0 ? (int32_t)(0u - (uint32_t)v) : v;
}

} // namespace io
} // namespace opennova

#endif // OPENNOVA_IO_BAM_H
