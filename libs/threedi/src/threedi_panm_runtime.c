#include "threedi/threedi_panm_runtime.h"

#include <string.h>

// Helpers mirror sub_4354B0 / PANM_SampleTrack from IDA.

static uint32_t g_rand_state = 1; // mirrors MSVC rand seed default

static inline uint16_t msvc_rand15(void) {
    g_rand_state = g_rand_state * 214013u + 2531011u;
    return (uint16_t)((g_rand_state >> 16) & 0x7FFFu);
}

// Wave lookup matching sub_4354B0.
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
            return 16 * (int32_t)(msvc_rand15() & 0x0FFF);
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

int32_t threedi_panm_sample_track_raw(const ThreediTransform *track,
                                      uint32_t time_ms,
                                      const uint16_t *ctrl_table) {
    if (!track) {
        return 0;
    }
    // func high nibble must be non-zero to be active.
    if ((track->control & 0xF0) == 0) {
        return 0;
    }

    const int32_t start_fp8 = ((int32_t)(int16_t)track->start) << 8;
    const int32_t delta = (int32_t)(int16_t)track->end - (int32_t)(int16_t)track->start;

    if (track->control == 24) { // SET
        return start_fp8;
    }

    // a3 = (param << 8) + (time_ms << 8) / 1000 * rate
    int32_t a3 = ((int32_t)track->control_param << 8)
               + (int32_t)((time_ms << 8) / 1000u) * (int32_t)(int16_t)track->rate;

    int32_t wave_fp8 = 0;
    if (track->control == 113) { // control register wave
        uint16_t ctrl = ctrl_table ? ctrl_table[(uint8_t)track->control_param * 2] : 0;
        wave_fp8 = (int32_t)ctrl; // already 16-bit; scaled later by >>8
    } else {
        const uint8_t *table = threedi_panm_wave_table();
        wave_fp8 = panm_wave_lookup(table, track->control, (int16_t)a3);
    }

    // wave_fp8 is 24.8, delta is signed 16; scale result by >>8
    return start_fp8 + ((wave_fp8 * delta) >> 8);
}
