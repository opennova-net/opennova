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
    std::vector<AdmDroppedLine> dropped;
    CHECK(adm_parse_buffer(commented, strlen(commented), &adm, &dropped) == 0);
    CHECK(adm.count == 6);
    // Each line whose input the table leaves out, on the row it keeps: the comments after
    // clips (lines 1, 2, 3, 5), the rest after BIRD1's /no (line 4), and the rows that
    // register nothing (lines 6, 7, 8). The last line leaves nothing out; none blocks.
    CHECK(dropped.size() == 8);
    if (dropped.size() == 8) {
        const size_t rows[8] = {0, 1, 2, 3, 4, SIZE_MAX, SIZE_MAX, SIZE_MAX};
        for (size_t i = 0; i < 8; ++i) {
            CHECK(dropped[i].line == i + 1 && dropped[i].row == rows[i] && !dropped[i].blocks);
        }
        CHECK(dropped[0].key == "anim_walk_forward" && dropped[0].what.find("comment") != std::string::npos);
        CHECK(dropped[3].what.find("'/no'") != std::string::npos);
        CHECK(dropped[5].key == "anim_stop" && dropped[7].key == "anim_crouch");
    }
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

    // A row names its slot by its key past the first five characters, whatever
    // they are and in any case: `ANIM_RESET` and `xxxx_idle` are rows, kept as
    // authored, and name slots 0 and idle. A key of five characters or fewer
    // names none (BIRD1.ADM's `(if` comment line). [orig: AnimMap_ParseConfigLine
    // @0x40CB60 -> AnimMap_FindSlotByName @0x40CFA0, stricmp on key + 5]
    const char *slots =
        "ANIM_RESET \"rst.bad\"\r\n"
        "xxxx_idle \"idle.bad\"\r\n"
        "reset \"five.bad\"\r\n"
        "(if anim is not availble\r\n";
    CHECK(adm_parse_buffer(slots, strlen(slots), &adm) == 0);
    CHECK(adm.count == 2);
    if (adm.count == 2) {
        CHECK(strcmp(adm.entries[0].key, "ANIM_RESET") == 0);
        CHECK(adm_key_names_slot(adm.entries[0].key, "reset"));
        CHECK(adm_slot_key(adm.entries[0].key) == "anim_reset");
        CHECK(strcmp(adm.entries[1].key, "xxxx_idle") == 0);
        CHECK(adm_slot_key(adm.entries[1].key) == "anim_idle");
        CHECK(!adm_key_names_slot(adm.entries[1].key, "reset"));
    }
    CHECK(adm_slot_key("reset").empty() && adm_slot_key("anim_").empty());
    adm_free(&adm);

    // The lines the walk never hands the row parser: a "//" or ';' comment, a first
    // token that starts with '/', a line of delimiters alone; a blank line holds
    // nothing and is not reported. A row naming ten clips keeps 8 and blocks: the
    // game registers every one. [orig: File_ParseASCIIFile @0x53D915, @0x53D91E;
    // AnimMap_ParseConfigLine @0x40CBAE..0x40CC05]
    const char *skipped =
        "// header\r\n"
        "  ; a note\r\n"
        "/x y\r\n"
        "   \r\n"
        ", ,\r\n"
        "anim_idle a b c d e f g h i j\r\n";
    dropped.clear();
    CHECK(adm_parse_buffer(skipped, strlen(skipped), &adm, &dropped) == 0);
    CHECK(adm.count == 1 && dropped.size() == 5);
    if (adm.count == 1) CHECK(adm.entries[0].variant_count == ADM_MAX_VARIANTS);
    if (dropped.size() == 5) {
        CHECK(dropped[0].line == 1 && dropped[0].what.find("comment") != std::string::npos);
        CHECK(dropped[1].line == 2 && dropped[1].what.find("comment") != std::string::npos);
        CHECK(dropped[2].line == 3 && dropped[2].key == "/x");
        CHECK(dropped[3].line == 5 && dropped[3].row == SIZE_MAX && !dropped[3].blocks);
        CHECK(dropped[4].line == 6 && dropped[4].row == 0 && dropped[4].blocks &&
              dropped[4].what.find("10 clips") != std::string::npos);
    }
    adm_free(&adm);

    if (failures == 0) printf("adm_variants_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
