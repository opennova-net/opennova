#pragma once
#include <cstdint>
namespace opennova::io {
// [orig: PRNG_Next16_B @ 0x6130F0] The same recurrence as the A stream,
// with a distinct owner/seed. Callers own lifetime and call order.
inline uint16_t rotating_prng_next16(uint32_t &state) noexcept {
    const uint32_t rotated = (state << 11) | (state >> 21);
    const uint32_t sum = state + rotated;
    state = ((sum << 4) | (sum >> 28)) ^ 1u;
    return static_cast<uint16_t>(state);
}
inline constexpr uint32_t kPrng16BSeed = 0x5ADEADA5u; // Game_StartMission @ 0x52460B
inline uint16_t rotating_prng_callback(void *state) {
    return rotating_prng_next16(*static_cast<uint32_t *>(state));
}
} // namespace opennova::io
