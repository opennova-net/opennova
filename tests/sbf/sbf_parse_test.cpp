#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sbf/sbf.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef SBF_FIXTURE_DIR
#define SBF_FIXTURE_DIR "fixtures/sbf"
#endif

static uint8_t *slurp(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    fread(buf, 1, (size_t)n, f);
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

static int test_open_bhd_menumus(void) {
    size_t n;
    uint8_t *buf = slurp(SBF_FIXTURE_DIR "/bhd_menumus.sbf", &n);
    CHECK(buf, "fixture must exist");
    SbfArchive arc;
    CHECK(sbf_open_memory(&arc, buf, n) == 0, "open_memory should succeed");
    CHECK(arc.header.magic       == SBF_MAGIC, "magic");
    CHECK(arc.header.entry_count == 155,       "BHD menumus has 155 entries");
    CHECK(arc.entries != NULL,                  "entries allocated");
    CHECK(strncmp(arc.entries[0].name, "MENU101", 7) == 0, "first entry MENU101");
    sbf_close(&arc);
    free(buf);
    return 1;
}

static int test_open_file_jo(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SBF_FIXTURE_DIR "/jo_gamemus.sbf") == 0, "open file");
    CHECK(arc.header.magic       == SBF_MAGIC, "magic");
    CHECK(arc.header.entry_count == 13,        "JO gamemus has 13 entries");
    CHECK(strncmp(arc.entries[0].name, "NULLS", 5) == 0, "first NULLS");
    sbf_close(&arc);
    return 1;
}

static int test_find_by_name(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SBF_FIXTURE_DIR "/jo_gamemus.sbf") == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "NULLS");
    CHECK(e != NULL, "NULLS found");
    CHECK(strncmp(e->name, "NULLS", 5) == 0, "name match");
    /* Engine string compare in jointops!Sbf_StartEntry's caller is uppercase-folded;
       we keep the same case-insensitive semantics. */
    const SbfRawEntry *e2 = sbf_find_by_name(&arc, "nulls");
    CHECK(e2 == e, "case-insensitive match");
    const SbfRawEntry *miss = sbf_find_by_name(&arc, "NOPE");
    CHECK(miss == NULL, "miss returns NULL");
    sbf_close(&arc);
    return 1;
}

static int test_find_by_index(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SBF_FIXTURE_DIR "/jo_gamemus.sbf") == 0, "open");
    const SbfRawEntry *e = sbf_find_by_index(&arc, 0);
    CHECK(e != NULL, "index 0");
    CHECK(strncmp(e->name, "NULLS", 5) == 0, "first is NULLS");
    const SbfRawEntry *oob = sbf_find_by_index(&arc, 999);
    CHECK(oob == NULL, "out-of-range returns NULL");
    sbf_close(&arc);
    return 1;
}

static int test_read_raw(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SBF_FIXTURE_DIR "/jo_gamemus.sbf") == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "NULLS");
    CHECK(e != NULL, "NULLS found");
    CHECK(e->total_size == 0x1008, "NULLS total_size = 4104");
    uint8_t buf[0x1008];
    int got = sbf_read_raw(&arc, e, buf, sizeof(buf));
    CHECK(got == 0x1008, "read full entry");
    /* First 4 bytes are chunk header valid_samples = 0x08A8 in JO gamemus NULLS */
    uint32_t valid_samples;
    memcpy(&valid_samples, buf, 4);
    CHECK(valid_samples == 0x08A8, "valid_samples in NULLS chunk");
    sbf_close(&arc);
    return 1;
}

static int test_read_chunk(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SBF_FIXTURE_DIR "/bhd_menumus.sbf") == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "MENU101");
    CHECK(e != NULL, "MENU101 found");
    uint8_t buf[0x1008];
    int got = sbf_read_chunk(&arc, e, 0, buf, sizeof(buf));
    CHECK(got == (int)e->block_size, "chunk size matches block_size");
    /* Chunk header valid_samples should be 0x1000 (full chunk) for BHD menumus first chunk */
    uint32_t valid_samples;
    memcpy(&valid_samples, buf, 4);
    CHECK(valid_samples == 0x1000, "BHD menumus first chunk full");
    sbf_close(&arc);
    return 1;
}

static int test_open_bad_magic_safe_close(void) {
    SbfArchive arc;
    /* Garbage in struct memory before open */
    memset(&arc, 0xCD, sizeof(arc));
    /* Open a non-existent file */
    int rc = sbf_open(&arc, "fixtures/sbf/__definitely_not_an_sbf__.sbf");
    CHECK(rc != 0, "open should fail on missing file");
    /* sbf_close should be safe even after failed open */
    sbf_close(&arc);
    return 1;
}

int main(void) {
    RUN_TEST(test_open_bhd_menumus);
    RUN_TEST(test_open_file_jo);
    RUN_TEST(test_find_by_name);
    RUN_TEST(test_find_by_index);
    RUN_TEST(test_read_raw);
    RUN_TEST(test_read_chunk);
    RUN_TEST(test_open_bad_magic_safe_close);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
