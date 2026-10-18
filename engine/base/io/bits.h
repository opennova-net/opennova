// Bit primitives: the x86 ROL the original compiles its keystreams and PRNGs to, and the power-of-two
// test (CPU primitives, not witnessed arithmetic).

#pragma once

#include <cstdint>

namespace opennova {
namespace io {

// Rotate left by `count` mod 32, as ROL masks its count to five bits; a count of 0 returns the
// value.
inline constexpr uint32_t rotl32(uint32_t value, unsigned count)
{
    count &= 31u;
    return count == 0u ? value : (value << count) | (value >> (32u - count));
}

// Rotate right by `count` mod 32 (x86 ROR), the same count rule.
inline constexpr uint32_t rotr32(uint32_t value, unsigned count)
{
    count &= 31u;
    return count == 0u ? value : (value >> count) | (value << (32u - count));
}

// One bit set: a power of two (0 is none). C++17 has no std::has_single_bit.
inline constexpr bool is_power_of_two(uint32_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

} // namespace io
} // namespace opennova
