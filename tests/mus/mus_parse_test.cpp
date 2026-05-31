#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mus/mus.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef MUS_FIXTURE_DIR
#define MUS_FIXTURE_DIR "fixtures/mus"
#endif

static uint8_t *slurp(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

static int test_open_jo_gamemus(void) {
    size_t n;
    uint8_t *buf = slurp(MUS_FIXTURE_DIR "/jo_gamemus.bin", &n);
    CHECK(buf, "fixture present");
    MusFile mf;
    int rc = mus_open_memory(&mf, buf, n);
    CHECK(rc == 0, "open succeeded");
    CHECK(mf.header.magic == MUS_MAGIC_SCR0, "magic");
    CHECK(mf.header.chunk_count == 1, "single chunk");
    CHECK(mf.header.name_count == 11, "11 intrinsic names");
    CHECK(mf.scripts != NULL, "scripts allocated");
    CHECK(strncmp(mf.scripts[0].name, "gamescript", 10) == 0, "first script gamescript");
    CHECK(mf.scripts[0].globals_size == 0x40, "globals_size 0x40");
    CHECK(mf.scripts[0].locals_size == 0x28, "locals_size 0x28");
    CHECK(mf.scripts[0].section_count == 8, "8 sections");
    CHECK(mf.scripts[0].entry_section_index == 0, "entry section 0");
    /* Bytecode size: bytecode_offset 0x88, debug_info_offset 0x10C => 132 bytes. */
    CHECK(mf.scripts[0].code_size == 132, "code_size 132 bytes");
    CHECK(mf.scripts[0].code != NULL, "code allocated");
    /* First opcode of section 0 entry-PC is at chunk+0x89; the byte at +0x88 is 0x00 (nop). */
    CHECK(mf.scripts[0].code[0] == 0x00, "first opcode is nop");
    /* Section table content matches witness. */
    /* code_offset is bytecode-relative (0x89 chunk-relative - 0x88 bytecode_offset = 0x01). */
    CHECK(mf.scripts[0].sections[0].code_offset == 0x01, "sec[0] code_offset (bytecode-relative)");
    CHECK(mf.scripts[0].sections[7].code_offset == 0x54, "sec[7] code_offset (bytecode-relative)");
    /* Section names are populated from the editor debug-export table when present.
       The fixture is MDEdit-built and carries the original section labels. */
    CHECK(strcmp(mf.scripts[0].sections[0].name, "Begin") == 0, "sec[0] name Begin");
    CHECK(strcmp(mf.scripts[0].sections[7].name, "Multiplayerstart") == 0,
          "sec[7] name Multiplayerstart");
    /* Editor source path lives at the head of the debug section. */
    CHECK(strstr(mf.scripts[0].source_path, "gamemus.mus") != NULL,
          "source_path embeds gamemus.mus");
    /* Intrinsic names */
    CHECK(mf.intrinsic_count == 11, "11 intrinsic names parsed");
    CHECK(strncmp(mf.intrinsic_names[0],  "GEcho",    5)  == 0, "intrinsic[0] GEcho");
    CHECK(strncmp(mf.intrinsic_names[1],  "GGRnd",    5)  == 0, "intrinsic[1] GGRnd");
    CHECK(strncmp(mf.intrinsic_names[2],  "GSV",      3)  == 0, "intrinsic[2] GSV");
    CHECK(strncmp(mf.intrinsic_names[3],  "GSDV",     4)  == 0, "intrinsic[3] GSDV");
    CHECK(strncmp(mf.intrinsic_names[4],  "GFB",      3)  == 0, "intrinsic[4] GFB");
    CHECK(strncmp(mf.intrinsic_names[5],  "FSet",     4)  == 0, "intrinsic[5] FSet");
    CHECK(strncmp(mf.intrinsic_names[6],  "FClear",   6)  == 0, "intrinsic[6] FClear");
    CHECK(strncmp(mf.intrinsic_names[7],  "FIsSet",   6)  == 0, "intrinsic[7] FIsSet");
    CHECK(strncmp(mf.intrinsic_names[8],  "FIsClear", 8)  == 0, "intrinsic[8] FIsClear");
    CHECK(strncmp(mf.intrinsic_names[9],  "TStart",   6)  == 0, "intrinsic[9] TStart");
    CHECK(strncmp(mf.intrinsic_names[10], "TStop",    5)  == 0, "intrinsic[10] TStop");
    mus_close(&mf);
    free(buf);
    return 1;
}

static int test_close_idempotent(void) {
    MusFile mf;
    memset(&mf, 0, sizeof(mf));
    mus_close(&mf);  /* should not crash on zero-init */
    mus_close(&mf);  /* idempotent */
    return 1;
}

static int test_open_file(void) {
    MusFile mf;
    int rc = mus_open(&mf, MUS_FIXTURE_DIR "/jo_gamemus.bin");
    CHECK(rc == 0, "open from path");
    CHECK(mf.header.chunk_count == 1, "single chunk");
    CHECK(strncmp(mf.scripts[0].name, "gamescript", 10) == 0, "gamescript");
    mus_close(&mf);
    return 1;
}

static int test_open_missing_file(void) {
    MusFile mf;
    memset(&mf, 0xCD, sizeof(mf));
    int rc = mus_open(&mf, "fixtures/mus/__definitely_not_present__.bin");
    CHECK(rc != 0, "fail on missing file");
    /* close after failed open must be safe */
    mus_close(&mf);
    return 1;
}

static int test_find_section(void) {
    MusFile mf;
    CHECK(mus_open(&mf, MUS_FIXTURE_DIR "/jo_gamemus.bin") == 0, "open");
    CHECK(mf.scripts[0].section_count == 8, "8 sections");
    /* Real section name from the debug export table should round-trip through find. */
    const MusSection *s3 = mus_find_section(&mf.scripts[0], "Win000");
    CHECK(s3 != NULL, "Win000 found");
    CHECK(s3 == &mf.scripts[0].sections[3], "ptr match");
    CHECK(s3->code_offset == 0x21, "Win000 code_offset 0x21 (bytecode-relative)");
    const MusSection *miss = mus_find_section(&mf.scripts[0], "NOPE");
    CHECK(miss == NULL, "miss returns NULL");
    /* NULL guards */
    CHECK(mus_find_section(NULL, "x") == NULL, "NULL script returns NULL");
    CHECK(mus_find_section(&mf.scripts[0], NULL) == NULL, "NULL name returns NULL");
    mus_close(&mf);
    return 1;
}

int main(void) {
    RUN_TEST(test_open_jo_gamemus);
    RUN_TEST(test_close_idempotent);
    RUN_TEST(test_open_file);
    RUN_TEST(test_open_missing_file);
    RUN_TEST(test_find_section);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
