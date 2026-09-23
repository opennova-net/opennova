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

    // The retail config reader: an unquoted "//" or ";" ends the line, a
    // token starting with '/' ends the row, and a row with no clip is empty
    // rather than fatal. JOTAC US01.adm keeps an older walk clip behind "//".
    // [orig: Terrain_TokenizeConfigLine @0x53CC16..0x53CC31;
    //  AnimMap_ParseConfigLine @0x40CBD0..0x40CBD2]
    const char *commented =
        "anim_walk_forward\t\t\"Dt1RunF.bad\"\t//\t\"D4WLK_F.bad\"\r\n"
        "anim_wpn_fire \"first.bad\" \"second.bad\" // \"unused.bad\"\r\n"
        "anim_idle \"dir/idle.bad\" // an unmatched quote: \"\r\n"
        "anim_run_forward \"a.bad\" \"b.bad\" \"B_FORWA1.bad\"/no target or near goal\r\n"
        "anim_jog_forward \"a.bad\";\"b.bad\"\r\n"
        "anim_stop \"/lead.bad\" \"b.bad\"\r\n"
        "anim_emote_10\r\n"
        "anim_crouch // \"a.bad\"\r\n"
        "anim_prone \"a.bad\",\"b.bad\"\r\n";
    CHECK(adm_parse_buffer(commented, strlen(commented), &adm) == 0);
    CHECK(adm.count == 6);
    if (adm.count == 6) {
        CHECK(strcmp(adm.entries[0].key, "anim_walk_forward") == 0);
        CHECK(adm.entries[0].variant_count == 1);
        CHECK(strcmp(adm.entries[0].variants[0], "Dt1RunF.bad") == 0);
        CHECK(adm.entries[1].variant_count == 2);
        CHECK(strcmp(adm.entries[1].variants[1], "second.bad") == 0);
        CHECK(adm.entries[2].variant_count == 1);
        CHECK(strcmp(adm.entries[2].variants[0], "dir/idle.bad") == 0);
        // BIRD1.ADM's shape: the unquoted /no token is the row's break.
        CHECK(adm.entries[3].variant_count == 3);
        CHECK(strcmp(adm.entries[3].variants[2], "B_FORWA1.bad") == 0);
        CHECK(strcmp(adm.entries[4].key, "anim_jog_forward") == 0);
        CHECK(adm.entries[4].variant_count == 1);
        // A quoted token that starts with '/' also ends the row, so
        // anim_stop, anim_emote_10 and anim_crouch register nothing.
        CHECK(strcmp(adm.entries[5].key, "anim_prone") == 0);
        CHECK(adm.entries[5].variant_count == 2);
    }
    adm_free(&adm);

    // Lines split only on CR LF, and a tail line without one loses its final
    // byte. [orig: File_ParseASCIIFile @0x53D8C7..0x53D8F5]
    const char *tail = "anim_idle \"x.bad\"Z";
    CHECK(adm_parse_buffer(tail, strlen(tail), &adm) == 0);
    CHECK(adm.count == 1 && adm.entries[0].variant_count == 1);
    if (adm.count == 1) CHECK(strcmp(adm.entries[0].variants[0], "x.bad") == 0);
    adm_free(&adm);

    if (failures == 0) printf("adm_variants_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
