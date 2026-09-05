/* SBF parse: the header/index/entry reads over the synthetic bank
   fixtures/sbf/synth_gamemus.sbf (minted by tests/fixtures/minimal_sbf_gen.cpp;
   13 entries, SILENCE first as one partial chunk of 0x08A8 samples, TONE01
   spanning three chunks). The retail banks are swept by
   sbf_jo_install_sweep_test behind OPENNOVA_JO_DIR. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <formats/sbf/sbf.h>

using namespace opennova::sbf;

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef SBF_FIXTURE_DIR
#define SBF_FIXTURE_DIR "fixtures/sbf"
#endif
#define SYNTH_BANK SBF_FIXTURE_DIR "/synth_gamemus.sbf"

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

static int test_open_memory(void) {
    size_t n;
    uint8_t *buf = slurp(SYNTH_BANK, &n);
    CHECK(buf, "fixture must exist");
    SbfArchive arc;
    CHECK(sbf_open_memory(&arc, buf, n) == 0, "open_memory should succeed");
    CHECK(arc.header.magic       == SBF_MAGIC, "magic");
    CHECK(arc.header.entry_count == 13,        "synth bank has 13 entries");
    CHECK(arc.entries != NULL,                  "entries allocated");
    CHECK(strncmp(arc.entries[0].name, "SILENCE", 7) == 0, "first entry SILENCE");
    sbf_close(&arc);
    free(buf);
    return 1;
}

static int test_open_file(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SYNTH_BANK) == 0, "open file");
    CHECK(arc.header.magic       == SBF_MAGIC, "magic");
    CHECK(arc.header.entry_count == 13,        "synth bank has 13 entries");
    CHECK(strncmp(arc.entries[0].name, "SILENCE", 7) == 0, "first SILENCE");
    sbf_close(&arc);
    return 1;
}

static int test_find_by_name(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "SILENCE");
    CHECK(e != NULL, "SILENCE found");
    CHECK(strncmp(e->name, "SILENCE", 7) == 0, "name match");
    /* Engine string compare in jointops!Sbf_StartEntry's caller is uppercase-folded;
       we keep the same case-insensitive semantics. */
    const SbfRawEntry *e2 = sbf_find_by_name(&arc, "silence");
    CHECK(e2 == e, "case-insensitive match");
    const SbfRawEntry *miss = sbf_find_by_name(&arc, "NOPE");
    CHECK(miss == NULL, "miss returns NULL");
    sbf_close(&arc);
    return 1;
}

static int test_find_by_index(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_index(&arc, 0);
    CHECK(e != NULL, "index 0");
    CHECK(strncmp(e->name, "SILENCE", 7) == 0, "first is SILENCE");
    const SbfRawEntry *last = sbf_find_by_index(&arc, 12);
    CHECK(last != NULL && strncmp(last->name, "TONE12", 6) == 0, "index 12 is TONE12");
    const SbfRawEntry *oob = sbf_find_by_index(&arc, 999);
    CHECK(oob == NULL, "out-of-range returns NULL");
    sbf_close(&arc);
    return 1;
}

static int test_read_raw(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "SILENCE");
    CHECK(e != NULL, "SILENCE found");
    CHECK(e->total_size == 0x1008, "SILENCE total_size = 4104 (one chunk)");
    uint8_t buf[0x1008];
    int got = sbf_read_raw(&arc, e, buf, sizeof(buf));
    CHECK(got == 0x1008, "read full entry");
    /* First 4 bytes are the chunk header's valid_samples: 0x08A8, the retail NULLS shape. */
    uint32_t valid_samples;
    memcpy(&valid_samples, buf, 4);
    CHECK(valid_samples == 0x08A8, "valid_samples in the SILENCE chunk");
    sbf_close(&arc);
    return 1;
}

static int test_read_chunk(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "TONE01");
    CHECK(e != NULL, "TONE01 found");
    CHECK(e->total_size == 3 * 0x1008, "TONE01 spans three chunks");
    uint8_t buf[0x1008];
    int got = sbf_read_chunk(&arc, e, 0, buf, sizeof(buf));
    CHECK(got == (int)e->block_size, "chunk size matches block_size");
    uint32_t valid_samples;
    memcpy(&valid_samples, buf, 4);
    CHECK(valid_samples == 0x1000, "TONE01 first chunk full");
    CHECK(sbf_read_chunk(&arc, e, 2, buf, sizeof(buf)) == (int)e->block_size, "last chunk reads");
    memcpy(&valid_samples, buf, 4);
    CHECK(valid_samples == 1000, "TONE01 last chunk carries the 1000-sample tail");
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
    RUN_TEST(test_open_memory);
    RUN_TEST(test_open_file);
    RUN_TEST(test_find_by_name);
    RUN_TEST(test_find_by_index);
    RUN_TEST(test_read_raw);
    RUN_TEST(test_read_chunk);
    RUN_TEST(test_open_bad_magic_safe_close);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
