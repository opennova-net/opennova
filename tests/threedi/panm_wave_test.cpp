#include <stdio.h>
#include <stdint.h>

#include <formats/threedi/threedi_panm.h>

using namespace opennova::threedi;

static int expect_eq(const char *label, uint8_t got, uint8_t expected) {
    if (got != expected) {
        fprintf(stderr, "%s: got %u expected %u\n", label,
                (unsigned)got, (unsigned)expected);
        return 0;
    }
    return 1;
}

int main(void) {
    const uint8_t *t = threedi_panm_wave_table();
    int ok = 1;

    if (!t) {
        fprintf(stderr, "wave table is null\n");
        return 1;
    }

    ok &= expect_eq("band0[0]", t[0], 0xFF);
    ok &= expect_eq("band0[127]", t[127], 0xFF);
    ok &= expect_eq("band0[128]", t[128], 0x00);

    ok &= expect_eq("sin band[256]", t[256], 127);
    ok &= expect_eq("sin band[263]", t[263], 149);
    ok &= expect_eq("cos band[512]", t[512], 255);
    ok &= expect_eq("cos band[519]", t[519], 253);

    ok &= expect_eq("triangle[768]", t[768], 0);
    ok &= expect_eq("triangle[769]", t[769], 2);
    ok &= expect_eq("triangle[895]", t[895], 254);

    // The noise band (C-250): the CRT's default-state stream past the NAPI
    // cookie ring's draw (41), so the draws 18467, 6334, 26500 ... masked to
    // 12 bits, smoothed as CWaveformTable_Build smooths them [orig:
    // CWaveformTable_Build @ 0x5DE360; CSessionIdRing_Init's rand @ 0x4DBC45].
    ok &= expect_eq("rand band[1536]", t[1536], 0x83);
    ok &= expect_eq("rand band[1537]", t[1537], 0x8C);
    ok &= expect_eq("rand band[1538]", t[1538], 0x9D);
    ok &= expect_eq("rand band[1539]", t[1539], 0xAD);
    ok &= expect_eq("rand band[1551]", t[1551], 0x92);
    ok &= expect_eq("rand band[1791]", t[1791], 0x8E);

    ok &= expect_eq("sin255 band[1792]", t[1792], 0);
    ok &= expect_eq("sin255 band[1799]", t[1799], 43);

    ok &= expect_eq("sin255/128 band[2048]", t[2048], 0);
    ok &= expect_eq("sin255/128 band[2055]", t[2055], 85);

    ok &= expect_eq("double-sin band[2304]", t[2304], 127);
    ok &= expect_eq("double-sin band[2311]", t[2311], 137);

    // The style-0xF band is never written: all zero (C-251) [orig:
    // CWaveformTable_Build @ 0x5DE360 writes 0..2559; the lookup's case 0xF
    // @ 0x5DE7BD reads table + 2560 @ 0x5DE7C2].
    bool tail_zero = true;
    for (int i = 2560; i < THREEDI_PANM_WAVE_TABLE_SIZE; ++i) tail_zero = tail_zero && t[i] == 0;
    ok &= expect_eq("tail band all zero", tail_zero ? 1 : 0, 1);

    return ok ? 0 : 1;
}
