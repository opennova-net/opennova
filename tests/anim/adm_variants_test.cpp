// Multi-clip .adm rows: several quoted tokens on one row register as VARIANTS of
// the same anim slot — the engine rotates them round-robin [orig:
// AnimMap_ParseConfigLine @ 0x40cb60 loops every token on the line;
// AnimMap_RegisterBoneNode @ 0x40c2d0 links each into the slot's circular ring].
// Pins: the runtime parser fills variants[]/variant_count without a separate
// first-value field.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <formats/adm/adm.h>

using namespace opennova::adm;

static int failures = 0;
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

int main(void) {
    // The REVVY M4_1ST.adm shape: single-, double-, and triple-clip rows.
    const char *src =
        "\r\n"
        "anim_reset\t\t\t\t\"m4_RST\"\r\n"
        "anim_wpn_idle\t\t\t\t\"m4_1i\"\r\n"
        "anim_wpn_reload\t\t\t\t\"m4_1r\" \"m4_1r\" \"m4_1r2\"\r\n"
        "anim_wpn_fire\t\t\t\t\"m4_1f\"  \"m4_1f2\"\r\n";

    AdmFile adm;
    memset(&adm, 0, sizeof(adm));
    CHECK(adm_parse_buffer(src, strlen(src), &adm) == 0);
    CHECK(adm.count == 4);

    // Single-clip rows have one variant.
    CHECK(adm.entries[0].variant_count == 1);
    CHECK(strcmp(adm.entries[0].variants[0], "m4_RST") == 0);

    // Triple-clip row keeps every token IN ORDER — the authored duplication is the
    // rotation weighting (r plays twice per r2 cycle).
    CHECK(strcmp(adm.entries[2].key, "anim_wpn_reload") == 0);
    CHECK(adm.entries[2].variant_count == 3);
    CHECK(strcmp(adm.entries[2].variants[0], "m4_1r") == 0);
    CHECK(strcmp(adm.entries[2].variants[1], "m4_1r") == 0);
    CHECK(strcmp(adm.entries[2].variants[2], "m4_1r2") == 0);

    // Double-clip row (extra whitespace between tokens).
    CHECK(adm.entries[3].variant_count == 2);
    CHECK(strcmp(adm.entries[3].variants[0], "m4_1f") == 0);
    CHECK(strcmp(adm.entries[3].variants[1], "m4_1f2") == 0);

    adm_free(&adm);

    if (failures == 0) printf("adm_variants_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
