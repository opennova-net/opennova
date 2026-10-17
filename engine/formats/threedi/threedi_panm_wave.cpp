#include <formats/threedi/threedi_panm.h>

#include <base/io/crt_rand.h>

#include <math.h>

namespace opennova::threedi {

// Port of the waveform-table builder used by PANM_SampleTrack [orig:
// CWaveformTable_Build @ 0x5DE360, run by its static initializer @ 0x794120,
// C++ initializer slot 0x7C0664]. Its noise band draws 256 calls from the
// main thread's CRT rand, unseeded at static init (the default state 1 [orig:
// _initptd @ 0x779069..0x77906C]), after the one draw an earlier initializer
// takes: the NAPI cookie ring's [orig: sub_793B00 @ 0x793B00 in slot 0x7C0594
// -> CSessionIdRing_Init @ 0x4DBC20, rand @ 0x4DBC45]. The port draws from a
// CRT recurrence of its own from that state, so the band is retail's while the
// process-wide stream's later order stays D-3DI-2's.

#define TABLE_BYTES THREEDI_PANM_WAVE_TABLE_SIZE

// Zero-initialized as retail's table is (it lies in .data past the section's
// raw end, 0x85D000, so the loader zero-fills it). CWaveformTable_Build writes
// bands 0..2559 only, so the band the lookup's style 0xF reads
// (table + 2560..2815) stays zero: an active 0x?F track (high nibble set) holds
// its start value [orig: g_WaveformTable @ 0x2BF8ED0; CWaveformTable_WaveLookup
// @ 0x5DE6B0, case 0xF @ 0x5DE7BD (the load @ 0x5DE7C2); PANM_SampleTrack
// @ 0x5B2270].
static uint8_t g_wave_table[TABLE_BYTES];
static int g_wave_table_built = 0;

static void build_wave_table(void) {
    if (g_wave_table_built) {
        return;
    }

    const double kW0 = 0.01227184630308594;
    const double kW1 = 0.02454369260617188;
    const double kW2 = 0.1718058482432031;
    const double kW3 = 0.04908738521234375;

    for (int i = 0; i < 256; ++i) {
        g_wave_table[i] = (uint8_t)(((i >= 128) ? 1 : 0) - 1);
        g_wave_table[256 + i] = (uint8_t)(sin(i * kW1) * 127.5 + 127.5);
        g_wave_table[512 + i] = (uint8_t)(cos(i * kW1) * 127.5 + 127.5);
        g_wave_table[768 + i] = (uint8_t)((i >= 128) ? (-2 - 2 * i) : (2 * i));
        g_wave_table[1024 + i] = (uint8_t)i;
        g_wave_table[1280 + i] = (uint8_t)(-1 - i);

        const uint8_t v4 = (i >= 128) ? 0 : (uint8_t)(sin(i * kW1) * 255.0);
        g_wave_table[1792 + i] = v4;

        const uint8_t v3 = (i >= 64) ? 0 : (uint8_t)(sin(i * kW3) * 255.0);
        g_wave_table[2048 + i] = v3;

        const double v2 = sin(i * kW0);
        g_wave_table[2304 + i] = (uint8_t)(sin(i * kW2) * v2 * 127.5 + 127.5);
    }

    // The noise band: 256 draws masked to 12 bits, each even slot the mean of its
    // two odd neighbours, two [1 2 1] smoothing passes (indices wrapping at 256),
    // the high byte of the 16-bit result [orig: @ 0x5DE580..0x5DE597 the draws,
    // @ 0x5DE599..0x5DE5D6 the means, @ 0x5DE5E0..0x5DE619 and
    // @ 0x5DE620..0x5DE65F the passes, @ 0x5DE670..0x5DE686 the store].
    io::CrtRand rng;
    rng.next(); // the cookie ring's draw ahead of this initializer (see above)
    uint32_t noise[256];
    uint32_t pass[256];
    for (int i = 0; i < 256; ++i) {
        noise[i] = rng.next() & 0xFFFu;
    }
    for (int i = 0; i < 256; i += 2) {
        noise[i] = (noise[(i + 1) & 255] + noise[(i - 1) & 255]) >> 1;
    }
    for (int i = 0; i < 256; ++i) {
        pass[i] = noise[(i + 1) & 255] + noise[(i - 1) & 255] + 2 * noise[i];
    }
    for (int i = 0; i < 256; ++i) {
        noise[i] = pass[(i + 1) & 255] + pass[(i - 1) & 255] + 2 * pass[i];
    }
    for (int i = 0; i < 256; ++i) {
        g_wave_table[1536 + i] = (uint8_t)(noise[i] >> 8);
    }

    g_wave_table_built = 1;
}

const uint8_t *threedi_panm_wave_table(void) {
    if (!g_wave_table_built) {
        build_wave_table();
    }
    return g_wave_table;
}

} // namespace opennova::threedi
