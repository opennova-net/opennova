// LW .3di format detection test.
// Verifies threedi_lw_detect distinguishes LW v8/v10 ("3DI"+version) from the
// modern "3DI3" container and from GP formats, and rejects short/garbage input.
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "threedi/threedi_lw.h"

static int failures = 0;

static void check(const char *label, ThreediLwVersion got, ThreediLwVersion want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d, want %d\n", label, (int)got, (int)want);
        failures++;
    }
}

int main(void) {
    const uint8_t v10[4] = {'3', 'D', 'I', 0x0A};
    const uint8_t v8[4]  = {'3', 'D', 'I', 0x08};
    const uint8_t modern[4] = {'3', 'D', 'I', '3'}; // 0x33 — must NOT be claimed by LW
    const uint8_t gpm[4] = {'G', 'P', 'M', 0x02};
    const uint8_t junk[4] = {0x00, 0x11, 0x22, 0x33};

    check("v10", threedi_lw_detect(v10, 4), THREEDI_LW_VERSION_10);
    check("v8", threedi_lw_detect(v8, 4), THREEDI_LW_VERSION_8);
    check("modern-3DI3-not-lw", threedi_lw_detect(modern, 4), THREEDI_LW_VERSION_UNKNOWN);
    check("gpm-not-lw", threedi_lw_detect(gpm, 4), THREEDI_LW_VERSION_UNKNOWN);
    check("junk", threedi_lw_detect(junk, 4), THREEDI_LW_VERSION_UNKNOWN);
    check("too-short", threedi_lw_detect(v10, 3), THREEDI_LW_VERSION_UNKNOWN);
    check("null", threedi_lw_detect(NULL, 4), THREEDI_LW_VERSION_UNKNOWN);

    if (failures) {
        printf("lw_detect test: %d failures\n", failures);
        return 1;
    }
    printf("lw_detect test: all checks passed\n");
    return 0;
}
