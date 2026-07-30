#include "threedi/threedi_panm_runtime.h"

#include <string.h>

static uint32_t g_rand_state = 1; // MSVC CRT default seed

uint16_t threedi_wave_rand15(void) {
    g_rand_state = g_rand_state * 214013u + 2531011u;
    return (uint16_t)((g_rand_state >> 16) & 0x7FFFu);
}

void threedi_wave_srand(uint32_t seed) {
    g_rand_state = seed;
}

// Retail uses two-operand 32-bit IMUL and keeps the low dword, then performs
// an arithmetic shift. Spell both operations in unsigned bits so overflow is
// intentional rather than C++ signed-overflow undefined behavior.
// [orig: PANM_SampleTrack @ 0x5B22B1..0x5B22B4, 0x5B22FE..0x5B2301]
static int32_t retail_imul_sar8(int32_t lhs, int32_t rhs) {
    const uint32_t product = (uint32_t)lhs * (uint32_t)rhs;
    uint32_t shifted = product >> 8;
    if ((product & 0x80000000u) != 0) {
        shifted |= 0xFF000000u;
    }
    int32_t result;
    memcpy(&result, &shifted, sizeof(result));
    return result;
}

// [orig: wave_lookup @ 0x5DE6B0]
static int32_t panm_wave_lookup(const uint8_t *table, uint8_t func, int16_t a3) {
    uint8_t idx = (uint8_t)(a3 >> 8);
    switch (func & 0x0F) {
        case 1:
            return ((int32_t)table[idx]) << 8;
        case 2:
            return ((int32_t)table[256 + idx]) << 8;
        case 3:
            return ((int32_t)table[768 + idx]) << 8;
        case 4:
            return ((int32_t)table[1024 + idx]) << 8;
        case 5:
            return ((int32_t)table[1280 + idx]) << 8;
        case 6:
            return 16 * (int32_t)(threedi_wave_rand15() & 0x0FFF);
        case 7: {
            int32_t v0 = table[1536 + idx];
            int32_t v1 = table[1536 + (uint8_t)(idx + 1)];
            return (v1 - v0) * (uint8_t)a3 + (v0 << 8);
        }
        case 8:
            return ((int32_t)table[1792 + idx]) << 8;
        case 9:
            return ((int32_t)table[2048 + idx]) << 8;
        case 0xA: {
            int32_t v0 = table[2304 + idx];
            int32_t v1 = table[2304 + (uint8_t)(idx + 1)];
            return (v1 - v0) * (uint8_t)a3 + (v0 << 8);
        }
        case 0xF:
            return ((int32_t)table[2560 + idx]) << 8;
        default:
            return 0;
    }
}

// [orig: PANM_SampleTrack @ 0x5B2270]
int32_t threedi_panm_sample_track_raw(const ThreediTransform *track,
                                      uint32_t time_ms,
                                      const int32_t *ctrl_values) {
    if (!track) {
        return 0;
    }
    // func high nibble must be non-zero to be active.
    if ((track->control & 0xF0) == 0) {
        return 0;
    }

    const int32_t start_fp8 = (int32_t)(int16_t)track->start * 256;
    const int32_t delta = (int32_t)(int16_t)track->end - (int32_t)(int16_t)track->start;

    if (track->control == 24) { // SET
        return start_fp8;
    }

    // Retail PANM recognizes only 113 as a control-register sampler. Codes
    // 114..117 deliberately fall through to wave_lookup by their low nibble.
    if (track->control == 113) {
        const uint8_t ordinal = track->control_param;
        const int32_t ctrl =
            ctrl_values && ordinal < THREEDI_CTRL_REGISTER_COUNT
                ? ctrl_values[ordinal]
                : 0;
        return start_fp8 + retail_imul_sar8(ctrl, delta);
    }

    // a3 = (param << 8) + (time_ms << 8) / 1000 * rate. Only the
    // low word reaches wave_lookup; keep the accumulation unsigned so the
    // original 32-bit wrap is defined under optimization.
    const uint32_t a3_bits =
            (static_cast<uint32_t>(track->control_param) << 8) +
            ((time_ms << 8) / 1000u) *
                    static_cast<uint32_t>(
                            static_cast<uint16_t>(track->rate));
    const uint16_t phase_bits = static_cast<uint16_t>(a3_bits);
    int16_t phase;
    memcpy(&phase, &phase_bits, sizeof(phase));
    const uint8_t *table = threedi_panm_wave_table();
    const int32_t wave_fp8 =
            panm_wave_lookup(table, track->control, phase);
    return start_fp8 + retail_imul_sar8(wave_fp8, delta);
}
