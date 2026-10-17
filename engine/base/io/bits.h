// Bit rotation: the x86 ROL the original compiles its keystreams and PRNGs to (a CPU primitive,
// not witnessed arithmetic); and the power-of-two test.

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

// Whether `v` is a power of two (0 is not). C++17 has no std::has_single_bit.
constexpr bool is_power_of_two(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

} // namespace io
} // namespace opennova
