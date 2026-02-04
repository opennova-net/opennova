#include <stdio.h>
#include <stdint.h>

#include "threedi/threedi_panm.h"

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

    ok &= expect_eq("rand band[1536]", t[1536], 111);
    ok &= expect_eq("rand band[1537]", t[1537], 183);

    ok &= expect_eq("sin255 band[1792]", t[1792], 0);
    ok &= expect_eq("sin255 band[1799]", t[1799], 43);

    ok &= expect_eq("sin255/128 band[2048]", t[2048], 0);
    ok &= expect_eq("sin255/128 band[2055]", t[2055], 85);

    ok &= expect_eq("double-sin band[2304]", t[2304], 127);
    ok &= expect_eq("double-sin band[2311]", t[2311], 137);

    ok &= expect_eq("tail table[2560]", t[2560], 0x18);
    ok &= expect_eq("tail table[2567]", t[2567], 0x8f);

    return ok ? 0 : 1;
}
