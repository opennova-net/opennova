// Multi-clip .adm rows: several quoted tokens on one row register as VARIANTS of
// the same anim slot — the engine rotates them round-robin [orig:
// AnimMap_ParseConfigLine @ 0x40cb60 loops every token on the line;
// AnimMap_RegisterBoneNode @ 0x40c2d0 links each into the slot's circular ring].
// Pins: the parser fills values[]/value_count (value stays == values[0] for the
// single-clip read path), and adm_write emits every variant back onto one row.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adm/adm.h"

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

    // Single-clip rows: one variant, value == values[0].
    CHECK(adm.entries[0].value_count == 1);
    CHECK(strcmp(adm.entries[0].values[0], "m4_RST") == 0);
    CHECK(strcmp(adm.entries[0].value, adm.entries[0].values[0]) == 0);

    // Triple-clip row keeps every token IN ORDER — the authored duplication is the
    // rotation weighting (r plays twice per r2 cycle).
    CHECK(strcmp(adm.entries[2].key, "anim_wpn_reload") == 0);
    CHECK(adm.entries[2].value_count == 3);
    CHECK(strcmp(adm.entries[2].values[0], "m4_1r") == 0);
    CHECK(strcmp(adm.entries[2].values[1], "m4_1r") == 0);
    CHECK(strcmp(adm.entries[2].values[2], "m4_1r2") == 0);
    CHECK(strcmp(adm.entries[2].value, "m4_1r") == 0);

    // Double-clip row (extra whitespace between tokens).
    CHECK(adm.entries[3].value_count == 2);
    CHECK(strcmp(adm.entries[3].values[0], "m4_1f") == 0);
    CHECK(strcmp(adm.entries[3].values[1], "m4_1f2") == 0);

    // Writer roundtrip: variants survive one row, reparse-equal.
    char out_path[4096];
    const char *tmp = getenv("TMP");
    if (!tmp) tmp = getenv("TEMP");
    if (!tmp) tmp = ".";
    snprintf(out_path, sizeof(out_path), "%s/adm_variants_roundtrip.adm", tmp);
    CHECK(adm_write(out_path, adm.entries, adm.count) == 0);

    AdmFile again;
    memset(&again, 0, sizeof(again));
    CHECK(adm_parse(out_path, &again) == 0);
    CHECK(again.count == adm.count);
    for (size_t i = 0; i < again.count && i < adm.count; ++i) {
        CHECK(strcmp(again.entries[i].key, adm.entries[i].key) == 0);
        CHECK(strcmp(again.entries[i].value, adm.entries[i].value) == 0);
        CHECK(again.entries[i].value_count == adm.entries[i].value_count);
        for (size_t v = 0; v < again.entries[i].value_count; ++v)
            CHECK(strcmp(again.entries[i].values[v], adm.entries[i].values[v]) == 0);
    }
    adm_free(&again);
    adm_free(&adm);
    remove(out_path);

    if (failures == 0) printf("adm_variants_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
