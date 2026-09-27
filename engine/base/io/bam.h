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

#pragma once

#include <cmath>
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

// The BAM32 <-> radian scales, exact (a full turn is 2^32): the one spelling
// every port aliases. A port that reproduces a retail double VERBATIM (the
// x87 dbl_7C3608 = 1.4629627251502471e-9, 30.5 ppm off) keeps its own
// constant and says so; everything else uses these.
constexpr double kRadiansPerBam = 6.283185307179586 / 4294967296.0;
constexpr double kBamPerRadian = 4294967296.0 / 6.283185307179586;

// Radians to the nearest BAM32, wrapped to the signed 32-bit turn.
inline int32_t bam_from_radians(double radians) {
	return static_cast<int32_t>(static_cast<int64_t>(std::llround(radians * kBamPerRadian)));
}

// The plain angle constants every port aliases (the same one-home rule as the
// BAM scales above): pi and the degree <-> radian scales.
constexpr double kPi = 3.14159265358979323846;
constexpr double kRadiansPerDegree = kPi / 180.0;
constexpr double kDegreesPerRadian = 180.0 / kPi;

// The retail 1024-entry Q22 sine table (g_BamSinTableQ22 @0x31bfbc0) and
// its cosine view (off_849934 = &g_BamSinTableQ22[256]): one entry per
// 2^22 BAM, indexed by `angle >> 22`, each read as `entry * (1 / 4194304)`.
// The entries are the truncated Q22 sines.
constexpr int kBamTableEntries = 1024;

inline double bam_table_sin(int index) {
	const double v = std::sin(static_cast<double>(index & (kBamTableEntries - 1)) *
			(2.0 * kPi / static_cast<double>(kBamTableEntries)));
	return static_cast<double>(static_cast<int32_t>(v * 4194304.0)) / 4194304.0;
}

inline double bam_table_cos(int index) {
	const double v = std::cos(static_cast<double>(index & (kBamTableEntries - 1)) *
			(2.0 * kPi / static_cast<double>(kBamTableEntries)));
	return static_cast<double>(static_cast<int32_t>(v * 4194304.0)) / 4194304.0;
}

// The table index of a BAM angle: its top ten bits (`angle >> 22`, an
// unsigned shift of the wrapped angle).
inline int bam_table_index(uint32_t angle) {
	return static_cast<int>(angle >> 22);
}

} // namespace io
} // namespace opennova
