// Bit rotation: the x86 ROL the original compiles its keystreams and PRNGs to (a CPU primitive,
// not witnessed arithmetic).

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

} // namespace io
} // namespace opennova
